// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * WLAN-specific WIM (Wireless Interface Message) Functions
 *
 * This file contains WLAN-specific WIM command builders and handlers.
 * These functions use HAL ops wrapper functions (nrc_hal_ops_wim_alloc_skb_vif,
 * nrc_hal_ops_wim_skb_add_tlv, nrc_hal_ops_wim_request) to construct and send
 * WLAN-specific commands to firmware via the core module.
 */

/* Linux headers */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/etherdevice.h>
#include <net/mac80211.h>

/* Common headers */
#include "nrc-wim-types.h"
#include "nrc-hal-core-interface.h"
#include "nrc-debug-common.h"
#include "nrc-debug.h"
#include "nrc-hif.h"
#include "nrc.h"

/* Local headers */
#include "nrc-wim-wlan.h"
#include "nrc-mac80211.h"
#include "nrc-wlan-params.h"
#include "nrc-s1g.h"
#if defined(CONFIG_SUPPORT_BD)
#endif

/**
 * Rate control mode parameters
 * These are WLAN-specific parameters, not used by core module
 */
uint8_t ap_rc_mode = 0xff;
uint8_t sta_rc_mode = 0xff;
uint8_t ap_rc_default_mcs = 0xff;
uint8_t sta_rc_default_mcs = 0xff;

/* Forward declarations */
static int wim_request_and_extract_return(struct sk_buff *skb, int timeout);

/*
 * ===========================================================================
 * Internal TLV helper functions
 * ===========================================================================
 */

static void nrc_wim_set_twt_responder(struct sk_buff *skb, u8 enable)
{
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_TWT_RESPONDER, sizeof(u8),
				    &enable);
}

static void nrc_wim_set_twt_requester(struct sk_buff *skb, u8 enable)
{
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_TWT_REQUESTER, sizeof(u8),
				    &enable);
}

static void nrc_wim_set_rc_mode(struct sk_buff *skb, u8 mode)
{
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_RC_MODE, sizeof(u8), &mode);
}

static void nrc_wim_set_default_mcs(struct sk_buff *skb, u8 mcs)
{
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_DEFAULT_MCS, sizeof(u8), &mcs);
}

static void nrc_wim_wlan_set_ndp_preq(struct sk_buff *skb, u8 enable)
{
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_NDP_PREQ, sizeof(u8), &enable);
}

static void nrc_wim_wlan_add_mac_addr(struct sk_buff *skb, u8 *addr)
{
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_MACADDR, ETH_ALEN, addr);
}

/*
 * ===========================================================================
 * Station Management
 * ===========================================================================
 */

int nrc_wim_wlan_change_sta(struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta, u8 cmd, bool sleep)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;
	struct wim_sta_param *p;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	if (!sta) {
		ERR_WLAN("Invalid sta");
		return -EINVAL;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_STA_CMD,
					    tlv_len(sizeof(*p)));
	if (!skb)
		return -ENOMEM;
	p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_STA_PARAM, sizeof(*p),
					NULL);
	*p = (struct wim_sta_param){0};

	p->cmd = cmd;
	p->flags = 0;
	p->sleep = sleep;
	ether_addr_copy(p->addr, sta->addr);
	p->aid = sta->aid;

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

static int to_wim_sta_type(enum nl80211_iftype type)
{
	int sta_type;

	switch (type) {
	case NL80211_IFTYPE_STATION:
		sta_type = WIM_STA_TYPE_STA;
		break;
	case NL80211_IFTYPE_AP:
		sta_type = WIM_STA_TYPE_AP;
		break;
	case NL80211_IFTYPE_P2P_GO:
		sta_type = WIM_STA_TYPE_P2P_GO;
		break;
	case NL80211_IFTYPE_P2P_CLIENT:
		sta_type = WIM_STA_TYPE_P2P_GC;
		break;
#ifdef CONFIG_SUPPORT_P2P
	case NL80211_IFTYPE_P2P_DEVICE:
		sta_type = WIM_STA_TYPE_P2P_DEVICE;
		break;
#endif
	case NL80211_IFTYPE_MONITOR:
		sta_type = WIM_STA_TYPE_MONITOR;
		break;
	case NL80211_IFTYPE_MESH_POINT:
		sta_type = WIM_STA_TYPE_MESH_POINT;
		break;
	case NL80211_IFTYPE_UNSPECIFIED:
		sta_type = WIM_STA_TYPE_NONE;
		break;
#ifdef CONFIG_SUPPORT_IBSS
	case NL80211_IFTYPE_ADHOC:
		sta_type = WIM_STA_TYPE_IBSS;
		break;
#endif
	default:
		sta_type = -ENOTSUPP;
		break;
	}

	return sta_type;
}

int nrc_wim_wlan_set_sta_type(struct ieee80211_vif *vif)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;
	int sta_type, skb_len;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	sta_type = to_wim_sta_type(ieee80211_iftype_p2p(vif->type, vif->p2p));
	if (sta_type < 0)
		return -ENOTSUPP;

	skb_len = tlv_len(sizeof(u32));
	if (nrc_mac_is_s1g(hdev)) {
		skb_len += tlv_len(sizeof(u8)); /* 1M CTRL RESP SUPPORT */
		if (sta_type == WIM_STA_TYPE_AP) {
			skb_len += tlv_len(sizeof(u8)); /* NDP PREQ SUPPORT */
		}
		skb_len += tlv_len(sizeof(u8)); /* rc_mode */
		skb_len += tlv_len(sizeof(u8)); /* rc_default_mcs */
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, skb_len);
	if (!skb)
		return -ENOMEM;
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_STA_TYPE, sizeof(u32),
				    &sta_type);
	if (nrc_mac_is_s1g(hdev)) {
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_NDP_ACK_1M, sizeof(u8),
					    &hdev->params->ndp_ack_1m);
		if (sta_type == WIM_STA_TYPE_AP) {
			nrc_wim_wlan_set_ndp_preq(skb, true);
			nrc_wim_set_twt_requester(skb, false);
			if (hdev->params->twt_sp + hdev->params->twt_num +
				    hdev->params->twt_int !=
			    0) {
				nrc_wim_set_twt_responder(skb, true);
			} else {
				nrc_wim_set_twt_responder(skb, false);
			}

			if (ap_rc_mode >= 1 && ap_rc_mode <= 3) {
				DBG_MAC("set ap rc_mode to %d", ap_rc_mode);
				nrc_wim_set_rc_mode(skb, ap_rc_mode);
			}

			if ((ap_rc_default_mcs >= 0 &&
			     ap_rc_default_mcs <= 7) ||
			    ap_rc_default_mcs == 10) {
				DBG_MAC("set ap default mcs to %d",
					ap_rc_default_mcs);
				nrc_wim_set_default_mcs(skb, ap_rc_default_mcs);
			}
		} else if (sta_type == WIM_STA_TYPE_STA) {
			nrc_wim_set_twt_responder(skb, false);
			if (hdev->params->twt_sp + hdev->params->twt_num +
				    hdev->params->twt_int !=
			    0) {
				nrc_wim_set_twt_requester(skb, true);
			} else {
				nrc_wim_set_twt_requester(skb, false);
			}

			if (sta_rc_mode >= 1 && sta_rc_mode <= 3) {
				DBG_MAC("set sta rc_mode to %d", sta_rc_mode);
				nrc_wim_set_rc_mode(skb, sta_rc_mode);
			}

			if ((sta_rc_default_mcs >= 0 &&
			     sta_rc_default_mcs <= 7) ||
			    sta_rc_default_mcs == 10) {
				DBG_MAC("set sta default mcs to %d",
					sta_rc_default_mcs);
				nrc_wim_set_default_mcs(skb,
							sta_rc_default_mcs);
			}
		}
	}

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

int nrc_wim_wlan_unset_sta_type(struct ieee80211_vif *vif)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;
	int sta_type;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	sta_type = to_wim_sta_type(NL80211_IFTYPE_UNSPECIFIED);
	if (sta_type < 0)
		return -ENOTSUPP;

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET,
					    tlv_len(sizeof(u32)));
	if (!skb)
		return -ENOMEM;
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_STA_TYPE, sizeof(u32),
				    &sta_type);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

/*
 * ===========================================================================
 * MAC Address Management
 * ===========================================================================
 */

static int nrc_wim_set_sta_mac_addr(struct nrc_hif_device *hdev,
				    struct ieee80211_vif *vif, char *addr,
				    bool enable, bool p2p)
{
	struct sk_buff *skb;
	struct wim_addr_param *p;

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET,
					    tlv_len(ETH_ALEN));
	if (!skb)
		return -ENOMEM;
	p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_MACADDR_PARAM, sizeof(*p),
					NULL);
	p->enable = enable;
	p->p2p = p2p;
	ether_addr_copy(p->addr, addr);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

int nrc_wim_wlan_set_p2p_addr(struct ieee80211_vif *vif)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	return nrc_wim_set_sta_mac_addr(hdev, vif, vif->addr, true, true);
}

int nrc_wim_wlan_set_mac_addr(struct ieee80211_vif *vif)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	return nrc_wim_set_sta_mac_addr(hdev, vif, vif->addr, true, false);
}

/*
 * ===========================================================================
 * Scan Operations
 * ===========================================================================
 */

static struct sk_buff *nrc_wim_alloc_scan_param(struct nrc_hif_device *hdev,
						struct ieee80211_vif *vif,
						u16 cmd, int req_ie_len,
						struct ieee80211_scan_ies *ies,
						int param_size)
{
	struct sk_buff *skb;
	int size = tlv_len(param_size);

	if (ies) {
		size += tlv_len(ies->common_ie_len);
		size += sizeof(struct wim_tlv);
#ifdef CONFIG_S1G_CHANNEL
		size += ies->len[NL80211_BAND_S1GHZ];
#else
		size += ies->len[NL80211_BAND_2GHZ];
		size += ies->len[NL80211_BAND_5GHZ];
#endif /* ifdef CONFIG_S1G_CHANNEL */
	}

	if (req_ie_len) {
		size += tlv_len(req_ie_len);
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, cmd, size);

	return skb;
}

static void nrc_wim_build_scan_param(struct nrc_hif_device *hdev,
				     struct sk_buff *skb,
				     struct cfg80211_scan_request *req,
				     struct ieee80211_scan_ies *ies)
{
	struct wim_scan_param *p;
	int i;
#if defined(CONFIG_SUPPORT_BD)
	int j;
	bool avail_ch_flag = false;
	const struct bd_supp_param *supp_ch_list;
#endif /* defined(CONFIG_SUPPORT_BD) */

	/* WIM_TL_SCAN_PARAM */
	p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SCAN_PARAM, sizeof(*p),
					NULL);
	*p = (struct wim_scan_param){0};

	if (WARN_ON(req->n_channels > WIM_MAX_SCAN_CHANNEL))
		req->n_channels = WIM_MAX_SCAN_CHANNEL;

	p->n_channels = req->n_channels;
	for (i = 0; i < req->n_channels; i++) {
#ifdef CONFIG_S1G_CHANNEL
		p->channel[i] = FREQ_TO_100KHZ(req->channels[i]->center_freq,
					       req->channels[i]->freq_offset);
#else
		p->channel[i] = req->channels[i]->center_freq;
#endif /* ifdef CONFIG_S1G_CHANNEL */
	}

#if defined(CONFIG_SUPPORT_BD)
	supp_ch_list = nrc_s1g_get_supp_ch_list();
	if (supp_ch_list && supp_ch_list->num_ch) {
		for (i = 0; i < req->n_channels; i++) {
			for (j = 0; j < supp_ch_list->num_ch; j++) {
				if (p->channel[i] ==
				    supp_ch_list->nons1g_ch_freq[j])
					avail_ch_flag = true;
			}
			if (!avail_ch_flag) {
				p->n_channels--;
				p->channel[i] = 0;
			} else
				avail_ch_flag = false;
		}

		j = 0;
		for (i = 0; i < req->n_channels; i++) {
			if (p->channel[i]) {
#ifdef CONFIG_S1G_CHANNEL
				p->channel[j] = FREQ_TO_100KHZ(
					req->channels[i]->center_freq,
					req->channels[i]->freq_offset);
#else
				p->channel[j] = req->channels[i]->center_freq;
#endif /* ifdef CONFIG_S1G_CHANNEL */
				j++;
			}
		}
	}
#endif /* defined(CONFIG_SUPPORT_BD) */

	p->n_ssids = req->n_ssids;
	for (i = 0; i < req->n_ssids; i++) {
		p->ssid[i].ssid_len = req->ssids[i].ssid_len;
		memcpy(p->ssid[i].ssid, req->ssids[i].ssid,
		       req->ssids[i].ssid_len);
	}

	if (ies) {
		u8 *ie_p;
		int b;

		ie_p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SCAN_BAND_IE,
#ifdef CONFIG_S1G_CHANNEL
						   ies->len[NL80211_BAND_S1GHZ],
#else
						   ies->len[NL80211_BAND_2GHZ] +
							   ies->len[NL80211_BAND_5GHZ],
#endif /* ifdef CONFIG_S1G_CHANNEL */
						   NULL);

		for (b = NL80211_BAND_2GHZ; b < ARRAY_SIZE(ies->ies); b++) {
			if (ies->ies[b] && ies->len[b] > 0) {
				memcpy(ie_p, ies->ies[b], ies->len[b]);
				ie_p += ies->len[b];
			}
		}

		ie_p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SCAN_COMMON_IE,
						   ies->common_ie_len,
						   (void *)ies->common_ies);
	}

	if (req->ie) {
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SCAN_PROBE_REQ_IE,
					    req->ie_len, (void *)req->ie);
	}
}

static void
nrc_wim_build_sched_scan_param(struct nrc_hif_device *hdev, struct sk_buff *skb,
			       struct cfg80211_sched_scan_request *req)
{
	struct wim_sched_scan_param *p;
	struct cfg80211_match_set *match;
	struct cfg80211_sched_scan_plan *scan_plan;
	int i;

	/* WIM_TL_SCAN_PARAM */
	p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SCHED_SCAN_PARAM,
					sizeof(*p), NULL);
	*p = (struct wim_sched_scan_param){0};

	p->min_rssi_thold = S8_MIN;
	if (req->min_rssi_thold != NL80211_SCAN_RSSI_THOLD_OFF) {
		if (req->min_rssi_thold >= S8_MIN &&
		    req->min_rssi_thold <= S8_MAX) {
			p->min_rssi_thold = req->min_rssi_thold;
		}
	}

	p->delay = req->delay;

	p->n_match_sets = req->n_match_sets;

	for (i = 0; i < req->n_match_sets; i++) {
		match = &req->match_sets[i];
		memcpy(p->match_sets[i].ssid.ssid, match->ssid.ssid,
		       match->ssid.ssid_len);
		p->match_sets[i].ssid.ssid_len = match->ssid.ssid_len;
		p->match_sets[i].rssi_thold = S8_MIN;
		if (match->rssi_thold >= S8_MIN &&
		    match->rssi_thold <= S8_MAX) {
			p->match_sets[i].rssi_thold = match->rssi_thold;
		}
	}

	p->n_scan_plans = req->n_scan_plans;

	for (i = 0; i < req->n_scan_plans; i++) {
		scan_plan = &req->scan_plans[i];
		p->scan_plans[i].interval = scan_plan->interval;
		p->scan_plans[i].iterations = scan_plan->iterations;
	}
}

int nrc_wim_wlan_hw_scan(struct ieee80211_vif *vif,
			 struct cfg80211_scan_request *req,
			 struct ieee80211_scan_ies *ies)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	if (!req) {
		ERR_WLAN("Invalid scan request");
		return -EINVAL;
	}

	skb = nrc_wim_alloc_scan_param(hdev, vif, WIM_CMD_SCAN_START,
				       req->ie_len, ies,
				       sizeof(struct wim_scan_param));

	nrc_wim_build_scan_param(hdev, skb, req, ies);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

int nrc_wim_wlan_sched_scan_start(struct ieee80211_vif *vif,
				  struct cfg80211_sched_scan_request *req,
				  struct ieee80211_scan_ies *ies)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;
	struct cfg80211_scan_request *scan_req;
	int i;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	if (!req) {
		ERR_WLAN("Invalid sched scan request");
		return -EINVAL;
	}

	skb = nrc_wim_alloc_scan_param(
		hdev, vif, WIM_CMD_SCHED_SCAN_START, req->ie_len, ies,
		sizeof(struct wim_sched_scan_param) +
			sizeof(struct wim_scan_param) + sizeof(struct wim_tlv));

	scan_req = kzalloc(sizeof(struct cfg80211_scan_request) +
				   req->n_channels *
					   sizeof(struct ieee80211_channel *),
			   GFP_KERNEL);
	if (!scan_req) {
		ERR_WLAN("Failed to allocate scan_req");
		return -ENOMEM;
	}

	/* mapping cfg80211_sched_scan_request to cfg80211_scan_request */
	scan_req->n_ssids = req->n_ssids;
	scan_req->ssids = req->ssids;

	scan_req->n_channels = req->n_channels;
	for (i = 0; i < req->n_channels; i++) {
		scan_req->channels[i] = req->channels[i];
	}

	scan_req->ie_len = req->ie_len;
	if (req->ie_len > 0 && req->ie) {
		scan_req->ie = kzalloc(req->ie_len, GFP_KERNEL);
		if (!scan_req->ie) {
			ERR_WLAN("Failed to allocate IE buffer");
			kfree(scan_req);
			return -ENOMEM;
		}
		memcpy((u8 *)scan_req->ie, req->ie, req->ie_len);
	}

	nrc_wim_build_scan_param(hdev, skb, scan_req, ies);

	nrc_wim_build_sched_scan_param(hdev, skb, req);

	kfree(scan_req->ie);
	kfree(scan_req);

	return wim_request_and_extract_return(skb, WIM_RESP_TIMEOUT * 20);
}

int nrc_wim_wlan_sched_scan_stop(struct ieee80211_vif *vif)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SCHED_SCAN_STOP, 0);
	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

/*
 * ===========================================================================
 * Key Management
 * ===========================================================================
 */

static const char *ieee80211_cipher_str(u32 cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return "WEP40";
	case WLAN_CIPHER_SUITE_WEP104:
		return "WEP104";
	case WLAN_CIPHER_SUITE_TKIP:
		return "TKIP";
	case WLAN_CIPHER_SUITE_CCMP:
		return "CCMP";
	case WLAN_CIPHER_SUITE_CCMP_256:
		return "CCMP-256";
	case WLAN_CIPHER_SUITE_GCMP:
		return "GCMP";
	case WLAN_CIPHER_SUITE_GCMP_256:
		return "GCMP-256";
	case WLAN_CIPHER_SUITE_AES_CMAC:
		return "BIP-CMAC";
	case WLAN_CIPHER_SUITE_BIP_GMAC_128:
		return "BIP-GMAC-128";
	case WLAN_CIPHER_SUITE_BIP_GMAC_256:
		return "BIP-GMAC-256";
	default:
		return "unknown";
	}
}

enum wim_cipher_type nrc_wim_wlan_to_wim_cipher_type(u32 cipher)
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

u32 nrc_wim_wlan_to_ieee80211_cipher(enum wim_cipher_type cipher)
{
	switch (cipher) {
	case WIM_CIPHER_TYPE_WEP40:
		return WLAN_CIPHER_SUITE_WEP40;
	case WIM_CIPHER_TYPE_WEP104:
		return WLAN_CIPHER_SUITE_WEP104;
	case WIM_CIPHER_TYPE_TKIP:
		return WLAN_CIPHER_SUITE_TKIP;
	case WIM_CIPHER_TYPE_CCMP:
		return WLAN_CIPHER_SUITE_TKIP;
	default:
		return -1;
	}
}

int nrc_wim_wlan_install_key(enum set_key_cmd cmd, struct ieee80211_vif *vif,
			     struct ieee80211_sta *sta,
			     struct ieee80211_key_conf *key)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;
	struct wim_key_param *p;
	u8 cipher;
	const u8 *addr;
	u16 aid = 0;
	int ret = 0;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	cipher = nrc_wim_wlan_to_wim_cipher_type(key->cipher);
	if (cipher == -1)
		return -ENOTSUPP;

	if (key->keyidx > WIM_KEY_MAX_INDEX)
		return -EINVAL;

	DBG_MAC("install_key VIF%d %s %s/%s idx=%d aid=%d",
		((struct nrc_vif *)vif->drv_priv)->index,
		(cmd == SET_KEY) ? "SET" : "DEL",
		(key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ? "PTK" : "GTK",
		ieee80211_cipher_str(key->cipher), key->keyidx, aid);

	skb = nrc_hal_ops_wim_alloc_skb_vif(
		vif, WIM_CMD_SET_KEY + (cmd - SET_KEY), tlv_len(sizeof(*p)));

	p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_KEY_PARAM, sizeof(*p),
					NULL);

	*p = (struct wim_key_param){0};

	if (sta) {
		addr = sta->addr;
	} else if (vif->type == NL80211_IFTYPE_AP ||
		   vif->type == NL80211_IFTYPE_MESH_POINT
#if defined(CONFIG_SUPPORT_IBSS)
		   || vif->type == NL80211_IFTYPE_ADHOC
#endif
		   || vif->type == NL80211_IFTYPE_P2P_GO) {
		addr = vif->addr;
	} else {
		addr = vif->bss_conf.bssid;
	}

	if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) {
		if (!((vif->type == NL80211_IFTYPE_AP) ||
		      (vif->type == NL80211_IFTYPE_MESH_POINT)
#if defined(CONFIG_SUPPORT_IBSS)
		      || (vif->type == NL80211_IFTYPE_ADHOC)
#endif
		      || (vif->type == NL80211_IFTYPE_P2P_GO)))
			aid = vif->cfg.aid;
		else if (sta)
			aid = sta->aid;
	} else {
		if (vif->type == NL80211_IFTYPE_MESH_POINT) {
			if (sta) {
				aid = sta->aid;
			} else {
				aid = vif->cfg.aid;
			}
		} else {
			aid = 0;
		}
	}

	ether_addr_copy(p->mac_addr, addr);
	p->aid = aid;
	if (key->keylen > WIM_KEY_MAX_LEN) {
		ERR_WLAN("Key length %u exceeds max %u", key->keylen,
			 WIM_KEY_MAX_LEN);
		dev_kfree_skb(skb);
		return -EINVAL;
	}
	memcpy(p->key, key->key, key->keylen);
	p->cipher_type = cipher;
	p->key_index = key->keyidx;
	p->key_len = key->keylen;
	p->key_flags = (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ?
			       WIM_KEY_FLAG_PAIRWISE :
			       WIM_KEY_FLAG_GROUP;

	ret = wim_request_and_extract_return(skb, WIM_RESP_TIMEOUT * 10);

	DBG_MAC("install_key VIF%d %s %s/%s idx=%d aid=%d → ret=%d",
		((struct nrc_vif *)vif->drv_priv)->index,
		(cmd == SET_KEY) ? "SET" : "DEL",
		(key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ? "PTK" : "GTK",
		ieee80211_cipher_str(key->cipher), key->keyidx, p->aid, ret);

	return ret;
}

/*
 * ===========================================================================
 * AMPDU Operations
 * ===========================================================================
 */

int nrc_wim_wlan_ampdu_action(struct ieee80211_vif *vif,
			      enum WIM_AMPDU_ACTION action,
			      struct ieee80211_sta *sta, u16 tid)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *skb;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return -EINVAL;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_AMPDU_ACTION,
					    tlv_len(sizeof(u16)) +
						    tlv_len(ETH_ALEN) +
						    tlv_len(sizeof(u16)));

	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_AMPDU_MODE, sizeof(u16),
				    &action);
	nrc_wim_wlan_add_mac_addr(skb, sta->addr);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_TID, sizeof(u16), &tid);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

/*
 * ===========================================================================
 * TSF Operations
 * ===========================================================================
 */

u64 nrc_wim_wlan_get_tsf(struct ieee80211_vif *vif)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *req_skb, *resp_skb;
	struct wim *wim;
	int ret;
	u64 tsf = 0;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		return 0;
	}

	req_skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_GET, tlv_len(0));
	nrc_hal_ops_wim_skb_add_tlv(req_skb, WIM_TLV_TSF, 0, NULL);
	ret = nrc_hal_ops_wim_request(req_skb, 0, WIM_RESP_TIMEOUT, false,
				      &resp_skb);
	if (ret) {
		ERR_WLAN("Failed to get TSF: %d", ret);
		goto done;
	}

	wim = (struct wim *)resp_skb->data;
	if (wim->cmd == WIM_CMD_GET) {
		struct wim_tlv *tlv = (struct wim_tlv *)(wim + 1);
		if (tlv->t == WIM_TLV_TSF) {
			memcpy(&tsf, &tlv->v, tlv->l);
		}
	}

	/* Track free of WIM response SKB with parsed cmd/event */
	NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, wim->cmd, wim->event, true,
			       false);

done:
	return tsf;
}

/*
 * ===========================================================================
 * APF (Android Packet Filter) Operations
 * ===========================================================================
 */

int nrc_wim_wlan_apf_get_enable(struct nrc_hif_device *hdev, int *enable)
{
	struct sk_buff *req_skb, *resp_skb;
	struct wim *wim;
	int ret;

	if (!enable) {
		ERR_WLAN("Invalid enable pointer");
		return -EINVAL;
	}

	req_skb = nrc_hal_ops_wim_alloc_skb_vif(0, WIM_CMD_GET, tlv_len(0));
	nrc_hal_ops_wim_skb_add_tlv(req_skb, WIM_TLV_APF_ENABLE, 0, NULL);
	ret = nrc_hal_ops_wim_request(req_skb, 0, WIM_RESP_TIMEOUT, false,
				      &resp_skb);
	if (ret) {
		ERR_WLAN("Failed to get APF enable: %d", ret);
		return ret;
	}

	wim = (struct wim *)resp_skb->data;
	if (wim->cmd == WIM_CMD_GET) {
		struct wim_tlv *tlv = (struct wim_tlv *)(wim + 1);
		if (tlv->t == WIM_TLV_APF_ENABLE) {
			memcpy(enable, &tlv->v, tlv->l);
		}
	}

	/* Track free of WIM response SKB with parsed cmd/event */
	NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, wim->cmd, wim->event, true,
			       false);

	return 0;
}

int nrc_wim_wlan_apf_set_enable(struct nrc_hif_device *hdev, int enable)
{
	struct sk_buff *req_skb;

	req_skb = nrc_hal_ops_wim_alloc_skb_vif(0, WIM_CMD_SET,
						tlv_len(sizeof(int)));
	nrc_hal_ops_wim_skb_add_tlv(req_skb, WIM_TLV_APF_ENABLE, sizeof(int),
				    &enable);

	return nrc_hal_ops_wim_request(req_skb, 0, 0, false, NULL);
}

u32 nrc_wim_wlan_apf_get_version(struct nrc_hif_device *hdev)
{
	struct sk_buff *req_skb, *resp_skb;
	struct wim *wim;
	int ret;
	u32 version = -1;

	req_skb = nrc_hal_ops_wim_alloc_skb_vif(0, WIM_CMD_GET, tlv_len(0));
	nrc_hal_ops_wim_skb_add_tlv(req_skb, WIM_TLV_APF_VERSION, 0, NULL);
	ret = nrc_hal_ops_wim_request(req_skb, 0, WIM_RESP_TIMEOUT, false,
				      &resp_skb);
	if (ret) {
		ERR_WLAN("Failed to get APF version: %d", ret);
		goto done;
	}

	wim = (struct wim *)resp_skb->data;
	if (wim->cmd == WIM_CMD_GET) {
		struct wim_tlv *tlv = (struct wim_tlv *)(wim + 1);
		if (tlv->t == WIM_TLV_APF_VERSION) {
			memcpy(&version, &tlv->v, tlv->l);
		}
	}

	/* Track free of WIM response SKB with parsed cmd/event */
	NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, wim->cmd, wim->event, true,
			       false);

done:
	return version;
}

u32 nrc_wim_wlan_apf_get_maxlen(struct nrc_hif_device *hdev)
{
	struct sk_buff *req_skb, *resp_skb;
	struct wim *wim;
	int ret;
	u32 maxlen = -1;

	req_skb = nrc_hal_ops_wim_alloc_skb_vif(0, WIM_CMD_GET, tlv_len(0));
	nrc_hal_ops_wim_skb_add_tlv(req_skb, WIM_TLV_APF_MAXLEN, 0, NULL);
	ret = nrc_hal_ops_wim_request(req_skb, 0, WIM_RESP_TIMEOUT, false,
				      &resp_skb);
	if (ret) {
		ERR_WLAN("Failed to get APF maxlen: %d", ret);
		goto done;
	}

	wim = (struct wim *)resp_skb->data;
	if (wim->cmd == WIM_CMD_GET) {
		struct wim_tlv *tlv = (struct wim_tlv *)(wim + 1);
		if (tlv->t == WIM_TLV_APF_MAXLEN) {
			memcpy(&maxlen, &tlv->v, tlv->l);
		}
	}

	/* Track free of WIM response SKB with parsed cmd/event */
	NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, wim->cmd, wim->event, true,
			       false);

done:
	return maxlen;
}

int nrc_wim_wlan_apf_set_packet_filter(struct nrc_hif_device *hdev, u8 *program,
				       size_t len)
{
	struct sk_buff *skb;

	skb = nrc_hal_ops_wim_alloc_skb_vif(0, WIM_CMD_SET, tlv_len(len));
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_APF_SET_FILTER, len, program);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

int nrc_wim_wlan_apf_get_packet_filter(struct nrc_hif_device *hdev,
				       u32 src_offset, u8 *host_dst, u32 len)
{
	struct sk_buff *req_skb, *resp_skb;
	struct wim *wim;
	u64 param;
	int ret = 0;

	if (len > WIM_MAX_SIZE) {
		ERR_WLAN("Filter length (%u) exceeds WIM size (%zu)",
		 len, WIM_MAX_SIZE);
		return -EMSGSIZE;
	}

	if (!host_dst) {
		ERR_WLAN("Invalid host_dst pointer");
		return -EINVAL;
	}

	param = (u64)src_offset << 32 | len;

	req_skb = nrc_hal_ops_wim_alloc_skb_vif(0, WIM_CMD_GET, tlv_len(0));
	nrc_hal_ops_wim_skb_add_tlv(req_skb, WIM_TLV_APF_GET_FILTER,
				    sizeof(u32) * 2, &param);
	ret = nrc_hal_ops_wim_request(req_skb, 0, WIM_RESP_TIMEOUT * 10, false,
				      &resp_skb);
	if (ret) {
		ERR_WLAN("Failed to get packet filter: %d", ret);
		goto done;
	}

	wim = (struct wim *)resp_skb->data;
	if (wim->cmd == WIM_CMD_GET) {
		struct wim_tlv *tlv = (struct wim_tlv *)(wim + 1);
		if (tlv->t == WIM_TLV_APF_GET_FILTER) {
			memcpy(host_dst, &tlv->v, tlv->l);
		}
	}

	/* Track free of WIM response SKB with parsed cmd/event */
	NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, wim->cmd, wim->event, true,
			       false);

done:
	return ret;
}

/*
 * ===========================================================================
 * Internal Helper Functions
 * ===========================================================================
 */

/**
 * wim_request_and_extract_return - Send WIM request and extract return value
 * @skb: WIM request SKB
 * @timeout: Timeout in jiffies
 *
 * Internal wrapper around nrc_hal_ops_wim_request() that extracts WIM_TLV_RETURN value.
 * Uses WLAN path (not MCP path).
 *
 * Return: WIM_TLV_RETURN value on success, -1 on failure
 */
static int wim_request_and_extract_return(struct sk_buff *skb, int timeout)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct sk_buff *resp_skb = NULL;
	struct wim *req_wim, *resp_wim;
	u16 cmd;
	int ret = -1;

	if (!hdev) {
		ERR_WLAN("Invalid HIF device");
		/* SKB ownership: caller passed SKB, but nrc_hal_ops_wim_request will free it */
		nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
		return -EINVAL;
	}

	req_wim = (struct wim *)skb->data;
	cmd = req_wim->cmd;

	/* Call nrc_hal_ops_wim_request with response pointer */
	if (nrc_hal_ops_wim_request(skb, 0, timeout, false, &resp_skb) != 0) {
		/* nrc_hal_ops_wim_request already freed skb on error */
		return -1;
	}

	/* Extract WIM_TLV_RETURN value from response */
	if (resp_skb) {
		resp_wim = (struct wim *)resp_skb->data;
		if (cmd == resp_wim->cmd) {
			struct wim_tlv *tlv = (struct wim_tlv *)(resp_wim + 1);
			if (tlv->t == WIM_TLV_RETURN) {
				memcpy(&ret, &tlv->v, tlv->l);
			}
		} else {
			ERR_WLAN("WIM request/response mismatch (%d vs %d)",
				 cmd, resp_wim->cmd);
		}

		/* Free response SKB */
		NRC_SKB_TRACK_WIM_FREE(hdev, resp_skb, resp_wim->cmd,
				       resp_wim->event, true, false);
	}

	return ret;
}
