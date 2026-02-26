/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
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

	if (NRC_DRV_IS_CLOSING(nw->hdev) || NRC_DRV_IS_REBOOT(nw->hdev)) {
		goto done;
	}

	/* WLAN-specific pre-sleep operations */
	if (!nw->params->disable_cqm) {
		try_to_del_timer_sync(&nw->bcn_mon_timer);
	}

	ieee80211_stop_queues(hw);

#ifdef CONFIG_USE_TXQ
	nrc_cleanup_txq_all(nw);
#endif

	/* Request sleep via HAL (handles state machine + HW ops) */
	ret = nrc_hal_ops_ps_request_sleep(mode, timeout, wowlan, reason);

	if (ret < 0) {
		/* Sleep failed - resume operations */
		ieee80211_wake_queues(hw);

		/* Recovery: restart dynamic PS and beacon monitor */
		nrc_ps_dyn_start_custom_timeout(nw, nw->beacon_timeout + 2000);
		if (!nw->params->disable_cqm && nw->associated_vif) {
			mod_timer(&nw->bcn_mon_timer,
				  jiffies +
					  msecs_to_jiffies(nw->beacon_timeout));
		}
	}

done:
	mutex_unlock(&ps_set_mode);
	return ret;
}

/* Dynamic PS */

int g_custom_timeout;

/* Work handler for dynamic PS - called in process context */
static void nrc_ps_dynamic_work(struct work_struct *work)
{
	struct nrc *nw = container_of(work, struct nrc, dynamic_ps_work);
	struct nrc_hif_device *hdev = nw->hdev;

	DBG_PS("Dynamic PS work: enabled=%d timeout=%dms extra=%dms custom=%dms state=%s",
	       nw->hdev->ps.enabled, nw->hw->conf.dynamic_ps_timeout,
	       nw->params->extra_ps_timeout, g_custom_timeout,
	       NRC_DRV_STATE_STR(hdev));

	if (g_custom_timeout) {
		g_custom_timeout = 0;
		nrc_ps_dyn_start(nw);
		return;
	}

	if ((int)atomic_read(&hdev->fw.state) == NRC_FW_LOADING) {
		DBG_PS("FW is loading, skip PS");
		return;
	}

	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE) {
		DBG_PS("Scanning in progress, skip PS");
		return;
	}

	if (hdev->ps.enabled) {
		if (NRC_DRV_IS_ASLEEP(nw->hdev)) {
			/*
			 * if the current state is already NRC_DRV_PS,
			 * there's nothing to do in here even if mac80211 notifies wake-up.
			 * the actual action to wake up for target will be done by
			 * nrc_wake_tx_queue() with changing gpio signal.
			 * (when driver receives a data frame.)
			 */
			DBG_PS("Target is already in deepsleep...");
			return;
		}

		/* Use unified PS set mode path */
		nrc_ps_set_mode(
			nw, NRC_PARAM_POWER_SAVE(hdev),
			hdev->params->sleep_duration[0] *
				(hdev->params->sleep_duration[1] ? 1000 : 1),
			NULL, NRC_PS_REASON_DRV_DYNAMIC_PS);
	}
}

/* Timer callback - runs in atomic context, just schedules work */
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
static void nrc_ps_timeout_timer(unsigned long data)
{
	struct nrc *nw = (struct nrc *)data;
#else
static void nrc_ps_timeout_timer(struct timer_list *t)
{
	struct nrc *nw = from_timer(nw, t, dynamic_ps_timer);
#endif
	/* Schedule work to handle PS in process context */
	schedule_work(&nw->dynamic_ps_work);
}

void nrc_ps_dyn_init(struct nrc *nw)
{
	if (!nw->hdev->ps.supports_dynamic_ps)
		return;

	g_custom_timeout = 0;

	/* Initialize work queue for dynamic PS */
	INIT_WORK(&nw->dynamic_ps_work, nrc_ps_dynamic_work);

#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
	setup_timer(&nw->dynamic_ps_timer, nrc_ps_timeout_timer,
		    (unsigned long)nw);
#else
	timer_setup(&nw->dynamic_ps_timer, nrc_ps_timeout_timer, 0);
#endif
}

void nrc_ps_dyn_deinit(struct nrc *nw)
{
	if (!nw->hdev->ps.supports_dynamic_ps)
		return;

	g_custom_timeout = 0;

	/* Cancel timer and pending work */
	del_timer_sync(&nw->dynamic_ps_timer);
	cancel_work_sync(&nw->dynamic_ps_work);
}

void nrc_ps_dyn_start_custom_timeout(struct nrc *nw, int custom_timeout)
{
	int timeout;

	if (!nw->hdev->ps.supports_dynamic_ps || !NRC_DRV_IS_READY(nw->hdev) ||
	    (nw->twt_sched && nw->params->twt_force_sleep) ||
	    nw->hw->conf.dynamic_ps_timeout <= 0)
		return;

	/* Don't start PS timer during scan - need to stay awake for PROBE_RESP */
	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE)
		return;

	g_custom_timeout = custom_timeout;

	if (custom_timeout >
	    nw->params->extra_ps_timeout + nw->hw->conf.dynamic_ps_timeout) {
		DBG_STATE("custom timeout is set to %dms", custom_timeout);
		timeout = custom_timeout;
	} else {
		timeout = nw->params->extra_ps_timeout +
			  nw->hw->conf.dynamic_ps_timeout;
	}

	mod_timer(&nw->dynamic_ps_timer, jiffies + msecs_to_jiffies(timeout));
}

void nrc_ps_dyn_start(struct nrc *nw)
{
	if (g_custom_timeout)
		return; /* don't rearm until custom_timeout end. */

	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE)
		return;

	nrc_ps_dyn_start_custom_timeout(nw, 0);
}

void nrc_ps_dyn_stop(struct nrc *nw)
{
	g_custom_timeout = 0;

	if (!nw->hdev->ps.supports_dynamic_ps)
		return;

	if (NRC_DRV_IS_READY(nw->hdev) && nw->hw->conf.dynamic_ps_timeout > 0) {
		DBG_PS("%s Dynamic PS timer off %ul", __func__,
		       nw->hw->conf.dynamic_ps_timeout);
		try_to_del_timer_sync(&nw->dynamic_ps_timer);
	}
}

void nrc_ps_dyn_start_twt(struct nrc *nw)
{
	int twt_timeout_ms;

	if (!nw->hdev->ps.supports_dynamic_ps || !NRC_DRV_IS_READY(nw->hdev) ||
	    !nw->twt_sched || !nw->params->twt_force_sleep)
		return;

	/* Set timeout based on TWT service period */
	twt_timeout_ms = (int)(div_u64(nw->twt_sched->sp, USEC_PER_MSEC));
	nw->hw->conf.dynamic_ps_timeout = twt_timeout_ms;

	mod_timer(&nw->dynamic_ps_timer,
		  jiffies + msecs_to_jiffies(twt_timeout_ms));
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
		WARN_WLAN("idle_mode_work_handler: not handled");
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
