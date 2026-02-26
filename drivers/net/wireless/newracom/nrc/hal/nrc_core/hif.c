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
#include <linux/gpio.h>
#include <linux/ip.h>
#include <linux/tcp.h>

/* Linux networking headers */
#include <net/mac80211.h>

/* Common directory headers - Core */
#include "nrc-build-config.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* HAL module trace system */
#if defined(CONFIG_NRC_TRACING)
#include "nrc-trace.h"
#endif

/* Common directory headers - Interfaces */
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-backend-hif-interface.h"

// #if defined(CONFIG_NRC_HIF_SSP)
// #include "nrc-hif-ssp.h"
// #elif defined(CONFIG_NRC_HIF_UART)
// #include "nrc-hif-uart.h"
// #elif defined(CONFIG_NRC_HIF_DEBUG)
// #include "nrc-hif-debug.h"
// #elif defined(CONFIG_NRC_HIF_CSPI)
// /* SPI HIF functions are accessed through nrc-hif.h interface */
// #elif defined(CONFIG_NRC_HIF_SDIO)
// #include "nrc-hif-sdio.h"
// #endif

/* Local module headers */
#include "hif.h"
#include "wim.h"
#include "nrc-dump.h"
#include "nrc-vendor.h"
#include "nrc-tx.h"
#include "nrc-init.h"
#include "nrc-debug.h"
#include "nrc-ps.h"
#if defined(DEBUG)
#include "nrc-debug-common.h"
#endif

static void restart_worker(struct work_struct *work)
{
	// struct nrc_hif_device *hdev =
	// 	container_of(work, struct nrc_hif_device, restart_work);

#if defined(CONFIG_SUPPORT_BD)
	struct regulatory_request request;
	request.initiator = NL80211_REGDOM_SET_BY_DRIVER;
#endif

	INFO("Restart NRC");

	nrc_nw_stop(true); /* restart=true: ignore frontend check */
	nrc_hif_ops_probe();
#if defined(CONFIG_SUPPORT_BD)
	/* Trigger regulatory notifier via HAL callback system */
	{
		struct nrc_hal_event_data hal_event = {
			.frontend_type = NRC_FRONTEND_WLAN,
			.type = NRC_HAL_EVT_REG_NOTIFIER,
			.data = &request,
			.data_len = sizeof(request)};
		nrc_hal_trigger_event(&hal_event);
	}
#endif
	nrc_nw_start(true);
}

struct nrc_hif_device *nrc_hif_alloc(struct device *dev, void *priv,
				     struct nrc_hif_ops *ops)
{
	struct nrc_hif_device *hdev;
	int i;

	hdev = kzalloc(sizeof(*hdev), GFP_KERNEL);
	if (!hdev) {
		return NULL;
	}

	/* Initialize WLAN frontend wim and frame queue */
	for (i = 0; i < ARRAY_SIZE(hdev->queue); i++) {
		skb_queue_head_init(&hdev->queue[i]);
	}
	atomic_set(&hdev->queue_pending, 0);

	/* Initialize MCP frontend wim and frame queue */
	for (i = 0; i < ARRAY_SIZE(hdev->mcp_queue); i++) {
		skb_queue_head_init(&hdev->mcp_queue[i]);
	}
	atomic_set(&hdev->mcp_queue_pending, 0);
	atomic_set(&hdev->mcp_active, 0);

	atomic_set(&hdev->drv_state, NRC_DRV_INIT);
	atomic_set(&hdev->frontend_count, 0);

	/* Initialize WLAN frontend work */
	INIT_WORK(&hdev->work, nrc_hif_wlan_work);
	/* Initialize MCP frontend work */
	INIT_WORK(&hdev->mcp_work, nrc_hif_mcp_work);
	/* Initialize common work structures */
	INIT_WORK(&hdev->restart_work, restart_worker);

	init_completion(&hdev->wake_done);
#ifdef TEST_BLOCK_TX
	init_completion(&hdev->sleep_done);
#endif

	hdev->hif_ops = ops;
	hdev->priv = priv;
	hdev->dev = dev;

	/* Allocate shared parameters structure */
	hdev->params = nrc_params_alloc();
	if (!hdev->params) {
		ERR_HIF("Failed to allocate parameters structure");
		kfree(hdev);
		return NULL;
	}

	/* Initialize WIM response system */
	if (nrc_wim_response_init(hdev) < 0) {
		ERR_HIF("Failed to initialize WIM response system");
		nrc_params_free(hdev->params);
		kfree(hdev);
		return NULL;
	}

	/* Allocate shared debug structure */
	hdev->debug = nrc_debug_alloc();
	if (!hdev->debug) {
		ERR_HIF("Failed to allocate debug structure");
		nrc_params_free(hdev->params);
		kfree(hdev);
		return NULL;
	}

	/* Initialize power save structure */
	nrc_ps_lock_init(&hdev->ps);
	hdev->ps.mode = NRC_PS_NONE;
	hdev->ps.state = NRC_PS_STATE_WAKE;
	hdev->ps.enabled = false;
	hdev->ps.modem_enabled = false;
	hdev->ps.timeout = 0;
	hdev->ps.last_sleep_timeout_ms = 0;

	return hdev;
}

void nrc_hif_free(struct nrc_hif_device *hdev)
{
	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return;
	}

	DBG_HIF("free()");

	nrc_hal_stop(hdev);

	/* Cleanup WIM response system */
	nrc_wim_response_deinit(hdev);

	/* Free shared parameters structure */
	if (hdev->params) {
		nrc_params_free(hdev->params);
		hdev->params = NULL;
	}

	/* Free shared debug structure */
	if (hdev->debug) {
		nrc_debug_free(hdev->debug);
		hdev->debug = NULL;
	}

	kfree(hdev);
}

/**
 * nrc_hif_reset_slot_credit - Reset HIF slot and credit counters
 *
 * Initializes slot head/tail pointers and credit queues to zero.
 * Called during FW download, PS wake, and WDT recovery.
 */
void nrc_hif_reset_slot_credit(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	int ac;

	if (!hdev) {
		ERR_HIF("No HIF device for reset_slot_credit");
		return;
	}

	DBG_HIF("[%s] ps_state=%s", __func__, NRC_PS_STATE_STR(hdev));

	/* Reset slot head/tail pointers */
	hdev->slot[RX_SLOT].tail = hdev->slot[RX_SLOT].head = 0;
	hdev->slot[TX_SLOT].tail = hdev->slot[TX_SLOT].head = 0;

	/* Reset credit queues */
	{
		unsigned long flags;
		CREDIT_LOCK(hdev, flags);
		for (ac = 0; ac < CREDIT_QUEUE_MAX; ac++) {
			hdev->credit.front[ac] = 0;
			hdev->credit.rear[ac] = 0;
		}
		CREDIT_UNLOCK(hdev, flags);
	}
}

void nrc_hif_free_skb(struct nrc_hif_device *hdev, struct sk_buff *skb)
{
	struct hif *hif = NULL;
	struct frame_hdr *fh = NULL;
	int credit;
	struct nrc_hal_event_data event = {
		.type = NRC_HAL_EVT_FREE_SKB, .data = NULL, .data_len = 0};

	if (WARN_ON(!skb)) {
		ERR_HIF("skb is NULL!");
		return;
	}
	hif = (void *)skb->data;
	fh = (struct frame_hdr *)(hif + 1);

	/* Validate HIF type */
	if (hif->type >= HIF_TYPE_MAX) {
		ERR_HIF("Invalid HIF type %d (>= HIF_TYPE_MAX=%d), freeing SKB anyway",
			hif->type, HIF_TYPE_MAX);
		/* Use HIF_TYPE_WIM as fallback for invalid types */
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false, false);
		return;
	}

	if (hif->type == HIF_TYPE_FRAME) {
		u8 ac;

		/* Check if this is an MCP frame (no frame_hdr structure) */
		if (hif->subtype == HIF_FRAME_SUB_MCP_PROTOCOL ||
		    hif->subtype == HIF_FRAME_SUB_MCP_DATA) {
			/* MCP frames: HIF header only, no frame_hdr, no credit accounting */
			DBG_HIF("MCP frame free: subtype=%d, len=%u",
				hif->subtype, skb->len);
			NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_FRAME, false,
					   false);
			return;
		}

		/* WLAN frames: HIF + frame_hdr, requires credit accounting */
		ac = fh->flags.tx.ac;

		/* Validate AC index to prevent array overflow */
		if (ac >= (IEEE80211_NUM_ACS * 3)) {
			ERR_HIF("Invalid AC %d (>= %d) in FRAME, possible corruption",
				ac, IEEE80211_NUM_ACS * 3);
			/* Free SKB without credit accounting */
			NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_FRAME, false,
					   false);
			return;
		}

		credit = DIV_ROUND_UP(skb->len, hdev->fw.info.buffer_size);
		DBG_TX("%s ac:%d, pend:%d, credit:%d", __func__, ac,
		       atomic_read(&hdev->credit.tx_pend[ac]), credit);
		atomic_sub(credit, &hdev->credit.tx_pend[ac]);

		/* Track FRAME SKB free after SPI TX complete (indirect alloc from mac80211) */
		/* Note: mac80211 will free this SKB, so count only without freeing */
		NRC_SKB_TRACK_FREE(hdev, NULL, HIF_TYPE_FRAME, false, true);

		event.data = skb;
		event.data_len = skb->len;
		nrc_hal_trigger_event(&event);

		return;
	}

	/* Other types not created by mac80211 (WIM, LOG, etc.) */
	/* Track all HIF types uniformly */
	NRC_SKB_TRACK_FREE(hdev, skb, hif->type, false, false);
}

#if defined(CONFIG_TXQ_ORDER_CHANGE_NRC_DRV)
/*******************************************************************************
* FunctionName : is_mgmt
* Description : Check if the skb is a management frame
* Parameters : skb(socket buffer)
* Returns : T/F (bool) T:management frame, F:not management frame
*******************************************************************************/
static bool is_mgmt(struct sk_buff *skb)
{
	struct hif *hif;
	struct ieee80211_hdr *mhdr;

	u8 *p;
	p = (u8 *)skb->data;
	hif = (void *)p;

	if (hif->type != HIF_TYPE_FRAME)
		return false;

	/* MCP frames don't have frame_hdr, so they can't be mgmt frames */
	if (hif->subtype == HIF_FRAME_SUB_MCP_PROTOCOL ||
	    hif->subtype == HIF_FRAME_SUB_MCP_DATA)
		return false;

	mhdr = (void *)(p + sizeof(struct hif) + sizeof(struct frame_hdr));

	if (ieee80211_is_mgmt(mhdr->frame_control))
		return true;

	return false;
}

/*******************************************************************************
* FunctionName : is_urgent_frame
* Description : Check if the frame is urgent
* Parameters : skb(socket buffer)
* Returns : T/F (bool)
*******************************************************************************/
static bool is_urgent_frame(struct sk_buff *skb)
{
	bool ret = false;
	if (is_mgmt(skb))
		ret = true;
	/*
	 * add other conditions for checking urgent frame.
	 * else if (is_tcp_ack(skb)) {...}
	 */
	return ret;
}
#if 0
#error "If you enable this, consider new feature of 7393 that supports vif1"
/*******************************************************************************
* FunctionName : skb_change_ac
* Description : force change the access category of the skb
* Parameters : hdev, skb, ac(aceess category want to change)
						ac:0 is for BK. ac:1 is BE, ac:2 is VI, ac:3 is VO)
* Returns : -1 Not change aceess category
			 0 access category change done
*******************************************************************************/
int skb_change_ac(struct nrc_hif_device *hdev, struct sk_buff *skb, uint8_t ac)
{
	struct hif *hif;
	struct frame_hdr *fh;
	int credit;

	u8 *p;
	p = (u8*)skb->data;
	hif = (void*)p;
	if (hif->type != HIF_TYPE_FRAME) {
		return -1;
	}

	fh = (void*)(p+sizeof(struct hif));

	if (ac>3)
		return -1;

	credit = DIV_ROUND_UP(skb->len, hdev->fw.fwinfo.buffer_size);

	atomic_sub(credit, &hdev->credit.tx_pend[fh->flags.tx.ac]);
	fh->flags.tx.ac = (hif->vifindex == 0 ? ac : ac+6);
	atomic_add(credit, &hdev->credit.tx_pend[fh->flags.tx.ac]);

	return 0;
}
#endif
#endif /* defined(CONFIG_TXQ_ORDER_CHANGE_NRC_DRV) */

int hif_enqueue_skb(struct nrc_hif_device *hdev, struct sk_buff *skb)
{
	struct hif *hif = (void *)skb->data;

	if ((hif == NULL) || (hdev == NULL)) {
		ERR_HIF("enqueue_skb failed: null pointer (hif=%p, hdev=%p)",
			hif, hdev);
		return -EINVAL;
	}

	/* Validate HIF header before enqueuing */
	VALIDATE_HIF_HEADER(skb, "hif_enqueue_skb");

	if (hif->type != HIF_TYPE_FRAME && hif->type != HIF_TYPE_WIM &&
	    hif->type != HIF_TYPE_LOOPBACK) {
		ERR_HIF("enqueue_skb failed: invalid HIF type (%d)", hif->type);
		WARN_ON(true);
		return -EINVAL;
	}

	/* Check workqueue before enqueuing to prevent orphaned SKBs */
	if (hdev->workqueue == NULL) {
		ERR_HIF("enqueue_skb failed: workqueue is null");
		return -EINVAL;
	}

	/* Map HIF type to queue index:
	 * queue[0] = FRAME packets
	 * queue[1] = WIM commands (and LOOPBACK)
	 */
#if defined(CONFIG_TXQ_ORDER_CHANGE_NRC_DRV)
	if (NRC_DRV_IS_RUNNING(hdev) && is_urgent_frame(skb)) {
		/*
		 * Case 1: enqueue to the head (urgent frames)
		 */
		if (hif->type == HIF_TYPE_FRAME) {
			skb_queue_head(&hdev->queue[0], skb);
		} else {
			/* WIM or LOOPBACK */
			skb_queue_head(&hdev->queue[1], skb);
		}

		/*
		 * Case 2: change AC
		 */
		// skb_change_ac(hdev, skb, 3);

		/*
		 * Case 3: change AC and enqueue to the tail
		 */
	} else {
		if (hif->type == HIF_TYPE_FRAME) {
			skb_queue_tail(&hdev->queue[0], skb);
		} else {
			/* WIM or LOOPBACK -> queue[1] */
			skb_queue_tail(&hdev->queue[1], skb);
		}
	}
#else
	/*
	 * HIF_TYPE_WIM and HIF_TYPE_LOOPBACK are using the same queue.
	 * Explicitly map: FRAME->queue[0], WIM/LOOPBACK->queue[1]
	 */
	if (hif->type == HIF_TYPE_FRAME) {
		skb_queue_tail(&hdev->queue[0], skb);
	} else {
		/* WIM or LOOPBACK -> queue[1] */
		skb_queue_tail(&hdev->queue[1], skb);
	}
#endif /* defined(CONFIG_TXQ_ORDER_CHANGE_NRC_DRV) */

	NRC_SKB_TRACK_QUEUE(hdev, skb, hif->type, false);

	/* Schedule work handler */
	if (atomic_cmpxchg(&hdev->queue_pending, 0, 1) == 0) {
		queue_work(hdev->workqueue, &hdev->work);
	}

	return 0;
}

/**
 * hif_enqueue_mcp_skb - Enqueue SKB to MCP queue
 * @hdev: HIF device
 * @skb: Socket buffer to enqueue
 *
 * Enqueues frame to MCP-specific queue and schedules MCP work handler.
 * MCP uses separate queues from WLAN to avoid interference.
 */
int hif_enqueue_mcp_skb(struct nrc_hif_device *hdev, struct sk_buff *skb)
{
	struct hif *hif = (void *)skb->data;

	if ((hif == NULL) || (hdev == NULL)) {
		ERR_HIF("MCP enqueue_skb failed: null pointer (hif=%p, hdev=%p)",
			hif, hdev);
		return -EINVAL;
	}

	/* Validate HIF header before enqueuing to MCP queue */
	VALIDATE_HIF_HEADER(skb, "hif_enqueue_mcp_skb");

	if (hif->type != HIF_TYPE_FRAME && hif->type != HIF_TYPE_WIM &&
	    hif->type != HIF_TYPE_LOOPBACK) {
		ERR_HIF("MCP enqueue_skb failed: invalid HIF type (%d)",
			hif->type);
		WARN_ON(true);
		return -EINVAL;
	}

	/* Check MCP workqueue before enqueuing to prevent orphaned SKBs */
	if (hdev->mcp_workqueue == NULL) {
		ERR_HIF("MCP enqueue_skb failed: mcp_workqueue is null");
		return -EINVAL;
	}

	/* Map HIF type to queue index:
	 * mcp_queue[0] = FRAME packets
	 * mcp_queue[1] = WIM commands (and LOOPBACK)
	 */
	if (hif->type == HIF_TYPE_FRAME) {
		skb_queue_tail(&hdev->mcp_queue[0], skb);
		DBG_HIF("MCP: Enqueued frame to queue[0], len=%d",
			NRC_MCP_FRAME_QUEUE_LEN(hdev));
	} else {
		/* WIM or LOOPBACK -> queue[1] */
		skb_queue_tail(&hdev->mcp_queue[1], skb);
		DBG_HIF("MCP: Enqueued WIM/LOOPBACK to queue[1], len=%d",
			NRC_MCP_WIM_QUEUE_LEN(hdev));
	}

	NRC_SKB_TRACK_QUEUE(hdev, skb, hif->type, false);

	/* Schedule MCP work if not already pending */
	if (atomic_cmpxchg(&hdev->mcp_queue_pending, 0, 1) == 0) {
		queue_work(hdev->mcp_workqueue, &hdev->mcp_work);
	}

	return 0;
}

/**
 * nrc_skb_append_tx_info - Appends tx meta data to the end of skb
 *
 */
static u32 nrc_skb_append_tx_info(struct nrc_hif_device *hdev, u16 aid,
				  struct sk_buff *skb, bool frame_injection)
{
	struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
	struct frame_tx_info_param *p;

	if (skb_tailroom(skb) < tlv_len(sizeof(*p))) {
		if (WARN_ON(pskb_expand_head(skb, 0, tlv_len(sizeof(*p)),
					     GFP_ATOMIC)))
			return 0;
	}

	p = nrc_wim_skb_add_tlv(skb, WIM_TLV_EXTRA_TX_INFO, sizeof(*p), NULL);
#ifdef CONFIG_SUPPORT_TX_CONTROL
	p->use_rts = txi->control.use_rts;
	p->use_11b_protection = txi->control.use_cts_prot;
	p->short_preamble = txi->control.short_preamble;
#endif
	p->ampdu = !!(txi->flags & IEEE80211_TX_CTL_AMPDU);
	p->after_dtim = !!(txi->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM);
	p->no_ack = !!(txi->flags & IEEE80211_TX_CTL_NO_ACK);
	p->eosp = 0;
	p->inject = !!(frame_injection || hdev->params->wlantest);
	p->aid = aid;

#if defined(CONFIG_NRC_HIF_PRINT_TX_INFO)
	DBG_HIF("rts:%d cts_prot:%d sp:%d amdpu:%d noack:%d eosp:%d",
		p->use_rts, p->use_cts_prot, p->short_preamble, p->ampdu,
		p->no_ack, p->eosp);
#endif

	return tlv_len(sizeof(*p));
}

/**
 * nrc_xmit_injected_frame - transmit a injected 802.11 frame
 */
int nrc_xmit_injected_frame(struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (void *)skb->data;
	struct frame_hdr *fh;
	struct hif *hif;
	__le16 fc = hdr->frame_control;
	struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
	int extra_len, ret = 0;
	int credit;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	BUG_ON(hdev == NULL);

	extra_len =
		nrc_skb_append_tx_info(hdev, (!!sta ? sta->aid : 0), skb, true);

	/* Prepend a HIF and frame header */
	hif = (void *)skb_push(skb, hdev->fw.info.tx_head_size);
	memset(hif, 0, hdev->fw.info.tx_head_size);
	hif->type = HIF_TYPE_FRAME;
	hif->len = skb->len - sizeof(*hif);
	hif->vifindex = hw_vifindex(vif);

	/* Validate HIF header after creation */
	VALIDATE_HIF_HEADER(skb, "nrc_xmit_injected_frame");

	/* Frame header */
	fh = (void *)(hif + 1);
	fh->info.tx.tlv_len = extra_len;
	fh->info.tx.cipher = WIM_CIPHER_TYPE_NONE;
#ifdef CONFIG_SUPPORT_TX_CONTROL
	fh->flags.tx.ac = txi->hw_queue;
#endif

	if (ieee80211_is_data(fc)) {
		hif->subtype = HIF_FRAME_SUB_DATA_BE;
	} else if (ieee80211_is_mgmt(fc)) {
		hif->subtype = HIF_FRAME_SUB_MGMT;
		fh->flags.tx.ac = (hif->vifindex == 0 ?
					   3 :
					   (hdev->hw_queues < 7 ? 5 : 9));
	} else if (ieee80211_is_ctl(fc)) {
		hif->subtype = HIF_FRAME_SUB_CTRL;
		fh->flags.tx.ac = (hif->vifindex == 0 ?
					   3 :
					   (hdev->hw_queues < 7 ? 5 : 9));
	} else {
		WARN_ON(true);
	}

	credit = DIV_ROUND_UP(skb->len, hdev->fw.info.buffer_size);
	DBG_TX("%s ac:%d, pend:%d, credit:%d", __func__, fh->flags.tx.ac,
	       atomic_read(&hdev->credit.tx_pend[fh->flags.tx.ac]), credit);
	atomic_add(credit, &hdev->credit.tx_pend[fh->flags.tx.ac]);
	ret = hif_enqueue_skb(hdev, skb);
	if (ret != 0) {
		nrc_hif_free_skb(hdev, skb);
	}

	return ret;
}

/**
 * nrc_xmit_frame - transmit a 802.11 frame
 */
int nrc_xmit_wlan_frame(s8 vif_index, u16 aid, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (void *)skb->data;
	struct frame_hdr *fh;
	struct hif *hifh;
	__le16 fc = hdr->frame_control;
	struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
	struct ieee80211_key_conf *key = txi->control.hw_key;
	int extra_len, ret = 0;
	int credit;
#if defined(CONFIG_SUPPORT_KEY_RESERVE_TAILROOM)
	int crypto_tail_len = 0;
#endif
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	BUG_ON(hdev == NULL);
	BUG_ON(hdev->nw == NULL);

	if (!hdev->nw) {
		nrc_hif_free_skb(hdev, skb);
		return -EINVAL;
	}

	if (atomic_read(&hdev->nw->d_deauth.delayed_deauth)) {
		if (key) {
			key = &hdev->nw->d_deauth.p;
		}
	}

#if defined(CONFIG_SUPPORT_KEY_RESERVE_TAILROOM)
	if ((key && (key->flags & IEEE80211_KEY_FLAG_RESERVE_TAILROOM)) &&
	    (hdev->cap.cap_mask & WIM_SYSTEM_CAP_HWSEC_OFFL)) {
		switch (key->cipher) {
		case WLAN_CIPHER_SUITE_WEP40:
		case WLAN_CIPHER_SUITE_WEP104:
			crypto_tail_len = IEEE80211_WEP_ICV_LEN;
			break;
		case WLAN_CIPHER_SUITE_TKIP:
			crypto_tail_len = IEEE80211_TKIP_ICV_LEN;
			break;
		case WLAN_CIPHER_SUITE_CCMP:
			crypto_tail_len = IEEE80211_CCMP_MIC_LEN;
			break;
#ifdef CONFIG_SUPPORT_CCMP_256
		case WLAN_CIPHER_SUITE_CCMP_256:
			crypto_tail_len = IEEE80211_CCMP_256_MIC_LEN;
			break;
#endif
#ifdef CONFIG_SUPPORT_GCMP
		case WLAN_CIPHER_SUITE_GCMP:
			crypto_tail_len = IEEE80211_GCMP_MIC_LEN;
			break;
		case WLAN_CIPHER_SUITE_GCMP_256:
			crypto_tail_len = IEEE80211_GCMP_MIC_LEN;
			break;
#endif
#ifdef CONFIG_SUPPORT_GMAC
		case WLAN_CIPHER_SUITE_BIP_GMAC_128:
			crypto_tail_len = IEEE80211_GMAC_PN_LEN;
			break;
		case WLAN_CIPHER_SUITE_BIP_GMAC_256:
			crypto_tail_len = IEEE80211_GMAC_PN_LEN;
			break;
#endif

		default:
			ERR_TX("Unknown cipher detected.(%d)", key->cipher);
		}
		skb_put(skb, crypto_tail_len);
	}
#endif

	extra_len = nrc_skb_append_tx_info(hdev, aid, skb, false);

	/* Prepend a HIF and frame header */
	hifh = (void *)skb_push(skb, hdev->fw.info.tx_head_size);
	memset(hifh, 0, hdev->fw.info.tx_head_size);
	hifh->type = HIF_TYPE_FRAME;
	hifh->len = skb->len - sizeof(*hifh);
	hifh->vifindex = vif_index;

	/* Validate HIF header after creation */
	VALIDATE_HIF_HEADER(skb, "nrc_xmit_wlan_frame");

	/* Frame header */
	fh = (void *)(hifh + 1);
	fh->info.tx.tlv_len = extra_len;
	fh->info.tx.cipher = WIM_CIPHER_TYPE_NONE;
#ifdef CONFIG_SUPPORT_TX_CONTROL
	fh->flags.tx.ac = txi->hw_queue;
#endif

	if (key) {
		if (!ieee80211_has_protected(fc))
			DBG_HIF("Key found but protected frame bit is 0.");
		fh->info.tx.cipher = nrc_to_wim_cipher_type(key->cipher);
		if (fh->info.tx.cipher == (uint8_t)WIM_CIPHER_TYPE_INVALID) {
			if (ieee80211_has_protected(fc)) {
				DBG_STATE(
					"protected frame bit is 1 but invalid cipher type(%d).",
					key->cipher);
				/* Track FRAME SKB free (TX path failure) */
				NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_FRAME,
						   false, false);
				return ret;
			}
			fh->info.tx.cipher = WIM_CIPHER_TYPE_NONE;
		}
	}

	/*
	 * TODO:
	 * - vif index
	 */
	if (ieee80211_is_data(fc)) {
		hifh->subtype = HIF_FRAME_SUB_DATA_BE;
		/* temporarily use a BE hw_queue instead of the invalid value. */
	} else if (ieee80211_is_mgmt(fc)) {
		hifh->subtype = HIF_FRAME_SUB_MGMT;
		fh->flags.tx.ac = (hifh->vifindex == 0 ?
					   3 :
					   (hdev->hw_queues < 7 ? 5 : 9));
	} else if (ieee80211_is_ctl(fc)) {
		hifh->subtype = HIF_FRAME_SUB_CTRL;
		fh->flags.tx.ac = (hifh->vifindex == 0 ?
					   3 :
					   (hdev->hw_queues < 7 ? 5 : 9));
	} else {
		WARN_ON(true);
	}

	if (hdev->params->nullfunc_enable) {
		if (ieee80211_is_pspoll(fc)) {
			print_hex_dump(KERN_DEBUG, "tx ps-poll ",
				       DUMP_PREFIX_NONE, 16, 1, fh, 20, false);
		}
	}
	credit = DIV_ROUND_UP(skb->len, hdev->fw.info.buffer_size);
	DBG_TX("%s ac:%d, pend:%d, credit:%d", __func__, fh->flags.tx.ac,
	       atomic_read(&hdev->credit.tx_pend[fh->flags.tx.ac]), credit);
	atomic_add(credit, &hdev->credit.tx_pend[fh->flags.tx.ac]);

#ifdef NRC_DBG_PRINT_ARP_FRAME
	if (IS_ARP(skb)) {
		DBG_TX("TX ARP [type:%d sype:%d, protected:%d, len:%d] [vif:%d, ac:%d]",
		       WLAN_FC_GET_TYPE(fc), WLAN_FC_GET_STYPE(fc),
		       ieee80211_has_protected(fc), skb->len, vif_index,
		       fh->flags.tx.ac);
	}
#endif

	if (!hdev->params->loopback) {
		ret = hif_enqueue_skb(hdev, skb);
		if (ret != 0) {
			nrc_hif_free_skb(hdev, skb);
		}
	} else {
		/* Loopback mode: just free SKB */
		nrc_hif_free_skb(hdev, skb);
		ret = 0;
	}

	return ret;
}

/**
 * nrc_xmit_mcp_frame - transmit a MCP data frame
 * @subtype: HIF frame subtype
 * @skb: Socket buffer containing the frame data (without HIF header)
 *
 * Optimized transmission path for MCP frontend:
 * - No WLAN-specific processing (VIF, STA, IEEE80211)
 * - Power save handling in MCP work handler
 * - Uses dedicated MCP queue and workqueue
 * - Simplified HIF header construction
 * - MCP always uses vif_index=0
 */
int nrc_xmit_mcp_frame(int subtype, struct sk_buff *skb,
		       bool hif_header_included)
{
	struct hif *hifh;
	int ret = 0;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev) {
		/* Error drop: no hdev available */
		NRC_SKB_TRACK_FREE(NULL, skb, -1, false, false);
		return -ENODEV;
	}

	if (hif_header_included) {
		/* HIF header already present - just validate */
		hifh = (struct hif *)skb->data;
		DBG_HIF("MCP TX: HIF header already included, subtype=%d len=%u",
			hifh->subtype, skb->len);
	} else {
		/* Add HIF header to frame */
		hifh = (void *)skb_push(skb, sizeof(*hifh));
		memset(hifh, 0, sizeof(*hifh));
		hifh->type = HIF_TYPE_FRAME;
		hifh->subtype = subtype;
		hifh->vifindex = 0; /* MCP always uses vif 0 */
		hifh->len = skb->len - sizeof(*hifh);
		hifh->flags = HIF_HEADER_CALC_CHECKSUM(hifh->type, hifh->len);

		DBG_HIF("MCP TX: subtype=%d len=%u", subtype, hifh->len);
	}

	/* Validate HIF header before enqueuing to MCP queue */
	VALIDATE_HIF_HEADER(skb, "nrc_xmit_mcp_frame");

	/* Enqueue to MCP-specific queue */
	ret = hif_enqueue_mcp_skb(hdev, skb);
	if (ret < 0) {
		nrc_hif_free_skb(hdev, skb);
	}

	return ret;
}

void nrc_hal_debug_send(struct sk_buff *skb)
{
	int ret = 0;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		/* Free SKB on error path to prevent leak */
		if (skb)
			dev_kfree_skb(skb);
		return;
	}

	if (!hdev->debug) {
		ERR_HIF("Invalid debug structure");
		/* Free SKB with tracking on error path */
		nrc_hif_free_skb(hdev, skb);
		return;
	}

	ret = hif_enqueue_skb(hdev, skb);
	if (ret < 0)
		nrc_hif_free_skb(hdev, skb);
}

/* trial code */
#ifdef TEST_BLOCK_TX
int nrc_hif_wait_tx(struct nrc_hif_device *dev)
{
	int ret = -1;
	int timeout_ms = 2000;

	ret = wait_for_completion_timeout(&dev->sleep_done,
					  msecs_to_jiffies(timeout_ms));
	if (ret == 0) {
		DBG_PS("Timeout(%dmsec) sleeping target", timeout_ms);
		ret = -1;
	} else {
		ret = 0;
	}

	return ret;
}

void nrc_hif_resume_tx(struct nrc_hif_device *dev, int mode)
{
	complete_all(&dev->sleep_done);
}

void nrc_hif_suspend_tx(struct nrc_hif_device *dev, int mode)
{
	reinit_completion(&dev->sleep_done);
}
#endif
/* HAL Wrapper Functions for ops pattern consistency */
int nrc_hal_start(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	int ret;

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return -EINVAL;
	}

	DBG_HIF("start()");

	if (hdev->started)
		return 0;

	ret = nrc_hif_ops_start();
	if (ret == 0) {
		hdev->started = true;

		/* Allocate wakeup pin GPIO if power save mode requires it */
		if (NRC_PARAM_POWER_SAVE(hdev) >= NRC_PS_DEEPSLEEP_TIM ||
		    NRC_PARAM_IDLE_MODE(hdev)) {
			int wakeup_gpio = NRC_PARAM_POWER_SAVE_GPIO(hdev, 0);
			if (wakeup_gpio > 0) {
				ret = nrc_hif_ops_gpio_alloc(wakeup_gpio,
							     "nrc-wakeup");
				if (ret) {
					ERR_HIF("Failed to allocate wakeup GPIO %d: %d",
						wakeup_gpio, ret);
					nrc_hif_ops_stop();
					hdev->started = false;
					return ret;
				}
				INFO_HIF(
					"Wakeup GPIO %d allocated successfully",
					wakeup_gpio);
			}
		}
	}

	return ret;
}

int nrc_hal_stop(struct nrc_hif_device *hdev)
{
	int wakeup_gpio;

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return -EINVAL;
	}

	DBG_HIF("stop()");

	/* Free wakeup pin GPIO unconditionally if allocated */
	wakeup_gpio = NRC_PARAM_POWER_SAVE_GPIO(hdev, 0);
	if (wakeup_gpio > 0) {
		DBG_HIF("Freeing wakeup GPIO %d (started=%d, ps=%d, idle=%d)",
			wakeup_gpio, hdev->started, NRC_PARAM_POWER_SAVE(hdev),
			NRC_PARAM_IDLE_MODE(hdev));
		nrc_hif_ops_gpio_free(wakeup_gpio);
		INFO_HIF("Wakeup GPIO %d freed", wakeup_gpio);
	}

	if (!hdev->started)
		return 0;

	/* Ensure device is awake before stopping (HAL Master responsibility) */
	if (!NRC_PS_IS_AWAKE(hdev)) {
		DBG_HIF("Device not awake before stop, requesting wake");
		nrc_ps_request_wake_sync(hdev, 2000,
					 NRC_PS_REASON_HAL_SHUTDOWN);
	}

	/* Flush TX work queue before stopping */
	nrc_tx_flush_wq(hdev);

	hdev->started = false;

	return nrc_hif_ops_stop();
}

/* ===========================================================================
 * HIF Slot Management Functions
 * =========================================================================== */

/**
 * nrc_hif_set_slot_index - Set slot index directly in hdev
 * @hdev: HIF device structure
 * @slot_type: Slot type (TX_SLOT or RX_SLOT)
 * @index_type: Index type (SLOT_HEAD or SLOT_TAIL)
 * @value: Value to set
 */
void nrc_hif_set_slot_index(struct nrc_hif_device *hdev, int slot_type,
			    int index_type, int value)
{
	int old_value;

	if (!hdev || slot_type < 0 || slot_type > 1 || index_type < 0 ||
	    index_type > 1)
		return;

	if (index_type == SLOT_HEAD) {
		old_value = hdev->slot[slot_type].head;
		if (old_value != value) {
			DBG_SLOT("Set %s slot HEAD: %d -> %d (TAIL=%d)",
				 slot_type == 0 ? "TX" : "RX", old_value, value,
				 hdev->slot[slot_type].tail);
			hdev->slot[slot_type].head = value;
		}
	} else if (index_type == SLOT_TAIL) {
		old_value = hdev->slot[slot_type].tail;
		if (old_value != value) {
			DBG_SLOT("Set %s slot TAIL: %d -> %d (HEAD=%d)",
				 slot_type == 0 ? "TX" : "RX", old_value, value,
				 hdev->slot[slot_type].head);
			hdev->slot[slot_type].tail = value;
		}
	}
}

/**
 * nrc_hif_inc_slot_index - Increment slot index directly in hdev
 * @hdev: HIF device structure
 * @slot_type: Slot type (TX_SLOT or RX_SLOT)
 * @index_type: Index type (SLOT_HEAD or SLOT_TAIL)
 */
void nrc_hif_inc_slot_index(struct nrc_hif_device *hdev, int slot_type,
			    int index_type)
{
	if (!hdev || slot_type < 0 || slot_type > 1 || index_type < 0 ||
	    index_type > 1)
		return;

	if (index_type == SLOT_HEAD) {
		DBG_SLOT("Inc %s slot HEAD: %d -> %d (TAIL=%d)",
			 slot_type == 0 ? "TX" : "RX",
			 hdev->slot[slot_type].head,
			 hdev->slot[slot_type].head + 1,
			 hdev->slot[slot_type].tail);
		hdev->slot[slot_type].head++;
	} else if (index_type == SLOT_TAIL) {
		DBG_SLOT("Inc %s slot TAIL: %d -> %d (HEAD=%d)",
			 slot_type == 0 ? "TX" : "RX",
			 hdev->slot[slot_type].tail,
			 hdev->slot[slot_type].tail + 1,
			 hdev->slot[slot_type].head);
		hdev->slot[slot_type].tail++;
	}
}
