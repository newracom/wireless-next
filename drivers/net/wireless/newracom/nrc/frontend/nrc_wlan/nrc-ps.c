// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 */

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"
#include "nrc-ps-common.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Local module headers */
#include "nrc-mac80211.h"
#include "nrc-ps.h"
#include "nrc-twt-sched.h"

#define ATOMIC_LOCK_UNLOCKED (0)
#define ATOMIC_LOCK_LOCKED (1)

static DEFINE_MUTEX(ps_set_mode);

/**
 * nrc_has_active_ap_vif - Check if any AP VIF is currently active
 * @nw: NRC structure
 *
 * Returns: true if at least one AP (or P2P_GO) VIF is registered.
 *
 * Concurrent PS policy:
 *   AP+STA: FW power-save engine is shared and cannot operate correctly
 *           while an AP VIF is serving clients. PS sleep is suppressed.
 *   AP+AP : Same — both APs must stay awake to send beacons.
 *   STA+STA: PS is allowed, but FW tracks only a single VIF (vif_id=0).
 *            Only STA0's PS setting is honoured by the firmware.
 */
static bool nrc_has_active_ap_vif(struct nrc *nw)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		if (!nw->vif[i])
			continue;
		if (nw->vif[i]->type == NL80211_IFTYPE_AP ||
		    nw->vif[i]->type == NL80211_IFTYPE_P2P_GO)
			return true;
	}
	return false;
}

/**
 * nrc_ps_set_mode - Set power save mode
 * @nw: NRC structure
 * @mode: Power save mode (NRC_PS_NONE for wake, others for sleep)
 * @timeout: Timeout in milliseconds (-1 for infinite)
 * @wowlan: WoWLAN configuration (NULL if not used)
 * @reason: Power save reason code
 *
 * Returns: 0 on success, negative on error
 */
int nrc_ps_set_mode(struct nrc *nw, enum NRC_PS_MODE mode, u64 timeout,
		    struct cfg80211_wowlan *wowlan, enum NRC_PS_REASON reason)
{
	struct ieee80211_hw *hw = nw->hw;
	int ret = 0;

	/* Skip if power save disabled */
	if (nw->params->power_save == 0)
		return 0;

	/*
	 * Concurrent PS policy: suppress sleep when an AP VIF is active.
	 * Wake requests (NRC_PS_NONE) are always allowed so the chip can
	 * be brought back from any unexpected sleep state.
	 */
	if (mode != NRC_PS_NONE && nrc_has_active_ap_vif(nw)) {
		INFO_PS("PS sleep suppressed: AP VIF active (concurrent mode)");
		return 0;
	}

	/* Simple logging - HAL handles state/mode checking */
	if (mode == NRC_PS_NONE) {
		DBG_PS("Wake by %s", nrc_ps_reason_str(reason));
	} else {
		DBG_PS("%s (%llu ms) by %s", nrc_ps_mode_str(mode), timeout,
		       nrc_ps_reason_str(reason));
	}

	mutex_lock(&ps_set_mode);

	if (mode == NRC_PS_NONE) {
		/* Wake request - HAL checks if already awake */

		/* Scheduled scan needs extra timeout */
		if (atomic_read(&nw->scan_mode) ==
		    NRC_SCAN_MODE_SCHED_SCANNING) {
			timeout += 20000;
			INFO("Add more time to wait for sched scan (%llu)",
			     timeout);
		}

		/* Request wake via HAL */
		ret = nrc_hal_ops_ps_request_wake((int)timeout, reason);
		goto done;
	}

	/* Sleep request - HAL checks if already in same mode */

	if (NRC_DRV_IS_STOPPED(nw->hdev) || NRC_DRV_IS_REBOOT(nw->hdev)) {
		goto done;
	}

	/* WLAN-specific pre-sleep operations */
	if (!nw->params->disable_cqm) {
		int _i;

		for (_i = 0; _i < NR_NRC_VIF; _i++) {
			if (nw->vif[_i] &&
			    nw->vif[_i]->type == NL80211_IFTYPE_STATION)
				try_to_del_timer_sync(
					&to_i_vif(nw->vif[_i])->bcn_mon_timer);
		}
	}

	ieee80211_stop_queues(hw);

	nrc_cleanup_txq_all(nw);

	/* Request sleep via HAL (handles state machine + HW ops) */
	ret = nrc_hal_ops_ps_request_sleep(mode, timeout, wowlan, reason);

	if (ret < 0) {
		/* Sleep failed - resume operations */
		ieee80211_wake_queues(hw);

		/* Recovery: restart dynamic PS and beacon monitor */
		{
			unsigned long fallback_timeout = 5000;
			int _i;

			for (_i = 0; _i < NR_NRC_VIF; _i++) {
				struct nrc_vif *_iv;

				if (!nw->vif[_i] ||
				    nw->vif[_i]->type != NL80211_IFTYPE_STATION)
					continue;
				_iv = to_i_vif(nw->vif[_i]);
				if (_iv->associated && _iv->beacon_timeout)
					fallback_timeout = _iv->beacon_timeout;
			}
			nrc_ps_dyn_start(nw, fallback_timeout + 2000,
					 NRC_PS_REASON_TARGET_FAILED_ENTER_PS);
			if (!nw->params->disable_cqm) {
				for (_i = 0; _i < NR_NRC_VIF; _i++) {
					struct nrc_vif *_iv;

					if (!nw->vif[_i] ||
					    nw->vif[_i]->type !=
						    NL80211_IFTYPE_STATION)
						continue;
					_iv = to_i_vif(nw->vif[_i]);
					if (_iv->associated)
						mod_timer(
							&_iv->bcn_mon_timer,
							jiffies +
								msecs_to_jiffies(
									_iv->beacon_timeout));
				}
			}
		}
	}

done:
	mutex_unlock(&ps_set_mode);
	return ret;
}

/* Dynamic PS */

/* Work handler for dynamic PS - called in process context */
static void nrc_ps_dynamic_work(struct work_struct *work)
{
	struct nrc *nw = container_of(work, struct nrc, dynamic_ps_work);
	struct nrc_hif_device *hdev = nw->hdev;

	VBS_PS("PS timer start: to=%dms ex=%dms busy_delay=%dms st=%s",
	       nw->hdev->ps.timeout, nw->params->extra_ps_timeout,
	       atomic_read(&nw->ps_busy_delay_ms), NRC_DRV_STATE_STR(hdev));

	/*
	 * If a busy-guard delay was requested (e.g. BA setup, netlink cmd),
	 * clear it and re-arm the timer once to let the operation finish
	 * before entering sleep.
	 */
	if (atomic_read(&nw->ps_busy_delay_ms)) {
		atomic_set(&nw->ps_busy_delay_ms, 0);
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_DYNAMIC_PS);
		return;
	}

	/*
	 * Defer sleep if TX is active.
	 * Check both queue data and work-in-progress flags to avoid
	 * sleeping while TX work handlers are still processing frames.
	 * This replaces the old per-packet PS delay logic in nrc-tx.c.
	 */
	if (NRC_QUEUE_HAS_DATA(hdev) || NRC_MCP_QUEUE_HAS_DATA(hdev) ||
	    atomic_read(&hdev->queue_pending) ||
	    atomic_read(&hdev->mcp_queue_pending)) {
		DBG_PS("TX active, defer PS (wlan_q=%d/%d mcp_q=%d/%d pending=%d/%d)",
		       NRC_FRAME_QUEUE_LEN(hdev), NRC_WIM_QUEUE_LEN(hdev),
		       NRC_MCP_FRAME_QUEUE_LEN(hdev),
		       NRC_MCP_WIM_QUEUE_LEN(hdev),
		       atomic_read(&hdev->queue_pending),
		       atomic_read(&hdev->mcp_queue_pending));
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_DYNAMIC_PS);
		return;
	}

	if ((int)atomic_read(&hdev->fw.state) == NRC_FW_LOADING) {
		DBG_PS("FW is loading, skip PS");
		return;
	}

	if ((int)atomic_read(&hdev->fw.state) == NRC_FW_FAILED) {
		ERR_PS("FW is in FAILED state, skip PS (will retry via IRQ path)");
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_DYNAMIC_PS);
		return;
	}

	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE) {
		DBG_PS("Scanning in progress, skip PS");
		return;
	}

	if (NRC_DRV_IS_ASLEEP(nw->hdev)) {
		DBG_PS("Target is already in deepsleep...");
		return;
	}

	/*
	 * TWT configured but session not yet established: suppress sleep.
	 * twt_sched is created at module load when twt_sp is set, but the
	 * TWT session is only active when twt_sched->started is true.
	 * Calling DEEPSLEEP_TIM before the session starts conflicts with FW
	 * TWT init.  Sleep is driven by TWT QUIET events once started.
	 */
	if (nw->params->twt_sp > 0 && nw->params->twt_force_sleep &&
	    nw->twt_sched && !READ_ONCE(nw->twt_sched->started)) {
		DBG_PS("TWT configured but not yet active — skip DEEPSLEEP_TIM");
		return;
	}

	/* Use unified PS set mode path */
	nrc_ps_set_mode(nw, NRC_PARAM_POWER_SAVE(hdev),
			hdev->params->sleep_duration[0] *
				(hdev->params->sleep_duration[1] ? 1000 : 1),
			NULL, NRC_PS_REASON_DRV_DYNAMIC_PS);
}

/* Timer callback - runs in atomic context, just schedules work */
static void nrc_ps_timeout_timer(struct timer_list *t)
{
	struct nrc *nw = from_timer(nw, t, dynamic_ps_timer);
	/* Schedule work to handle PS in process context */
	schedule_work(&nw->dynamic_ps_work);
}

void nrc_ps_dyn_init(struct nrc *nw)
{
	if (!nw->hdev->ps.supports_dynamic_ps)
		return;

	atomic_set(&nw->ps_busy_delay_ms, 0);

	/* Initialize work queue for dynamic PS */
	INIT_WORK(&nw->dynamic_ps_work, nrc_ps_dynamic_work);

	timer_setup(&nw->dynamic_ps_timer, nrc_ps_timeout_timer, 0);
}

void nrc_ps_dyn_deinit(struct nrc *nw)
{
	if (!nw->hdev->ps.supports_dynamic_ps)
		return;

	atomic_set(&nw->ps_busy_delay_ms, 0);

	/* Cancel timer and pending work */
	del_timer_sync(&nw->dynamic_ps_timer);
	cancel_work_sync(&nw->dynamic_ps_work);
}

/**
 * nrc_ps_dyn_start - Start (or re-arm) the dynamic PS idle timer
 * @nw:            NRC driver instance
 * @busy_delay_ms: Minimum ms to stay awake before the idle timer may fire.
 *                 Pass 0 for a normal base-timeout start (no extra guard).
 *                 Pass N > 0 when the caller knows an ongoing operation
 *                 needs at least N ms to complete (e.g. beacon wait, EAPOL).
 * @reason:        Why PS is being (re-)started; used only for debug logging.
 *
 * Single entry point for all dynamic PS timer arming.  Handles two modes:
 *
 *  TWT mode (twt_sched && twt_force_sleep):
 *    Timeout = TWT service period (sp).  hdev->ps.timeout is updated so the
 *    work handler and debugfs always reflect the active TWT interval.
 *    busy-guard and scan checks are bypassed — TWT timing is AP-dictated.
 *
 *  Normal mode:
 *    Timeout = max(ps.timeout + extra_ps_timeout, busy_delay_ms).
 *    If a busy-guard is already active (ps_busy_delay_ms != 0), a plain
 *    start (busy_delay_ms == 0) is suppressed so the guard is not cancelled
 *    prematurely.
 */
void nrc_ps_dyn_start(struct nrc *nw, int busy_delay_ms,
		      enum NRC_PS_REASON reason)
{
	int base_timeout = nw->hdev->ps.timeout + nw->params->extra_ps_timeout;
	int timeout;

	if (!nw->hdev->ps.supports_dynamic_ps || !NRC_DRV_IS_READY(nw->hdev))
		return;

	/* TWT force-sleep: timer period is the TWT service period */
	if (nw->twt_sched && nw->params->twt_force_sleep) {
		timeout = (int)(div_u64(nw->twt_sched->sp, USEC_PER_MSEC));
		nw->hdev->ps.timeout = timeout;
		goto arm_timer;
	}

	/*
	 * TWT configured but session not yet established: suppress timer.
	 * twt_sched->started becomes true only after TWT negotiation completes.
	 */
	if (nw->params->twt_sp > 0 && nw->params->twt_force_sleep &&
	    nw->twt_sched && !READ_ONCE(nw->twt_sched->started)) {
		DBG_PS("dyn_start: suppressed — TWT configured but not yet active");
		return;
	}

	/* Normal dynamic PS guards */
	if (nw->params->power_save == 0 || nw->hdev->ps.timeout <= 0)
		return;

	/* Never arm the PS timer during an active scan */
	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE)
		return;

	/* Concurrent PS policy: do not start the idle timer when an AP VIF
	 * is active.  nrc_ps_set_mode() carries the same guard, so even if
	 * the timer fires it would be a no-op — but stopping here is cleaner.
	 */
	if (nrc_has_active_ap_vif(nw))
		return;

	/* Suppress plain re-arm while a busy-guard is still active */
	if (busy_delay_ms == 0 && atomic_read(&nw->ps_busy_delay_ms))
		return;

	atomic_set(&nw->ps_busy_delay_ms, busy_delay_ms);
	timeout = busy_delay_ms > base_timeout ? busy_delay_ms : base_timeout;

arm_timer:
	DBG_PS("dyn_start: t=%d bd=%d r=%d", timeout, busy_delay_ms, reason);
	mod_timer(&nw->dynamic_ps_timer, jiffies + msecs_to_jiffies(timeout));
}

void nrc_ps_dyn_stop(struct nrc *nw, enum NRC_PS_REASON reason)
{
	struct nrc_hif_device *hdev = nw->hdev;

	atomic_set(&nw->ps_busy_delay_ms, 0);

	if (!hdev->ps.supports_dynamic_ps)
		return;

	if (NRC_DRV_IS_READY(hdev) && hdev->ps.timeout > 0) {
		DBG_PS("dyn_stop: timer off r=%d", reason);
		try_to_del_timer_sync(&nw->dynamic_ps_timer);
	}

	if (NRC_DRV_IS_ASLEEP(hdev) || hdev->ps.modem_enabled) {
		DBG_PS("dyn_stop: asleep, wake r=%d", reason);
		nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL, reason);
	}
}

int nrc_ps_set_idle_mode(struct nrc *nw, char *msg)
{
	if (nrc_idle_mode_get_state(nw)) {
		DBG_MAC("Entering IDLE: %s", msg);
		nrc_ps_set_mode(nw, NRC_PS_DEEPSLEEP_NONTIM, -1, NULL,
				NRC_PS_REASON_MAC_IDLE_ENTER);
		return 0;
	}

	return -1;
}

/* This delay gives a chance to connect with scanned info before entering idle mode */
int nrc_ps_set_idle_mode_delay(struct nrc *nw, char *msg, int delay_ms)
{
	if (nw->hdev->event_workqueue == NULL)
		return -1;

	DBG_MAC("Entering IDLE with delay:%s (%dms)", msg, delay_ms);
	queue_delayed_work(nw->hdev->event_workqueue, &nw->idle_work,
			   msecs_to_jiffies(delay_ms));

	return 0;
}

void nrc_ps_set_idle_mode_work_handler(struct work_struct *work)
{
	struct nrc *nw =
		container_of(to_delayed_work(work), struct nrc, idle_work);
	int ret;

	if (!mutex_trylock(&nw->state_mtx)) {
		/* need to wait until receiving wim resp
		 * or other threads process ps */
		WRN("idle_mode_work_handler: not handled");
		return;
	}

	/* there are no other threads to enter idle mode right now */
	ret = nrc_ps_set_idle_mode(nw, "idle_mode_work_handler");
	if (ret != 0) {
		DBG_MAC("idle_mode_work_handler: not idle");
	}

	mutex_unlock(&nw->state_mtx);
}

/**
 * nrc_ps_get_state_str - Get current power save state as string
 * @nw: NRC structure
 *
 * Returns: String representation of current power save state
 */
const char *nrc_ps_get_state_str(struct nrc *nw)
{
	return NRC_PS_STATE_STR(nw->hdev);
}
