/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC TWT Definitions - Target Wake Time structures
 */

#ifndef _NRC_TWT_H_
#define _NRC_TWT_H_

#include <linux/types.h>

/* Forward declarations */
struct twt_sched_entry;
struct dentry;
struct nrc;

/* TWT structure definitions - use kernel provided ieee80211_twt_params */
struct ieee80211_twt_setup_assoc_ie {
	u8 control;
	struct ieee80211_twt_params params;
} __packed;

/* TWT definitions */
#define NRC_MAX_STA_TWT_AGRT 1

struct nrc_twt_flow {
	u8 id;
	u8 duration;
	u16 mantissa;
	u8 exp;
	u64 twt;
	struct ieee80211_twt_setup_assoc_ie twt_ie;
	struct twt_sched_entry *entry;
};

struct nrc_twt {
	u8 flowid_mask;
	struct nrc_twt_flow flow[NRC_MAX_STA_TWT_AGRT];
	s8 assoc_flowid; /* 1-based: set while TWT IE is pending in AssocResp */
};

/* TWT debugfs function declaration - implemented in frontend layer */
extern void nrc_mac_twt_debugfs_init(struct dentry *root, void *nw);

#endif /* _NRC_TWT_H_ */
