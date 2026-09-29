/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * mac80211 extension
 */
#ifndef __MAC80211_EXT_H__
#define __MAC80211_EXT_H__

#include <net/mac80211.h>

struct bss_max_idle_period_ie {
	__le16 max_idle_period;
	u8 idle_option;
} __packed;

static inline u32 ieee80211_usf_to_sf(u8 usf)
{
	return (usf == 1) ? 10 : (usf == 2) ? 1000 : (usf == 3) ? 10000 : 1;
}

struct ieee80211_sta *ieee80211_find_all_sta(struct ieee80211_vif *vif,
					     u8 *adddr);

u8 *ieee80211_append_ie(struct sk_buff *skb, u8 eid, u8 len);

struct sk_buff *ieee80211_deauth_get(struct ieee80211_hw *hw, u8 *da, u8 *sa,
				     u8 *bssid, __le16 reason, struct ieee80211_sta *sta, bool tosta);

void ieee80211_iterate_active_netdev(struct ieee80211_hw *hw,
				     void (*iterator)(void *data,
						      struct net_device *dev),
				     void *data);

#endif
