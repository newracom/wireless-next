// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 */

/* Linux kernel headers */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>

/* Networking headers */
#include <net/ieee80211_radiotap.h>

/* Networking headers */
#include <net/mac80211.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Callback */
#include "nrc-hal-core-callback.h"
#include "nrc-backend-hif-callback.h"

/* Common directory headers - Interface */
#include "nrc-backend-hif-interface.h"

/* Local module headers */
#include "nrc-tx.h"
#include "nrc-ps.h"
#include "wim.h"
#include "hif.h"
#ifdef CONFIG_SUPPORT_RECOVERY
#include "nrc-recovery.h"
#endif

/*
 * LEGACY CODE - Kept for historical reference only
 * This function searched for obsolete WIM_CMD_SLEEP format with embedded duration.
 * Current PS architecture uses nrc_wim_set_ps() which sends WIM_CMD_SET + WIM_TLV_PS_ENABLE.
 * This format is never generated in current codebase. - 2025-12-02
 */

/**
 * nrc_hif_update_loopback_debug_time - Update loopback debug timing information
 * @hdev: HIF device containing debug information
 * @skb: socket buffer containing HIF data
 *
 * Updates timing information for loopback debugging when DEBUG is enabled.
 * Tracks transmission times and stores them in debug arrays for analysis.
 */
static void nrc_hif_update_loopback_debug_time(struct nrc_hif_device *hdev,
					       struct sk_buff *skb)
{
#if defined(DEBUG)
	struct hif_lb_hdr *hif = (struct hif_lb_hdr *)skb->data;

	if (hif->type == HIF_TYPE_LOOPBACK) {
		hdev->debug->tx_time_last = ktime_to_us(ktime_get());

		if (hif->index == 0) {
			hdev->debug->tx_time_first = hdev->debug->tx_time_last;
		}

		if (hif->subtype != LOOPBACK_MODE_RX_ONLY &&
		    hdev->debug->time_info_array) {
			(hdev->debug->time_info_array + hif->index)->_i =
				hif->index;
			(hdev->debug->time_info_array + hif->index)->_txt =
				hdev->debug->tx_time_last;
		}
	}
#endif
}

/**
 * enum nrc_xmit_result - Result codes for SKB transmission
 * @XMIT_OK: Successfully transmitted and freed
 * @XMIT_REQUEUE: Transmission failed, caller should requeue SKB
 * @XMIT_DROPPED: SKB was freed due to signal interrupt
 */
enum nrc_xmit_result {
	XMIT_OK,
	XMIT_REQUEUE,
	XMIT_DROPPED,
};

/**
 * nrc_hif_xmit_skb - Common SKB transmission through HIF layer
 * @hdev: HIF device
 * @skb: socket buffer to transmit
 *
 * Waits for xmit availability, transmits the SKB, and frees it on success.
 * This is the shared xmit path used by both WLAN and MCP work handlers.
 *
 * Return: XMIT_OK on success, XMIT_REQUEUE if caller should retry,
 *         XMIT_DROPPED if SKB was freed due to interrupt.
 */
static enum nrc_xmit_result nrc_hif_xmit_skb(struct nrc_hif_device *hdev,
					     struct sk_buff *skb)
{
	int ret;

	DBG_TX("xmit: len=%u %s/%s", skb->len,
	       nrc_hif_type_str(((struct hif *)skb->data)->type),
	       nrc_hif_subtype_str(((struct hif *)skb->data)->type,
				   ((struct hif *)skb->data)->subtype));

	ret = nrc_hif_ops_wait_for_xmit(skb);
	if (ret < 0) {
		if (ret == -1) {
			ERR_HIF("HIF: xmit wait timeout, requeue skb");
#ifdef CONFIG_SUPPORT_RECOVERY
			nrc_recovery_inc(hdev, NRC_RECOVERY_TX_ERR);
#endif
			return XMIT_REQUEUE;
		}
		ERR_HIF("HIF: xmit wait interrupted (%d), free skb", ret);
		nrc_hif_free_skb(hdev, skb);
		return XMIT_DROPPED;
	}

	/* Final sync check: Ensure device is still AWAKE before physical SPI write.
	 * In Non-TIM mode, the target might have entered sleep immediately after
	 * providing a slot if there was a delay in host packet processing. */
	if (!NRC_PS_IS_AWAKE(hdev)) {
		VBS_PS("HIF: Device slept before xmit, requeue (ps_state=%s)",
		       NRC_PS_STATE_STR(hdev));
		/* Deadlock prevention: If device is in SLEEP state, must request wake
		 * to ensures the requeued packet eventually gets transmitted. */
		if (NRC_PS_IS_ASLEEP(hdev))
			nrc_ps_request_wake(hdev, NRC_PS_REASON_HAL_TX_WAKEUP);
		return XMIT_REQUEUE;
	}

	nrc_hif_update_loopback_debug_time(hdev, skb);

	ret = nrc_hif_ops_xmit(skb);
	if (ret != HIF_TX_COMPLETE) {
		VBS_HIF("HIF: xmit failed (%d), requeue skb", ret);

		/* If xmit failed (likely due to SPI ACK error/timeout), ensure
		 * the device is awake before retrying. This is crucial for
		 * recovering from autonomous sleep races in Non-TIM mode. */
		if (!NRC_PS_IS_AWAKE(hdev)) {
			nrc_ps_request_wake(hdev, NRC_PS_REASON_HAL_TX_WAKEUP);
		}
		return XMIT_REQUEUE;
	}

	nrc_hif_free_skb(hdev, skb);
#ifdef CONFIG_SUPPORT_RECOVERY
	nrc_recovery_zero(hdev, NRC_RECOVERY_TX_ERR);
	/* TX success means FW is alive — reset WDT like RX path does */
	nrc_recovery_wdt_kick(hdev);
#endif
	return XMIT_OK;
}

/**
 * nrc_hif_check_wim_priority - Check if WIM queue needs priority over frames
 * @queue: the queue array to check
 *
 * When processing frame queue (queue[0]), yields to pending WIM commands
 * unless PS is in SLEEPING state (PS WIM must not be preempted).
 *
 * Return: true if frame processing should yield to WIM, false otherwise.
 */
static inline bool nrc_hif_check_wim_priority(struct nrc_hif_device *hdev,
					      struct sk_buff_head *queue)
{
	if (NRC_PS_IS_SLEEPING(hdev))
		return false;
	return !skb_queue_empty(&queue[1]);
}

/**
 * nrc_hif_reschedule_work - Reschedule work if queue has pending data
 * @pending: atomic pending flag
 * @has_data: whether the queue has data
 * @wq: workqueue to schedule on
 * @work: work struct to schedule
 */
static void nrc_hif_reschedule_work(atomic_t *pending, bool has_data,
				    struct workqueue_struct *wq,
				    struct work_struct *work)
{
	if (has_data) {
		if (atomic_cmpxchg(pending, 0, 1) == 0)
			queue_work(wq, work);
	}
}

/**
 * nrc_hif_dequeue_wlan_skb - Dequeue next SKB from WLAN queues with deauth priority
 * @hdev: HIF device
 * @qi: current queue index (1=WIM, 0=frame)
 * @skb_frame: pointer to cached frame SKB from deauth check (in/out)
 *
 * Implements WLAN-specific dequeue logic:
 * - WIM queue (qi=1): dequeues WIM, but checks frame queue for deauth frames
 *   that must be sent first [CB#12004]
 * - Frame queue (qi=0): yields to WIM priority, uses cached frame if available
 *
 * Return: next SKB to transmit, or NULL if queue is empty/should yield.
 */
static struct sk_buff *nrc_hif_dequeue_wlan_skb(struct nrc_hif_device *hdev,
						int qi,
						struct sk_buff **skb_frame)
{
	struct sk_buff *skb;

	if (qi > 0) {
		/* WIM queue: check for deauth frame that needs priority */
		skb = skb_dequeue(&hdev->queue[qi]);
		if (skb && !*skb_frame) {
			*skb_frame = skb_dequeue(&hdev->queue[0]);
			if (*skb_frame) {
				u8 *p = (u8 *)(*skb_frame)->data;
				struct ieee80211_hdr *mh;

				mh = (void *)(p + sizeof(struct hif) +
					      sizeof(struct frame_hdr));
				if (ieee80211_is_deauth(mh->frame_control)) {
					/* Deauth takes priority over WIM */
					skb_queue_head(&hdev->queue[qi], skb);
					skb = *skb_frame;
					*skb_frame = NULL;
				}
			}
		}
		return skb;
	}

	/* Frame queue */
	/* Yield to WIM if pending (unless PS sleeping) */
	if (nrc_hif_check_wim_priority(hdev, hdev->queue)) {
		if (*skb_frame) {
			skb_queue_head(&hdev->queue[0], *skb_frame);
			*skb_frame = NULL;
		}
		return NULL;
	}

	if (*skb_frame) {
		skb = *skb_frame;
		*skb_frame = NULL;
	} else {
		skb = skb_dequeue(&hdev->queue[0]);
	}
	return skb;
}

/**
 * nrc_hif_wlan_work - WLAN HIF transmission work handler
 * @work: work structure
 *
 * Handles transmission of WLAN frames and WIM commands through the HIF layer.
 * Processes queues in priority order (WIM first) with deauth frame prioritization.
 */
void nrc_hif_wlan_work(struct work_struct *work)
{
	struct nrc_hif_device *hdev;
	struct sk_buff *skb, *skb_frame = NULL;
	enum nrc_xmit_result xret;
	int i;

	hdev = container_of(work, struct nrc_hif_device, work);

	/* Defer to MCP if it has priority */
	if (NRC_PARAM_MCP_PRIORITY(hdev) && atomic_read(&hdev->mcp_active)) {
		DBG_HIF("WLAN TX suspended: MCP is active (queue0=%d, queue1=%d)",
			NRC_FRAME_QUEUE_LEN(hdev), NRC_WIM_QUEUE_LEN(hdev));
		atomic_set(&hdev->queue_pending, 0);
		usleep_range(100, 200);
		nrc_hif_reschedule_work(&hdev->queue_pending, true,
					hdev->workqueue, &hdev->work);
		return;
	}

	if (NRC_WIM_QUEUE_LEN(hdev) != 0) {
		DBG_TX("WLAN TX: queue0=%d, queue1=%d",
		       NRC_FRAME_QUEUE_LEN(hdev), NRC_WIM_QUEUE_LEN(hdev));
	}

	/* Check for SLEEPING state timeout */
	if (nrc_ps_check_sleeping_timeout(&hdev->ps, 1000)) {
		WARN_PS("SLEEPING state timeout, forcing WAKE (queue0=%d, queue1=%d)",
			NRC_FRAME_QUEUE_LEN(hdev), NRC_WIM_QUEUE_LEN(hdev));
		nrc_ps_request_wake(hdev, NRC_PS_REASON_HAL_TX_TIMEOUT);
	}

	for (i = ARRAY_SIZE(hdev->queue) - 1; i >= 0; i--) {
		for (;;) {
			skb = nrc_hif_dequeue_wlan_skb(hdev, i, &skb_frame);
			if (!skb)
				break;

			if (NRC_PS_IS_ASLEEP(hdev) ||
			    NRC_PS_IS_SLEEPING(hdev)) {
				VBS(CAT(TX) | CAT(PS),
				    "PS(%s): requeue (q%d, frm=%d wim=%d)",
				    NRC_PS_STATE_STR(hdev), i,
				    NRC_FRAME_QUEUE_LEN(hdev),
				    NRC_WIM_QUEUE_LEN(hdev));
				skb_queue_head(&hdev->queue[i], skb);
				if (skb_frame)
					skb_queue_head(&hdev->queue[0],
						       skb_frame);
				/*
				 * SLEEPING: flush_wq is waiting for us to
				 * finish - just return, frames stay queued.
				 * SLEEP: request wake, frames sent after wake.
				 */
				if (NRC_PS_IS_ASLEEP(hdev))
					nrc_ps_request_wake(
						hdev,
						NRC_PS_REASON_HAL_TX_WAKEUP);
				return;
			}

			xret = nrc_hif_xmit_skb(hdev, skb);
			if (xret == XMIT_REQUEUE) {
				skb_queue_head(&hdev->queue[i], skb);
				break;
			}
			if (xret == XMIT_DROPPED)
				break;
		}
	}

	if (skb_frame) {
		WARN_TX("Orphaned skb_frame returned to queue[0]");
		skb_queue_head(&hdev->queue[0], skb_frame);
		skb_frame = NULL;
	}

	atomic_set(&hdev->queue_pending, 0);

	/* Prevent work rescheduling during PS state transitions */
	if (!NRC_PS_IS_WAKING(hdev) && !NRC_PS_IS_SLEEPING(hdev)) {
		nrc_hif_reschedule_work(&hdev->queue_pending,
					NRC_QUEUE_HAS_DATA(hdev),
					hdev->workqueue, &hdev->work);
	}
}

/**
 * nrc_hif_mcp_work - MCP HIF transmission work handler
 * @work: work structure
 *
 * Handles transmission of MCP frames and commands through the HIF layer.
 * Simpler than WLAN: no deauth priority, no PS timeout checks.
 */
void nrc_hif_mcp_work(struct work_struct *work)
{
	struct nrc_hif_device *hdev;
	struct sk_buff *skb;
	enum nrc_xmit_result xret;
	int i;

	hdev = container_of(work, struct nrc_hif_device, mcp_work);

	if (NRC_PARAM_MCP_PRIORITY(hdev))
		atomic_set(&hdev->mcp_active, 1);

	DBG_TX("MCP TX: queue0=%d, queue1=%d", NRC_MCP_FRAME_QUEUE_LEN(hdev),
	       NRC_MCP_WIM_QUEUE_LEN(hdev));

	for (i = ARRAY_SIZE(hdev->mcp_queue) - 1; i >= 0; i--) {
		for (;;) {
			/* Frame queue yields to WIM priority */
			if (i == 0 &&
			    nrc_hif_check_wim_priority(hdev, hdev->mcp_queue))
				break;

			skb = skb_dequeue(&hdev->mcp_queue[i]);
			if (!skb)
				break;

			if (NRC_PS_IS_ASLEEP(hdev) ||
			    NRC_PS_IS_SLEEPING(hdev)) {
				VBS(CAT(TX) | CAT(PS),
				    "MCP: PS(%s), requeue (queue[%d], queue0=%d, queue1=%d)",
				    NRC_PS_STATE_STR(hdev), i,
				    NRC_MCP_FRAME_QUEUE_LEN(hdev),
				    NRC_MCP_WIM_QUEUE_LEN(hdev));
				skb_queue_head(&hdev->mcp_queue[i], skb);
				if (NRC_PS_IS_ASLEEP(hdev))
					nrc_ps_request_wake(
						hdev,
						NRC_PS_REASON_HAL_TX_WAKEUP);
				return;
			}

			xret = nrc_hif_xmit_skb(hdev, skb);
			if (xret == XMIT_REQUEUE) {
				skb_queue_head(&hdev->mcp_queue[i], skb);
				break;
			}
			if (xret == XMIT_DROPPED)
				break;
		}
	}

	atomic_set(&hdev->mcp_queue_pending, 0);

	if (NRC_MCP_QUEUE_HAS_DATA(hdev)) {
		nrc_hif_reschedule_work(&hdev->mcp_queue_pending, true,
					hdev->mcp_workqueue, &hdev->mcp_work);
	} else if (NRC_PARAM_MCP_PRIORITY(hdev)) {
		/* Clear MCP active flag and resume suspended WLAN TX */
		atomic_set(&hdev->mcp_active, 0);
		nrc_hif_reschedule_work(&hdev->queue_pending,
					NRC_QUEUE_HAS_DATA(hdev),
					hdev->workqueue, &hdev->work);
	}
}

void nrc_tx_flush_wq(struct nrc_hif_device *hdev)
{
	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return;
	}

	/* Flush WLAN workqueue */
	if (hdev->workqueue != NULL)
		flush_work(&hdev->work);

	/* Flush MCP workqueue */
	if (hdev->mcp_workqueue != NULL)
		flush_work(&hdev->mcp_work);
}

/**
 * nrc_tx_cleanup_queues - Cleanup all SKBs in TX queues
 *
 * Cleans up all pending SKBs in both WLAN and MCP queues.
 * This is typically called during module shutdown or FW recovery.
 */
void nrc_tx_cleanup_queues(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;
	int i;

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return;
	}

	DBG_HIF("Cleaning up TX queues");

	/* Cleanup WLAN queues */
	for (i = ARRAY_SIZE(hdev->queue) - 1; i >= 0; i--) {
		for (;;) {
			skb = skb_dequeue(&hdev->queue[i]);
			if (!skb)
				break;
			/* Track SKB free during cleanup */
			nrc_hif_free_skb(hdev, skb);
		}
	}

	/* Cleanup MCP queues */
	for (i = ARRAY_SIZE(hdev->mcp_queue) - 1; i >= 0; i--) {
		for (;;) {
			skb = skb_dequeue(&hdev->mcp_queue[i]);
			if (!skb)
				break;
			/* Track SKB free during cleanup */
			nrc_hif_free_skb(hdev, skb);
		}
	}
}
