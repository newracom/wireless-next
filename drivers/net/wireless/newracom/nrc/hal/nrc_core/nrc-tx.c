/*
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

/*
 * LEGACY CODE - Kept for historical reference only
 * This function searched for obsolete WIM_CMD_SLEEP format with embedded duration.
 * Current PS architecture uses nrc_wim_set_ps() which sends WIM_CMD_SET + WIM_TLV_PS_ENABLE.
 * This format is never generated in current codebase. - 2025-12-02
 */
#if 0
static bool nrc_hif_sleep_find(struct sk_buff *skb, u32 *duration_ms)
{
	struct hif *hif = (void *)skb->data;
	struct wim *wim = (void *)(hif + 1);
	struct wim_sleep_duration *sleep_duration = (void *)(wim + 1);

	if ((hif->type != HIF_TYPE_WIM) ||
	    (hif->subtype != HIF_WIM_SUB_REQUEST)) {
		return false;
	}

	if (wim->cmd == WIM_CMD_SLEEP &&
	    sleep_duration->h.type == WIM_TLV_SLEEP_DURATION) {
		*duration_ms = sleep_duration->v.sleep_ms;
		return true;
	}

	return false;
}
#endif

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

/*
 * LEGACY CODE - Kept for historical reference only
 * This function handled PS via embedded WIM sleep commands with manual GPIO toggling.
 * REPLACED by state machine-based PS: nrc_ps_handle_event() manages all sleep/wake.
 * Problems with this approach:
 * - Bypassed PS state machine
 * - Blocked TX work queue with msleep() (violates async design)
 * - Manually toggled GPIO without state tracking
 * Current architecture: TX checks NRC_PS_IS_ASLEEP() and calls nrc_ps_request_wake()
 * - 2025-12-02
 */
#if 0
static int nrc_hif_ctrl_ps(struct nrc_hif_device *hdev, struct sk_buff *skb)
{
	int ret;
	u32 sleep_ms = 0;
	const u32 wakeup_ms = 10;

	if (!nrc_hif_sleep_find(skb, &sleep_ms))
		return HIF_TX_PASSOVER;

	DBG_PS("Checking for PS control in TX skb");

	ret = nrc_hif_ops_xmit(skb);
#if defined(SPI_DBG)
	nrc_hif_ops_gpio_set(SPI_DBG, 0);
#endif
	msleep(sleep_ms + wakeup_ms);

	nrc_hif_ops_update();

#if defined(SPI_DBG)
	nrc_hif_ops_gpio_set(SPI_DBG, 1);
#endif
	return HIF_TX_COMPLETE;
}
#endif

#define MAX_PS_DELAY_PKT_CNT 10

/**
 * nrc_hif_wlan_work - HIF transmission work handler
 * @work: work structure
 *
 * This function handles the transmission of frames and WIM commands
 * through the HIF layer. It processes queues in priority order and
 * handles special cases like deauth frame prioritization.
 */
void nrc_hif_wlan_work(struct work_struct *work)
{
	struct nrc_hif_device *hdev;
	struct sk_buff *skb, *skb_frame = NULL;
	int i, ret = 0;

	hdev = container_of(work, struct nrc_hif_device, work);

	/* Check if MCP priority is enabled and MCP is actively transmitting */
	if (NRC_PARAM_MCP_PRIORITY(hdev) && atomic_read(&hdev->mcp_active)) {
		DBG_HIF("WLAN TX suspended: MCP is active (queue0=%d, queue1=%d)",
			NRC_FRAME_QUEUE_LEN(hdev), NRC_WIM_QUEUE_LEN(hdev));
		/* Reschedule WLAN work after a short delay */
		atomic_set(&hdev->queue_pending, 0);
		usleep_range(100, 200);
		if (atomic_cmpxchg(&hdev->queue_pending, 0, 1) == 0) {
			queue_work(hdev->workqueue, &hdev->work);
		}
		return;
	}

	if (NRC_WIM_QUEUE_LEN(hdev) != 0) {
		DBG_TX("WLAN TX: queue0=%d, queue1=%d",
		       NRC_FRAME_QUEUE_LEN(hdev), NRC_WIM_QUEUE_LEN(hdev));
	}

	/* Check for SLEEPING state timeout to prevent infinite loops */
	if (nrc_ps_check_sleeping_timeout(&hdev->ps, 1000)) {
		dev_warn(
			hdev->dev,
			"SLEEPING state timeout detected, forcing WAKE (queue0=%d, queue1=%d)\n",
			NRC_FRAME_QUEUE_LEN(hdev), NRC_WIM_QUEUE_LEN(hdev));
		/* Request wake directly from HAL (async, no wait) */
		nrc_ps_request_wake(hdev, NRC_PS_REASON_HAL_TX_TIMEOUT);
	}

	if (NRC_PARAM_POWER_SAVE(hdev) &&
	    (NRC_FRAME_QUEUE_LEN(hdev) > MAX_PS_DELAY_PKT_CNT ||
	     NRC_MCP_FRAME_QUEUE_LEN(hdev) > MAX_PS_DELAY_PKT_CNT)) {
		DBG_HIF("Need to delay PS for tx frames (WLAN:%d, MCP:%d)",
			NRC_FRAME_QUEUE_LEN(hdev),
			NRC_MCP_FRAME_QUEUE_LEN(hdev));
		/* max credit is 80, 1 sec is enough,but
		 * aligned the usage consistently with other parts
		 */
		{
			u32 custom_timeout =
				2000; /* 2 seconds in milliseconds */
			struct nrc_hal_event_data event = {
				.type = NRC_HAL_EVT_PS_DYN_START_CUSTOM_TIMEOUT,
				.data = &custom_timeout,
				.data_len = sizeof(custom_timeout),
			};
			nrc_hal_trigger_event(&event);
		}
	}

	for (i = ARRAY_SIZE(hdev->queue) - 1; i >= 0; i--) {
		for (;;) {
			/*
			 * [CB#12004][NRC7292 Host Mode] Deauth Fail on WPA3-SAE and OWE
			 * the deauth frame should be transferred first even though
			 * the current for-loop is for the queue of WIM.
			 */
			if (i) { // WIM
				skb = skb_dequeue(&hdev->queue[i]);
				if (skb && !skb_frame) {
					skb_frame =
						skb_dequeue(&hdev->queue[0]);
					if (skb_frame) {
						u8 *p;
						struct ieee80211_hdr *mh;
						p = (u8 *)skb_frame->data;
						mh = (void *)(p +
							      sizeof(struct hif) +
							      sizeof(struct frame_hdr));
						if (ieee80211_is_deauth(
							    mh->frame_control)) {
							skb_queue_head(
								&hdev->queue[i],
								skb);
							skb = skb_frame;
							skb_frame = NULL;
						}
					}
				}
			} else { // Frame
#ifndef CONFIG_USE_TXQ
				if (NRC_DRV_IS_ASLEEP(hdev))
					break;
#endif
				/*
				 * UDP packets can reach here continuously
				 * without checking credit and then it makes infinite loop.
				 * Here is the workaround to give priority for WIM command.
				 */
				if (!NRC_PS_IS_SLEEPING(hdev)) {
					/* ps wim is arrived here.
					 * if scheduled, no chance to tx frames. */

					if (!skb_queue_empty(&hdev->queue[1])) {
						if (skb_frame) {
							skb_queue_head(
								&hdev->queue[i],
								skb_frame);
						}
						break;
					}
				}

				if (skb_frame) {
					skb = skb_frame;
					skb_frame = NULL;
				} else
					skb = skb_dequeue(&hdev->queue[i]);
			}
			if (!skb)
				break;

			if (NRC_PS_IS_ASLEEP(hdev)) {
				/* TX while in SLEEP state - target is asleep
				 * Request wake and reschedule after wake complete
				 */
				DBG_TX("TX on SLEEP: request wake (ps_state=%s, queue[%d], queue0=%d, queue1=%d)",
				       NRC_PS_STATE_STR(hdev), i,
				       NRC_FRAME_QUEUE_LEN(hdev),
				       NRC_WIM_QUEUE_LEN(hdev));
				skb_queue_head(&hdev->queue[i], skb);
				/* Request wake directly from HAL (async, no wait) */
				nrc_ps_request_wake(
					hdev, NRC_PS_REASON_HAL_TX_WAKEUP);
				return;
			}
			{
				struct hif *hif_hdr = (struct hif *)skb->data;
				DBG_TX("HIF: TX skb len=%u type=%d(%s) subtype=%d(%s)",
				       skb->len, hif_hdr->type,
				       nrc_hif_type_str(hif_hdr->type),
				       hif_hdr->subtype,
				       nrc_hif_subtype_str(hif_hdr->type,
							   hif_hdr->subtype));
			}

			/* Wait for xmit availability and transmit */
			ret = nrc_hif_ops_wait_for_xmit(skb);
			if (ret < 0) {
				if (ret == -1) {
					/* Timeout: requeue for retry later */
					ERR_HIF("HIF: xmit wait timeout, requeue skb");
					skb_queue_head(&hdev->queue[i], skb);
				} else {
					/* Signal interrupt (e.g., -ERESTARTSYS): free skb */
					ERR_HIF("HIF: xmit wait interrupted (%d), free skb",
						ret);
					nrc_hif_free_skb(hdev, skb);
				}
				break;
			}

			nrc_hif_update_loopback_debug_time(hdev, skb);
			ret = nrc_hif_ops_xmit(skb);

			if (ret != HIF_TX_COMPLETE) {
				/* TX failed (HIF_TX_FAILED etc): requeue for retry */
				DBG_HIF("HIF: xmit failed (%d), requeue skb",
					ret);
				skb_queue_head(&hdev->queue[i], skb);
				break;
			}

			/*
			 * Free SKB after transmission
			 * Note: nrc_hif_ops_xmit() returns 0 (HIF_TX_COMPLETE) on success.
			 */
			nrc_hif_free_skb(hdev, skb);
		}
	}

	atomic_set(&hdev->queue_pending, 0);

	/* Prevent work rescheduling during PS state transitions */
	if (!NRC_PS_IS_WAKING(hdev) && !NRC_PS_IS_SLEEPING(hdev)) {
		if (NRC_QUEUE_HAS_DATA(hdev)) {
			if (atomic_cmpxchg(&hdev->queue_pending, 0, 1) == 0) {
				queue_work(hdev->workqueue, &hdev->work);
			}
		}
	}
}

/**
 * nrc_hif_mcp_work - MCP frontend HIF transmission work handler
 * @work: work structure
 *
 * Simplified transmission handler for MCP frontend.
 * Processes MCP queues without WLAN-specific frame handling.
 * Includes power save wakeup handling when device is asleep.
 */
void nrc_hif_mcp_work(struct work_struct *work)
{
	struct nrc_hif_device *hdev;
	struct sk_buff *skb;
	int i, ret = 0;

	hdev = container_of(work, struct nrc_hif_device, mcp_work);

	/* Set MCP active flag if priority control is enabled */
	if (NRC_PARAM_MCP_PRIORITY(hdev)) {
		atomic_set(&hdev->mcp_active, 1);
	}

	DBG_TX("MCP TX: queue0=%d, queue1=%d",
	       NRC_MCP_FRAME_QUEUE_LEN(hdev), NRC_MCP_WIM_QUEUE_LEN(hdev));

	/* Process MCP queues: WIM first (queue[1]), then frame (queue[0]) */
	for (i = ARRAY_SIZE(hdev->mcp_queue) - 1; i >= 0; i--) {
		for (;;) {
			/* Give priority to WIM */
			if (!NRC_PS_IS_SLEEPING(hdev)) {
				if (i == 0 &&
				    !skb_queue_empty(&hdev->mcp_queue[1])) {
					break;
				}
			}

			skb = skb_dequeue(&hdev->mcp_queue[i]);
			if (!skb)
				break;

			/* Check if device is asleep and needs wakeup */
			if (NRC_PS_IS_ASLEEP(hdev)) {
				/* MCP TX while in SLEEP state - target is asleep
				 * Request wake and reschedule after wake complete
				 */
				DBG_HIF("MCP: TX on SLEEP, request wake (ps_state=%s, queue[%d], queue0=%d, queue1=%d)",
					NRC_PS_STATE_STR(hdev), i,
					NRC_MCP_FRAME_QUEUE_LEN(hdev),
					NRC_MCP_WIM_QUEUE_LEN(hdev));
				/* Put skb back to queue head */
				skb_queue_head(&hdev->mcp_queue[i], skb);
				/* Request wake directly from HAL (async, no wait) */
				nrc_ps_request_wake(
					hdev, NRC_PS_REASON_HAL_TX_WAKEUP);
				return;
			}

			/* MCP always uses xmit operation */
			ret = nrc_hif_ops_wait_for_xmit(skb);
			if (ret < 0) {
				if (ret == -1) {
					/* Timeout: requeue for retry later */
					ERR_HIF("MCP HIF: xmit wait timeout, requeue skb");
					skb_queue_head(&hdev->mcp_queue[i],
						       skb);
				} else {
					/* Signal interrupt (e.g., -ERESTARTSYS): free skb */
					ERR_HIF("MCP HIF: xmit wait interrupted (%d), free skb",
						ret);
					nrc_hif_free_skb(hdev, skb);
				}
				break;
			}

			ret = nrc_hif_ops_xmit(skb);
			if (ret != HIF_TX_COMPLETE) {
				/* TX failed (HIF_TX_FAILED etc): requeue for retry */
				DBG_HIF("MCP HIF: xmit failed (%d), requeue skb",
					ret);
				skb_queue_head(&hdev->mcp_queue[i], skb);
				break;
			}

			/*
			 * Free SKB after transmission
			 * Note: nrc_hif_ops_xmit() returns 0 (HIF_TX_COMPLETE) on success.
			 */
			nrc_hif_free_skb(hdev, skb);
		}
	}

	atomic_set(&hdev->mcp_queue_pending, 0);

	/* Reschedule if more packets in queue */
	if (NRC_MCP_QUEUE_HAS_DATA(hdev)) {
		if (atomic_cmpxchg(&hdev->mcp_queue_pending, 0, 1) == 0) {
			queue_work(hdev->mcp_workqueue, &hdev->mcp_work);
		}
	} else {
		/* Clear MCP active flag when all transmissions are done */
		if (NRC_PARAM_MCP_PRIORITY(hdev)) {
			atomic_set(&hdev->mcp_active, 0);
			/* Resume WLAN TX if it was suspended */
			if (NRC_QUEUE_HAS_DATA(hdev)) {
				if (atomic_cmpxchg(&hdev->queue_pending, 0,
						   1) == 0) {
					queue_work(hdev->workqueue,
						   &hdev->work);
				}
			}
		}
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
