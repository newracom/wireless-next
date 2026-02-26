/*
 * NRC Power Save Module
 *
 * Copyright (c) 2016-2019 Newracom, Inc.
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
#include "nrc-hal-core-callback.h"
#include "nrc-backend-hif-interface.h"
#include "nrc-debug.h"

#ifdef CONFIG_NRC_TRACING
#include "nrc-trace.h"
#else
#include "nrc-debug-common.h"
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
 *   WAKE            SLEEP_REQ       SLEEPING       Start sleep sequence
 *   WAKE            WAKE_REQ        WAKE           No-op (already awake)
 *   SLEEPING        SLEEP_DONE      SLEEP          Complete sleep
 *   SLEEPING        SLEEP_FAIL      WAKE           Abort sleep
 *   SLEEPING        TIMEOUT         WAKE           Timeout recovery
 *   SLEEP           WAKE_REQ        WAKING         Toggle GPIO
 *   SLEEP           SLEEP_REQ       SLEEP          No-op (already asleep)
 *   WAKING          FW_READY        WAKE           Complete wake
 *   WAKING          TIMEOUT         WAKE           Timeout recovery
 *   WAKING          WAKE_REQ        WAKING         No-op (already waking)
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
			DBG_PS("Already awake, ignoring wake request");
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
			new_state = NRC_PS_STATE_WAKE;
			hdev->ps.mode = NRC_PS_NONE;
			hdev->ps.wake_pending = false;
			ret = (event == NRC_PS_EVT_TIMEOUT) ? -ETIMEDOUT : -EIO;
		} else if (event == NRC_PS_EVT_WAKE_REQ) {
			/* TX data arrived during sleep transition - set pending flag */
			hdev->ps.wake_pending = true;
			DBG_PS("Wake requested during SLEEPING - will wake after sleep done (reason=%s)",
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
			DBG_PS("Already asleep, ignoring sleep request");
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
			DBG_PS("Already waking, ignoring wake request");
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
		/* Unified state transition log with event, mode, and reason */
		DBG_PS("%s: %s → %s (mode=%s, reason=%s)",
		       nrc_ps_event_str(event), nrc_ps_state_str(old_state),
		       nrc_ps_state_str(new_state),
		       nrc_ps_mode_str(event_data->mode),
		       nrc_ps_reason_str(event_data->reason));

#if defined(ANDROID) && defined(CONFIG_PM_SLEEP)
		/* Android PM control on state transitions */
		if (new_state == NRC_PS_STATE_SLEEP && hdev->dev) {
			DBG_PS("pm_relax: entering sleep");
			pm_relax(hdev->dev);
		} else if ((new_state == NRC_PS_STATE_WAKE ||
			    new_state == NRC_PS_STATE_WAKING) &&
			   old_state == NRC_PS_STATE_SLEEP && hdev->dev) {
			DBG_PS("pm_stay_awake: waking up");
			pm_stay_awake(hdev->dev);
		}
#endif
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
			DBG_PS("Passive wake detected, skipping GPIO toggle (FW already waking)");
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
			DBG_PS("Wakeup GPIO %d → %d (wake)", wakeup_gpio,
			       active_value);
			nrc_hif_ops_gpio_set(wakeup_gpio, active_value);
		}
	} else if (ret == 1) {
		DBG_PS("Already waking/awake, skip GPIO toggle (async, reason=%d)",
		       reason);
	} else {
		ERR_PS("Failed to request wake (async, reason=%d): ret=%d",
		       reason, ret);
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
		DBG_PS("Delaying wake by %u ms",
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
			return -ETIMEDOUT;
		}
	}

	DBG_PS("Sync wake completed successfully (timeout=%d, reason=%d)",
	       timeout_ms, reason);

	return 0;
}

/**
 * nrc_ps_handle_fw_ready - Handle FW_READY_FROM_PS interrupt
 *
 * Called by FW_READY_FROM_PS interrupt handler (IRQ 0xEC).
 * Updates PS state machine from WAKING to WAKE and signals completion.
 */
void nrc_ps_handle_fw_ready(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct nrc_ps_event_data event_data = {
		.event = NRC_PS_EVT_FW_READY,
		.mode = NRC_PS_NONE,
	};

	/* Update state machine - this transitions PS state to WAKE */
	nrc_ps_handle_event(hdev, &event_data);

	/* Record wake event for debugfs monitoring */
	/* Use pending_wake_reason if set, otherwise default to TARGET_FW_READY */
	nrc_ps_record_event(hdev, false,
			    hdev->ps.pending_wake_reason ?
				    hdev->ps.pending_wake_reason :
				    NRC_PS_REASON_TARGET_FW_READY,
			    0);

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
			DBG_PS("Wake done: trigger MCP TX work (queue0=%d, queue1=%d)",
			       NRC_MCP_FRAME_QUEUE_LEN(hdev),
			       NRC_MCP_WIM_QUEUE_LEN(hdev));
			queue_work(hdev->mcp_workqueue, &hdev->mcp_work);
		} else {
			DBG_PS("Wake done: MCP work already pending");
		}
	}
}

/*
 * ============================================================================
 * HAL Master PS Operations
 * ============================================================================
 */

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
	int ret;
	int wakeup_gpio;
	int active_high;

	if (!hdev)
		return -ENODEV;

	/* Check if already asleep */
	if (hdev->ps.state == NRC_PS_STATE_SLEEP) {
		DBG_PS("Already in sleep state, skip");
		return 0;
	}

	/* Prepare wake completion for future wake request */
	reinit_completion(&hdev->wake_done);

	/* Request sleep via state machine */
	event_data.event = NRC_PS_EVT_SLEEP_REQ;
	event_data.mode = mode;
	event_data.timeout_ms = (int)timeout;
	event_data.reason = reason;

	ret = nrc_ps_handle_event(hdev, &event_data);
	if (ret != 0) {
		if (ret > 0)
			return 0; /* No-op, already in desired state */
		return ret;
	}

	/* Flush workqueue before GPIO setting and WIM transmission */
	nrc_tx_flush_wq(hdev);

	/* Set wakeup pin for deep sleep modes (before WIM command) */
	if (mode >= NRC_PS_DEEPSLEEP_TIM) {
		int inactive_value;
		wakeup_gpio = NRC_PARAM_POWER_SAVE_GPIO(hdev, 0);
		active_high = NRC_PARAM_POWER_SAVE_GPIO(hdev, 2);
		inactive_value = active_high ? 0 : 1;
		DBG_PS("Wakeup GPIO %d → %d (sleep)", wakeup_gpio,
		       inactive_value);
		nrc_hif_ops_gpio_set(wakeup_gpio, inactive_value);
	}

	/* Send WIM PS command with polling verification */
	ret = nrc_wim_set_ps_sync(hdev, mode, timeout, wowlan);
	if (ret < 0) {
		/* Update state machine: sleep failed */
		event_data.event = NRC_PS_EVT_SLEEP_FAIL;
		nrc_ps_handle_event(hdev, &event_data);

		/* Process delayed frames from hif_work */
		queue_work(hdev->workqueue, &hdev->work);
		return ret;
	}

	/* Handle mode-specific operations */
	if (mode == NRC_PS_MODEMSLEEP) {
		hdev->ps.modem_enabled = true;
	} else if (mode >= NRC_PS_DEEPSLEEP_TIM) {
		nrc_hif_ops_rx_thread_suspend();
	}

	/* Update state machine: sleep done */
	event_data.event = NRC_PS_EVT_SLEEP_DONE;
	event_data.mode = mode;
	nrc_ps_handle_event(hdev, &event_data);

#if defined(CONFIG_DELAY_WAKE_TARGET)
	/* Record precise timing after target enters PS */
	hdev->ps_time = ktime_get_boottime();
#endif

	/* Record sleep event for debugfs monitoring */
	nrc_ps_record_event(hdev, true, reason, timeout);

	/* Check if wake was requested during sleep transition */
	if (hdev->ps.wake_pending) {
		DBG_PS("Wake pending after sleep - triggering immediate wake");
		/* Request wake immediately (this will handle state transition and GPIO) */
		nrc_ps_request_wake(hdev, hdev->ps.pending_wake_reason ?
						  hdev->ps.pending_wake_reason :
						  NRC_PS_REASON_HAL_TX_WAKEUP);
	}

	return ret;
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
		DBG_PS("Already awake, skip");
		return 0;
	}

	/* Use existing wake functions which handle state machine */
	if (timeout_ms == 0) {
		return nrc_ps_request_wake(hdev, reason);
	}

	return nrc_ps_request_wake_sync(hdev, timeout_ms, reason);
}
