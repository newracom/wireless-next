// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Callback Implementation
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
static int nrc_wlan_handle_wdt_expired(struct nrc_hal_event_data *event);
static int nrc_wlan_handle_ps_enter_failed(struct nrc_hal_event_data *event);
static int nrc_wlan_handle_twt_service(struct nrc_hal_event_data *event);
static int nrc_wlan_handle_twt_quiet(struct nrc_hal_event_data *event);
static int
nrc_wlan_handle_ps_dyn_start_custom_timeout(struct nrc_hal_event_data *event);
#ifdef CONFIG_SUPPORT_RECOVERY
static int nrc_wlan_handle_recovery_trigger(struct nrc_hal_event_data *event);
#endif

/**
 * nrc_wlan_handle_spi_irq - Handle SPI interrupt event
 * @event: HAL event data containing SPI interrupt information
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_handle_spi_irq(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR("Invalid event data for SPI IRQ");
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
		ERR("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	if (!event || !event->data) {
		ERR("Invalid event data for RX ready");
		return -EINVAL;
	}

	skb = (struct sk_buff *)event->data;
	if (!skb || skb->len < sizeof(struct hif)) {
		ERR("Invalid SKB in RX ready event");
		return -EINVAL;
	}

	hif = (struct hif *)skb->data;

	DBG_RX("WLAN HIF type=%s(%u) subtype=%s(%u) len=%u skb_len=%u",
	       nrc_hif_type_str(hif->type), hif->type,
	       nrc_hif_subtype_str(hif->type, hif->subtype), hif->subtype,
	       hif->len, skb->len);

	WARN_ON(skb->len != hif->len + sizeof(*hif));

	if (NRC_HIF_DRV_STATE(hdev) < NRC_DRV_START) {
		ERR("WLAN RX: Driver not ready (state=%d), dropping packet\n",
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
		ERR("WLAN: Unknown HIF packet type %u forwarded from HAL\n",
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
		ERR("Invalid event data for TX complete");
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
		ERR("Invalid event data for error");
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
	int i;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	if (!event) {
		ERR("Invalid event for connection loss");
		return -EINVAL;
	}

	/* Handle W_DISABLE_ASSERTED: signal connection loss on all active VIFs */
	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		if (nw->vif[i])
			ieee80211_connection_loss(nw->vif[i]);
	}
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
		ERR("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	/* Wake IEEE80211 queues */
	ieee80211_wake_queues(nw->hw);

	/* Kick TXQ to process pending TX frames */
	nrc_kick_txq(nw);

	/* Re-send country code / board data to FW after wakeup */
	nrc_restore_reg_domain(nw);

	/* Restart dynamic PS timer (function checks supports_dynamic_ps internally) */
	nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_TARGET_FW_READY);

	if (!ieee80211_hw_check(nw->hw, SUPPORTS_PS)) {
		/* PS not supported - handle beacon loss */
		if (hdev->params->power_save >= NRC_PS_DEEPSLEEP_NONTIM) {
			nrc_send_beacon_loss(nw);
		} else
			nw->invoke_beacon_loss = true;
	}

	if (!hdev->params->disable_cqm) {
		int _i;

		for (_i = 0; _i < NR_NRC_VIF; _i++) {
			struct nrc_vif *_iv;

			if (!nw->vif[_i] ||
			    nw->vif[_i]->type != NL80211_IFTYPE_STATION)
				continue;
			_iv = to_i_vif(nw->vif[_i]);
			if (_iv->associated)
				mod_timer(&_iv->bcn_mon_timer,
					  jiffies +
						  msecs_to_jiffies(
							  _iv->beacon_timeout));
		}
	}

	VBS_PS("WLAN: Wake done processing complete");

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
		ERR("No nw available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	DBG_PS("WLAN: Processing PS enter failed event");

	/* Need to check if AP is alive, increase timeout more than beacon_timeout
	 * 2000msec is enough time to check with probe req/resp
	 */
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

		/* Restart beacon monitoring */
		if (!hdev->params->disable_cqm) {
			for (_i = 0; _i < NR_NRC_VIF; _i++) {
				struct nrc_vif *_iv;

				if (!nw->vif[_i] ||
				    nw->vif[_i]->type != NL80211_IFTYPE_STATION)
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
		ERR("No nw available");
		return -EINVAL;
	}

	if (!event) {
		ERR("Invalid event for PS dynamic start with custom timeout");
		return -EINVAL;
	}

	/* Extract custom timeout from event data if available */
	if (event->data && event->data_len == sizeof(u32)) {
		custom_timeout = *(u32 *)event->data;
	}

	/* Start dynamic power save with custom timeout */
	nrc_ps_dyn_start(nw, custom_timeout, NRC_PS_REASON_HAL_PS_DYNAMIC);

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
		ERR("No nw available");
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
		ERR("No nw available");
		return -EINVAL;
	}

	if (!nw->params->disable_cqm) {
		int _i;

		for (_i = 0; _i < NR_NRC_VIF; _i++) {
			if (nw->vif[_i] &&
			    nw->vif[_i]->type == NL80211_IFTYPE_STATION)
				try_to_del_timer_sync(
					&to_i_vif(nw->vif[_i])->bcn_mon_timer);
		}
	}

	ieee80211_stop_queues(nw->hw);

	/* Call the cleanup all TX queues function */
	nrc_cleanup_txq_all(nw);

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
	struct nrc *nw;
	struct nrc_hif_device *hdev;

	if (!nrc_wlan_is_initialized()) {
		ERR("WLAN not initialized");
		return -EINVAL;
	}

	nw = nrc_wlan_get_nw();
	if (!nw || !nw->hdev) {
		ERR("No nw or hdev available");
		return -EINVAL;
	}
	hdev = nw->hdev;

	if (!event || !event->data) {
		ERR("Invalid event data");
		return -EINVAL;
	}

	skb = (struct sk_buff *)event->data;
	if (!skb || skb->len < sizeof(struct hif)) {
		ERR("Invalid SKB in event");
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
		ERR("Invalid HAL event data");
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
	case NRC_HAL_EVT_TARGET_NOTI_WDT_EXPIRED:
		ret = nrc_wlan_handle_wdt_expired(hal_event);
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
#ifdef CONFIG_SUPPORT_RECOVERY
	case NRC_HAL_EVT_RECOVERY_TRIGGER:
		ret = nrc_wlan_handle_recovery_trigger(hal_event);
		break;
#endif
	default:
		ERR("Unknown HAL event type %d", hal_event->type);
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
		ERR("Failed to alloc memory in %s", __FUNCTION__);
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
		ERR("Invalid WIM event SKB");
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
		ERR("No network device for WIM event processing");
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
		if (nw->params->power_save >= NRC_PS_DEEPSLEEP_TIM) {
			int i;

			for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
				if (nw->vif[i] &&
				    nw->vif[i]->type == NL80211_IFTYPE_STATION)
					ieee80211_connection_loss(nw->vif[i]);
			}
		}
		break;

	case WIM_EVENT_CSA:
		DBG_MAC("WLAN: Processing WIM_EVENT_CSA");
		if (vif) {
			ieee80211_csa_finish(vif);
		}
		break;

	case WIM_EVENT_CH_SWITCH:
		DBG_MAC("WLAN: Processing WIM_EVENT_CH_SWITCH");
		if (vif) {
			ieee80211_chswitch_done(vif, true,
						vif->bss_conf.link_id);
		}
		break;

	case WIN_EVENT_CLEAN_TXQ_STA:
		DBG_MAC("WLAN: Processing WIN_EVENT_CLEAN_TXQ_STA");
		{
			struct wim_tlv *tlv = (struct wim_tlv *)wim->payload;
			if (tlv->t == WIM_TLV_MACADDR_PARAM) {
				nrc_cleanup_txq_by_macaddr(nw, vif, tlv->v);
			} else {
				DBG_MAC("WLAN: Invalid TLV type for WIN_EVENT_CLEAN_TXQ_STA");
			}
		}
		break;

	case WIM_EVENT_REQ_DEAUTH_BY_FORCE:
		/* FW detected abnormal TSF and requests forced disconnection */
		DBG_MAC("WLAN: Processing WIM_EVENT_REQ_DEAUTH_BY_FORCE");
		{
			int i;

			for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
				if (nw->vif[i])
					ieee80211_connection_loss(nw->vif[i]);
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
	struct nrc *nw = nrc_hal_core_get_nw();

	if (!nw || !nw->hw) {
		ERR("Network device not available for reg notifier");
		return -ENODEV;
	}

	nrc_restore_reg_domain(nw);
	return 0;
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
		ERR("Failed to register HAL callback: %d", ret);
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
		ERR("No nw available");
		return -EINVAL;
	}

	DBG_STATE("TARGET_NOTI_TWT_SERVICE");
	nw->params->twt_service = true;
	sysfs_notify(&THIS_MODULE->mkobj.kobj, NULL, "twt_service");

	/*
	 * TWT service period = device is AWAKE and exchanging frames.
	 * Wake the PS state machine so any pending TX can proceed.
	 * Do NOT arm the sleep timer here — sleeping during the service
	 * period would conflict with the firmware's TWT schedule and cause
	 * repeated DEEPSLEEP_TIM timeouts.
	 *
	 * Sleep (if twt_force_sleep is set) is triggered from the QUIET
	 * handler below, once the service period ends.
	 */
	nrc_ps_dyn_stop(nw, NRC_PS_REASON_TARGET_TWT_SERVICE);

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
		ERR("No nw available");
		return -EINVAL;
	}

	DBG_STATE("TARGET_NOTI_TWT_QUIET");
	nw->params->twt_service = false;
	sysfs_notify(&THIS_MODULE->mkobj.kobj, NULL, "twt_service");

	/*
	 * TWT quiet period = device should sleep until next service period.
	 * Arm the PS timer so the driver enters sleep aligned with the
	 * FW TWT schedule (only when twt_force_sleep is enabled).
	 */
	if (nw->twt_sched && nw->params->twt_force_sleep)
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_TARGET_TWT_QUIET);

	return 0;
}

/**
 * nrc_wlan_handle_wdt_expired - Handle firmware watchdog timer expired event
 * @event: HAL event data
 *
 * Called by HAL when the firmware WDT fires. The firmware is rebooting;
 * a subsequent NRC_HAL_EVT_TARGET_NOTI_FW_READY_FROM_WDT event will arrive
 * once it comes back up.
 *
 * In STA mode, notify mac80211 of connection loss so it can start
 * reassociation. In AP mode, just log — the FW_READY_FROM_WDT handler
 * will restart the hardware and restore AP operation.
 *
 * Returns: 0 (always — WDT expiry is an expected recovery path, not an error)
 */
static int nrc_wlan_handle_wdt_expired(struct nrc_hal_event_data *event)
{
	struct nrc *nw;
	int i;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		WARN_MAC("WDT expired: nw not available yet");
		return 0;
	}

	WARN_MAC("FW WDT expired - waiting for FW_READY_FROM_WDT");

	/*
	 * Stop all mac80211 TX queues before notifying connection loss.
	 *
	 * Without this, a race exists between the WDT recovery path and
	 * mac80211's beacon_loss workqueue: ieee80211_connection_loss()
	 * schedules ieee80211_beacon_connection_loss_work(), which calls
	 * __ieee80211_disconnect() -> nl80211_send_disconnected() ->
	 * skb_clone() -> kmem_cache_alloc(). If the slab cache is already
	 * in a torn-down state (driver REBOOT), this causes a NULL pointer
	 * dereference. This is especially likely in ap+sta concurrent mode
	 * where AP and STA VIFs share hardware state.
	 *
	 * Stopping TX queues serialises mac80211 activity before we trigger
	 * the disconnect notification. Queues are re-enabled in
	 * nrc_wlan_handle_fw_ready_from_wdt() after full recovery.
	 */
	ieee80211_stop_queues(nw->hw);

	/* Notify connection loss for all STA VIFs (covers multi-VIF concurrent mode) */
	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		if (nw->vif[i] && nw->vif[i]->type == NL80211_IFTYPE_STATION) {
			DBG_MAC("WDT: notifying mac80211 of connection loss (vif[%d])",
				i);
			ieee80211_connection_loss(nw->vif[i]);
		}
	}

	return 0;
}

/**
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
		ERR("No nw available");
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

	/*
	 * FW has rebooted — BD is no longer loaded.  Invalidate the BD gate
	 * so that nrc_mac_start(), nrc_mac_add_interface(), and start_ap()
	 * block any WLAN operation until nrc_restore_reg_domain() re-sends
	 * the BD to the freshly booted FW.
	 *
	 * nrc_restore_reg_domain() → nrc_hal_ops_wim_request() is synchronous
	 * (blocks until FW ACKs the WIM).  By the time ieee80211_restart_hw()
	 * schedules its reconfig work, g_bd_valid is already true and FW has
	 * a valid BD, preventing the "api: invalid bd" FW ASSERT.
	 */
	nrc_mac_bd_invalidate();

	/*
	 * If alpha2 is "99" (driver sentinel for "no CC set yet"), default to
	 * "US" so that nrc_restore_reg_domain() can push a valid BD to FW.
	 * This handles WDT at cold boot before 'iw reg set' is called.
	 */
	if (nw->alpha2[0] == '9' && nw->alpha2[1] == '9') {
		WARN_MAC(
			"wdt_recovery: no valid CC set (alpha2=99); defaulting to US");
		nw->alpha2[0] = 'U';
		nw->alpha2[1] = 'S';
	}
	nrc_restore_reg_domain(nw);

	if (ret == 1) {
		DBG_STATE("Restart hw because target reset by WDT");
		ieee80211_restart_hw(nw->hw);
	}

	/* Re-enable TX queues stopped during WDT expiry handling */
	ieee80211_wake_queues(nw->hw);
	nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_HAL_CALLBACK);

	return 0;
}

#ifdef CONFIG_SUPPORT_RECOVERY
/**
 * nrc_wlan_handle_recovery_trigger - Handle software recovery trigger from HAL
 * @event: HAL event data containing recovery reason string
 *
 * Called when HAL recovery engine detects error threshold exceeded or WDT bark.
 * Sends NL_CMD_RECOVERY netlink notification to user-space.
 * User-space daemon (recoveryd.py) performs the actual rmmod/insmod restart.
 *
 * Module parameter 'recovery' controls behavior:
 *   0 (monitor): Netlink notification only — user-space decides
 *   1 (auto):    Netlink notification — user-space daemon auto-restarts
 */
static int nrc_wlan_handle_recovery_trigger(struct nrc_hal_event_data *event)
{
	struct nrc *nw;
	const char *reason = (const char *)event->data;

	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR("recovery: No nw available");
		return -EINVAL;
	}

	ERR("recovery: triggered (reason=%s, mode=%s)",
	    reason ? reason : "unknown",
	    (nw->params && nw->params->recovery) ? "auto" : "monitor");

	/* Notify user-space via netlink — actual restart happens there */
	nrc_netlink_trigger_recovery(nw);

	return 0;
}
#endif
