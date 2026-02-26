/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * TX/RX routines
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
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

/* Linux networking headers */
#include <net/dst.h>
#include <net/genetlink.h>
#include <net/ieee80211_radiotap.h>
#include <net/mac80211.h>
#include <net/xfrm.h>

/* Common directory headers - Core */
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* WLAN module trace system (declarations only) */
#if defined(CONFIG_NRC_TRACING)
#define CREATE_TRACE_POINTS
#include "nrc-trace.h"
// #ifdef TRACE_INCLUDE_FILE
// #undef TRACE_INCLUDE_FILE
// #define TRACE_INCLUDE_FILE wlan
// #endif
#endif

/* Local module headers */
#include "compat.h"
#include "nrc-mac80211.h"
#include "nrc-mac80211-twt.h"
#include "nrc-stats.h"
#include "nrc-vendor.h"
#include "nrc-wlan-hal-init.h"
#include "nrc-hal-core-interface.h"
#ifdef CONFIG_S1G_CHANNEL
#include "nrc-s1g.h"
#endif
#include "nrc-ps.h"
#include "nrc-pm.h"

/* Forward declarations for TX/RX handlers (local to this file) */
static int tx_h_debug_state(struct nrc_trx_data *tx);
static int tx_h_wfa_halow_filter(struct nrc_trx_data *tx);
static int tx_h_frame_filter(struct nrc_trx_data *tx);
static int tx_h_managed_p2p_intf_addr(struct nrc_trx_data *tx);
static int tx_h_put_iv(struct nrc_trx_data *tx);
static int tx_h_put_qos_control(struct nrc_trx_data *tx);
static int tx_h_twt_assoc(struct nrc_trx_data *tx);

static int rx_h_vendor(struct nrc_trx_data *rx);
static int rx_h_decrypt(struct nrc_trx_data *rx);
static int rx_h_check_sn(struct nrc_trx_data *rx);
static int rx_h_action(struct nrc_trx_data *rx);
static int rx_h_twt_assoc(struct nrc_trx_data *rx);
static int rx_h_twt_monitor(struct nrc_trx_data *rx);

#if NRC_DBG_PRINT_FRAME_TX
static int tx_h_debug_print(struct nrc_trx_data *tx);
#endif
#if NRC_DBG_PRINT_FRAME_RX
static int rx_h_debug_print(struct nrc_trx_data *rx);
#endif
#if defined(CONFIG_SUPPORT_IBSS)
static int rx_h_ibss_get_bssid_tsf(struct nrc_trx_data *rx);
#endif
#ifdef CONFIG_SUPPORT_MESH_ROUTING
static int rx_h_mesh(struct nrc_trx_data *rx);
#endif

static int is_eapol(struct sk_buff *skb, struct nrc *nw);
static void setup_ba_session(struct nrc *nw, struct ieee80211_vif *vif,
			     struct sk_buff *skb);

/* TX */
#define USF2SF(usf) \
	((usf == 0) ? 1 : (usf == 1) ? 10 : (usf == 2) ? 1000 : 10000)

static bool nrc_is_valid_vif(struct nrc *nw, struct ieee80211_vif *vif)
{
	u8 i;
	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		if (vif == nw->vif[i])
			return true;
	}
	DBG_MAC("[%s] Invallid vif", __func__);
	return false;
}

static void nrc_debug_print_frame(struct ieee80211_hdr *hdr,
				  int direction) /* 0:TX, 1:RX */
{
	__le16 fc;
	u8 fc_type, fc_subtype;
	u8 *addr1, *addr2;
	const char *type_str = NULL;
	const char *subtype_str = NULL;

	fc = hdr->frame_control;
	fc_type = (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE));
	fc_subtype = (fc & cpu_to_le16(IEEE80211_FCTL_STYPE));

	addr1 = hdr->addr1;
	addr2 = hdr->addr2;

	/* Determine frame type */
	if (fc_type == cpu_to_le16(IEEE80211_FTYPE_MGMT)) {
		type_str = "MGMT";
		/* Skip beacon frames to reduce log noise */
		if (fc_subtype == cpu_to_le16(IEEE80211_STYPE_BEACON))
			return;
		subtype_str = get_mgmt_str(fc_subtype);
	} else if (fc_type == cpu_to_le16(IEEE80211_FTYPE_CTL)) {
		type_str = "CTL";
		/* Control frame subtypes */
		switch (fc_subtype) {
		case cpu_to_le16(IEEE80211_STYPE_PSPOLL):
			subtype_str = "PSPOLL";
			break;
		case cpu_to_le16(IEEE80211_STYPE_RTS):
			subtype_str = "RTS";
			break;
		case cpu_to_le16(IEEE80211_STYPE_CTS):
			subtype_str = "CTS";
			break;
		case cpu_to_le16(IEEE80211_STYPE_ACK):
			subtype_str = "ACK";
			break;
		case cpu_to_le16(IEEE80211_STYPE_CFEND):
			subtype_str = "CFEND";
			break;
		case cpu_to_le16(IEEE80211_STYPE_CFENDACK):
			subtype_str = "CFENDACK";
			break;
		default:
			subtype_str = "UNKNOWN";
			break;
		}
	} else if (fc_type == cpu_to_le16(IEEE80211_FTYPE_DATA)) {
		type_str = "DATA";
		/* Data frame subtypes */
		if (ieee80211_is_data_qos(fc)) {
			if (ieee80211_is_nullfunc(fc))
				subtype_str = "QOS_NULL";
			else
				subtype_str = "QOS_DATA";
		} else if (ieee80211_is_nullfunc(fc)) {
			subtype_str = "NULL";
		} else {
			subtype_str = "DATA";
		}
	} else {
		/* Unknown frame type */
		type_str = "UNKNOWN";
		subtype_str = "";
	}

	/* Print with appropriate debug macro based on frame type and direction */
	if (fc_type == cpu_to_le16(IEEE80211_FTYPE_DATA)) {
		/* DATA frames use DBG_TX/DBG_RX */
		if (direction) {
			DBG_RX("%s %s %s, DA: %pM, SA: %pM", type_str,
			       subtype_str, "RX", addr1, addr2);
		} else {
			DBG_TX("%s %s %s, DA: %pM, SA: %pM", type_str,
			       subtype_str, "TX", addr1, addr2);
		}
	} else {
		if (direction) {
			DBG(CAT(MAC) | CAT(RX), "%s %s %s, DA: %pM, SA: %pM",
			    type_str, subtype_str, "RX", addr1, addr2);
		} else {
			DBG(CAT(MAC) | CAT(TX), "%s %s %s, DA: %pM, SA: %pM",
			    type_str, subtype_str, "TX", addr1, addr2);
		}
	}
}

/**
 * nrc_mac_tx() - main tx routine
 *
 * @hw: the hardware
 * @control: tx control data
 * @skb: the skb
 */

#ifdef CONFIG_SUPPORT_NEW_MAC_TX
void nrc_mac_tx_process(struct ieee80211_hw *hw,
			struct ieee80211_tx_control *control,
			struct sk_buff *skb, bool from_mac80211)
#else
void nrc_mac_tx_process(struct ieee80211_hw *hw, struct sk_buff *skb,
			bool from_mac80211)
#endif
{
	struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
	const struct nrc_trx_handler *h;
	int res = 0;
	int i;
	struct nrc_trx_data tx = {
		.nw = hw->priv,
		.vif = txi->control.vif,
#ifdef CONFIG_SUPPORT_TX_CONTROL
		.sta = control->sta,
#else
		.sta = txi->control.sta,
#endif
		.skb = skb,
		.result = 0,
	};

	struct ieee80211_hdr *mh = (void *)skb->data;
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;

	s8 vif_id = 0;
	int eapol_msg;

	/* Track SKB allocation if from mac80211 (indirect allocation) */
	if (from_mac80211) {
		NRC_SKB_TRACK_ALLOC(hdev, skb, HIF_TYPE_FRAME, false, true);
	}

#ifdef CONFIG_USE_TXQ /* Here is not data frame */
	nrc_debug_print_frame(mh, 0);
#endif /* CONFIG_USE_TXQ */

	if (!nrc_is_valid_vif(tx.nw, tx.vif)) {
		ERR_WLAN("WLAN TX: Invalid VIF, dropping packet");
		goto txh_out;
	}

	vif_id = hw_vifindex(tx.vif);

	/* Set BA Session */
	if (tx.nw->params->ampdu_mode == NRC_AMPDU_AUTO) {
		if (ieee80211_is_data_qos(mh->frame_control) &&
		    !is_multicast_ether_addr(mh->addr1) &&
		    !is_eapol(tx.skb, tx.nw)) {
			setup_ba_session(tx.nw, tx.vif, tx.skb);
		}
	}

	/* Only for PS STA */
	if (tx.nw->vif[vif_id]->type == NL80211_IFTYPE_STATION) {
#ifndef CONFIG_USE_TXQ
		if (NRC_DRV_IS_ASLEEP(tx.nw->hdev) &&
		    !(tx.nw->twt_sched && tx.nw->params->twt_force_sleep)) {
			/* Request wake using state machine (atomic context safe) */
			DBG_TX("TX wakeup: Device asleep, frame type=0x%04x subtype=0x%04x",
			       WLAN_FC_GET_TYPE(mh->frame_control),
			       WLAN_FC_GET_STYPE(mh->frame_control));
			nrc_hal_ops_ps_request_wake(
				0, NRC_PS_REASON_DRV_TX_WAKEUP);
		}
#endif
		if (tx.nw->params->power_save == NRC_PS_MODEMSLEEP) {
			if (tx.nw->hdev->ps.modem_enabled) {
				struct sk_buff *skb1;
				struct wim_pm_param *p;
				/**
				 * ps_drv_state is used to check the driver's own power save state
				 * when mac80211 is aware of the driver is in power saving state.
				 * for example,
				 * when driver needs to send qos null frame by keep-alive timer,
				 * driver has to wake-up the target silently before sending frames.
				 */

				DBG_PS("[%s] Wakeup MODEMSLEEP...", __func__);
				skb1 = nrc_hal_ops_wim_alloc_skb(
					WIM_CMD_SET,
					tlv_len(sizeof(struct wim_pm_param)));
				p = nrc_hal_ops_wim_skb_add_tlv(
					skb1, WIM_TLV_PS_ENABLE,
					sizeof(struct wim_pm_param), NULL);
				memset(p, 0, sizeof(struct wim_pm_param));
				p->ps_mode = tx.nw->params->power_save;
				p->ps_enable = false;
				nrc_hal_ops_wim_request(skb1, 0, 0, false,
							NULL);
				tx.nw->hdev->ps.modem_enabled = false;
			}
		}

		if (tx.nw->params->power_save >= NRC_PS_DEEPSLEEP_TIM) {
			if (NRC_DRV_IS_ASLEEP(tx.nw->hdev)) {
				memset(&tx.nw->d_deauth, 0,
				       sizeof(struct nrc_delayed_deauth));
				if (ieee80211_is_deauth(mh->frame_control)) {
					INFO("[%s,L%d][deauth] drv_state:%s(%d)",
					     __func__, __LINE__,
					     nrc_drv_state_str(
						     NRC_HIF_DRV_STATE(
							     tx.nw->hdev)),
					     NRC_HIF_DRV_STATE(tx.nw->hdev));
					memcpy(&tx.nw->d_deauth.v, tx.vif,
					       sizeof(struct ieee80211_vif));
					memcpy(&tx.nw->d_deauth.s, tx.sta,
					       sizeof(struct ieee80211_sta));
					/* Request wake using state machine (atomic context safe) */
					nrc_hal_ops_ps_request_wake(
						0, NRC_PS_REASON_DRV_TX_WAKEUP);
					nrc_hal_ops_tx_cleanup_queues();
					tx.nw->d_deauth.deauth_frm =
						skb_copy(skb, GFP_ATOMIC);
#ifdef REMOVE_DELAYED_DEAUTH
					atomic_set(
						&tx.nw->d_deauth.delayed_deauth,
						1);
#endif
					tx.nw->d_deauth.vif_index =
						hw_vifindex(tx.vif);
					tx.nw->d_deauth.aid = tx.sta->aid;
					goto txh_out;
				}
				if (ieee80211_is_qos_nullfunc(
					    mh->frame_control)) {
					DBG_PS("[%s,L%d][qos_null] make target wake for keep-alive",
					       __func__, __LINE__);
					/* Request wake using state machine (atomic context safe) */
					nrc_hal_ops_ps_request_wake(
						0, NRC_PS_REASON_DRV_TX_WAKEUP);
					goto txh_out;
				}
			} else if (NRC_HIF_DRV_STATE(tx.nw->hdev) !=
				   NRC_DRV_RUNNING) {
				/*
				 * Sometimes a deauth frame is transferred while nrc_unregister_hw() is
				 * in processing(NRC_DRV_CLOSING) at the result of 'ifconfig down'.
				 * We don't need to handle this.
				 */
				if (ieee80211_is_deauth(mh->frame_control)) {
					DBG_PS("[%s,L%d][deauth] drv_state:%d ===> discard!!!",
					       __func__, __LINE__,
					       NRC_HIF_DRV_STATE(tx.nw->hdev));
				}
				goto txh_out;
			}
		}

		if (NRC_PS_IS_AWAKE(tx.nw->hdev)) {
			/* without check, ps_timer is expired before wake-up if ps_time is too short (100ms~300ms) */
			/* after done wake-up, nrc_ps_dyn_start is called */
			nrc_ps_dyn_start(tx.nw);
		}
	} //if (tx.nw->vif[vif_id]->type == NL80211_IFTYPE_STATION)

	/* Iterate over tx handlers */
	for (i = 0; i < nrc_tx_handlers_count; i++) {
		h = &nrc_tx_handlers[i];
		if (!(h->vif_types & BIT(tx.vif->type)))
			continue;

		res = h->handler(&tx);
		if (res < 0)
			goto txh_out;
	}

	trace_nrc_tx_hdr(tx.nw, skb->data, skb->len, 0, 0);
	trace_nrc_tx_payload(tx.nw, skb->data, skb->len);

	eapol_msg = is_eapol(tx.skb, tx.nw);
	if (eapol_msg) {
		DBG_MAC("TX EAPOL(%d), ADDR1: %pM, ADDR2: %pM", eapol_msg,
			mh->addr1, mh->addr2);
	}

	if (!atomic_read(&tx.nw->d_deauth.delayed_deauth)) {
		DBG_TX("WLAN TX: Calling xmit_frame vif=%d aid=%d len=%u",
		       vif_id, (!!tx.sta ? tx.sta->aid : 0), tx.skb->len);
		nrc_hal_ops_xmit_wlan_frame(
			vif_id, (!!tx.sta ? tx.sta->aid : 0), tx.skb);
	} else {
		DBG_TX("WLAN TX: Delayed deauth active, skipping xmit");
	}

	return;

txh_out:
	ERR_WLAN("TX:dropping packet");
	if (tx.skb) {
		/* Track FRAME SKB free (TX path failure) */
		NRC_SKB_TRACK_FREE(tx.nw->hdev, tx.skb, HIF_TYPE_FRAME, false,
				   false);
	}
}

struct eapol_hdr {
	uint8_t version;
	uint8_t type;
	uint16_t length;
} __attribute__((packed));

struct eapol_key {
	uint8_t type;
	uint8_t key_info[2];
	uint8_t key_length[2];
	uint8_t replay_counter[8];
	uint8_t key_nonce[32];
	uint8_t key_iv[16];
	uint8_t key_rsc[8];
	uint8_t key_id[8];
	uint8_t key_mic[16];
	uint16_t key_data_length;
} __attribute__((packed));

enum eapol_msg {
	EAPOL_MSG_NONE = 0,
	EAPOL_MSG_M1,
	EAPOL_MSG_M2,
	EAPOL_MSG_M3,
	EAPOL_MSG_M4,
	EAPOL_MSG_MAX
};

/* EAPOL MSG */
#define EAPOL_TYPE_EAPOL_KEY (3)
#define EAPOL_KEY_INFO_KEY_TYPE BIT(3)
#define EAPOL_KEY_INFO_KEY_INDEX (BIT(4) | BIT(5))
#define EAPOL_KEY_INFO_INSTALL BIT(6)
#define EAPOL_KEY_INFO_TXRX BIT(6)
#define EAPOL_KEY_INFO_ACK BIT(7)
#define EAPOL_KEY_INFO_MIC BIT(8)
#define EAPOL_KEY_INFO_SECURE BIT(9)
#define EAPOL_KEY_INFO_ERROR BIT(10)
#define EAPOL_KEY_INFO_REQUEST BIT(11)
#define EAPOL_KEY_INFO_ENCR_KEY_DATA BIT(12)
#define EAPOL_KEY_INFO_SMK_MESSAGE BIT(13)

static int is_eapol(struct sk_buff *skb, struct nrc *nw)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	u16 hdrlen = ieee80211_hdrlen(hdr->frame_control);
	u16 ethertype;
	u8 *pos;

	struct eapol_hdr *ehdr;
	struct eapol_key *key;
	uint16_t key_info;

	__le16 fc = hdr->frame_control;

	if (!ieee80211_is_data(hdr->frame_control))
		return EAPOL_MSG_NONE;

	if (ieee80211_is_any_nullfunc(hdr->frame_control)) {
		return EAPOL_MSG_NONE;
	}

	if (ieee80211_has_protected(fc)) {
		if (nw->params->sw_enc != WIM_ENCDEC_HW)
			return EAPOL_MSG_NONE;
		hdrlen += 8; // CCMP Header
	}

	pos = skb->data + hdrlen;

	pos += 6; /* rfc1042, aaaa 0300 0000 */

	ethertype = *pos << 8 | *(pos + 1);

	if (ethertype != ETH_P_PAE) {
		return EAPOL_MSG_NONE;
	}

	ehdr = (struct eapol_hdr *)(pos + 2);
	key = (struct eapol_key *)(pos + 2 + sizeof(struct eapol_hdr));
	key_info = (key->key_info[0] << 8) | key->key_info[1];

	if (ehdr->type != EAPOL_TYPE_EAPOL_KEY)
		return EAPOL_MSG_MAX;

	if (key_info & EAPOL_KEY_INFO_REQUEST)
		return EAPOL_MSG_MAX;

	if (key_info & EAPOL_KEY_INFO_KEY_TYPE) {
		uint16_t masked = key_info &
				  (EAPOL_KEY_INFO_INSTALL | EAPOL_KEY_INFO_ACK);
		if (masked == EAPOL_KEY_INFO_ACK) {
#ifdef EAPOL_DEBUG
			DBG_MAC("M1 RA:" MACSTR " (%x,%x,%x,%x,%x,%x,%x,%x)",
				MAC2STR(hdr->addr1), key->replay_counter[0],
				key->replay_counter[1], key->replay_counter[2],
				key->replay_counter[3], key->replay_counter[4],
				key->replay_counter[5], key->replay_counter[6],
				key->replay_counter[7]);
#endif
			return EAPOL_MSG_M1;
		} else if (masked ==
			   (EAPOL_KEY_INFO_INSTALL | EAPOL_KEY_INFO_ACK)) {
#ifdef EAPOL_DEBUG
			DBG_MAC("M3 RA:" MACSTR " (%x,%x,%x,%x,%x,%x,%x,%x)",
				MAC2STR(hdr->addr1), key->replay_counter[0],
				key->replay_counter[1], key->replay_counter[2],
				key->replay_counter[3], key->replay_counter[4],
				key->replay_counter[5], key->replay_counter[6],
				key->replay_counter[7]);
#endif
			return EAPOL_MSG_M3;
		} else {
			if (key->key_data_length > 0) {
#ifdef EAPOL_DEBUG
				DBG_MAC("M2 RA:" MACSTR
					" (%x,%x,%x,%x,%x,%x,%x,%x)\n",
					MAC2STR(hdr->addr1),
					key->replay_counter[0],
					key->replay_counter[1],
					key->replay_counter[2],
					key->replay_counter[3],
					key->replay_counter[4],
					key->replay_counter[5],
					key->replay_counter[6],
					key->replay_counter[7]);
#endif
				return EAPOL_MSG_M2;
			} else {
#ifdef EAPOL_DEBUG
				DBG_MAC("M4 RA:" MACSTR
					" (%x,%x,%x,%x,%x,%x,%x,%x)\n",
					MAC2STR(hdr->addr1),
					key->replay_counter[0],
					key->replay_counter[1],
					key->replay_counter[2],
					key->replay_counter[3],
					key->replay_counter[4],
					key->replay_counter[5],
					key->replay_counter[6],
					key->replay_counter[7]);
#endif
				return EAPOL_MSG_M4;
			}
		}
	}
	return EAPOL_MSG_MAX;
}

static void setup_ba_session(struct nrc *nw, struct ieee80211_vif *vif,
			     struct sk_buff *skb)
{
	struct ieee80211_sta *peer_sta = NULL;
	struct nrc_sta *i_sta = NULL;
	struct ieee80211_hdr *qmh = (struct ieee80211_hdr *)skb->data;
	int tid = *ieee80211_get_qos_ctl(qmh) & IEEE80211_QOS_CTL_TID_MASK;

	DBG_AMPDU("Start BA session (%pM, %d)", qmh->addr1, tid);

	if (nw->frag_threshold != -1) { /* Fragmentation enabled by iwconfig */
		ERR_WLAN(
			"Since fragmentation enabled by iwconfig, ignore to setup BA session");
		return;
	}

	/* tid range : 0 ~ 7 */
	if (tid < 0 || tid >= NRC_MAX_TID) {
		ERR_WLAN("Invalid TID(%d) with peer %pM", tid, qmh->addr1);
		return;
	}
	rcu_read_lock();
	peer_sta = ieee80211_find_sta(vif, qmh->addr1);
	if (!peer_sta) {
		ERR_WLAN("Fail to set up BA. Fail to find peer_sta (%pM)",
			 qmh->addr1);
		goto out;
	}

	/* Set up BA session if not created */
	/* Find Peer STA with ADDR1  to set up BA Session */
#ifdef CONFIG_S1G_CHANNEL
	peer_sta->ht_cap.ht_supported = true;
#endif /* #ifdef CONFIG_S1G_CHANNEL */
	i_sta = to_i_sta(peer_sta);
	if (!i_sta) {
		ERR_WLAN("Fail to set up BA. Fail to find nrc_sta (%pM)",
			 qmh->addr1);
		goto out;
	}

	ieee80211_queue_work(nw->hw,
			     &i_sta->tx_ba_session[tid].ba_session_work);

out:
	rcu_read_unlock();
	return;
}

static const char *to_state(enum ieee80211_sta_state state)
{
	switch (state) {
	case IEEE80211_STA_NOTEXIST:
		return "NOTEXIST";
	case IEEE80211_STA_NONE:
		return "NONE";
	case IEEE80211_STA_AUTH:
		return "AUTH";
	case IEEE80211_STA_ASSOC:
		return "ASSOC";
	case IEEE80211_STA_AUTHORIZED:
		return "AUTHORIZED";
	default:
		return "unknown";
	};
}

#if NRC_DBG_PRINT_FRAME_TX
static int tx_h_debug_print(struct nrc_trx_data *tx)
{
	struct ieee80211_sta *sta;
	struct nrc_vif *i_vif;
	struct nrc_sta *i_sta;
	const struct ieee80211_hdr *hdr;
	__le16 fc;

	if (!tx) {
		DBG_MAC("[%s] tx is NULL", __func__);
		return 0;
	}

	i_vif = to_i_vif(tx->vif);
	if (!i_vif) {
		DBG_MAC("[%s] vif is NULL", __func__);
		return 0;
	}

	hdr = (const struct ieee80211_hdr *)tx->skb->data;
	fc = hdr->frame_control;

	DBG_MAC("[%s] %s vif:%d type:%d sype:%d, protected:%d", __func__,
		(tx->vif->type == NL80211_IFTYPE_STATION) ? "STA" :
		(tx->vif->type == NL80211_IFTYPE_AP)	  ? "AP" :
		(tx->vif->type == NL80211_IFTYPE_ADHOC)	  ? "ADHOC" :
							    "MONITORP",
		i_vif->index, WLAN_FC_GET_TYPE(fc), WLAN_FC_GET_STYPE(fc),
		ieee80211_has_protected(fc));

	if (is_unicast_ether_addr(hdr->addr1)) {
		sta = ieee80211_find_sta(tx->vif, hdr->addr1);
		if (!sta) {
			DBG_MAC("[%s] Unable to find %pM in mac80211", __func__,
				hdr->addr1);
			return 0;
		}
	}

	i_sta = to_i_sta(sta);
	if (!i_sta) {
		DBG_MAC("[%s] Unable to find %pM in nrc drv", __func__,
			hdr->addr1);
		return 0;
	}

	// set filter here : add filter as you wish
	//eg. ieee80211_is_data(fc) ieee80211_is_mgmt(fc)
	//eg. ieee80211_is_assoc_req
	if (ieee80211_is_deauth(fc)) {
		print_hex_dump(KERN_DEBUG,
			       "tx deauth frame: ", DUMP_PREFIX_NONE, 16, 1,
			       tx->skb->data, tx->skb->len, false);
	}
	return 0;
}

#endif //#if NRC_DBG_PRINT_FRAME_TX

static int tx_h_debug_state(struct nrc_trx_data *tx)
{
	struct ieee80211_sta *sta;
	struct nrc_sta *i_sta;
	const struct ieee80211_hdr *hdr =
		(const struct ieee80211_hdr *)tx->skb->data;

	if (!tx || !tx->sta ||
	    (!ieee80211_is_data(hdr->frame_control) ||
	     is_eapol(tx->skb, tx->nw)))
		return 0;

	sta = ieee80211_find_sta(tx->vif, hdr->addr1);

	if (!sta) {
		DBG_MAC("Unable to find %pM", hdr->addr1);
		return 0;
	}

	i_sta = to_i_sta(sta);

	if (!i_sta)
		return 0;

	if (i_sta->state != IEEE80211_STA_AUTHORIZED)
		DBG_MAC("%s: Wrong state (%s)/%pM", __func__,
			to_state(i_sta->state), hdr->addr1);

	return 0;
}

static int tx_h_wfa_halow_filter(struct nrc_trx_data *tx)
{
	struct nrc *nw = tx->nw;

	if (nw->block_frame) {
		DBG_MAC("%s: TX is blocked (Halow)", __func__);
		return -EINVAL;
	}

	return 0;
}

static int tx_h_frame_filter(struct nrc_trx_data *tx)
{
	const struct ieee80211_hdr *hdr;
	__le16 fc;

	if (!tx) {
		DBG_MAC("[%s] tx is NULL", __func__);
		return 0;
	}

	if (tx->nw->params->discard_deauth &&
	    tx->vif->type == NL80211_IFTYPE_STATION) {
		hdr = (const struct ieee80211_hdr *)tx->skb->data;
		fc = hdr->frame_control;
		if (ieee80211_is_deauth(fc)) {
			DBG_MAC("[%s] discard TX deauth frame", __func__);
			return -1;
		}
	}
	return 0;
}

#ifdef CONFIG_SUPPORT_P2P
static int tx_h_managed_p2p_intf_addr(struct nrc_trx_data *tx)
{
	struct ieee80211_mgmt *mgmt = NULL;
	const u8 *p2p_ie = NULL;
	u8 *pos = NULL, *p2p_len = NULL, *cnt = NULL;
	u16 *attr_len = NULL, len = 0;
	const int P2P_ATTR_INTERFACE = 0x10;
	u8 target[ETH_ALEN];
	u8 i;

	if (tx->vif->p2p)
		return 0;

	mgmt = (void *)tx->skb->data;

	if (!(ieee80211_is_assoc_req(mgmt->frame_control) &&
	      !ieee80211_is_reassoc_req(mgmt->frame_control)))
		return 0;

	p2p_ie = cfg80211_find_vendor_ie(
		WLAN_OUI_WFA, WLAN_OUI_TYPE_WFA_P2P, mgmt->u.assoc_req.variable,
		tx->skb->len - (mgmt->u.assoc_req.variable - tx->skb->data));

	if (!p2p_ie)
		return 0;

	ether_addr_copy(target, tx->vif->addr);
	target[0] = 0x6;
	target[1] = 0x1;

	pos = (u8 *)p2p_ie;
	p2p_len = (pos + 1);
	pos += 6; /* Skip OUI + OUI Type */

	while (pos - p2p_ie < *p2p_len) {
		attr_len = (u16 *)(pos + 1);
		len = le16_to_cpu(*attr_len);

		if (len == 0)
			return 0;

		if (*pos != P2P_ATTR_INTERFACE) {
			pos += (3 /*ID+Len*/ + len);
			continue;
		}
		pos += 3; /* Skip id + len */
		pos += 6; /* Skip Device Address */

		cnt = pos++;
		for (i = 0; i < *cnt; i++, pos += 6) {
			if (ether_addr_equal(target, pos))
				return 0;
		}
		*cnt += 1;
		*p2p_len += 6;
		*attr_len = cpu_to_le16(len + 6);
		skb_put(tx->skb, 6);
		memmove(pos + 6, pos, tx->skb->len - (pos - tx->skb->data));
		ether_addr_copy(pos, target);

		return 0;
	}

	return 0;
}

#endif

/**
 * tx_h_put_iv() - Put a space for IV header.
 *
 * @tx: points to the tx context.
 *
 * If IEEE80211_SKB_CB(@tx->skb)
 */
static int tx_h_put_iv(struct nrc_trx_data *tx)
{
	struct sk_buff *skb = tx->skb;
	struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
	struct ieee80211_key_conf *key = txi->control.hw_key;
	struct ieee80211_hdr *mh = (void *)skb->data;
	__le16 fc = mh->frame_control;
	int hdrlen = ieee80211_hdrlen(fc);

	if (ieee80211_has_protected(fc) && key &&
	    !(key->flags & IEEE80211_KEY_FLAG_GENERATE_IV))
		memcpy(skb_push(skb, key->iv_len), mh, hdrlen);

	return 0;
}

#if defined(CONFIG_CONVERT_NON_QOSDATA)
static bool ieee80211_is_data_data(__le16 fc)
{
	return (fc &
		cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
	       cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA);
}

static void insert_qos_ctrl_field_in_skb(struct sk_buff *skb,
					 unsigned int hdr_len)
{
	struct ieee80211_hdr *mh = (void *)skb->data;
	bool is_multi = ((mh->addr1[0] & 0x01) != 0);
	u16 fc = 0;
	u16 qos_ctrl =
		0; //Set TID '0' in QoS Field when converting to qos data.

	if (!skb) {
		DBG_MAC("invalid skb [%s, %d] ## ", __func__, __LINE__);
		BUG();
	}

	memcpy(&fc, &mh->frame_control, sizeof(mh->frame_control));
	fc |= cpu_to_le16(IEEE80211_STYPE_QOS_DATA);
	if (is_multi)
		qos_ctrl |= cpu_to_le16(0x01 << 5); // no ack

	skb_push(skb, sizeof(qos_ctrl));
	memmove(skb->data, skb->data + sizeof(qos_ctrl), hdr_len);

	memcpy(skb->data + hdr_len, &qos_ctrl, sizeof(qos_ctrl));
	memcpy(skb->data, &fc, sizeof(mh->frame_control));
}

/**
 * tx_h_put_qos_control() - change non-QoS data frame to QoS data frame.
 *
 * @tx: points to the tx context.
 *
 */
static int tx_h_put_qos_control(struct nrc_trx_data *tx)
{
	struct sk_buff *skb = tx->skb;
	struct ieee80211_hdr *mh = (void *)skb->data;
	u16 fc = mh->frame_control;

	if (ieee80211_is_data_data(fc)) {
		if (!(ieee80211_has_protected(fc) &&
		      tx->nw->params->sw_enc != WIM_ENCDEC_HW))
			insert_qos_ctrl_field_in_skb(skb, ieee80211_hdrlen(fc));
	}
	return 0;
}

#endif /* if defined (CONFIG_CONVERT_NON_QOSDATA) */

static int tx_h_twt_assoc(struct nrc_trx_data *tx)
{
	struct nrc *nw = tx->nw;
	struct ieee80211_sta *sta = tx->sta;

	struct sk_buff *skb = tx->skb;
	struct sk_buff *new_skb;
	struct ieee80211_mgmt *mgmt = (void *)skb->data;
	int len;

	__u16 fc = mgmt->frame_control;

	if (likely(nw->twt_responder == false)) {
		goto done;
	}

	if (likely(!ieee80211_is_assoc_resp(fc))) {
		return 0;
	}

	len = sizeof(struct ieee80211_twt_setup_assoc_ie) + 8;
#ifdef NRC_TWT_VENDOR_IE_ENABLE
	len += sizeof(struct ieee80211_twt_setup_vendor_ie);
#endif

	new_skb = skb_copy_expand(skb, nw->hw->extra_tx_headroom, len,
				  GFP_ATOMIC);
	/* Track FRAME SKB free (TX path - replaced with expanded SKB) */
	NRC_SKB_TRACK_FREE(nw->hdev, skb, HIF_TYPE_FRAME, false, false);
	tx->skb = new_skb;
	//nrc_mac_rx_twt_setup_assoc_resp(nw, sta, mgmt, skb->len);
	nrc_mac_rx_twt_setup_assoc_resp(nw, sta, new_skb);

done:
	return 0;
}

/* RX */

static void nrc_mac_rx_h_status(struct nrc *nw, struct sk_buff *skb)
{
	struct frame_hdr *fh;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_hdr *mh;

	fh = (void *)skb->data; /* frame header */
	mh = (void *)(skb->data + nw->hdev->fw.info.rx_head_size -
		      sizeof(struct hif));

	memset(status, 0, sizeof(*status));

	status->signal = fh->flags.rx.rssi;
#if KERNEL_VERSION(4, 7, 0) <= NRC_TARGET_KERNEL_VERSION
	status->boottime_ns = ktime_to_ns(ktime_get_boottime());
#endif
#if defined(CONFIG_S1G_CHANNEL)
	status->freq = (fh->info.rx.frequency) / 10;
	status->freq_offset = (fh->info.rx.frequency % 10 ? 1 : 0);
#else
	status->freq = fh->info.rx.frequency;
#endif /* defined(CONFIG_S1G_CHANNEL) */
	status->band = nw->band; /* I hate this */
	status->rate_idx = 0;
	if (fh->flags.rx.error_mic)
		status->flag |= RX_FLAG_MMIC_ERROR;
	if (fh->flags.rx.iv_stripped)
		status->flag |= RX_FLAG_IV_STRIPPED;

#if ((KERNEL_VERSION(4, 4, 132) <= NRC_TARGET_KERNEL_VERSION) && \
     (KERNEL_VERSION(4, 5, 0) > NRC_TARGET_KERNEL_VERSION)) ||   \
	(KERNEL_VERSION(4, 7, 0) <= NRC_TARGET_KERNEL_VERSION)
	if (mh->frame_control & 0x0400) {
		status->flag |= RX_FLAG_ALLOW_SAME_PN;
	}
#endif

	//update snr and rssi only if signal monitor is enabled
	nrc_stats_update(mh->addr2, fh->flags.rx.snr, fh->flags.rx.rssi);
	//nrc_stats_print();

	if (ieee80211_is_probe_resp(mh->frame_control))
		if (nrc_stats_channel_noise_update(
			    status->freq,
			    fh->flags.rx.rssi - fh->flags.rx.snr) < 0)
			DBG_MAC("Channel noise update fail : freq(%d)",
				status->freq);
}

static int nrc_vendor_ann_event(struct nrc *nw, const u8 *data, u16 len,
				enum nrc_vendor_event eid)
{
	struct ieee80211_hw *hw = nw->hw;
	struct sk_buff *skb;

	print_hex_dump(KERN_DEBUG, "event: ", DUMP_PREFIX_NONE, 16, 1, data,
		       len, false);

	skb = cfg80211_vendor_event_alloc(hw->wiphy, NULL, 255, eid,
					  GFP_KERNEL);

	if (!skb)
		return -ENOMEM;

	if (nla_put(skb, NRC_VENDOR_ATTR_DATA, len, data)) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	cfg80211_vendor_event(skb, GFP_KERNEL);

	return 0;
}

static int rx_h_vendor(struct nrc_trx_data *rx)
{
	struct ieee80211_hdr *mh = (void *)rx->skb->data;
	__le16 fc = mh->frame_control;
	u16 ies_offset = 0, len = 0;
	const u8 *pos, *data,
		ann_subs[] = {NRC_SUBCMD_WOWLAN_PATTERN, NRC_SUBCMD_ANNOUNCE1,
			      NRC_SUBCMD_ANNOUNCE2,	 NRC_SUBCMD_ANNOUNCE3,
			      NRC_SUBCMD_ANNOUNCE4,	 NRC_SUBCMD_ANNOUNCE5,
			      NRC_SUBCMD_REMOTECMD,	 NRC_SUBCMD_ANNOUNCE6,
			      NRC_SUBCMD_ANNOUNCE7,	 NRC_SUBCMD_ANNOUNCE8,
			      NRC_SUBCMD_ANNOUNCE9,	 NRC_SUBCMD_ANNOUNCE10,
			      NRC_SUBCMD_ANNOUNCE11,	 NRC_SUBCMD_ANNOUNCE12,
			      NRC_SUBCMD_ANNOUNCE13,	 NRC_SUBCMD_ANNOUNCE14,
			      NRC_SUBCMD_ANNOUNCE15,	 NRC_SUBCMD_ANNOUNCE16,
			      NRC_SUBCMD_ANNOUNCE17,	 NRC_SUBCMD_ANNOUNCE18,
			      NRC_SUBCMD_ANNOUNCE19,	 NRC_SUBCMD_ANNOUNCE20};
	const int OUIT_LEN = 4;
	u8 i;

	if (ieee80211_is_beacon(fc)
#if KERNEL_VERSION(5, 10, 0) <= NRC_TARGET_KERNEL_VERSION
	    || ieee80211_is_s1g_beacon(fc)
#endif /* KERNEL_VERSION(5, 10, 0) <= NRC_TARGET_KERNEL_VERSION */
	) {
		DBG_RX("Beacon(%d)", rx->nw->is_bcn_timeout);
		if (!rx->nw->params->disable_cqm && rx->nw->associated_vif) {
			if (rx->nw->is_bcn_timeout) {
				DBG_MAC("beacon receive, is_bcn_timeout to false");
				rx->nw->is_bcn_timeout = false;
			}
			mod_timer(&rx->nw->bcn_mon_timer,
				  jiffies + msecs_to_jiffies(
						    rx->nw->beacon_timeout));
		}

		ies_offset = offsetof(struct ieee80211_mgmt, u.beacon.variable);

	} else if (ieee80211_is_probe_req(fc)) {
		DBG_RX("Probe Request");
		ies_offset =
			offsetof(struct ieee80211_mgmt, u.probe_req.variable);
	} else if (ieee80211_is_probe_resp(fc)) {
		DBG_RX("Probe Response");
		ies_offset =
			offsetof(struct ieee80211_mgmt, u.probe_resp.variable);
	} else if (ieee80211_is_assoc_req(fc)) {
		DBG_RX("Assoc Request");
		ies_offset =
			offsetof(struct ieee80211_mgmt, u.assoc_req.variable);
	} else if (ieee80211_is_data(fc)) {
		DBG_RX("Data");
	}

	if (ies_offset == 0)
		goto done;

	for (i = 0; i < ARRAY_SIZE(ann_subs); i++) {
		pos = cfg80211_find_vendor_ie(VENDOR_OUI, ann_subs[i],
					      rx->skb->data + ies_offset,
					      rx->skb->len - ies_offset);

		if (pos) {
			len = *(pos + 1);
			data = pos + 2 /*ID(1)+LEN(1)*/ + OUIT_LEN;
			len -= OUIT_LEN;
			nrc_vendor_ann_event(rx->nw, data, len, ann_subs[i]);
		}
	}
done:
	return 0;
}

static void nrc_rx_handler(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct nrc_trx_data *rx = data;
	struct ieee80211_hdr *mh = (struct ieee80211_hdr *)rx->skb->data;
	struct ieee80211_sta *sta;
	const struct nrc_trx_handler *h;
	int res = 0;
	int i;

	rcu_read_lock();

	if (ieee80211_is_probe_req(mh->frame_control) ||
	    ieee80211_is_probe_resp(mh->frame_control) ||
	    ieee80211_is_assoc_req(mh->frame_control)) {
		res = rx_h_vendor(rx);
		if (res < 0)
			goto rxh_out;
	}
#if KERNEL_VERSION(5, 10, 0) <= NRC_TARGET_KERNEL_VERSION
	/*
	 * Beacon (Management) frame header format (type:0, subtype:8)
	 * fc(2) + duration(2) + addr1(da)(6) + addr2(sa)(6) + addr3(6) + ...
	 * S1G Beacon (extension) frame header format (type:3, subtype:1)
	 * fc(2) + duration(2) + sa(6) + timestamp(4) + change_seq(1) + ...
	 */
	if (!ieee80211_is_s1g_beacon(mh->frame_control)) {
		sta = ieee80211_find_all_sta(vif, mh->addr2);
	} else {
		struct ieee80211_ext *ext_h =
			(struct ieee80211_ext *)rx->skb->data;
		sta = ieee80211_find_all_sta(vif, ext_h->u.s1g_beacon.sa);
	}
#else
	sta = ieee80211_find_all_sta(vif, mh->addr2);
#endif /* KERNEL_VERSION(5, 10, 0) <= NRC_TARGET_KERNEL_VERSION */

#if defined(CONFIG_SUPPORT_BEACON_BYPASS)
	if (!rx->nw->params->enable_beacon_bypass) {
#endif /* CONFIG_SUPPORT_BEACON_BYPASS */
		if (!sta)
			goto rxh_out;
#if defined(CONFIG_SUPPORT_BEACON_BYPASS)
	}
#endif /* CONFIG_SUPPORT_BEACON_BYPASS */

	/* The received frame is from @sta to @vif */
	rx->vif = vif;
	rx->sta = sta;

	/* Call rx handlers */
	for (i = 0; i < nrc_rx_handlers_count; i++) {
		h = &nrc_rx_handlers[i];
		if (!(h->vif_types & BIT(vif->type)))
			continue;

		res = h->handler(rx);
		if (res < 0)
			goto rxh_out;
	}

rxh_out:
	rcu_read_unlock();
	rx->result = res;
}

static int nrc_mac_s1g_monitor_rx(struct nrc *nw, struct sk_buff *skb);

/**
 * nrc_mac_rx() - main rx routine
 *
 * @nw: points to NRC controller data
 * @skb: a received frame with the leading HIF header peeled off.
 */
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
	struct nrc_trx_data rx = {
		.nw = nw,
		.skb = skb,
	};
	struct frame_hdr *fh;
	struct ieee80211_hdr *mh;
	__le16 fc;
	int ret = 0;
	u64 now = 0, diff = 0;
	int eapol_msg;

	if (!NRC_DRV_IS_READY(nw->hdev) || atomic_read(&nw->hw_unregistering) ||
	    atomic_read(&nw->d_deauth.delayed_deauth)) {
		DBG_MAC("Target not ready or unregistering, discarding frame");
		/* Track FRAME SKB free (RX path from SPI) */
		NRC_SKB_TRACK_FREE(nw->hdev, skb, HIF_TYPE_FRAME, true, false);
		return 0;
	}

	nrc_mac_rx_h_status(nw, skb);

	if (nw->promisc) {
		ret = nrc_mac_s1g_monitor_rx(nw, skb);
		return ret;
	}

	fh = (void *)skb->data; /* frame header */

	/* Peel off frame header */
	skb_pull(skb, nw->hdev->fw.info.rx_head_size - sizeof(struct hif));
	mh = (void *)skb->data;
	fc = mh->frame_control;
	now = ktime_to_us(ktime_get_real());

	nrc_debug_print_frame(mh, 1);

#ifdef CONFIG_SUPPORT_ITERATE_INTERFACE
	/* Iterate over active interfaces */
	ieee80211_iterate_interfaces(nw->hw, IEEE80211_IFACE_ITER_ACTIVE,
				     nrc_rx_handler, &rx);
#else
	/* Iterate over active interfaces */
	ieee80211_iterate_active_interfaces(nw->hw, nrc_rx_handler, &rx);
#endif

	/**
	 * When associated, the scan results only include probe responses.
	 */
	if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_ACTIVE_SCANNING) {
		if (nw->associated_vif != NULL &&
		    nw->associated_vif->type == NL80211_IFTYPE_STATION &&
		    ieee80211_is_beacon(fc)) {
			/* Track FRAME SKB free (RX path from SPI) */
			NRC_SKB_TRACK_FREE(nw->hdev, skb, HIF_TYPE_FRAME, true,
					   false);
			return 0;
		}
	}

	diff = ktime_to_us(ktime_get_real()) - now;
	//if ((!diff) || (diff > NRC_MAC80211_RCU_LOCK_THRESHOLD))
	if (diff > NRC_MAC80211_RCU_LOCK_THRESHOLD)
		DBG_MAC("%s, diff=%lu", __func__, (unsigned long)diff);

	if (!rx.result) {
		if (!rx.nw->params->disable_cqm) {
			if (nw->associated_vif &&
			    (ieee80211_is_probe_resp(fc) ||
			     ieee80211_is_beacon(fc)
#if KERNEL_VERSION(5, 10, 0) <= NRC_TARGET_KERNEL_VERSION
			     || ieee80211_is_s1g_beacon(fc)
#endif /* KERNEL_VERSION(5, 10, 0) <= NRC_TARGET_KERNEL_VERSION */
				     ) &&
			    atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_IDLE) {
				mod_timer(&nw->bcn_mon_timer,
					  jiffies +
						  msecs_to_jiffies(
							  nw->beacon_timeout));
			}
		}

		if ((ieee80211_is_assoc_req(mh->frame_control) ||
		     ieee80211_is_reassoc_req(mh->frame_control)) &&
		    rx.sta) {
			struct nrc_sta *i_sta = to_i_sta(rx.sta);
			struct ieee80211_mgmt *mgmt =
				(struct ieee80211_mgmt *)skb->data;
			i_sta->listen_interval =
				mgmt->u.assoc_req.listen_interval;
		}

		/* Modified assoc issue after STA OFF/ON (connection without deauth) */
		if (ieee80211_is_auth(mh->frame_control) && rx.sta && rx.vif &&
		    rx.vif->type == NL80211_IFTYPE_AP) {
			struct nrc_sta *i_sta = to_i_sta(rx.sta);
			if (i_sta && i_sta->state > IEEE80211_STA_NOTEXIST) {
				struct ieee80211_mgmt *mgmt =
					(struct ieee80211_mgmt *)skb->data;
				struct sk_buff *skb_deauth;
				/* Pretend to receive a deauth from @sta */
				skb_deauth = ieee80211_deauth_get(
					nw->hw, mgmt->bssid, mgmt->sa,
					mgmt->bssid, WLAN_REASON_DEAUTH_LEAVING,
					NULL, false);
				if (!skb_deauth) {
					DBG_STATE("%s Fail to alloc skb",
						  __func__);
					/* Track FRAME SKB free (RX path from SPI) */
					NRC_SKB_TRACK_FREE(nw->hdev, skb,
							   HIF_TYPE_FRAME, true,
							   false);
					return 0;
				}
				DBG_MAC("%s: %pM connected, but auth received. send to kernel after convert auth -> deauth.",
					__func__, mgmt->sa);
				ieee80211_rx_irqsafe(nw->hw, skb_deauth);
				/* Track FRAME SKB free (RX path from SPI) */
				NRC_SKB_TRACK_FREE(nw->hdev, skb,
						   HIF_TYPE_FRAME, true, false);
				return 0;
			}
		}

#if NRC_DBG_PRINT_ARP_FRAME
		if (ieee80211_is_data_qos(fc)) {
			/* find to LLC TYPE field to check ARP */
			uint8_t *ptr;
			uint16_t llc_type;
			uint8_t ccmp_hdr = 0;
			if (ieee80211_has_protected(fc)) {
				ccmp_hdr = 8;
			}
			ptr = (uint8_t *)mh +
			      (ieee80211_hdrlen(fc) + ccmp_hdr + 6); //6(LLC)
			llc_type = *((uint16_t *)ptr);
			if (llc_type == htons(ETH_P_ARP)) {
				DBG_PS("[%s] RX ARP [type:%d sype:%d, protected:%d, len:%d]",
				       __func__, WLAN_FC_GET_TYPE(fc),
				       WLAN_FC_GET_STYPE(fc),
				       ieee80211_has_protected(fc),
				       rx.skb->len);
			}
		}
#endif

		trace_nrc_rx_hdr(nw, rx.skb->data, rx.skb->len,
				 fh->flags.rx.snr, fh->flags.rx.rssi);
		trace_nrc_rx_payload(nw, rx.skb->data, rx.skb->len);

		eapol_msg = is_eapol(rx.skb, nw);
		if (eapol_msg) {
			DBG_MAC("RX EAPOL(%d), ADDR1: %pM, ADDR2: %pM",
				eapol_msg, mh->addr1, mh->addr2);
			/* key exchange and install key */
			nrc_ps_dyn_start_custom_timeout(nw, 1000);
		}

		/* During hw_scan, log PROBE_RESP and pass to mac80211.
		 * Let mac80211/cfg80211 handle BSS registration to maintain
		 * proper scan session context.
		 */
		if (ieee80211_is_probe_resp(fc) &&
		    (atomic_read(&nw->scan_mode) ==
			     NRC_SCAN_MODE_ACTIVE_SCANNING ||
		     atomic_read(&nw->scan_mode) ==
			     NRC_SCAN_MODE_PASSIVE_SCANNING)) {
			struct ieee80211_rx_status *rxs =
				IEEE80211_SKB_RXCB(rx.skb);
			struct ieee80211_channel *channel;

			/* Get channel for logging */
			channel =
				ieee80211_get_channel(nw->hw->wiphy, rxs->freq);
			if (channel) {
				DBG_MAC("SCAN: %pM ch=%d freq=%d rssi=%d",
					mh->addr2, channel->hw_value, rxs->freq,
					rxs->signal);
			}
		}

		/* Track FRAME SKB before passing to mac80211 (ownership transfer) */
		/* Note: mac80211 will free this SKB, so count only without freeing */
		NRC_SKB_TRACK_FREE(nw->hdev, NULL, HIF_TYPE_FRAME, true, true);
		ieee80211_rx_irqsafe(nw->hw, rx.skb);

		if (ieee80211_is_data(fc))
			nrc_ps_dyn_start(nw);

		if (!ieee80211_hw_check(nw->hw, SUPPORTS_PS)) {
			if (nw->invoke_beacon_loss) {
				nw->invoke_beacon_loss = false;
				nrc_send_beacon_loss(nw);
			}
		}
	} else {
		ERR_WLAN("RX handler error (%d)", rx.result);
		NRC_SKB_TRACK_FREE(nw->hdev, skb, HIF_TYPE_FRAME, true, false);
	}

	return 0;
}

/**
 * rx_h_decrypt() - increase the length of a frame by icv length
 *
 * TODO:
 * - Verify if using key information stored in i_sta is valid.
 * - Consider using ieee80211_iter_key to find the key.
 * - What if multiple keys for a station?
 */
static int rx_h_decrypt(struct nrc_trx_data *rx)
{
	struct ieee80211_hdr *mh = (void *)rx->skb->data;
	struct nrc *nw = rx->nw;
	struct ieee80211_rx_status *status;
	struct ieee80211_key_conf *key;
	struct nrc_sta *i_sta;
	struct nrc_vif *i_vif;
	int vif_id;
	__le16 fc;

	if (!rx->sta) {
		return 0;
	}

	i_sta = to_i_sta(rx->sta);
	i_vif = to_i_vif(rx->vif);
	vif_id = i_vif->index;
	fc = mh->frame_control;

	/* not proteced frame, just skip */
	if (!ieee80211_has_protected(fc))
		return 0;

	/* if SW SECURITY, just skip */
	if (!(nw->hdev->cap.vif_caps[vif_id].cap_mask & WIM_SYSTEM_CAP_HWSEC) ||
	    rx->nw->params->sw_enc == WIM_ENCDEC_SW) {
		return 0;
	}

	/*
	 * @skb->len from the target does not account for ICV/MIC.
	 * Since mac80211 trims it if RX_FLAG_IV_STRIPPED, we need to
	 * increment @skb->len by that amount.
	 */
	key = is_multicast_ether_addr(mh->addr1) ? i_sta->gtk : i_sta->ptk;

	if (!key) {
		/* driver does NOT have GTK on HYBRID SEC mode */
		if (nw->hdev->cap.vif_caps[vif_id].cap_mask &
		    WIM_SYSTEM_CAP_HYBRIDSEC) {
			if (ieee80211_is_data(mh->frame_control) &&
			    is_multicast_ether_addr(mh->addr1)) {
				return 0;
			}
		}
		WARN_WLAN("key is NULL");
		return 0;
	}

	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:
	case WLAN_CIPHER_SUITE_TKIP:
	case WLAN_CIPHER_SUITE_CCMP:
#ifdef CONFIG_SUPPORT_CCMP_256
	case WLAN_CIPHER_SUITE_CCMP_256:
#endif
#ifdef CONFIG_SUPPORT_GCMP
	case WLAN_CIPHER_SUITE_GCMP:
	case WLAN_CIPHER_SUITE_GCMP_256:
#endif
#ifdef CONFIG_SUPPORT_GMAC
	case WLAN_CIPHER_SUITE_BIP_GMAC_128:
	case WLAN_CIPHER_SUITE_BIP_GMAC_256:
#endif

		rx->skb->len += key->icv_len;
		break;
	default:
		DBG_MAC("%s: unknown cipher (%d)", __func__, key->cipher);
		return 0;
	}

	/*
	 * Target stripped ICV/MIC, but not IV.
	 * We do not set RX_FLAG_IV_STRPPED.
	 */
	status = IEEE80211_SKB_RXCB(rx->skb);
	status->flag |= RX_FLAG_DECRYPTED;
	status->flag |= RX_FLAG_MMIC_STRIPPED;

	return 0;
}

#if KERNEL_VERSION(4, 6, 0) <= NRC_TARGET_KERNEL_VERSION
static int rx_h_check_sn(struct nrc_trx_data *rx)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data;
	__le16 fc;

	if (!rx->sta) {
		return 0;
	}

	/* BA session sequence number inversion workaround.
	 * Over 2048 data frames can be transmitted while the peer is far away
	 * but enough to keep connection. When the peer comes back close to transmitter,
	 * the received packets will be dropped in mac80211
	 * because 2048 sequence numbers are already missed in air.
	 * SN will be recovered after it increses by 4096.
	 * This is a normal operation in protocol but may cause problem in usecases.
	 * So, we need to check SN and clear BA session if necessary.
	 * Allow additional AMPDU buffer size since we can't get a head SN of the BA session.
	 * SN Inversion: 2048 < SN Diff < 4096 - AMPDU Buffer
	 */

	fc = hdr->frame_control;
	if (ieee80211_is_data_qos(fc) && !is_multicast_ether_addr(hdr->addr1)) {
		struct nrc_sta *i_sta = to_i_sta(rx->sta);
#if KERNEL_VERSION(4, 17, 0) <= NRC_TARGET_KERNEL_VERSION
		u8 tid = ieee80211_get_tid(hdr);
#else
		u8 *qc = ieee80211_get_qos_ctl(hdr);
		u8 tid = qc[0] & IEEE80211_QOS_CTL_TID_MASK;
#endif
		if (tid < NRC_MAX_TID && i_sta->rx_ba_session[tid].started) {
			u16 sn = (le16_to_cpu(hdr->seq_ctrl) &
				  IEEE80211_SCTL_SEQ) >>
				 4;
			u16 sn_diff = (sn - i_sta->rx_ba_session[tid].sn) &
				      IEEE80211_SN_MASK;
			if (ieee80211_sn_less(sn,
					      i_sta->rx_ba_session[tid].sn) &&
			    sn_diff <=
				    IEEE80211_SN_MODULO -
					    i_sta->rx_ba_session[tid].buf_size) {
				ieee80211_mark_rx_ba_filtered_frames(
					rx->sta, tid, sn, 0,
					IEEE80211_SN_MODULO >> 1);
				DBG_MAC("BA session[%d] SN inversion! last_sn:%d, sn:%d",
					tid, i_sta->rx_ba_session[tid].sn, sn);
			}
			i_sta->rx_ba_session[tid].sn = sn;
		}
	}

	return 0;
}

#endif

#if defined(CONFIG_SUPPORT_IBSS)
extern u64 current_bssid_beacon_timestamp;
static int rx_h_ibss_get_bssid_tsf(struct nrc_trx_data *rx)
{
	struct ieee80211_mgmt *mgmt = (void *)rx->skb->data;
	__le16 fc = mgmt->frame_control;

	if (rx->vif->type != NL80211_IFTYPE_ADHOC)
		return 0;

	if (rx->vif->type == NL80211_IFTYPE_ADHOC && ieee80211_is_beacon(fc) &&
	    !memcmp(mgmt->bssid, rx->vif->bss_conf.bssid, ETH_ALEN)) {
		current_bssid_beacon_timestamp =
			le64_to_cpu(mgmt->u.beacon.timestamp);
		return 0;
	}

	return 0;
}

#endif /* CONFIG_SUPPORT_IBSS */

static int rx_h_action(struct nrc_trx_data *rx)
{
	struct nrc *nw = rx->nw;
	//struct ieee80211_hw *hw = nw->hw;
	struct ieee80211_sta *sta = rx->sta;
	//struct ieee80211_twt_info *twt_info;

	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)rx->skb->data;
	__le16 fc = mgmt->frame_control;

	if (likely(!ieee80211_is_action(fc))) {
		return 0;
	}

	if (unlikely(mgmt->u.action.category != WLAN_CATEGORY_S1G))
		return 0;

	/* Currently, No use twt ops of the mac80211 upper layer that is supported from version 5.15.56 */
	switch (mgmt->u.action.u.chan_switch
			.action_code) { /* same struct format */
	case WLAN_S1G_TWT_SETUP: /* 6 */
		DBG_MAC("Action: TWT Setup");
		nrc_mac_rx_twt_setup(nw, sta, mgmt);
		break;
	case WLAN_S1G_TWT_TEARDOWN: /* 7 */
		DBG_MAC("Action: TWT Teardown");
		nrc_mac_rx_twt_teardown(nw, sta, mgmt);
		break;
	case WLAN_S1G_TWT_INFORMATION: /* 11 */
		DBG_MAC("Action: TWT Information");
		//twt_info = (void *)mgmt->u.action.u.s1g.variable;
		//nrc_mac_twt_info_recv(hw, sta, twt_info);
		break;
	}

	return 0;
}

static int rx_h_twt_assoc(struct nrc_trx_data *rx)
{
	struct nrc *nw = rx->nw;
	struct ieee80211_sta *sta = rx->sta;

	struct sk_buff *skb = rx->skb;
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
	__le16 fc = mgmt->frame_control;

	if (likely(nw->twt_responder == false)) {
		goto done;
	}

	if (likely(!ieee80211_is_assoc_req(fc))) {
		goto done;
	}

	nrc_mac_rx_twt_setup_assoc_req(nw, sta, mgmt, skb->len);

done:
	return 0;
}

static int rx_h_twt_monitor(struct nrc_trx_data *rx)
{
	struct nrc *nw = rx->nw;
	struct ieee80211_sta *sta = rx->sta;

	struct sk_buff *skb = rx->skb;
	struct ieee80211_hdr *hdr = (void *)skb->data;
	__le16 fc = hdr->frame_control;
	u8 *qc;

	if (likely(nw->twt_responder == false)) {
		goto done;
	}

	if (likely(!ieee80211_is_qos_nullfunc(fc))) {
		goto done;
	}

	qc = ieee80211_get_qos_ctl(hdr);

	/*
	   to make sure if service is started or stopped,
	   check this field(bit7) which is reserved in QoS Null frame
	   since QoS Null frame is used for other functions.
	 */
	if (!(*qc & IEEE80211_QOS_CTL_A_MSDU_PRESENT)) {
		goto done;
	}

	if (ieee80211_has_pm(fc)) {
		nrc_mac_twt_sp_update(nw, sta, 1);
	} else {
		nrc_mac_twt_sp_update(nw, sta, 0);
	}

done:
	return 0;
}

#if NRC_DBG_PRINT_FRAME_RX
static int rx_h_debug_print(struct nrc_trx_data *rx)
{
	struct ieee80211_sta *sta;
	struct ieee80211_hdr *hdr;
	struct nrc_sta *i_sta;
	struct nrc_vif *i_vif;
	struct ieee80211_rx_status *status;
	__le16 fc;

	if (!rx) {
		DBG_MAC("[%s] rx is NULL", __func__);
		return 0;
	}

	if (!rx->sta) {
		DBG_MAC("[%s] rx->sta is NULL", __func__);
		return 0;
	}

	i_sta = to_i_sta(rx->sta);
	i_vif = to_i_vif(rx->vif);
	hdr = (struct ieee80211_hdr *)rx->skb->data;
	fc = hdr->frame_control;

	DBG_MAC("[%s] %s vif:%d type:%d sype:%d, protected:%d", __func__,
		(rx->vif->type == NL80211_IFTYPE_STATION) ? "STA" :
		(rx->vif->type == NL80211_IFTYPE_AP)	  ? "AP" :
							    "MONITORP",
		i_vif->index, WLAN_FC_GET_TYPE(fc), WLAN_FC_GET_STYPE(fc),
		ieee80211_has_protected(fc));

	if (is_unicast_ether_addr(hdr->addr2)) {
		sta = ieee80211_find_sta(rx->vif, hdr->addr2);
		if (!sta) {
			DBG_MAC("[%s] Unable to find %pM in mac80211", __func__,
				hdr->addr2);
			return 0;
		}
	}

	i_sta = to_i_sta(sta);
	if (!i_sta) {
		DBG_MAC("[%s] Unable to find %pM in nrc drv", __func__,
			hdr->addr2);
		return 0;
	}

	// set filter here : add filter as you wish
	//eg. ieee80211_is_data(fc) ieee80211_is_mgmt(fc)
	//eg. ieee80211_is_assoc_req
	if (ieee80211_is_deauth(fc)) {
		status = IEEE80211_SKB_RXCB(rx->skb);
		DBG_MAC("%s: status->flag:%d", __func__, status->flag);
		print_hex_dump(KERN_DEBUG,
			       "rx deauth frame: ", DUMP_PREFIX_NONE, 16, 1,
			       rx->skb->data, rx->skb->len, false);
	}

	return 0;
}

#endif //#if NRC_DBG_PRINT_FRAME_RX

#ifdef CONFIG_SUPPORT_MESH_ROUTING
static int rx_h_mesh(struct nrc_trx_data *rx)
{
	struct ieee80211_mgmt *mgmt;
	struct nrc_sta *i_sta;
	__le16 fc;
	int rssi = nrc_stats_get_mesh_rssi_threshold();
	bool is_mesh_action = false;

	if (!rx->sta) {
		return 0;
	}

	i_sta = to_i_sta(rx->sta);
	mgmt = (void *)rx->skb->data;
	fc = mgmt->frame_control;

	if (i_sta && i_sta->state == IEEE80211_STA_AUTHORIZED &&
	    ieee80211_is_action(fc)) {
		if (ieee80211_has_protected(fc) &&
		    rx->nw->params->sw_enc != WIM_ENCDEC_HW) {
			is_mesh_action = true;
		} else {
			if (mgmt->u.action.category ==
			    WLAN_CATEGORY_MESH_ACTION) {
				if (mgmt->u.action.u.mesh_action.action_code ==
				    WLAN_MESH_ACTION_HWMP_PATH_SELECTION) {
					is_mesh_action = true;
				}
			}
		}
	}

	if (is_mesh_action) {
		if (nrc_stats_metric(rx->sta->addr) < nrc_stats_calc_metric()) {
			DBG_MAC("%s: LOW RSSI!(< %d) drop mesh action from %pM",
				__func__, rssi, rx->sta->addr);
			return -1;
		}
	}

	return 0;
}

#endif

/**
 * nrc_mac_trx_init() - Initialize TRX data path
 *
 *
 * TODO:
 * Handlers are registered during compile time, not
 * via nrc_mac_{tx,rx}_h_register().
 */
void nrc_mac_trx_init(struct nrc *nw)
{
	struct nrc_hif_device *hdev = nw->hdev;
	int i;

	spin_lock_init(&nw->txq_lock);
	INIT_LIST_HEAD(&nw->txq);
	for (i = 0; i < ARRAY_SIZE(hdev->credit.tx_credit); i++) {
		atomic_set(&hdev->credit.tx_credit[i], 0);
		atomic_set(&hdev->credit.tx_pend[i], 0);
	}
}

#ifdef CONFIG_NRC_HIF_DUMP_S1G_RXINFO

/* S1G monitor mode */
#define RXIF                                                       \
	"-- RX Info (v:%d ok:%d crc:%d agg:%d ndp:%d mpdu_len:%d " \
	"timestamp:%d fcs:%#x bw:%d ptype:%d format:%d rcpi:%d)"

#define SIG1MF                                          \
	"-- S1G1M (len:%d agg:%d ndp:%d rsp:%d mcs:%d " \
	"coding:%d stbc:%d short_gi:%d)"

#define SIGSHORTF                                         \
	"-- S1GShort (len:%d id:%d agg:%d ndp:%d rsp:%d " \
	"mcs:%d coding:%d stbc:%d short_gi:%d bw:%d ul_ind:%d)"

static void nrc_dump_s1g_sig_rxinfo(struct sk_buff *skb)
{
	struct sigS1g *sig_s1g;
	struct rxInfo *rxi;
	bool s1g_1m_ppdu;

	sig_s1g = (void *)(skb->data + sizeof(struct frame_hdr));
	rxi = (void *)(sig_s1g + 1);

	s1g_1m_ppdu =
		rxi->bandwidth == WIM_BW_1M ||
		(rxi->bandwidth == WIM_BW_2M && rxi->format == WIM_S1G_DUP_1M);

	DBG_MAC(RXIF, rxi->valid, rxi->okay, rxi->error_crc, rxi->aggregation,
		rxi->ndp_ind, rxi->mpdu_length, rxi->timestamp, rxi->fcs,
		rxi->bandwidth, rxi->preamble_type, rxi->format, rxi->rcpi);

	if (rxi->bandwidth == WIM_BW_1M ||
	    (rxi->bandwidth == WIM_BW_2M && rxi->format == WIM_S1G_DUP_1M)) {
		/* 1M or 1M dup (or workaround : 2M & 1M_DUP) */
		DBG_MAC(SIG1MF, sig_s1g->flags.s1g1M.length,
			sig_s1g->flags.s1g1M.aggregation,
			sig_s1g->flags.s1g1M.ndp_ind,
			sig_s1g->flags.s1g1M.response_ind,
			sig_s1g->flags.s1g1M.mcs, sig_s1g->flags.s1g1M.coding,
			sig_s1g->flags.s1g1M.stbc,
			sig_s1g->flags.s1g1M.short_gi);
	} else {
		/* 2/4/8/16M */
		DBG_MAC(SIGSHORTF, sig_s1g->flags.s1gShort.length,
			sig_s1g->flags.s1gShort.id,
			sig_s1g->flags.s1gShort.aggregation,
			sig_s1g->flags.s1gShort.ndp_ind,
			sig_s1g->flags.s1gShort.response_ind,
			sig_s1g->flags.s1gShort.mcs,
			sig_s1g->flags.s1gShort.coding,
			sig_s1g->flags.s1gShort.stbc,
			sig_s1g->flags.s1gShort.short_gi,
			sig_s1g->flags.s1gShort.bandwidth,
			sig_s1g->flags.s1gShort.uplink_ind);
	}
}
#endif

#define NRC_CHAN_700MHZ 0x0001
#define NRC_CHAN_800MHZ 0x0002
#define NRC_CHAN_900MHZ 0x0004

#define MON_STA_LIST_NUM 32

typedef struct _mon_sta_t {
	struct list_head list;
	uint8_t addr[6];
	uint16_t scrambler;
	uint8_t rcpi;
	uint32_t first_ts;
	uint32_t last_ts;
} MON_STA_T;

static struct list_head m_sta_head[MON_STA_LIST_NUM];
static uint32_t m_ampdu_refnum;

void nrc_ampdu_mon_init(void)
{
	int i;

	for (i = 0; i < MON_STA_LIST_NUM; i++) {
		INIT_LIST_HEAD(&m_sta_head[i]);
	}
	m_ampdu_refnum = 0;
	DBG_AMPDU("Init AMPDU monitor");
}

void nrc_ampdu_mon_deinit(void)
{
	int i;
	MON_STA_T *cur, *next;

	for (i = 0; i < MON_STA_LIST_NUM; i++) {
		list_for_each_entry_safe(cur, next, &m_sta_head[i], list)
		{
			list_del(&cur->list);
			DBG_AMPDU("%s, Del m_sta: " MACSTR "", __func__,
				  MAC2STR(cur->addr));
			kfree(cur);
		}
	}
	m_ampdu_refnum = 0;
	DBG_AMPDU("%s, Done deinit AMPDU monitor ", __func__);
}

static MON_STA_T *nrc_ampdu_mon_find_sta(uint8_t *addr)
{
	MON_STA_T *cur, *next;

	if (list_empty(&m_sta_head[addr[ETH_ALEN - 1] % MON_STA_LIST_NUM]) !=
	    0) {
		DBG_AMPDU("%s, No m_sta added", __func__);
		return 0;
	}

	list_for_each_entry_safe(
		cur, next, &m_sta_head[addr[ETH_ALEN - 1] % MON_STA_LIST_NUM],
		list)
	{
		if (ether_addr_equal(cur->addr, addr))
			return cur;
	}

	return NULL;
}

static MON_STA_T *nrc_ampdu_mon_add_sta(uint8_t *addr)
{
	MON_STA_T *m_sta = kzalloc(sizeof(MON_STA_T), GFP_KERNEL);
	if (!m_sta) {
		DBG_AMPDU("%s, Failure: kzalloc ", __func__);
		return NULL;
	}
	memcpy(m_sta->addr, addr, 6);
	list_add_tail(&m_sta->list,
		      &m_sta_head[addr[ETH_ALEN - 1] % MON_STA_LIST_NUM]);
	DBG_AMPDU("%s, Success m_sta alloc " MACSTR " ", __func__,
		  MAC2STR(addr));
	return m_sta;
}

static void nrc_ampdu_mon_inc_refnum(void)
{
	m_ampdu_refnum++;
	if (m_ampdu_refnum == 0)
		m_ampdu_refnum = 1;
}

static uint32_t nrc_ampdu_mon_get_ref_id(struct nrc *nw, struct sk_buff *skb)
{
	struct sigS1g *sig_s1g;
	struct rxInfo *rxi;
	MON_STA_T *m_sta;

	struct frame_hdr *fh;
	struct ieee80211_hdr *mh;
	fh = (void *)skb->data;
	mh = (void *)(skb->data + nw->hdev->fw.info.rx_head_size -
		      sizeof(struct hif));

	sig_s1g = (void *)(skb->data + sizeof(struct frame_hdr));
	rxi = (void *)(sig_s1g + 1);

	m_sta = nrc_ampdu_mon_find_sta(mh->addr2);

	if (!m_sta) {
		m_sta = nrc_ampdu_mon_add_sta(mh->addr2);
		if (!m_sta) {
			DBG_AMPDU("%s, Failure: alloc monitor sta:" MACSTR "",
				  __func__, MAC2STR(mh->addr2));
			return 0;
		}
		m_sta->scrambler = rxi->scrambler_or_crc;
		m_sta->rcpi = rxi->rcpi;
		m_sta->first_ts = rxi->timestamp;
		m_sta->last_ts = rxi->timestamp;
		nrc_ampdu_mon_inc_refnum();
		DBG_AMPDU("" MACSTR " scramble#:%d, refnum:%d",
			  MAC2STR(m_sta->addr), m_sta->scrambler,
			  m_ampdu_refnum);
		return m_ampdu_refnum;
	}

	if (m_sta->scrambler == rxi->scrambler_or_crc) {
		uint32_t ts_diff = rxi->timestamp - m_sta->last_ts;
		if (ts_diff < 5000) { /* 5ms */
			m_sta->last_ts = rxi->timestamp;
			DBG_AMPDU("" MACSTR
				  " Same scramble#:%d, use same refnum:%d\n",
				  MAC2STR(m_sta->addr), m_sta->scrambler,
				  m_ampdu_refnum);
			return m_ampdu_refnum;
		} else {
			m_sta->first_ts = rxi->timestamp;
			m_sta->last_ts = rxi->timestamp;
			nrc_ampdu_mon_inc_refnum();
			DBG_AMPDU("" MACSTR " scramble:%d, new refnum:%d  ",
				  MAC2STR(m_sta->addr), m_sta->scrambler,
				  m_ampdu_refnum);
			return m_ampdu_refnum;
		}
	} else {
		m_sta->scrambler = rxi->scrambler_or_crc;
		m_sta->rcpi = rxi->rcpi;
		m_sta->first_ts = rxi->timestamp;
		m_sta->last_ts = rxi->timestamp;
		nrc_ampdu_mon_inc_refnum();
		DBG_AMPDU("" MACSTR " different scramble:%d, new refnum:%d",
			  MAC2STR(m_sta->addr), m_sta->scrambler,
			  m_ampdu_refnum);
		return m_ampdu_refnum;
	}
}

static void nrc_add_rx_s1g_radiotap_header(struct nrc *nw, struct sk_buff *skb)
{
	struct nrc_radiotap_hdr rt_hdr, *prt_hdr;
	struct frame_hdr *fh;
	struct sigS1g *sig_s1g;
	struct rxInfo *rxi;
	u32 mcs = 0, response_ind = 0;
	bool short_gi = false, s1g_1m_ppdu;
	bool ndp_ind, agg = false, error_crc = false;
	uint32_t fcs;
	uint8_t *p;
	struct nrc_radiotap_hdr_agg rt_hdr_agg, *prt_hdr_agg;
	struct nrc_radiotap_hdr_ndp rt_hdr_ndp, *prt_hdr_ndp;
	uint8_t color = 0;
	uint8_t uplink_ind = 0;

	fh = (void *)skb->data;
	sig_s1g = (void *)(skb->data + sizeof(struct frame_hdr));
	rxi = (void *)(sig_s1g + 1);
	ndp_ind = (bool)rxi->ndp_ind;
	error_crc = (bool)rxi->error_crc;
	fcs = rxi->fcs;

	/*
	 * Updates the rxi->rcpi value
	 * that was incorrectly changed when transmitted from target
	 * to the rssi value of the frame header.
	 */
	rxi->rcpi = (int8_t)fh->flags.rx.rssi;

#ifdef CONFIG_NRC_HIF_DUMP_S1G_RXINFO
	nrc_dump_s1g_sig_rxinfo(skb);
#endif

	s1g_1m_ppdu =
		rxi->bandwidth == WIM_BW_1M ||
		(rxi->bandwidth == WIM_BW_2M && rxi->format == WIM_S1G_DUP_1M);

	if (!ndp_ind) {
		if (s1g_1m_ppdu) {
			/* 1M or 1M dup (or workaround : 2M & 1M_DUP) */
			mcs = sig_s1g->flags.s1g1M.mcs;
			short_gi = sig_s1g->flags.s1g1M.short_gi & 0x1;
			response_ind = sig_s1g->flags.s1g1M.response_ind & 0x3;
			agg = sig_s1g->flags.s1g1M.aggregation;
		} else {
			/* 2/4/8/16M */
			mcs = sig_s1g->flags.s1gShort.mcs;
			short_gi = sig_s1g->flags.s1gShort.short_gi & 0x1;
			response_ind = sig_s1g->flags.s1gShort.response_ind &
				       0x3;
			agg = sig_s1g->flags.s1gShort.aggregation;
			color = sig_s1g->flags.s1gShort.id & 0x7;
			uplink_ind = sig_s1g->flags.s1gShort.uplink_ind;
		}
	} else {
		/* NDP frame */
		skb_put(skb, 6);
		p = (uint8_t *)(rxi + 1);
		*p = ((*(char *)sig_s1g & 0x07) == 0x07) ? 0xF0 : 0x04;
		/*
		 * README: Remove reserved field of 2MHz NDP frame
		 *			  Aaron - 2019-0412 - Gerrit#1973
		 */
		if (!s1g_1m_ppdu) {
			memcpy((char *)(p + 1), (char *)&sig_s1g->flags, 3);
			memcpy((char *)(p + 4), (char *)&sig_s1g->flags + 4, 2);
		} else
			memcpy((void *)(p + 1), (void *)&sig_s1g->flags, 5);

		*(p + 5) = (*(p + 5) & 0x3F) | ((s1g_1m_ppdu) ? 0 : (0x2 << 6));
	}

	/**
	 * README: Change RSSI value refernce - Aaron - 2019-0531
	 *   original: RCPI based calculation - RSSI = (RCPI/2)-122
	 *   new: replace RCPI with RSSI value of Rx Vector
	 **/
	/* prt_hdr->rt_rssi = rxi->rcpi; */

	if (ndp_ind) {
		rt_hdr_ndp.hdr.it_version = PKTHDR_RADIOTAP_VERSION;
		rt_hdr_ndp.hdr.it_pad = 0;

		rt_hdr_ndp.rt_tsft = cpu_to_le64(rxi->timestamp);
		/* frame include FCS */
		rt_hdr_ndp.rt_flags = ((ndp_ind) ? 0x00 : 0x10);
		rt_hdr_ndp.rt_flags |= ((error_crc) ? 0x40 : 0x00);
		rt_hdr_ndp.rt_ch_frequency = cpu_to_le16(rxi->center_freq);
		rt_hdr_ndp.rt_ch_flags =
			cpu_to_le16(IEEE80211_CHAN_OFDM | NRC_CHAN_900MHZ);

		rt_hdr_ndp.hdr.it_len = cpu_to_le64(sizeof(rt_hdr_ndp));
		rt_hdr_ndp.hdr.it_present = cpu_to_le32(
			BIT(IEEE80211_RADIOTAP_TSFT) |
			BIT(IEEE80211_RADIOTAP_FLAGS) |
			BIT(IEEE80211_RADIOTAP_CHANNEL) |
			BIT(IEEE80211_RADIOTAP_DBM_ANTSIGNAL) |
			/* BIT(IEEE80211_RADIOTAP_0_LENGTH_PSDU)); */
			BIT(26));
		/* S1G NDP CMAC frame */
		rt_hdr_ndp.rt_rssi = rxi->rcpi;
		rt_hdr_ndp.rt_zero_length_psdu = 0x02;
		rt_hdr_ndp.rt_pad = 0;
	} else if (agg) {
		rt_hdr_agg.hdr.it_version = PKTHDR_RADIOTAP_VERSION;
		rt_hdr_agg.hdr.it_pad = 0;

		rt_hdr_agg.rt_tsft = cpu_to_le64(rxi->timestamp);
		/* frame include FCS */
		rt_hdr_agg.rt_flags = ((ndp_ind) ? 0x00 : 0x10);
		rt_hdr_agg.rt_flags |= ((error_crc) ? 0x40 : 0x00);
		rt_hdr_agg.rt_ch_frequency = cpu_to_le16(rxi->center_freq);
		rt_hdr_agg.rt_ch_flags =
			cpu_to_le16(IEEE80211_CHAN_OFDM | NRC_CHAN_900MHZ);

		rt_hdr_agg.hdr.it_len = cpu_to_le64(sizeof(rt_hdr_agg));
		rt_hdr_agg.hdr.it_present =
			cpu_to_le32(BIT(IEEE80211_RADIOTAP_TSFT) |
				    BIT(IEEE80211_RADIOTAP_FLAGS) |
				    BIT(IEEE80211_RADIOTAP_CHANNEL) |
				    BIT(IEEE80211_RADIOTAP_AMPDU_STATUS) |
				    BIT(28)); //BIT(IEEE80211_RADIOTAP_TLV));
		rt_hdr_agg.rt_ampdu_ref = nrc_ampdu_mon_get_ref_id(nw, skb);
		rt_hdr_agg.rt_ampdu_flags = 2;
		rt_hdr_agg.rt_ampdu_crc = 3;
		rt_hdr_agg.rt_ampdu_reserved = 0;
		rt_hdr_agg.rt_pad = 0;
		rt_hdr_agg.rt_pad2 = 0;
		rt_hdr_agg.rt_tlv_type = cpu_to_le16(32); /* S1G */
		rt_hdr_agg.rt_tlv_length = cpu_to_le16(6);
		rt_hdr_agg.rt_s1g_known = 0x007F;
		rt_hdr_agg.rt_s1g_data1 =
			((rxi->bandwidth == 0) ?
				 0x0 :
				 ((rxi->preamble_type) ? 0x2 : 0x1)) |
			((response_ind & 0x03) << 2) | /* RI */
			((short_gi & 0x01) << 5) | /* GI */
			0x0 << 6 | /* NSS */
			((rxi->bandwidth & 0x0f) << 8) | /* BW */
			((mcs & 0x0f) << 12); /* MCS */
		rt_hdr_agg.rt_s1g_data2 = cpu_to_le16(
			(color & 0x07) | ((uplink_ind & 0x01) << 3) |
			(rxi->rcpi << 8));
	} else {
		rt_hdr.hdr.it_version = PKTHDR_RADIOTAP_VERSION;
		rt_hdr.hdr.it_pad = 0;

		rt_hdr.rt_tsft = cpu_to_le64(rxi->timestamp);
		/* frame include FCS */
		rt_hdr.rt_flags = ((ndp_ind) ? 0x00 : 0x10);
		rt_hdr.rt_flags |= ((error_crc) ? 0x40 : 0x00);
		rt_hdr.rt_ch_frequency = cpu_to_le16(rxi->center_freq);
		rt_hdr.rt_ch_flags =
			cpu_to_le16(IEEE80211_CHAN_OFDM | NRC_CHAN_900MHZ);

		rt_hdr.hdr.it_len = cpu_to_le64(sizeof(rt_hdr));
		rt_hdr.hdr.it_present =
			cpu_to_le32(BIT(IEEE80211_RADIOTAP_TSFT) |
				    BIT(IEEE80211_RADIOTAP_FLAGS) |
				    BIT(IEEE80211_RADIOTAP_CHANNEL) |
				    BIT(28)); //BIT(IEEE80211_RADIOTAP_TLV));
		rt_hdr.rt_pad = 0;
		rt_hdr.rt_pad2 = 0;
		rt_hdr.rt_tlv_type = cpu_to_le16(32); // S1G
		rt_hdr.rt_tlv_length = cpu_to_le16(6);
		rt_hdr.rt_s1g_known = 0x007F;
		rt_hdr.rt_s1g_data1 =
			((rxi->bandwidth == 0) ?
				 0x0 :
				 ((rxi->preamble_type) ? 0x2 : 0x1)) |
			((response_ind & 0x03) << 2) | /* RI */
			((short_gi & 0x01) << 5) | /* GI */
			0x0 << 6 | /* NSS */
			((rxi->bandwidth & 0x0f) << 8) | /* BW */
			((mcs & 0x0f) << 12); /* MCS */
		rt_hdr.rt_s1g_data2 = cpu_to_le16((color & 0x07) |
						  ((uplink_ind & 0x01) << 3) |
						  (rxi->rcpi << 8));
	}

	skb_pull(skb, nw->hdev->fw.info.rx_head_size - sizeof(struct hif));

	if (ndp_ind) {
		prt_hdr_ndp = (struct nrc_radiotap_hdr_ndp *)skb_push(
			skb, sizeof(rt_hdr_ndp));
		memcpy(prt_hdr_ndp, &rt_hdr_ndp, sizeof(rt_hdr_ndp));
	} else if (agg) {
		prt_hdr_agg = (struct nrc_radiotap_hdr_agg *)skb_push(
			skb, sizeof(rt_hdr_agg));
		memcpy(prt_hdr_agg, &rt_hdr_agg, sizeof(rt_hdr_agg));

		// add FCS
		memcpy((void *)skb_put(skb, 4), (void *)&fcs, 4);
	} else {
		prt_hdr = (struct nrc_radiotap_hdr *)skb_push(skb,
							      sizeof(rt_hdr));
		memcpy(prt_hdr, &rt_hdr, sizeof(rt_hdr));

		// add FCS
		memcpy((void *)skb_put(skb, 4), (void *)&fcs, 4);
	}

	skb_set_mac_header(skb, 0);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_802_2);
	memset(skb->cb, 0, sizeof(skb->cb));
}

static void s1g_monitor_rx(void *data, struct net_device *dev)
{
	struct sk_buff *skb, *origskb = data;
	struct wireless_dev *wdev = dev->ieee80211_ptr;

	if (wdev->iftype != NL80211_IFTYPE_MONITOR)
		return;

	skb = skb_clone(origskb, GFP_ATOMIC);
	if (!skb)
		return;

	skb->dev = dev;
	netif_rx(skb);
}

static int nrc_mac_s1g_monitor_rx(struct nrc *nw, struct sk_buff *skb)
{
	struct ieee80211_hw *hw = nw->hw;

#if defined(NRC7291)
	struct RxInfo *rx;

	rx = RXINFO(skb->data() + RXVECTOR_SIG_SIZE);

	if (rx->error_crc && !rx->ndp_ind) {
		DBG_MAC("discard this frame because of error_crc");
		/* Track FRAME SKB free (RX path - CRC error) */
		NRC_SKB_TRACK_FREE(nw->hdev, skb, HIF_TYPE_FRAME, true, false);
		return 0;
	}
#endif

	if (nrc_mac_is_s1g(nw->hdev)) {
		nrc_add_rx_s1g_radiotap_header(nw, skb);
		ieee80211_iterate_active_netdev(hw, s1g_monitor_rx, skb);
	}
	/* Track FRAME SKB free (RX path - monitor mode) */
	NRC_SKB_TRACK_FREE(nw->hdev, skb, HIF_TYPE_FRAME, true, false);

	return 0;
}

/*
 * Explicit TX/RX handler arrays
 * These replace the linker section-based registration
 */

/* TX handlers array */
const struct nrc_trx_handler nrc_tx_handlers[] = {
	{.handler = tx_h_sta_pm, .vif_types = BIT(NL80211_IFTYPE_STATION)},
#if NRC_DBG_PRINT_FRAME_TX
	{.handler = tx_h_debug_print, .vif_types = NL80211_IFTYPE_ALL},
#endif
	{.handler = tx_h_debug_state, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = tx_h_wfa_halow_filter, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = tx_h_frame_filter, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = tx_h_managed_p2p_intf_addr,
	 .vif_types = NL80211_IFTYPE_ALL},
	{.handler = tx_h_put_iv, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = tx_h_put_qos_control, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = tx_h_twt_assoc, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = tx_h_bss_max_idle_period, .vif_types = NL80211_IFTYPE_ALL},
};
const int nrc_tx_handlers_count = ARRAY_SIZE(nrc_tx_handlers);

/* RX handlers array */
const struct nrc_trx_handler nrc_rx_handlers[] = {
	{.handler = rx_h_fixup_ps_poll_sp,
	 .vif_types = BIT(NL80211_IFTYPE_STATION)},
	{.handler = rx_h_vendor, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = rx_h_decrypt, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = rx_h_check_sn, .vif_types = NL80211_IFTYPE_ALL},
#if defined(CONFIG_SUPPORT_IBSS)
	{.handler = rx_h_ibss_get_bssid_tsf, .vif_types = NL80211_IFTYPE_ALL},
#endif
	{.handler = rx_h_action, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = rx_h_twt_assoc, .vif_types = NL80211_IFTYPE_ALL},
	{.handler = rx_h_twt_monitor, .vif_types = NL80211_IFTYPE_ALL},
#if NRC_DBG_PRINT_FRAME_RX
	{.handler = rx_h_debug_print, .vif_types = NL80211_IFTYPE_ALL},
#endif
#ifdef CONFIG_SUPPORT_MESH_ROUTING
	{.handler = rx_h_mesh, .vif_types = BIT(NL80211_IFTYPE_MESH_POINT)},
#endif
	{.handler = rx_h_bss_max_idle_period, .vif_types = NL80211_IFTYPE_ALL},
};
const int nrc_rx_handlers_count = ARRAY_SIZE(nrc_rx_handlers);
