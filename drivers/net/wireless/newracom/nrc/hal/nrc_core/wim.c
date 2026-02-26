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

/* Linux networking headers */
#include <net/mac80211.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"
#include "nrc-vendor.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-backend-hif-interface.h"

/* Local module headers */
#include "nrc-fw.h"
#include "wim.h"
#include "nrc-debug.h"
#if defined(CONFIG_SUPPORT_BD)
#include "nrc-bd.h"
#endif
#ifdef CONFIG_S1G_CHANNEL
#include "nrc-s1g.h"
#endif
#include "hif.h"
#include "nrc-tx.h"

static void nrc_wim_skb_bind_vif(struct sk_buff *skb, struct ieee80211_vif *vif)
{
	struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);

	txi->control.vif = vif;
}

struct sk_buff *nrc_wim_alloc_skb(u16 cmd, int size)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;
	struct wim *wim;

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return NULL;
	}

	if (cmd >= WIM_CMD_MAX) {
		ERR_WIM("Invalid WIM command: %d", cmd);
		return NULL;
	}

	/* Simple allocation - no tracking needed
	 * SKB lifecycle: alloc -> enqueue -> TX thread free
	 * Enqueue failure is caller's responsibility to handle */
	skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));
	if (!skb) {
		ERR_WIM("Failed to allocate SKB for WIM cmd %d(%s), size=%d",
			cmd, nrc_wim_cmd_str(cmd),
			size + (int)sizeof(struct hif) +
			(int)sizeof(struct wim));
		return NULL;
	}

	/* Initialize SKB control block for driver-allocated SKBs */
	NRC_SKB_CB_INIT(skb);

	/* Track WIM SKB allocation - will be freed by TX thread */
	if (cmd == WIM_CMD_REQ_FW)
		NRC_SKB_TRACK_ALLOC(hdev, skb, HIF_TYPE_ND_WIM, false, false);
	else
		NRC_SKB_TRACK_ALLOC(hdev, skb, HIF_TYPE_WIM, false, false);

	/* Reserve room for HIF header (will be added during enqueue) */
	skb_reserve(skb, sizeof(struct hif));

	/* Put WIM header */
	wim = (struct wim *)skb_put(skb, sizeof(*wim));
	memset(wim, 0, sizeof(*wim));
	wim->cmd = cmd;
	wim->seqno = hdev->wim_seqno++;

	nrc_wim_skb_bind_vif(skb, NULL);

	return skb;
}

struct sk_buff *nrc_wim_alloc_skb_vif(struct ieee80211_vif *vif, u16 cmd,
				      int size)
{
	struct sk_buff *skb;

	skb = nrc_wim_alloc_skb(cmd, size);
	if (!skb)
		return NULL;

	nrc_wim_skb_bind_vif(skb, vif);

	return skb;
}

/**
 * wim_enqueue_to_tx - Enqueue WIM request to TX queue (internal use only)
 * @hdev: HIF device
 * @skb: WIM SKB (without HIF header)
 * @use_mcp_path: true for MCP queue, false for WLAN queue
 *
 * Prepends HIF header to WIM SKB and enqueues to appropriate TX queue.
 *
 * SKB ownership:
 * - On success: SKB ownership transferred to queue, freed by TX thread
 * - On failure: SKB freed immediately, caller notified with error
 *
 * WLAN path: Uses VIF index from mac80211, enqueues to hdev->queue[1]
 * MCP path: Uses VIF 0, enqueues to hdev->mcp_queue[1]
 *
 * Return: 0 on success, negative error code on failure
 */
static int wim_enqueue_to_tx(struct nrc_hif_device *hdev, struct sk_buff *skb,
			     bool use_mcp_path)
{
	struct hif *hif;
	struct wim *wim;
	u16 cmd = 0;
	int len = skb->len;
	int ret = 0;

	/* Extract WIM command for logging */
	if (len >= sizeof(struct wim)) {
		wim = (struct wim *)(skb->data);
		cmd = wim->cmd;
	}

	if (cmd >= WIM_CMD_MAX) {
		ERR_HIF("Invalid cmd %d", cmd);
		nrc_dump_wim(skb);
		/* Note: SKB doesn't have HIF header yet, so can't use nrc_hif_free_skb() */
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false, false);
		return -EINVAL;
	}

	/* Prepend HIF header */
	hif = (struct hif *)skb_push(skb, sizeof(struct hif));
	memset(hif, 0, sizeof(*hif));
	hif->type = HIF_TYPE_WIM;
	hif->subtype = HIF_WIM_SUB_REQUEST;
	hif->len = len;

	/* Validate HIF header after creation */
	VALIDATE_HIF_HEADER(skb, "nrc_xmit_wim_request");

	if (use_mcp_path) {
		/* MCP path: Always use VIF 0 */
		hif->vifindex = 0;
		hif->flags = HIF_HEADER_CALC_CHECKSUM(hif->type, hif->len);
		DBG_HIF("MCP WIM TX: subtype=%d, len=%d", HIF_WIM_SUB_REQUEST,
			len);
	} else {
		/* WLAN path: Get VIF index from mac80211 */
		struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
		struct ieee80211_vif *vif = txi->control.vif;

		if (hdev->nw) {
			if (atomic_read(&hdev->nw->d_deauth.delayed_deauth))
				hif->vifindex = hdev->nw->d_deauth.vif_index;
			else
				hif->vifindex = hw_vifindex(vif);
		} else {
			hif->vifindex = hw_vifindex(vif);
		}

		/* WLAN path validation */
		if (hif->vifindex < 0 || hif->vifindex > NR_NRC_VIF - 1) {
			ERR_HIF("Invalid VIF Index %d, Ignore WIM cmd %d(%s)",
				hif->vifindex, cmd, nrc_wim_cmd_str(cmd));
			nrc_dump_wim(skb);
			/* SKB has HIF header now, use nrc_hif_free_skb() */
			nrc_hif_free_skb(hdev, skb);
			return -EINVAL;
		}
	}

#if defined(CONFIG_NRC_HIF_PRINT_TX_DATA)
	nrc_dump_wim(skb);
	print_hex_dump(KERN_DEBUG,
		       use_mcp_path ? "mcp wim: " : "wim: ", DUMP_PREFIX_NONE,
		       16, 1, skb->data, skb->len, false);
#endif

	/* Check driver state - prevent WIM commands during shutdown/reboot/closing */
	if (NRC_HIF_DRV_STATE(hdev) == NRC_DRV_REBOOT ||
	    NRC_HIF_DRV_STATE(hdev) == NRC_DRV_CLOSING ||
	    NRC_HIF_DRV_STATE(hdev) == NRC_DRV_CLOSED) {
		ERR_HIF("%s: Driver in invalid state %s, ignore WIM cmd %d(%s)",
			use_mcp_path ? "MCP" : "WLAN", NRC_DRV_STATE_STR(hdev),
			cmd, nrc_wim_cmd_str(cmd));
		nrc_dump_wim(skb);
		/* SKB has HIF header now, use nrc_hif_free_skb() */
		nrc_hif_free_skb(hdev, skb);
		return -EBUSY;
	}

	if (!hdev->params->loopback) {
		/* Enqueue SKB to appropriate queue based on path */
		if (use_mcp_path)
			ret = hif_enqueue_mcp_skb(hdev, skb);
		else
			ret = hif_enqueue_skb(hdev, skb);

		/* On enqueue failure, free SKB */
		if (ret < 0) {
			ERR_HIF("%s WIM enqueue failed: cmd=%d(%s), ret=%d, workqueue=%p",
				use_mcp_path ? "MCP" : "WLAN", cmd,
				nrc_wim_cmd_str(cmd), ret,
				use_mcp_path ? hdev->mcp_workqueue :
					       hdev->workqueue);
			nrc_dump_wim(skb);
			/* SKB has HIF header, use nrc_hif_free_skb() */
			nrc_hif_free_skb(hdev, skb);
		}
		/* On success, SKB ownership transferred to queue */
	} else {
		/* Loopback mode: Free SKB immediately */
		nrc_hif_free_skb(hdev, skb);
		ret = 0;
	}

	return ret;
}

static void wim_set_tlv(struct wim_tlv *tlv, u16 t, u16 l)
{
	tlv->t = t;
	tlv->l = l;
}

void *nrc_wim_skb_add_tlv(struct sk_buff *skb, u16 T, u16 L, void *V)
{
	struct wim_tlv *tlv;

	if (L == 0) {
		tlv = (struct wim_tlv *)(skb_put(skb, sizeof(struct wim_tlv)));
		wim_set_tlv(tlv, T, L);
		return (void *)skb->data;
	}

	tlv = (struct wim_tlv *)(skb_put(skb, tlv_len(L)));
	wim_set_tlv(tlv, T, L);

	if (V)
		memcpy(tlv->v, V, L);

	return (void *)tlv->v;
}

/*
 * WLAN-specific WIM functions (nrc_wim_change_sta, nrc_wim_hw_scan,
 * nrc_wim_install_key, nrc_wim_ampdu_action, etc.) have been moved to
 * frontend/nrc_wlan/nrc-wim-wlan.c as nrc_wim_wlan_* functions.
 * Core module only retains infrastructure functions (alloc, request, etc.)
 * and PS/credit management functions.
 */

/* Cipher type conversion - used by hif.c for TX encryption */
enum wim_cipher_type nrc_to_wim_cipher_type(u32 cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return WIM_CIPHER_TYPE_WEP40;
	case WLAN_CIPHER_SUITE_WEP104:
		return WIM_CIPHER_TYPE_WEP104;
	case WLAN_CIPHER_SUITE_TKIP:
		return WIM_CIPHER_TYPE_TKIP;
	case WLAN_CIPHER_SUITE_CCMP:
		return WIM_CIPHER_TYPE_CCMP;
	case WLAN_CIPHER_SUITE_AES_CMAC:
	case WLAN_CIPHER_SUITE_BIP_GMAC_128:
	case WLAN_CIPHER_SUITE_BIP_GMAC_256:
		return WIM_CIPHER_TYPE_NONE;
	default:
		return WIM_CIPHER_TYPE_INVALID;
	}
}

/* HIF reset functions - used by SPI callback for error recovery */
bool nrc_wim_reset_hif_tx(struct nrc_hif_device *hdev)
{
	struct sk_buff *skb, *resp_skb = NULL;
	struct wim *wim;
	int ret;

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return false;
	}

	skb = nrc_wim_alloc_skb(WIM_CMD_RESET_HIF_TX, 0);
	if (!skb) {
		ERR_HIF("Failed to allocate WIM SKB for RESET_HIF_TX");
		return false;
	}

	ret = nrc_wim_request(skb, 0, WIM_RESP_TIMEOUT, false, &resp_skb);

	if (resp_skb) {
		wim = (struct wim *)resp_skb->data;
		NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, wim->cmd, wim->event,
				       true, false);
	}

	return (ret == 0);
}

bool nrc_wim_reset_hif_rx(struct nrc_hif_device *hdev)
{
	struct sk_buff *skb, *resp_skb = NULL;
	struct wim *wim;
	int ret;

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return false;
	}

	skb = nrc_wim_alloc_skb(WIM_CMD_RESET_HIF_RX, 0);
	if (!skb) {
		ERR_HIF("Failed to allocate WIM SKB for RESET_HIF_RX");
		return false;
	}

	ret = nrc_wim_request(skb, 0, WIM_RESP_TIMEOUT, false, &resp_skb);

	if (resp_skb) {
		wim = (struct wim *)resp_skb->data;
		NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, wim->cmd, wim->event,
				       true, false);
	}

	return (ret == 0);
}

/**
 * nrc_wim_cleanup_pending_responses - Clean up all pending WIM responses
 * @hdev: HIF device
 *
 * Cleans up all pending WIM response SKBs that may be left over after
 * firmware reset (WDT or power state transitions). This prevents memory
 * leaks by freeing any orphaned response SKBs and completing any pending
 * waiters so they can properly timeout.
 *
 * Called during:
 * - FW ready from WDT reset
 * - FW ready from power state transitions
 */
void nrc_wim_cleanup_pending_responses(struct nrc_hif_device *hdev)
{
	int i;

	if (!hdev || !hdev->wim_resp) {
		return;
	}

	for (i = 0; i < WIM_CMD_MAX; i++) {
		NRC_WIM_RESP_LOCK(hdev, i);
		if (hdev->wim_resp[i].skb) {
			struct sk_buff *orphan_skb = hdev->wim_resp[i].skb;
			hdev->wim_resp[i].skb = NULL;
			DBG_WIM("Cleaning up pending WIM response for cmd %d(%s)",
				i, nrc_wim_cmd_str(i));
			/* Track free of WIM response SKB */
			NRC_SKB_TRACK_WIM_FREE(hdev, orphan_skb, i, 0, true,
					       false);
		}
		/* Complete any waiters so they can timeout properly */
		if (!completion_done(&hdev->wim_resp[i].work)) {
			complete(&hdev->wim_resp[i].work);
		}
		NRC_WIM_RESP_UNLOCK(hdev, i);
	}
}

void nrc_wim_handle_fw_ready(struct nrc_hif_device *hdev)
{
#if defined(CONFIG_SUPPORT_BD)
	struct regulatory_request request;
	struct nrc_hal_event_data hal_event = {
		.frontend_type = NRC_FRONTEND_WLAN,
		.type = NRC_HAL_EVT_REG_NOTIFIER,
		.data = &request,
		.data_len = sizeof(request),
	};
#endif

	/* Clean up all pending WIM responses from before FW reset */
	nrc_wim_cleanup_pending_responses(hdev);

	nrc_hif_ops_rx_thread_resume();

#if defined(CONFIG_SUPPORT_BD)
	nrc_hal_trigger_event(&hal_event);
#endif
}

#define MAC_ADDR_LEN 6
void nrc_wim_handle_fw_reload(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (!hdev) {
		ERR_WIM("Invalid HIF device or ops");
		return;
	}

	DBG_PS("[%s,L%d] WDT recovery: Reloading firmware", __func__, __LINE__);

	/* Cleanup TX queues before FW reload */
	nrc_tx_cleanup_queues();

	/* nrc_fw_reload handles FW loading for PS wake scenarios */
	if (nrc_fw_reload(hdev) != 0) {
		ERR_WIM("Failed to reload firmware from host");
	}
}

int nrc_wim_response_init(struct nrc_hif_device *hdev)
{
	int i;

	if (hdev->wim_resp) {
		ERR_WIM("WIM response already exists, cleaning up first");
		nrc_wim_response_deinit(hdev);
	}

	hdev->wim_resp =
		kzalloc(sizeof(struct wim_response) * WIM_CMD_MAX, GFP_KERNEL);
	if (!hdev->wim_resp) {
		ERR_WIM("Failed to allocate memory for WIM response");
		return -ENOMEM;
	}

	for (i = 0; i < WIM_CMD_MAX; i++) {
		init_completion(&hdev->wim_resp[i].work);
		mutex_init(&hdev->wim_resp[i].lock);
		hdev->wim_resp[i].skb = NULL;
	}

	return 0;
}

int nrc_wim_response_deinit(struct nrc_hif_device *hdev)
{
	int i;

	if (!hdev) {
		ERR_WIM("Invalid HIF device or ops");
		return -EINVAL;
	}

	if (hdev->wim_resp) {
		/* Complete any pending completions to unblock waiters */
		for (i = 0; i < WIM_CMD_MAX; i++) {
			if (!completion_done(&hdev->wim_resp[i].work)) {
				complete(&hdev->wim_resp[i].work);
			}
			if (hdev->wim_resp[i].skb) {
				/* Track free of cloned WIM response SKB during cleanup */
				NRC_SKB_TRACK_FREE(hdev, hdev->wim_resp[i].skb,
						   HIF_TYPE_WIM, true, false);
				hdev->wim_resp[i].skb = NULL;
			}
		}
		kfree(hdev->wim_resp);
		hdev->wim_resp = NULL;
	}

	return 0;
}

int nrc_wim_response_handler(struct sk_buff *skb)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct wim *wim;

	if (!hdev || !skb) {
		ERR_WIM("Invalid parameters for WIM response handler");
		return -EINVAL;
	}

	wim = (struct wim *)skb->data;

	DBG_WIM("Processing response cmd %d(%s)", wim->cmd,
		nrc_wim_cmd_str(wim->cmd));

	/* Validate WIM command range */
	if (wim->cmd >= WIM_CMD_MAX) {
		ERR_WIM("Invalid WIM command %d >= %d", wim->cmd, WIM_CMD_MAX);
		return -EINVAL;
	}

	NRC_WIM_RESP_LOCK(hdev, wim->cmd);

	/* Check if completion is still pending */
	if (completion_done(&hdev->wim_resp[wim->cmd].work)) {
		/* No completion waiters - this happens when:
		 * 1. Request sent with nrc_xmit_wim_request() (fire-and-forget)
		 * 2. Timeout occurred and waiter already gave up
		 * 3. Duplicate response received
		 *
		 * SKB will be freed by caller (nrc_hal_process_wim_callback).
		 */
		DBG_WIM("[RX] No completion waiter for wim[%d] (%s), SKB will be freed by caller",
			wim->cmd, nrc_wim_cmd_str(wim->cmd));
		NRC_WIM_RESP_UNLOCK(hdev, wim->cmd);
		return 0; /* Not stored, caller will free */
	}

	/* Free any existing response SKB before storing new one */
	if (hdev->wim_resp[wim->cmd].skb) {
		ERR_WIM("Overwriting existing WIM response SKB for cmd[%d](%s)",
			wim->cmd, nrc_wim_cmd_str(wim->cmd));
		/* Track free of previously stored WIM response SKB */
		NRC_SKB_TRACK_WIM_FREE(hdev, hdev->wim_resp[wim->cmd].skb,
				       wim->cmd, wim->event, true, false);
	}

	/* Store original SKB directly - no clone needed */
	hdev->wim_resp[wim->cmd].skb = skb;
	NRC_WIM_RESP_UNLOCK(hdev, wim->cmd);
	complete(&hdev->wim_resp[wim->cmd].work);
	return 1; /* SKB stored, caller should NOT free */
}

#ifdef CONFIG_USE_TXQ
int nrc_wim_update_tx_credit(struct nrc_hif_device *hdev, struct wim *wim)
{
	struct wim_credit_report *r = (void *)(wim + 1);
	int ac;
	bool changed = false;
	bool credit_increased = false;
	static int prev_credit[IEEE80211_NUM_ACS * 3] = {0};
	/* Trigger TX queue kick event to WLAN layer */
	struct nrc_hal_event_data event = {
		.type = NRC_HAL_EVT_KICK_TXQ,
	};

	/* Check if any credit value has changed or increased */
	for (ac = 0; ac < (IEEE80211_NUM_ACS * 3); ac++) {
		if (prev_credit[ac] != r->v.ac[ac]) {
			changed = true;
			/* Credit increased means FW processed TX, so host can send more */
			if (r->v.ac[ac] > prev_credit[ac])
				credit_increased = true;
		}
	}

	/* Update credits */
	for (ac = 0; ac < (IEEE80211_NUM_ACS * 3); ac++) {
		atomic_set(&hdev->credit.tx_credit[ac], r->v.ac[ac]);
		prev_credit[ac] = r->v.ac[ac];
	}

	/* Log only when credit values actually change */
	if (changed) {
		DBG_CREDIT(
			"ac[0-3]=%d,%d,%d,%d ac[4-7]=%d,%d,%d,%d ac[8-11]=%d,%d,%d,%d%s",
			(int)atomic_read(&hdev->credit.tx_credit[0]),
			(int)atomic_read(&hdev->credit.tx_credit[1]),
			(int)atomic_read(&hdev->credit.tx_credit[2]),
			(int)atomic_read(&hdev->credit.tx_credit[3]),
			(int)atomic_read(&hdev->credit.tx_credit[4]),
			(int)atomic_read(&hdev->credit.tx_credit[5]),
			(int)atomic_read(&hdev->credit.tx_credit[6]),
			(int)atomic_read(&hdev->credit.tx_credit[7]),
			(int)atomic_read(&hdev->credit.tx_credit[8]),
			(int)atomic_read(&hdev->credit.tx_credit[9]),
			(int)atomic_read(&hdev->credit.tx_credit[10]),
			(int)atomic_read(&hdev->credit.tx_credit[11]),
			credit_increased ? " [+]" : "");
	}

	/* Trigger TXQ kick only when credit increased (FW ready for more TX) */
	if (credit_increased)
		nrc_hal_trigger_event(&event);

	return 0;
}
#endif

int nrc_wim_set_ps(struct nrc_hif_device *hdev, enum NRC_PS_MODE mode,
		   u64 timeout, struct cfg80211_wowlan *wowlan)
{
	struct sk_buff *skb;
	struct wim_pm_param *p;
	int i;

	skb = nrc_wim_alloc_skb(WIM_CMD_SET,
				tlv_len(sizeof(struct wim_pm_param)));

	p = nrc_wim_skb_add_tlv(skb, WIM_TLV_PS_ENABLE,
				sizeof(struct wim_pm_param), NULL);
	*(struct wim_pm_param *)p = (struct wim_pm_param){0};

	p->ps_mode = mode;
	p->ps_enable = 1;
	p->ps_timeout = 0; /* deprecated modem sleep config */
	p->ps_wakeup_pin = NRC_PARAM_POWER_SAVE_GPIO(hdev, 1);
	p->ps_wakeup_high = NRC_PARAM_POWER_SAVE_GPIO(hdev, 2);
	p->ps_duration = timeout;

	DBG_PS("WIM PS config: mode=%d(%s) enable=%d duration=%llu pin=%d active_high=%d",
	       p->ps_mode, nrc_ps_mode_str(mode), p->ps_enable, p->ps_duration,
	       p->ps_wakeup_pin, p->ps_wakeup_high);

	if (wowlan) {
		p->wowlan_wakeup_host_pin = TARGET_GPIO_FOR_WAKEUP_HOST;
		p->wowlan_enable_any = wowlan->any;
		p->wowlan_enable_magicpacket = wowlan->magic_pkt;
		p->wowlan_enable_disconnect = wowlan->disconnect;
		p->wowlan_n_patterns = wowlan->n_patterns;
		for (i = 0; i < wowlan->n_patterns; i++) {
			p->wp[i].offset = (u8)wowlan->patterns[i].pkt_offset;
			p->wp[i].pattern_len =
				(u8)wowlan->patterns[i].pattern_len;
			p->wp[i].mask_len =
				DIV_ROUND_UP(p->wp[i].pattern_len, 8);
			memcpy(p->wp[i].pattern, wowlan->patterns[i].pattern,
			       p->wp[i].pattern_len);
			memcpy(p->wp[i].mask, wowlan->patterns[i].mask,
			       p->wp[i].mask_len);
		}
	}

	return nrc_wim_request(skb, 0, 0, false, NULL);
}

#define NUM_WIM_SEND 5
#define NUM_PS_CHECK 10
#define NUM_PS_WAIT 10 /* ms */

int nrc_wim_set_ps_sync(struct nrc_hif_device *hdev, enum NRC_PS_MODE mode,
			u64 timeout, struct cfg80211_wowlan *wowlan)
{
	int ret = -1;
	int done_ps;
	int wim_ret;
	int i, j;

	for (i = 0; i < NUM_WIM_SEND; i++) {
		wim_ret = nrc_wim_set_ps(hdev, mode, timeout, wowlan);
		if (wim_ret != 0) {
			ERR_PS("Failed to send PS WIM (ret=%d, try=%d/%d)",
			       wim_ret, i + 1, NUM_WIM_SEND);
			goto done;
		}
		DBG_PS("Polling sleep status (try %d/%d)...", i + 1, NUM_WIM_SEND);
		for (j = 0; j < NUM_PS_CHECK; j++) {
#ifdef ISSUE /* scheduler stall issue */
			msleep(NUM_PS_WAIT);
			usleep_range(NUM_PS_WAIT * 1000, NUM_PS_WAIT * 2000);
#else
			mdelay(NUM_PS_WAIT);
#endif
			done_ps = nrc_hif_ops_ps_status();
			if (done_ps > 0) { /* wim sucess or halt */
				ret = 0;
				if (done_ps == 4) {
					ERR_PS("FW reset detected during PS operation");
					ret = 1;
				}
				DBG_PS("Sleep confirmed (polled %d times, result=%d)",
				       j + 1, done_ps);
				goto done;
			}
		}
		/* give chance to schedule hif_work */
		usleep_range(NUM_PS_WAIT * 1000, NUM_PS_WAIT * 2000);
	}
done:
	if (ret != 0) {
		ERR_PS("Sleep entry timeout: target not responding (mode=%s, polled %d times)",
		       nrc_ps_mode_str(mode), j + 1);
	}
	return ret;
}

/*
 * nrc_wim_request: - Transmit WIM REQUEST message and optionally wait
 * until WIM RESPONSE message received or timeout reached.
 * @skb: Socket buffer containing WIM data (can be NULL for simple requests)
 * @cmd: WIM command code (used when skb is NULL)
 * @timeout: Timeout in milliseconds (ignored if skb_resp is NULL)
 * @use_mcp_path: true to use MCP TX path, false to use WLAN TX path
 * @skb_resp: Pointer to store response SKB (NULL = no response expected)
 *
 * Return: 0 on success, negative error code on failure
 *         -EINVAL: Invalid HIF device
 *         -ENOMEM: Failed to allocate SKB or clone
 *         -EBUSY: WIM response slot busy
 *         -EIO: TX thread enqueue failed
 *         -ETIMEDOUT: Request timed out
 *
 * MEMORY OWNERSHIP (Clone-based model):
 * 1. Request SKB: ALWAYS freed by this function before returning
 *    - Caller allocates with nrc_wim_alloc_skb()
 *    - This function clones SKB for TX thread
 *    - Original SKB is freed on ALL exit paths (success/error)
 *    - Caller should NOT free the request SKB
 *
 * 2. Cloned SKB: Owned by TX thread
 *    - Created internally with skb_clone()
 *    - Enqueued to TX thread
 *    - TX thread frees after transmission
 *
 * 3. Response SKB: Caller must free if received
 *    - Returned via skb_resp parameter
 *    - Caller must free using NRC_SKB_TRACK_WIM_FREE()
 *
 * Example usage:
 *   struct sk_buff *req, *resp;
 *   req = nrc_wim_alloc_skb(WIM_CMD_GET, tlv_len(0));
 *   nrc_wim_skb_add_tlv(req, WIM_TLV_CHANNEL, ..., false);
 *
 *   ret = nrc_wim_request(req, 0, timeout, false, &resp);
 *   // req is automatically freed by nrc_wim_request - DON'T FREE IT
 *
 *   if (!ret) {
 *       struct wim *wim = (struct wim *)resp->data;
 *       // ... process response ...
 *       NRC_SKB_TRACK_WIM_FREE(hdev, resp, wim->cmd, wim->event, true, false);
 *   }
 */
int nrc_wim_request(struct sk_buff *skb, u16 cmd, int timeout,
		    bool use_mcp_path, struct sk_buff **skb_resp)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *resp;
	struct sk_buff *skb_tx = NULL;
	struct wim *wim;
	bool skb_allocated = false;
	bool no_resp = (skb_resp == NULL);
	int ret;

	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return -EINVAL;
	}

	if (skb_resp)
		*skb_resp = NULL;

	/* If skb is NULL, allocate a simple WIM request */
	if (!skb) {
		skb = nrc_wim_alloc_skb(cmd, 0);
		if (!skb) {
			ERR_HIF("Failed to allocate WIM SKB for cmd %d(%s)",
				cmd, nrc_wim_cmd_str(cmd));
			return -ENOMEM;
		}
		skb_allocated = true;
	} else {
		/* Use cmd from SKB if provided */
		wim = (struct wim *)skb->data;
		cmd = wim->cmd;
	}

	DBG_WIM("%d(%s) request timeout %d ms %s", cmd, nrc_wim_cmd_str(cmd),
		timeout, use_mcp_path ? "MCP" : "WLAN");

	BUG_ON(cmd >= WIM_CMD_MAX);

	if (!hdev->wim_resp) {
		ERR_WIM("wim response is null");
		ret = -EINVAL;
		goto free_skb;
	}

	/*
	 * For fire-and-forget requests (no_resp=true), skip wim_resp access
	 * during cleanup to prevent use-after-free when wim_resp is being freed.
	 */
	if (no_resp) {
		/* Send without waiting for response - skip wim_resp slot management */
		DBG_WIM("Fire-and-forget request for cmd %d(%s), skipping wim_resp access",
			cmd, nrc_wim_cmd_str(cmd));
	} else if (in_atomic()) {
		/* We cannot wait for response in atomic context */
		ERR_WIM("Cannot wait for response in atomic context for cmd %d(%s)",
			cmd, nrc_wim_cmd_str(cmd));
		ret = -EWOULDBLOCK;
		goto free_skb;
	} else {
		NRC_WIM_RESP_LOCK(hdev, cmd);

		if (hdev->wim_resp[cmd].skb != NULL) {
			ERR_WIM("WIM response SKB slot busy for cmd %d(%s), cleaning up previous response",
				cmd, nrc_wim_cmd_str(cmd));
			if (completion_done(&hdev->wim_resp[cmd].work)) {
				/* Safely clear the SKB pointer to prevent double-free */
				struct sk_buff *old_skb =
					hdev->wim_resp[cmd].skb;
				hdev->wim_resp[cmd].skb = NULL;
				NRC_SKB_TRACK_WIM_FREE(hdev, old_skb, cmd, 0,
						       true, false);
			} else {
				NRC_WIM_RESP_UNLOCK(hdev, cmd);
				ret = -EBUSY;
				goto free_skb;
			}
		}
	}

	/*
	 * Clone SKB for TX thread
	 * - Original SKB: owned by caller, freed by this function
	 * - Cloned SKB: owned by TX thread, freed after transmission
	 * This ensures clear ownership and prevents use-after-free
	 */
	skb_tx = skb_clone(skb, in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
	if (!skb_tx) {
		ERR_WIM("Failed to clone SKB for cmd %d(%s)", cmd,
			nrc_wim_cmd_str(cmd));
		NRC_WIM_RESP_UNLOCK(hdev, cmd);
		ret = -ENOMEM;
		goto free_skb;
	}

	/* Track cloned SKB allocation - will be freed by TX thread */
	NRC_SKB_TRACK_ALLOC(hdev, skb_tx, HIF_TYPE_WIM, false, false);

	if (!!wim_enqueue_to_tx(hdev, skb_tx, use_mcp_path)) {
		/* Enqueue failed - will free skb_tx at free_skb_tx label */
		if (!no_resp) {
			NRC_WIM_RESP_UNLOCK(hdev, cmd);
		}
		ret = -EIO;
		goto free_skb_tx;
	}

	/* Enqueue success - TX thread now owns skb_tx, set to NULL */
	skb_tx = NULL;

	/* If no response expected, return immediately after sending */
	if (no_resp) {
		ret = 0;
		goto free_skb;
	}

	reinit_completion(&hdev->wim_resp[cmd].work);
	NRC_WIM_RESP_UNLOCK(hdev, cmd);

	if (wait_for_completion_timeout(&hdev->wim_resp[cmd].work, timeout) ==
	    0) {
		ERR_WIM("Timeout cmd %d(%s)", cmd, nrc_wim_cmd_str(cmd));

		/* Clean up any response that arrived after timeout */
		NRC_WIM_RESP_LOCK(hdev, cmd);
		if (hdev->wim_resp[cmd].skb) {
			struct sk_buff *orphan_skb = hdev->wim_resp[cmd].skb;
			hdev->wim_resp[cmd].skb = NULL;
			ERR_WIM("Cleaning up orphaned response SKB for cmd %d(%s)",
				cmd, nrc_wim_cmd_str(cmd));
			/* Track free of WIM response SKB */
			NRC_SKB_TRACK_WIM_FREE(hdev, orphan_skb, cmd, 0, true,
					       false);
		}
		NRC_WIM_RESP_UNLOCK(hdev, cmd);

		ret = -ETIMEDOUT;
		goto free_skb;
	}

	NRC_WIM_RESP_LOCK(hdev, cmd);

	/* Safely extract and clear the SKB pointer to prevent races */
	resp = hdev->wim_resp[cmd].skb;
	hdev->wim_resp[cmd].skb = NULL;

	if (!resp) {
		ERR_WIM("WIM response SKB is NULL for cmd %d(%s)", cmd,
			nrc_wim_cmd_str(cmd));
		NRC_WIM_RESP_UNLOCK(hdev, cmd);
		ret = -EINVAL;
		goto free_skb;
	}

	/* Validate SKB data pointer before dereferencing */
	if (!resp->data || resp->len < sizeof(struct wim)) {
		ERR_WIM("Request: Invalid SKB data for cmd %d(%s)", cmd,
			nrc_wim_cmd_str(cmd));
		/* Track free of cloned WIM response SKB with known cmd */
		NRC_SKB_TRACK_WIM_FREE(hdev, resp, cmd, 0, true, false);
		NRC_WIM_RESP_UNLOCK(hdev, cmd);
		ret = -EINVAL;
		goto free_skb;
	}

	wim = (struct wim *)resp->data;

	/* Verify WIM structure integrity */
	if ((unsigned long)wim < PAGE_SIZE || !virt_addr_valid(wim)) {
		ERR_WIM("Invalid wim pointer %p for cmd=0x%x", wim, cmd);
		/* Track free of cloned WIM response SKB with known cmd */
		NRC_SKB_TRACK_WIM_FREE(hdev, resp, cmd, 0, true, false);
		NRC_WIM_RESP_UNLOCK(hdev, cmd);
		ret = -EINVAL;
		goto free_skb;
	}

	if (wim->cmd != cmd) {
		ERR_WIM("Cmd mismatch - expected 0x%x, got 0x%x, skb=%p, data=%p",
			cmd, wim->cmd, resp, resp->data);
		print_hex_dump(KERN_DEBUG, "wim_resp: ", DUMP_PREFIX_OFFSET, 16,
			       1, resp->data, min(16, (int)resp->len), true);
		/* Track free of cloned WIM response SKB with parsed WIM data */
		NRC_SKB_TRACK_WIM_FREE(hdev, resp, wim->cmd, wim->event, true,
				       false);
		NRC_WIM_RESP_UNLOCK(hdev, cmd);
		ret = -EINVAL;
		goto free_skb;
	}

	NRC_WIM_RESP_UNLOCK(hdev, cmd);

	/* Free original SKB - caller gets ownership of response SKB */
	NRC_SKB_TRACK_WIM_FREE(hdev, skb, cmd, 0, false, false);

	*skb_resp = resp;
	return 0;

free_skb_tx:
	/* Free cloned SKB if enqueue failed */
	if (skb_tx)
		NRC_SKB_TRACK_WIM_FREE(hdev, skb_tx, cmd, 0, false, false);

free_skb:
	/* Free original SKB on all exit paths */
	NRC_SKB_TRACK_WIM_FREE(hdev, skb, cmd, 0, false, false);
	return ret;
}
