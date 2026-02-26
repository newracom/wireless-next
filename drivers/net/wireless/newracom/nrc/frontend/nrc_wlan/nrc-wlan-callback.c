/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Callback Implementation
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

/* Linux networking headers */
#include <net/mac80211.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"
#include "nrc-ps-common.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-wim-types.h"

/* Local module headers */
#include "nrc-mac80211.h"
#include "nrc-netlink.h"
#include "nrc-ps.h"
#include "nrc-twt-sched.h"
#include "nrc-wlan-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-wlan-hal-init.h"
#include "nrc-wim-wlan.h"

/* Forward declarations */
static int nrc_wlan_hal_callback_handler(struct nrc_hal_event_data *hal_event);
static int nrc_wlan_handle_wim_event(struct nrc_hal_event_data *event);
static int nrc_wlan_handle_fw_ready_from_wdt(struct nrc_hal_event_data *event);
static int nrc_wlan_handle_ps_enter_failed(struct nrc_hal_event_data *event);
static int nrc_wlan_handle_twt_service(struct nrc_hal_event_data *event);
static int nrc_wlan_handle_twt_quiet(struct nrc_hal_event_data *event);
static int
nrc_wlan_handle_ps_dyn_start_custom_timeout(struct nrc_hal_event_data *event);

/**
 * nrc_wlan_handle_spi_irq - Handle SPI interrupt event
 * @event: HAL event data containing SPI interrupt information
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_handle_spi_irq(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR_WLAN("Invalid event data for SPI IRQ");
		return -EINVAL;
	}

	return 0;
}

/**
 * nrc_wlan_handle_rx_ready - Handle RX ready event from HAL
 * @event: HAL event data containing RX ready information
 *
 * Processes RX data packets that were forwarded from HAL intelligent routing.
 * HAL has already filtered the packets, so we only receive packets that
 * need WLAN processing (HIF_TYPE_FRAME, HIF_TYPE_WIM, HIF_TYPE_LOG, etc.)
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_handle_rx_ready(struct nrc_hal_event_data *event)
{
	struct sk_buff *skb;
	struct hif *hif;
	struct nrc_hif_device *hdev;
	struct nrc *nw;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	if (!event || !event->data) {
		ERR_WLAN("Invalid event data for RX ready");
		return -EINVAL;
	}

	skb = (struct sk_buff *)event->data;
	if (!skb || skb->len < sizeof(struct hif)) {
		ERR_WLAN("Invalid SKB in RX ready event");
		return -EINVAL;
	}

	hif = (struct hif *)skb->data;

	DBG_RX("WLAN HIF type=%s(%u) subtype=%s(%u) len=%u skb_len=%u",
	       nrc_hif_type_str(hif->type), hif->type,
	       nrc_hif_subtype_str(hif->type, hif->subtype), hif->subtype,
	       hif->len, skb->len);

	WARN_ON(skb->len != hif->len + sizeof(*hif));

	if (NRC_HIF_DRV_STATE(hdev) < NRC_DRV_START) {
		ERR_WLAN(
			"WLAN RX: Driver not ready (state=%d), dropping packet\n",
			NRC_HIF_DRV_STATE(hdev));
		/* Error drop: driver not ready, use actual hif type */
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
		return -EIO;
	}

	skb_pull(skb, sizeof(*hif));

	switch (hif->type) {
	case HIF_TYPE_FRAME:
		nrc_mac_rx(nw, skb);
		break;

	case HIF_TYPE_WIM:
		/* Error drop: WIM packets should not be forwarded to WLAN */
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, true, false);
		break;

	case HIF_TYPE_LOG:
		nrc_netlink_rx(nw, skb, hif->subtype);
		break;

#if defined(DEBUG)
	case HIF_TYPE_LOOPBACK:
		/* Error drop: Loopback packets should not be forwarded to WLAN */
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_LOOPBACK, true, false);
		break;
#endif

	default:
		ERR_WLAN(
			"WLAN: Unknown HIF packet type %u forwarded from HAL\n",
			hif->type);
		/* Error drop: unknown packet type, but use actual type value */
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
		break;
	}

	return 0;
}

/**
 * nrc_wlan_handle_tx_complete - Handle TX complete event
 * @event: HAL event data containing TX complete information
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_handle_tx_complete(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR_WLAN("Invalid event data for TX complete");
		return -EINVAL;
	}

	return 0;
}

/**
 * nrc_wlan_handle_error - Handle error event
 * @event: HAL event data containing error information
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_handle_error(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR_WLAN("Invalid event data for error");
		return -EINVAL;
	}

	return 0;
}

/**
 * nrc_wlan_handle_connection_loss - Handle connection loss event
 * @event: HAL event data containing connection loss information
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_handle_connection_loss(struct nrc_hal_event_data *event)
{
	struct nrc_hif_device *hdev;
	struct nrc *nw;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	if (!event) {
		ERR_WLAN("Invalid event for connection loss");
		return -EINVAL;
	}

	/* Handle W_DISABLE_ASSERTED in WLAN context */
	ieee80211_connection_loss(nw->vif[0]);
	mdelay(300);
	nrc_hal_ops_tx_cleanup_queues();
	nrc_mac_clean_txq(nw);
	nrc_mac_cancel_hw_scan(nw->hw, nw->vif[0]);

	return 0;
}

/**
 * nrc_wlan_handle_wake_done - Handle wakeup completion from HAL
 * @event: HAL event data
 *
 * Called by HAL after wakeup sequence is complete.
 * Handles WLAN-specific post-wakeup operations.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_handle_wake_done(struct nrc_hal_event_data *event)
{
	struct nrc_hif_device *hdev;
	struct nrc *nw;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	/* Wake IEEE80211 queues */
	ieee80211_wake_queues(nw->hw);

	/* Kick TXQ to process pending TX frames */
	nrc_kick_txq(nw);

#if defined(CONFIG_SUPPORT_BD)
	{
		struct regulatory_request request;
		request.alpha2[0] = nw->alpha2[0];
		request.alpha2[1] = nw->alpha2[1];
		request.initiator = NL80211_REGDOM_SET_BY_DRIVER;
		nrc_reg_notifier(nw->hw->wiphy, &request);
	}
#endif
#ifdef CONFIG_S1G_CHANNEL
	init_s1g_channels(nw);
#endif

	/* Restart dynamic PS timer (function checks supports_dynamic_ps internally) */
	nrc_ps_dyn_start(nw);

	if (!ieee80211_hw_check(nw->hw, SUPPORTS_PS)) {
		/* PS not supported - handle beacon loss */
		if (hdev->params->power_save >= NRC_PS_DEEPSLEEP_NONTIM) {
			if (!atomic_read(&nw->d_deauth.delayed_deauth))
				nrc_send_beacon_loss(nw);
		} else
			nw->invoke_beacon_loss = true;
	}

	if (!hdev->params->disable_cqm && nw->associated_vif) {
		mod_timer(&nw->bcn_mon_timer,
			  jiffies + msecs_to_jiffies(nw->beacon_timeout));
	}

	/* Send pending deauth frame if allocated (regardless of delayed_deauth flag) */
	if (nw->d_deauth.deauth_frm) {
		DBG_PS("Sending pending deauth frame (vif=%d, aid=%d)",
		       nw->d_deauth.vif_index, nw->d_deauth.aid);
		nrc_hal_ops_xmit_wlan_frame(nw->d_deauth.vif_index,
					    nw->d_deauth.aid,
					    nw->d_deauth.deauth_frm);
		nw->d_deauth.deauth_frm = NULL;
		msleep(50);
	}

	/* Process delayed deauth cleanup if flag is set */
	if (atomic_read(&nw->d_deauth.delayed_deauth)) {
		struct ieee80211_tx_info *txi;
		struct ieee80211_key_conf *key = NULL;
		struct sk_buff *skb;
		int i;

		/* Get key info if deauth frame was sent */
		if (nw->d_deauth.deauth_frm) {
			txi = IEEE80211_SKB_CB(nw->d_deauth.deauth_frm);
			key = txi->control.hw_key;
		}

		/* Finalize data : Common routine */
		if (nw->d_deauth.p.flags & IEEE80211_KEY_FLAG_PAIRWISE)
			nrc_wim_wlan_install_key(DISABLE_KEY, &nw->d_deauth.v,
						 &nw->d_deauth.s,
						 &nw->d_deauth.p);
		else if (key)
			nrc_wim_wlan_install_key(DISABLE_KEY, &nw->d_deauth.v,
						 &nw->d_deauth.s,
						 &nw->d_deauth.g);
		nrc_mac_sta_remove(nw->hw, &nw->d_deauth.v, &nw->d_deauth.s);
		nrc_mac_bss_info_changed(nw->hw, &nw->d_deauth.v,
					 &nw->d_deauth.b, 0x80309f);
		for (i = 0; i < IEEE80211_NUM_ACS; i++) {
#ifdef CONFIG_SUPPORT_CHANNEL_INFO
#ifdef CONFIG_USE_LINK_ID
			nrc_mac_conf_tx(nw->hw, &nw->d_deauth.v,
					nw->vif[nw->d_deauth.vif_index]
						->bss_conf.link_id,
					i, &nw->d_deauth.tqp[i]);
#else
			nrc_mac_conf_tx(nw->hw, &nw->d_deauth.v, i,
					&nw->d_deauth.tqp[i]);
#endif /* ifdef CONFIG_USE_LINK_ID */
#else
			nrc_mac_conf_tx(nw->hw, i, &nw->d_deauth.tqp[i]);
#endif
		}
		skb = nrc_hal_ops_wim_alloc_skb(WIM_CMD_SET, WIM_MAX_SIZE);
#ifdef CONFIG_SUPPORT_CHANNEL_INFO
		nrc_mac_add_tlv_channel(skb, &nw->d_deauth.c);
#else
		nrc_mac_add_tlv_channel(skb, &nw->d_deauth.c);
#endif
		nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
		if (nw->d_deauth.p.flags & IEEE80211_KEY_FLAG_PAIRWISE && key)
			nrc_wim_wlan_install_key(DISABLE_KEY, &nw->d_deauth.v,
						 &nw->d_deauth.s,
						 &nw->d_deauth.g);

		/* Remove interface : when 'ifconfig wlan0 down' or 'rmmod' */
		if (nw->d_deauth.removed) {
			nrc_wim_wlan_unset_sta_type(&nw->d_deauth.v);
			nw->vif[nw->d_deauth.vif_index] = NULL;
			nw->enable_vif[nw->d_deauth.vif_index] = false;
			atomic_set(&nw->d_deauth.delayed_deauth, 0);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
			nrc_mac_stop(nw->hw, false);
#else
			nrc_mac_stop(nw->hw);
#endif
		}
		atomic_set(&nw->d_deauth.delayed_deauth, 0);
	}

	DBG_PS("WLAN: Wake done processing complete");
	nrc_ps_dyn_start(nw);

	return 0;
}

/**
 * nrc_wlan_handle_ps_enter_failed - Handle PS enter failure notification from HAL
 * @event: HAL event data
 *
 * Called by HAL after PS recovery is complete (HIF resumed, state updated).
 * Handles WLAN-specific recovery operations:
 * - Start dynamic PS with extended timeout
 * - Restart beacon monitoring
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_handle_ps_enter_failed(struct nrc_hal_event_data *event)
{
	struct nrc *nw;
	struct nrc_hif_device *hdev;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	DBG_PS("WLAN: Processing PS enter failed event");

	/* Need to check if AP is alive, increase timeout more than beacon_timeout
	 * 2000msec is enough time to check with probe req/resp
	 */
	nrc_ps_dyn_start_custom_timeout(nw, nw->beacon_timeout + 2000);

	/* Restart beacon monitoring */
	if (!hdev->params->disable_cqm && nw->associated_vif) {
		mod_timer(&nw->bcn_mon_timer,
			  jiffies + msecs_to_jiffies(nw->beacon_timeout));
	}

	DBG_PS("WLAN: PS enter failed recovery complete");

	return 0;
}

static int
nrc_wlan_handle_ps_dyn_start_custom_timeout(struct nrc_hal_event_data *event)
{
	struct nrc *nw;
	u32 custom_timeout = 2000; /* Default timeout in milliseconds */

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}

	if (!event) {
		ERR_WLAN(
			"Invalid event for PS dynamic start with custom timeout");
		return -EINVAL;
	}

	/* Extract custom timeout from event data if available */
	if (event->data && event->data_len == sizeof(u32)) {
		custom_timeout = *(u32 *)event->data;
	}

	/* Start dynamic power save with custom timeout */
	nrc_ps_dyn_start_custom_timeout(nw, custom_timeout);

	return 0;
}

/**
 * nrc_wlan_handle_kick_txq - Handle TX queue kick request from HAL
 * @event: HAL event data containing network device pointer
 */
static int nrc_wlan_handle_kick_txq(struct nrc_hal_event_data *event)
{
	struct nrc *nw;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}

	/* Call the TX queue kick function */
	nrc_kick_txq(nw);

	return 0;
}

/**
 * nrc_wlan_handle_cleanup_txq_all - Handle cleanup all TX queues request from HAL
 * @event: HAL event data containing network device pointer
 */
static int nrc_wlan_handle_cleanup_txq_all(struct nrc_hal_event_data *event)
{
	struct nrc *nw;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}

	if (!nw->params->disable_cqm) {
		try_to_del_timer_sync(&nw->bcn_mon_timer);
	}

	ieee80211_stop_queues(nw->hw);

	/* Call the cleanup all TX queues function */
#ifdef CONFIG_USE_TXQ
	nrc_cleanup_txq_all(nw);
#endif

	return 0;
}

/**
 * nrc_wlan_handle_free_skb - Handle nrc_hif_free_skb from HAL
 * @event: HAL event data containing network device pointer
 */
static int nrc_wlan_handle_free_skb(struct nrc_hal_event_data *event)
{
	struct sk_buff *skb;
	struct ieee80211_tx_info *txi;
	struct hif *hif;
	struct frame_hdr *fh;
	bool ack = true;
	struct nrc *nw = nrc_wlan_get_nw();
	struct nrc_hif_device *hdev = nw->hdev;

	if (!nrc_wlan_is_initialized()) {
		ERR_WLAN("WLAN not initialized");
		return -EINVAL;
	}

	if (!event || !event->data) {
		ERR_WLAN("Invalid %s() event data", __func__);
		return -EINVAL;
	}

	skb = (struct sk_buff *)event->data;
	if (!skb || skb->len < sizeof(struct hif)) {
		ERR_WLAN("Invalid SKB in event");
		return -EINVAL;
	}

	hif = (void *)skb->data;
	fh = (struct frame_hdr *)(hif + 1);
	txi = IEEE80211_SKB_CB(skb);

	ieee80211_tx_info_clear_status(txi);
	if (!(txi->flags & IEEE80211_TX_CTL_NO_ACK) && ack)
		txi->flags |= IEEE80211_TX_STAT_ACK;

	/* Peel out our headers */
	skb_pull(skb, hdev->fw.info.tx_head_size);

	ieee80211_tx_status_irqsafe(hdev->nw->hw, skb);

	return 0;
}

/**
 * nrc_wlan_hal_callback_handler - Handle HAL events
 * @hal_event: HAL event data
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_hal_callback_handler(struct nrc_hal_event_data *hal_event)
{
	int ret = 0;

	if (!hal_event) {
		ERR_WLAN("Invalid HAL event data");
		return -EINVAL;
	}

	/* Dispatch to appropriate handler based on event type */
	switch (hal_event->type) {
	case NRC_HAL_EVT_SPI_IRQ:
		ret = nrc_wlan_handle_spi_irq(hal_event);
		break;
	case NRC_HAL_EVT_RX_READY:
		ret = nrc_wlan_handle_rx_ready(hal_event);
		break;
	case NRC_HAL_EVT_TX_COMPLETE:
		ret = nrc_wlan_handle_tx_complete(hal_event);
		break;
	case NRC_HAL_EVT_ERROR:
		ret = nrc_wlan_handle_error(hal_event);
		break;
	case NRC_HAL_EVT_TARGET_NOTI_W_DISABLE_ASSERTED:
		ret = nrc_wlan_handle_connection_loss(hal_event);
		break;
	case NRC_HAL_EVT_PS_DYN_START_CUSTOM_TIMEOUT:
		ret = nrc_wlan_handle_ps_dyn_start_custom_timeout(hal_event);
		break;
	case NRC_HAL_EVT_TARGET_NOTI_FW_READY_FROM_WDT:
		ret = nrc_wlan_handle_fw_ready_from_wdt(hal_event);
		break;
	case NRC_HAL_EVT_WAKE_DONE:
		ret = nrc_wlan_handle_wake_done(hal_event);
		break;
	case NRC_HAL_EVT_PS_ENTER_FAILED:
		ret = nrc_wlan_handle_ps_enter_failed(hal_event);
		break;
	case NRC_HAL_EVT_WIM_EVENT:
		ret = nrc_wlan_handle_wim_event(hal_event);
		break;
	case NRC_HAL_EVT_REG_NOTIFIER:
		ret = nrc_wlan_handle_reg_notifier(hal_event);
		break;
	case NRC_HAL_EVT_KICK_TXQ:
		ret = nrc_wlan_handle_kick_txq(hal_event);
		break;
	case NRC_HAL_EVT_CLEANUP_TXQ_ALL:
		ret = nrc_wlan_handle_cleanup_txq_all(hal_event);
		break;
	case NRC_HAL_EVT_FREE_SKB:
		ret = nrc_wlan_handle_free_skb(hal_event);
		break;
	case NRC_HAL_EVT_TARGET_NOTI_TWT_SERVICE:
		ret = nrc_wlan_handle_twt_service(hal_event);
		break;
	case NRC_HAL_EVT_TARGET_NOTI_TWT_QUIET:
		ret = nrc_wlan_handle_twt_quiet(hal_event);
		break;
	default:
		ERR_WLAN("Unknown HAL event type %d", hal_event->type);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void nrc_wim_event_enqueue(struct nrc *nw, struct ieee80211_vif *vif,
				  void *data, wim_event_handler_t handler)
{
	struct wim_event_work *w;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w) {
		ERR_WLAN("Failed to alloc memory in %s", __FUNCTION__);
		return;
	}
	/* free w in the handler function */

	w->nw = nw;
	w->vif = vif;
	w->data = data;
	INIT_WORK(&w->work, handler);

	queue_work(nw->hdev->event_workqueue, &w->work);
}

/**
 * nrc_wlan_handle_wim_event - Handle WIM event forwarded from HAL
 * @hal_event: HAL event data containing WIM event
 *
 * Process WIM events that require WLAN layer handling. HAL processes local events
 * and forwards events with IEEE80211/MAC layer dependencies to WLAN.
 *
 * Architecture Design:
 * - WIM events often have IEEE80211 dependencies (connection management, scanning, etc.)
 * - HAL handles local event processing, forwards complex events to WLAN
 * - Original nrc_wim_event_handler() logic distributed between HAL and WLAN
 * - SKB is freed here as WLAN is the final destination
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_handle_wim_event(struct nrc_hal_event_data *hal_event)
{
	struct sk_buff *skb = (struct sk_buff *)hal_event->data;
	struct hif *hif;
	struct wim *wim;
	struct nrc *nw;
	struct nrc_hif_device *hdev;
	struct ieee80211_vif *vif = NULL;
	u32 event_type = 0;
	u16 wim_cmd, wim_event;

	if (!skb || skb->len < sizeof(struct hif) + sizeof(struct wim)) {
		ERR_WLAN("Invalid WIM event SKB");
		if (skb) {
			struct nrc *nw = nrc_wlan_get_nw();
			struct nrc_hif_device *hdev = nw ? nw->hdev : NULL;
			NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, true,
					   false);
		}
		return -EINVAL;
	}

	hif = (struct hif *)skb->data;
	skb_pull(skb, sizeof(*hif)); /* Remove HIF header */
	wim = (struct wim *)skb->data;

	/* Save WIM cmd/event to local variables before processing */
	wim_cmd = wim->cmd;
	wim_event = wim->event;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No network device for WIM event processing");
		skb_push(skb, sizeof(*hif)); /* Restore HIF header */
		/* Error drop: no network device, use hif type from restored header */
		NRC_SKB_TRACK_FREE(NULL, skb, hif->type, true, false);
		return -ENODEV;
	}

	hdev = nw->hdev;

	if (hif->vifindex != -1 && hif->vifindex < ARRAY_SIZE(nw->vif))
		vif = nw->vif[hif->vifindex];

	event_type = wim_event;
	DBG_MAC("WLAN: Processing WIM event 0x%x (event type: %d)", event_type,
		hal_event->type);

	switch (event_type) {
	case WIM_EVENT_SCAN_COMPLETED:
		DBG_MAC("wim scan completed event");
		nrc_wim_event_enqueue(nw, vif, NULL,
				      nrc_mac_scan_completed_work_handler);
		break;
	case WIM_EVENT_SCHED_SCAN_COMPLETED:
		DBG_MAC("wim sched scan completed event");
		nrc_wim_event_enqueue(nw, vif, NULL,
				      nrc_mac_sched_scan_results_work_handler);
		break;
	case WIM_EVENT_REQ_DEAUTH:
		DBG_MAC("WLAN: Processing WIM_EVENT_REQ_DEAUTH");
		if (nw->params->power_save >= NRC_PS_DEEPSLEEP_TIM &&
		    nw->vif[0])
			ieee80211_connection_loss(nw->vif[0]);
		break;

	case WIM_EVENT_CSA:
		DBG_MAC("WLAN: Processing WIM_EVENT_CSA");
		if (vif) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
			ieee80211_csa_finish(vif, 0);
#else
			ieee80211_csa_finish(vif);
#endif
		}
		break;

	case WIM_EVENT_CH_SWITCH:
		DBG_MAC("WLAN: Processing WIM_EVENT_CH_SWITCH");
		if (vif) {
#if KERNEL_VERSION(6, 7, 0) <= NRC_TARGET_KERNEL_VERSION
			ieee80211_chswitch_done(vif, true,
						vif->bss_conf.link_id);
#else
			ieee80211_chswitch_done(vif, true);
#endif
		}
		break;

	case WIN_EVENT_CLEAN_TXQ_STA:
		DBG_MAC("WLAN: Processing WIN_EVENT_CLEAN_TXQ_STA");
		{
			struct wim_tlv *tlv = (struct wim_tlv *)wim->payload;
			if (tlv->t == WIM_TLV_MACADDR_PARAM) {
#ifdef CONFIG_USE_TXQ
				nrc_cleanup_txq_by_macaddr(nw, vif, tlv->v);
#endif
			} else {
				DBG_MAC("WLAN: Invalid TLV type for WIN_EVENT_CLEAN_TXQ_STA");
			}
		}
		break;

	default:
		DBG_MAC("WLAN: Unknown WIM event 0x%x forwarded from HAL",
			event_type);
		break;
	}

	skb_push(skb, sizeof(*hif));
	/* Track WIM event free with saved cmd/event from WLAN frontend */
	NRC_SKB_TRACK_WIM_FREE(hdev, skb, wim_cmd, wim_event, true, false);

	return 0;
}

/**
 * nrc_wlan_handle_reg_notifier - Handle regulatory domain notification event
 * @event: HAL event data containing regulatory request information
 *
 * This function handles regulatory domain change notifications that were
 * forwarded from HAL via the callback system instead of direct function calls.
 * This maintains proper architectural separation between HAL and WLAN layers.
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_handle_reg_notifier(struct nrc_hal_event_data *event)
{
#if defined(CONFIG_SUPPORT_BD)
	struct regulatory_request *request;
	struct nrc *nw;

	if (!event || !event->data) {
		ERR_WLAN("Invalid event data for reg notifier");
		return -EINVAL;
	}

	request = (struct regulatory_request *)event->data;
	nw = nrc_hal_core_get_nw();

	if (!nw || !nw->hw || !nw->hw->wiphy) {
		ERR_WLAN("Network device not available for reg notifier");
		return -ENODEV;
	}

	request->alpha2[0] = nw->alpha2[0];
	request->alpha2[1] = nw->alpha2[1];

	DBG_MAC("WLAN: Processing regulatory notifier via callback (alpha2: %c%c)",
		request->alpha2[0], request->alpha2[1]);

	/* Call the WLAN regulatory notifier function */
	nrc_reg_notifier(nw->hw->wiphy, request);

	return 0;
#else
	ERR_WLAN(
		"WLAN: Regulatory notifier not supported (CONFIG_SUPPORT_BD not defined)");
	return 0;
#endif
}

/**
 * nrc_wlan_callback_init - Initialize WLAN callback system
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_callback_init(void)
{
	int ret;

	ret = nrc_hal_register_callback(NRC_FRONTEND_WLAN,
					nrc_wlan_hal_callback_handler);
	if (ret) {
		ERR_WLAN("Failed to register HAL callback: %d", ret);
		return ret;
	}

	return 0;
}

/**
 * nrc_wlan_callback_cleanup - Cleanup WLAN callback system
 */
void nrc_wlan_callback_cleanup(void)
{
	nrc_hal_unregister_callback(NRC_FRONTEND_WLAN);
}

/**
 * nrc_wlan_handle_twt_service - Handle TWT service event
 * @event: HAL event data containing TWT service information
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_handle_twt_service(struct nrc_hal_event_data *event)
{
	struct nrc *nw;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}

	DBG_STATE("TARGET_NOTI_TWT_SERVICE");
	nrc_ps_dyn_start_twt(nw);
	nw->params->twt_service = true;
	sysfs_notify(&THIS_MODULE->mkobj.kobj, NULL, "twt_service");

	return 0;
}

/**
 * nrc_wlan_handle_twt_quiet - Handle TWT quiet event
 * @event: HAL event data containing TWT quiet information
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_handle_twt_quiet(struct nrc_hal_event_data *event)
{
	struct nrc *nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}

	DBG_STATE("TARGET_NOTI_TWT_QUIET");
	nw->params->twt_service = false;
	sysfs_notify(&THIS_MODULE->mkobj.kobj, NULL, "twt_service");

	return 0;
}

/**
 * nrc_wlan_handle_fw_ready_from_wdt - Handle firmware ready from WDT event
 * @event: HAL event data containing FW ready information
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_handle_fw_ready_from_wdt(struct nrc_hal_event_data *event)
{
	struct nrc_hif_device *hdev;
	struct nrc *nw;
	int ret;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	DBG_MAC("WLAN: Processing FW ready from WDT event");

	/*
	 * Note: PS and DRV states already updated by HAL (nrc_ps_handle_fw_ready)
	 * WLAN layer only handles WLAN-specific recovery operations
	 */
	nrc_vcmd_backup_set_wdt_flag(0);
	nrc_vcmd_backup_set_wdt_flag(1);
	ret = nrc_mac_restart(nw);
	if (ret == 1) {
		DBG_STATE("Restart hw because target reset by WDT");
		ieee80211_restart_hw(nw->hw);
	}

#if defined(CONFIG_SUPPORT_BD)
	{
		struct regulatory_request request;
		DBG_BD("Reload board data after wake");
		request.alpha2[0] = nw->alpha2[0];
		request.alpha2[1] = nw->alpha2[1];
		request.initiator = NL80211_REGDOM_SET_BY_DRIVER;
		nrc_reg_notifier(nw->hw->wiphy, &request);
	}
#endif
	nrc_ps_dyn_start(nw);

	return 0;
}
