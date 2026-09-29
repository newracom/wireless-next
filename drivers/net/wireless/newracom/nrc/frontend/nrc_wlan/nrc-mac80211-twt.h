/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 */

#ifndef _NRC_MAC80211_TWT_H_
#define _NRC_MAC80211_TWT_H_

#include "nrc-twt.h"

/* ieee80211_twt_setup_assoc_ie is defined in HAL layer */

struct ieee80211_twt_setup_vendor_ie {
	struct ieee80211_vendor_ie v;
	u64 sp;
} __packed;

#define NRC_MAX_STA_TWT_AGRT 1
#define NRC_TWT_VENDOR_IE_ENABLE

#define TWT_DEBUG_IE_FLAG (0)
#define TWT_DEBUG_TIME_FLAG (1)

/* TWT structures are defined in HAL layer nrc-twt.h */

/* upper */
struct nrc;

void nrc_mac_rx_twt_setup(struct nrc *nw, struct ieee80211_sta *sta,
			  struct ieee80211_mgmt *mgmt);

void nrc_mac_tx_twt_setup(struct nrc *nw, struct ieee80211_sta *sta,
			  struct ieee80211_twt_setup *twt);

void nrc_mac_rx_twt_teardown(struct nrc *nw, struct ieee80211_sta *sta,
			     struct ieee80211_mgmt *mgmt);

/* ops */
void nrc_mac_add_twt_setup(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
			   struct ieee80211_twt_setup *twt);

void nrc_mac_twt_teardown_request(struct ieee80211_hw *hw,
				  struct ieee80211_sta *sta, u8 flowid);
/* assoc */
void nrc_mac_rx_twt_setup_assoc_req(struct nrc *nw, struct ieee80211_sta *sta,
				    struct ieee80211_mgmt *mgmt, size_t len);
void nrc_mac_rx_twt_setup_assoc_resp(struct nrc *nw, struct ieee80211_sta *sta,
				     struct sk_buff *skb);

void nrc_mac_twt_debugfs_init(struct dentry *root, void *nw);

void nrc_mac_twt_sp_update(struct nrc *nw, struct ieee80211_sta *sta, int type);
#endif
