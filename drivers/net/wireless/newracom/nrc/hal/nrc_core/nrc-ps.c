// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * NRC Power Save Module
 *
 * Copyright (c) 2016-2019 Newracom, Inc.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/types.h>

#include "nrc.h"
#include "nrc-hif.h"
#include "nrc-ps-common.h"
#include "hif.h"
#include "wim.h"
#include "nrc-ps.h"
#include "nrc-tx.h"
#include "nrc-fw.h"
#include "nrc-hal-core-callback.h"
#include "nrc-backend-hif-interface.h"
#include "nrc-debug.h"

#include "nrc-debug-common.h"

#ifdef CONFIG_SUPPORT_RECOVERY
#include "nrc-recovery.h"
#endif

/*
 * ============================================================================
 * PS Timing Recording Functions (for debugfs monitoring)
 * ============================================================================
 */

/**
 * nrc_ps_record_event - Record PS timing event for debugfs
 * @hdev: HIF device structure
 * @is_sleep: true for sleep event, false for wake event
 * @reason: Reason for event
 * @timeout_ms: Timeout value (for sleep events)
 */
static void nrc_ps_record_event(struct nrc_hif_device *hdev, bool is_sleep,
				enum NRC_PS_REASON reason, u64 timeout_ms)
{
	struct nrc_ps_timing_event *event;
	unsigned long flags;
	int idx;

	if (!hdev)
		return;

	spin_lock_irqsave(&hdev->ps.lock, flags);

	idx = hdev->ps.history_idx;
	event = &hdev->ps.history[idx];

	event->timestamp = ktime_get_real();
	event->state = hdev->ps.state;
	event->mode = hdev->ps.mode;
	event->reason = reason;
	event->timeout_ms = timeout_ms;
	event->is_sleep = is_sleep;

	/* Update last event info */
	if (is_sleep) {
		hdev->ps.last_sleep_time = event->timestamp;
		hdev->ps.last_sleep_timeout_ms = timeout_ms;
		hdev->ps.last_sleep_reason = reason;
	} else {
		hdev->ps.last_wake_time = event->timestamp;
		hdev->ps.last_wake_reason = reason;
	}

	/* Advance circular buffer index */
	hdev->ps.history_idx = (idx + 1) % NRC_PS_HISTORY_SIZE;
	hdev->ps.history_count++;

	spin_unlock_irqrestore(&hdev->ps.lock, flags);
}

/*
 * ============================================================================
 * Power Save State Machine Implementation
 * ============================================================================
 *
 * State Transition Table:
 *
 *   Current State    Event           Next State     Action
 *   -------------    -----           ----------     ------
 *   WAKE            SLEEP_REQ       SLEEPING       WIM already sent; HW executing sleep
 *   WAKE            WAKE_REQ        WAKE           No-op (already awake)
 *   SLEEPING        SLEEP_DONE      SLEEP          Complete sleep
 *   SLEEPING        SLEEP_FAIL      WAKE           Abort sleep (unused: WIM failure stays WAKE)
 *   SLEEPING        TIMEOUT         WAKE           Timeout recovery
 *   SLEEP           WAKE_REQ        WAKING         Toggle GPIO
 *   SLEEP           SLEEP_REQ       SLEEP          No-op (already asleep)
 *   WAKING          FW_READY        WAKE           Complete wake
 *   WAKING          TIMEOUT         WAKE           Timeout recovery
 *   WAKING          WAKE_REQ        WAKING         No-op (already waking)
 *
 * NOTE: SLEEP_REQ event is fired AFTER nrc_wim_set_ps_sync() succeeds.
 *       This ensures SLEEPING state means "WIM delivered, waiting for HW sleep".
 *       TX work never blocks the PS WIM itself.
 */

/**
 * nrc_ps_handle_event - Handle PS state machine event
 * @hdev: HIF device structure
 * @event_data: Event data with type, mode, timeout, reason
 *
 * Single entry point for all PS state transitions.
 * Uses spinlock for atomic state changes.
 *
 * Returns: 0 on success, 1 if no-op, negative error code on failure
 */
int nrc_ps_handle_event(struct nrc_hif_device *hdev,
			struct nrc_ps_event_data *event_data)
{
	enum NRC_PS_STATE old_state, new_state;
	enum NRC_PS_EVENT event;
	unsigned long flags;
	int ret = 0;

	if (!hdev || !event_data)
		return -EINVAL;

	event = event_data->event;

	spin_lock_irqsave(&hdev->ps.lock, flags);

	old_state = hdev->ps.state;
	new_state = old_state; /* Default: no change */

	switch (old_state) {
	case NRC_PS_STATE_WAKE:
		if (event == NRC_PS_EVT_SLEEP_REQ) {
			new_state = NRC_PS_STATE_SLEEPING;
			hdev->ps.sleeping_start_jiffies = jiffies;
		} else if (event == NRC_PS_EVT_WAKE_REQ) {
			/* Already awake */
			VBS_PS("Already awake, ignoring wake request");
			ret = 1;
		}
		break;

	case NRC_PS_STATE_SLEEPING:
		if (event == NRC_PS_EVT_SLEEP_DONE) {
			/* Always transition to SLEEP first to maintain state consistency */
			new_state = NRC_PS_STATE_SLEEP;
			hdev->ps.mode = event_data->mode;
			NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_PS);
		} else if (event == NRC_PS_EVT_SLEEP_FAIL ||
			   event == NRC_PS_EVT_TIMEOUT) {
			/*
			 * In deep sleep modes (TIM and NonTIM), a fail/timeout here often
			 * means the chip already woke autonomously (0xDC) before the poll
			 * could confirm sleep. The IRQ handler owns FW reload recovery.
			 */
			if (NRC_PS_IS_DEEPSLEEP(hdev)) {
				VBS_PS("Sleep transition check bypassed (event=%s, ps=%s) - deep sleep wake race",
				       nrc_ps_event_str(event),
				       nrc_ps_state_str(old_state));
			} else {
				ERR_PS("Sleep transition failed (event=%s, ps=%s, drv=%s)",
				       nrc_ps_event_str(event),
				       nrc_ps_state_str(old_state),
				       NRC_DRV_STATE_STR(hdev));
			}
			new_state = NRC_PS_STATE_WAKE;
			hdev->ps.mode = NRC_PS_NONE;
			hdev->ps.wake_pending = false;
			ret = (event == NRC_PS_EVT_TIMEOUT) ? -ETIMEDOUT : -EIO;
		} else if (event == NRC_PS_EVT_WAKE_REQ) {
			/* TX data arrived during sleep transition - set pending flag */
			hdev->ps.wake_pending = true;
			DBG_PS("Wake req during SLEEPING - %s",
			       nrc_ps_reason_str(event_data->reason));
		}
		break;

	case NRC_PS_STATE_SLEEP:
		if (event == NRC_PS_EVT_WAKE_REQ) {
			new_state = NRC_PS_STATE_WAKING;
			hdev->ps.wake_pending = true;
		} else if (event == NRC_PS_EVT_FW_READY) {
			/* Target woke up spontaneously (e.g., beacon, data) */
			new_state = NRC_PS_STATE_WAKE;
			hdev->ps.mode = NRC_PS_NONE;
			hdev->ps.wake_pending = false;
			NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_RUNNING);
		} else if (event == NRC_PS_EVT_SLEEP_REQ) {
			/* Already asleep */
			VBS_PS("Already asleep, ignoring sleep request");
			ret = 1;
		}
		break;

	case NRC_PS_STATE_WAKING:
		if (event == NRC_PS_EVT_FW_READY) {
			new_state = NRC_PS_STATE_WAKE;
			hdev->ps.mode = NRC_PS_NONE;
			hdev->ps.wake_pending = false;
			NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_RUNNING);
		} else if (event == NRC_PS_EVT_TIMEOUT) {
			/* On timeout, return to SLEEP state so next wake request can retry */
			new_state = NRC_PS_STATE_SLEEP;
			hdev->ps.mode = NRC_PS_NONE;
			hdev->ps.wake_pending = false;
			ERR_PS("Wake timeout, reverting to SLEEP");
			ret = -ETIMEDOUT;
		} else if (event == NRC_PS_EVT_WAKE_REQ) {
			/* Already waking */
			VBS_PS("Already waking, ignoring wake request");
			ret = 1;
		}
		break;

	default:
		ERR_PS("Invalid PS state: %d", old_state);
		ret = -EINVAL;
		break;
	}

	if (new_state != old_state) {
		hdev->ps.state = new_state;
		/* Simplified state transition log */
		DBG_PS("%s: %s → %s (%s, %s)", nrc_ps_event_str(event),
		       nrc_ps_state_str(old_state), nrc_ps_state_str(new_state),
		       nrc_ps_mode_str(event_data->mode),
		       nrc_ps_reason_str(event_data->reason));
	}

	spin_unlock_irqrestore(&hdev->ps.lock, flags);

	return ret;
}

/**
 * nrc_ps_request_wake - Request wake from atomic context
 * @hdev: HIF device structure
 * @reason: Reason for wake request
 *
 * Safe to call from atomic context (TX tasklet).
 * Triggers GPIO immediately without workqueue.
 *
 * Returns: 0 if wake initiated, 1 if already awake/waking, negative on error
 */
int nrc_ps_request_wake(struct nrc_hif_device *hdev, enum NRC_PS_REASON reason)
{
	struct nrc_ps_event_data event_data = {
		.event = NRC_PS_EVT_WAKE_REQ,
		.mode = NRC_PS_NONE,
		.timeout_ms = 0,
		.reason = reason,
	};
	int ret;
	int wakeup_gpio;
	int active_high;

	if (!hdev)
		return -EINVAL;

	/* Store reason for debugfs logging */
	hdev->ps.pending_wake_reason = reason;

	ret = nrc_ps_handle_event(hdev, &event_data);

	/* If state transitioned to WAKING, toggle GPIO to wake device */
	if (ret == 0) {
		/*
		 * Skip GPIO toggle for passive wake (TARGET_FW_READY)
		 * In passive wake, FW initiates wake itself and sends IRQ 0xDC
		 * GPIO toggle is only needed for active wake (host-initiated)
		 */
		if (reason == NRC_PS_REASON_TARGET_FW_READY) {
			VBS_PS("Passive wake detected, skipping GPIO toggle (FW already waking)");
		} else {
			/*
			 * Note: No need to call nrc_hif_ops_rx_thread_resume() here
			 * SPI IRQ handler will automatically unpark RX thread when
			 * FW_READY_FROM_PS interrupt (0xEC) is received
			 */
			int active_value;
			wakeup_gpio = NRC_PARAM_POWER_SAVE_GPIO(hdev, 0);
			active_high = NRC_PARAM_POWER_SAVE_GPIO(hdev, 2);
			active_value = active_high ? 1 : 0;
			VBS_PS("Wakeup GPIO %d → %d (wake)", wakeup_gpio,
			       active_value);
			nrc_hif_ops_gpio_set(wakeup_gpio, active_value);
		}
	} else if (ret == 1) {
		VBS_PS("Already wake, skip (%s)", nrc_ps_reason_str(reason));
	} else {
		ERR_PS("Wake req fail (%s): %d", nrc_ps_reason_str(reason),
		       ret);
	}

	return ret;
}

/**
 * nrc_ps_request_wake_sync - Synchronous wake with timeout
 * @hdev: HIF device structure
 * @timeout_ms: Timeout in milliseconds
 * @reason: Reason for wake request
 *
 * Must be called from sleepable context.
 * Calls nrc_ps_request_wake() then waits for completion.
 *
 * Returns: 0 on success, negative on timeout/error
 */
int nrc_ps_request_wake_sync(struct nrc_hif_device *hdev, int timeout_ms,
			     enum NRC_PS_REASON reason)
{
	int ret;
#if defined(CONFIG_DELAY_WAKE_TARGET)
	ktime_t cur_time, elapsed;
	unsigned int elapsed_msecs;

	/* Add delay if device recently entered sleep */
	cur_time = ktime_get_boottime();
	elapsed = ktime_sub(cur_time, hdev->ps_time);
	elapsed_msecs = ktime_to_ms(elapsed);

	if (elapsed_msecs < TARGET_MAX_TIME_TO_FALL_ASLEEP) {
		VBS_PS("Delaying wake by %u ms",
		       TARGET_MAX_TIME_TO_FALL_ASLEEP - elapsed_msecs);
		msleep(TARGET_MAX_TIME_TO_FALL_ASLEEP - elapsed_msecs);
	}
#endif

	/* Trigger async wake (state machine + GPIO toggle) */
	ret = nrc_ps_request_wake(hdev, reason);
	if (ret != 0) {
		/* Already awake/waking or error */
		return ret > 0 ? 0 : ret;
	}

	/* Wait for FW_READY completion */
	if (timeout_ms > 0) {
		if (wait_for_completion_timeout(&hdev->wake_done,
						msecs_to_jiffies(timeout_ms)) ==
		    0) {
			struct nrc_ps_event_data timeout_event = {
				.event = NRC_PS_EVT_TIMEOUT,
			};
			ERR_PS("TIMEOUT(%dms) waiting for FW_READY! Target failed to wake up.",
			       timeout_ms);
			nrc_ps_handle_event(hdev, &timeout_event);
#ifdef CONFIG_SUPPORT_RECOVERY
			nrc_recovery_inc(hdev, NRC_RECOVERY_WAKEUP_ERR);
#endif
			return -ETIMEDOUT;
		}
	}

	DBG_PS("Sync wake done (%d, %s)", timeout_ms,
	       nrc_ps_reason_str(reason));

#ifdef CONFIG_SUPPORT_RECOVERY
	nrc_recovery_zero(hdev, NRC_RECOVERY_WAKEUP_ERR);
#endif
	return 0;
}

/**
 * nrc_ps_handle_fw_ready - Handle FW_READY_FROM_PS interrupt
 *
 * Called by FW_READY_FROM_PS interrupt handler (IRQ 0xEC or 0x11).
 * Updates PS state machine from WAKING/SLEEP to WAKE and signals completion.
 *
 * Note: PS_READY (0x11) is sent by the target in two distinct scenarios:
 *   1. Sleep entry confirmation (SLEEPING state): target acknowledges it has
 *      entered power save. In this case the state machine has no transition
 *      for FW_READY and stays in SLEEPING. Do NOT signal wake_done here to
 *      avoid a stale completion that would make the next
 *      wait_for_completion_timeout() return prematurely.
 *   2. Auto FW reboot wake (SLEEP/WAKING state): target woke without a prior
 *      0xDC/FW-download cycle. State transitions to WAKE normally.
 */
void nrc_ps_handle_fw_ready(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct nrc_ps_event_data event_data = {
		.event = NRC_PS_EVT_FW_READY,
		.mode = NRC_PS_NONE,
	};

	/* Update state machine - transitions PS state to WAKE (if applicable) */
	nrc_ps_handle_event(hdev, &event_data);

	/*
	 * Only signal wake completion if the state machine actually transitioned
	 * to WAKE. If state is still SLEEPING (PS_READY during sleep-entry race),
	 * skip complete_all() to prevent a stale completion that would cause
	 * the next nrc_ps_request_wake_sync() to return before the device wakes.
	 */
	if (hdev->ps.state != NRC_PS_STATE_WAKE) {
		VBS_PS("FW_READY in non-WAKE state (%s), skip wake_done signal",
		       NRC_PS_STATE_STR(hdev));
		return;
	}

	/* Record wake event for debugfs monitoring */
	/* Use pending_wake_reason if set, otherwise default to TARGET_FW_READY */
	nrc_ps_record_event(hdev, false,
			    hdev->ps.pending_wake_reason ?
				    hdev->ps.pending_wake_reason :
				    NRC_PS_REASON_TARGET_FW_READY,
			    0);

	/* Resume WDT after wake */
	nrc_recovery_wdt_set_suspended(hdev, false);

	/* Signal completion for synchronous waiters */
	complete_all(&hdev->wake_done);

	/* Clear pending wake reason */
	hdev->ps.pending_wake_reason = 0;

	/* Process queued HAL frames - both WLAN and MCP */
	if (NRC_QUEUE_HAS_DATA(hdev)) {
		queue_work(hdev->workqueue, &hdev->work);
	}
	if (NRC_MCP_QUEUE_HAS_DATA(hdev)) {
		if (atomic_cmpxchg(&hdev->mcp_queue_pending, 0, 1) == 0) {
			VBS_PS("Wake done: trigger MCP TX work (queue0=%d, queue1=%d)",
			       NRC_MCP_FRAME_QUEUE_LEN(hdev),
			       NRC_MCP_WIM_QUEUE_LEN(hdev));
			queue_work(hdev->mcp_workqueue, &hdev->mcp_work);
		} else {
			VBS_PS("Wake done: MCP work already pending");
		}
	}
}

/*
 * ============================================================================
 * HAL Master PS Operations
 * ============================================================================
 */

#define NUM_WIM_SEND 5
#define NUM_PS_CHECK 10
#define NUM_PS_WAIT 10 /* ms */

/**
 * nrc_ps_wait_device_sleep - Wait for target to enter sleep mode
 * @hdev: HIF device structure
 *
 * Polls the device status via backend ops to confirm sleep entry.
 * Since the driver is in SLEEPING state, SPI ACK failures are handled quietly.
 *
 * Returns: 0 on success, 1 on FW reset, negative on timeout
 */
static int nrc_ps_wait_device_sleep(struct nrc_hif_device *hdev)
{
	int j, done_ps;

	VBS_PS("Polling sleep confirmation...");

	for (j = 0; j < NUM_PS_CHECK; j++) {
		mdelay(NUM_PS_WAIT);

		done_ps = nrc_hif_ops_check_sleep();
		if (done_ps > 0) {
			/*
			 * 3: FW reset detected (TARGET_NOTI_REQUEST_FW_DOWNLOAD, not ready)
			 * 4: FW reset detected (TARGET_NOTI_REQUEST_FW_DOWNLOAD, ROM ready)
			 *
			 * In NonTIM mode this is the normal wake path: the device autonomously
			 * reboots to ROM and requests FW download on every wake cycle.
			 * Log as DBG for NonTIM, WARN otherwise (unexpected in TIM/modem sleep).
			 */
			if (done_ps == 3 || done_ps == 4) {
				if (NRC_PS_IS_DEEPSLEEP(hdev))
					DBG_PS("Deep sleep wake detected (0xDC, result=%d) - TIM/NonTIM normal path",
					       done_ps);
				else
					WARN_PS("FW reset detected during PS operation (result=%d)",
						done_ps);
				return 1;
			}
			VBS_PS("Sleep confirmed (polled %d times, result=%d)",
			       j + 1, done_ps);
			return 0;
		} else if (done_ps < 0) {
			/*
			 * In deep sleep modes, the target may shut down the SPI interface
			 * immediately after receiving the WIM command.
			 * If we get an SPI error (-EIO) while in SLEEPING state,
			 * we should treat it as a successful sleep entry.
			 */
			VBS_PS("SPI access failed during sleep polling (ret=%d) - assuming device is asleep",
			       done_ps);
			return 0;
		}
	}

	return -ETIMEDOUT;
}

/**
 * nrc_hal_ps_request_sleep - Request sleep mode (HAL Master)
 * @mode: Power save mode
 * @timeout: Sleep duration in ms
 * @wowlan: WoWLAN configuration (optional)
 * @reason: Reason for PS mode change
 *
 * Handles complete sleep sequence including state machine and HW operations.
 * Returns: 0 on success, negative on failure
 */
int nrc_hal_ps_request_sleep(enum NRC_PS_MODE mode, u64 timeout,
			     struct cfg80211_wowlan *wowlan,
			     enum NRC_PS_REASON reason)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct nrc_ps_event_data event_data;
	int ret = -EIO;
	int wakeup_gpio;
	int active_high;
	int i;

	if (!hdev)
		return -ENODEV;

	/* Check if already asleep */
	if (hdev->ps.state == NRC_PS_STATE_SLEEP) {
		if (hdev->ps.mode != mode ||
		    hdev->ps.last_sleep_timeout_ms != (u64)timeout) {
			INFO_PS("PS reconfig in sleep: %s(%llu) -> %s(%llu), waking",
				nrc_ps_mode_str(hdev->ps.mode),
				hdev->ps.last_sleep_timeout_ms,
				nrc_ps_mode_str(mode), (u64)timeout);
			ret = nrc_ps_request_wake_sync(
				hdev, 5000,
				NRC_PS_REASON_HAL_PS_RECONFIG);
			if (ret < 0) {
				ERR_PS("Wake for reconfig failed (ret=%d)",
				       ret);
				return ret;
			}
		} else {
			VBS_PS("Already in sleep state, skip");
			return 0;
		}
	}

	/* Reject PS if FW is not running */
	if ((int)atomic_read(&hdev->fw.state) != NRC_FW_ACTIVE) {
		ERR_PS("FW not active (state=%d), cannot enter PS",
		       (int)atomic_read(&hdev->fw.state));
		return -EIO;
	}

	/* Prepare wake completion for future wake request */
	reinit_completion(&hdev->wake_done);

	/* Flush pending TX work before touching HW */
	nrc_tx_flush_wq(hdev);

	/* Set wakeup pin for deep sleep modes (before WIM command) */
	if (mode >= NRC_PS_DEEPSLEEP_TIM) {
		int inactive_value;
		wakeup_gpio = NRC_PARAM_POWER_SAVE_GPIO(hdev, 0);
		active_high = NRC_PARAM_POWER_SAVE_GPIO(hdev, 2);
		inactive_value = active_high ? 0 : 1;
		VBS_PS("Wakeup GPIO %d → %d (sleep)", wakeup_gpio,
		       inactive_value);
		nrc_hif_ops_gpio_set(wakeup_gpio, inactive_value);
	}

	/* Initialization for state transitions */
	event_data.mode = mode;
	event_data.timeout_ms = (int)timeout;
	event_data.reason = reason;

	for (i = 0; i < NUM_WIM_SEND; i++) {
		/* 1. Send WIM PS command while still in WAKE state. */
		ret = nrc_wim_set_ps(hdev, mode, timeout, wowlan);
		if (ret < 0) {
			WARN_PS("Failed to send PS WIM (ret=%d, try=%d/%d)",
				ret, i + 1, NUM_WIM_SEND);
			continue;
		}

		/* 2. CRITICAL: Wait for the TX workqueue to actually transmit the
		 * WIM command over the SPI bus while we are still in WAKE state.
		 * If we change to SLEEPING before this, the TX worker will
		 * requeue the WIM command instead of sending it.
		 */
		nrc_tx_flush_wq(hdev);

		/* 3. Transition to SLEEPING state.
		 * Now that the command is physically sent, we can safely enter
		 * SLEEPING to handle subsequent SPI ACK failures quietly.
		 */
		event_data.event = NRC_PS_EVT_SLEEP_REQ;
		nrc_ps_handle_event(hdev, &event_data);

		/* 4. Wait/Poll for target to enter sleep mode */
		ret = nrc_ps_wait_device_sleep(hdev);
		if (ret == 0) {
			/* Success: target is confirmed to be in sleep */
			goto sleep_done;
		} else if (ret == 1) {
			/*
			 * FW reset detected during sleep polling
			 * (TARGET_NOTI_REQUEST_FW_DOWNLOAD, result=3/4).
			 *
			 * This is the normal NonTIM wake path: device exits deep
			 * sleep, reboots to ROM, and requests FW download via 0xDC
			 * notification. The IRQ handler (nrc_hal_handle_request_fw_
			 * download) is the designated owner of FW reload for this
			 * event and its work item is already queued.
			 *
			 * Do NOT call nrc_fw_reload() here — doing so races with
			 * the IRQ path on NRC_FW_LOADING state, causing the PS
			 * path's fw_wait_ready(3s) to time out while the IRQ path
			 * is blocked (EBUSY), resulting in periodic false
			 * "FW download completed but verification failed" errors.
			 */
			event_data.event = NRC_PS_EVT_SLEEP_FAIL;
			nrc_ps_handle_event(hdev, &event_data);
			return -EIO;
		}

		/* 4. Timeout: transition back to WAKE to retry WIM command */
		event_data.event = NRC_PS_EVT_SLEEP_FAIL;
		nrc_ps_handle_event(hdev, &event_data);

		/* Yield CPU before retry */
		usleep_range(NUM_PS_WAIT * 1000, NUM_PS_WAIT * 2000);
	}

	ERR_PS("Sleep timeout (%s, %d tries)", nrc_ps_mode_str(mode), i);
	return -ETIMEDOUT;

sleep_done:
	/* Handle mode-specific operations after confirmed sleep */
	if (mode == NRC_PS_MODEMSLEEP) {
		hdev->ps.modem_enabled = true;
	} else if (mode >= NRC_PS_DEEPSLEEP_TIM) {
		nrc_hif_ops_rx_thread_suspend();
	}

	/* Suspend WDT during sleep to prevent keep-alive TX waking target */
	nrc_recovery_wdt_set_suspended(hdev, true);

	/* Final transition to SLEEP state */
	event_data.event = NRC_PS_EVT_SLEEP_DONE;
	nrc_ps_handle_event(hdev, &event_data);

#if defined(CONFIG_DELAY_WAKE_TARGET)
	/* Record precise timing after target enters PS */
	hdev->ps_time = ktime_get_boottime();
#endif

	/* Record sleep event for debugfs monitoring */
	nrc_ps_record_event(hdev, true, reason, timeout);

	/* Check if immediate wake is needed */
	if (hdev->ps.wake_pending || NRC_QUEUE_HAS_DATA(hdev) ||
	    NRC_MCP_QUEUE_HAS_DATA(hdev)) {
		DBG_PS("Immediate wake (wp=%d, wlan=%d, mcp=%d)",
		       hdev->ps.wake_pending,
		       NRC_FRAME_QUEUE_LEN(hdev) + NRC_WIM_QUEUE_LEN(hdev),
		       NRC_MCP_FRAME_QUEUE_LEN(hdev) +
			       NRC_MCP_WIM_QUEUE_LEN(hdev));
		nrc_ps_request_wake(hdev, hdev->ps.pending_wake_reason ?
						  hdev->ps.pending_wake_reason :
						  NRC_PS_REASON_HAL_TX_WAKEUP);
	}

	return 0;
}

/**
 * nrc_hal_ps_request_wake - Request wake from sleep (HAL Master)
 * @timeout_ms: Timeout in ms (0 for async)
 * @reason: Reason for wake
 *
 * Handles complete wake sequence including state machine and HW operations.
 * Returns: 0 on success, negative on failure
 */
int nrc_hal_ps_request_wake(int timeout_ms, enum NRC_PS_REASON reason)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev)
		return -ENODEV;

	/* WLAN frontend already logged wake request with reason text */

	/* Store wake reason for later use in wake_target_done */
	hdev->ps.pending_wake_reason = reason;

	/* Check if already awake */
	if (hdev->ps.state == NRC_PS_STATE_WAKE &&
	    hdev->ps.mode == NRC_PS_NONE) {
		VBS_PS("Already awake, skip");
		return 0;
	}

	/* Use existing wake functions which handle state machine */
	if (timeout_ms == 0) {
		return nrc_ps_request_wake(hdev, reason);
	}

	return nrc_ps_request_wake_sync(hdev, timeout_ms, reason);
}
