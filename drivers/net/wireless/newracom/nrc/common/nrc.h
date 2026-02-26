/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC Core Definitions - Main driver structures and types
 */

#ifndef _NRC_H_
#define _NRC_H_

#include "nrc-build-config.h"
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/firmware.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/circ_buf.h>
#include <linux/completion.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include <net/ieee80211_radiotap.h>
#include "nrc-twt.h"
#include "nrc-wim-types.h"
#include "nrc-ps-common.h"
#include "nrc-params.h"
#include "nrc-hif-defs.h"

/* Forward declarations for TWT structures */
struct nrc_twt_sched;

#define NRC_DRIVER_NAME "nrc80211"
#define DEFAULT_INTERFACE_NAME "nrc"

struct nrc_hif_device;

#ifdef CONFIG_SUPPORT_AFTER_KERNEL_3_0_36
#else
#define IEEE80211_NUM_ACS (4)
#define IEEE80211_P2P_NOA_DESC_MAX (4)
#endif

enum NRC_SCAN_MODE {
	NRC_SCAN_MODE_IDLE = 0,
	NRC_SCAN_MODE_ACTIVE_SCANNING,
	NRC_SCAN_MODE_PASSIVE_SCANNING,
	NRC_SCAN_MODE_SCHED_SCANNING,
	NRC_SCAN_MODE_ABORTING,
	NRC_SCAN_MODE_MAX,
};

#ifdef CONFIG_SUPPORT_AFTER_KERNEL_3_0_36
#else
enum ieee80211_sta_state {
	/* NOTE: These need to be ordered correctly! */
	IEEE80211_STA_NOTEXIST,
	IEEE80211_STA_NONE,
	IEEE80211_STA_AUTH,
	IEEE80211_STA_ASSOC,
	IEEE80211_STA_AUTHORIZED,
};
#endif

enum ieee80211_tx_ba_state {
	IEEE80211_BA_NONE,
	IEEE80211_BA_REQUEST,
	IEEE80211_BA_ACCEPT,
	IEEE80211_BA_REJECT,
	IEEE80211_BA_CLOSE,
	IEEE80211_BA_DISABLE,
};

enum NRC_AMPDU_MODE {
	NRC_AMPDU_DISABLE, //AMPDU disabled
	NRC_AMPDU_MANUAL, //AMPDU enabled (manual)
	NRC_AMPDU_AUTO, //AMPDU enabled (auto)
};

struct fwinfo_t {
	enum NRC_FW_STATE state; /* HW state from EIRQ_STATUS */
	uint32_t ready;
	uint32_t version;
	uint16_t chip_rev_num;
	uint32_t tx_head_size;
	uint32_t rx_head_size;
	uint32_t payload_align;
	uint32_t buffer_size;
	uint16_t hw_version;
};

struct vif_capabilities {
	uint64_t cap_mask;
};

struct nrc_capabilities {
	uint64_t cap_mask;
	uint16_t listen_interval;
	uint32_t bss_max_idle;
	uint8_t max_vif;
	struct vif_capabilities vif_caps[NR_NRC_VIF];
};

#define BSS_MAX_ILDE_DEAUTH_LIMIT_COUNT \
	3 /* Keep Alive Timeout Limit Count on AP */
#define S1G_UNSCALED_INTERVAL_MAX (0x3fff)
struct nrc_max_idle {
	bool enable;
	u16 period;
	u8 options;
	u16 timeout_cnt;

	unsigned long idle_period; /*AP : SEC, STA : jiffies) */
	unsigned long sta_idle_timer;
};

/* Private txq driver data structure */
struct nrc_txq {
	u16 hw_queue; /* 0: AC_BK, 1: AC_BE, 2: AC_VI, 3: AC_VO */
	struct list_head list;
	unsigned long nr_fw_queueud;
	unsigned long nr_push_allowed;
	struct ieee80211_vif vif;
	struct ieee80211_sta *sta;
};

struct nrc_delayed_deauth {
	atomic_t delayed_deauth;
	s8 vif_index;
	u16 aid;
	bool removed;
	struct sk_buff *deauth_frm;
	struct sk_buff *ch_skb;
	struct ieee80211_vif v;
	struct ieee80211_sta s;
	struct ieee80211_key_conf p;
	struct ieee80211_key_conf g;
	struct ieee80211_bss_conf b;
#ifdef CONFIG_SUPPORT_CHANNEL_INFO
	struct cfg80211_chan_def c;
	struct ieee80211_channel ch;
#else
	struct ieee80211_conf c;
#endif
	struct ieee80211_tx_queue_params tqp[NRC_QUEUE_MAX];
};

struct wim_response {
	struct completion work;
	struct sk_buff *skb;
	struct mutex lock; /* Protects work and skb fields */
};

struct nrc {
	struct device *dev;
	struct nrc_hif_device *hdev;

	struct ieee80211_hw *hw;
	struct ieee80211_vif *vif[NR_NRC_VIF];
	int nr_active_vif;
	bool enable_vif[NR_NRC_VIF];
	spinlock_t vif_lock;
	bool promisc;
#ifdef CONFIG_USE_NEW_BAND_ENUM
	struct ieee80211_supported_band bands[NUM_NL80211_BANDS];
#else
	struct ieee80211_supported_band bands[IEEE80211_NUM_BANDS];
#endif

	u64 tsf_offset;
	u32 sleep_ms;
	struct nrc_txq ntxq[NRC_QUEUE_MAX];

	struct mutex state_mtx;
	atomic_t scan_mode; /* NRC_SCAN_MODE values (atomic) */
	struct delayed_work check_start;
	struct tasklet_struct tx_tasklet;
	struct delayed_work idle_work;
	struct delayed_work beacon_loss_work;

	/**
	 * This CC should be followed "ISO 3166-1 alpha-2" code.
	 */
	char alpha2[2];

	/* Move to vif or sta driver data */
	u8 frame_seqno;
	u8 band;
	u16 center_freq;
	u16 aid;
	u32 cipher_pairwise;
	u32 cipher_group;

	bool invoke_beacon_loss;
	struct timer_list dynamic_ps_timer;
	struct work_struct dynamic_ps_work;

	/* sync power management operation between
	 * mac80211 config and host interface
	 * Note: hif_*_stopped completions removed as unused */
	uint32_t vendor_arg;

	bool amsdu_supported;
	bool block_frame;
	bool ampdu_reject;
	bool ampdu_started;
	bool idle_state;

	/* tx */
	spinlock_t txq_lock;
	struct list_head txq;

	/* p2p remain on channel */
	struct delayed_work roc_finish;

	/* beacon interval (AP) */
	u16 beacon_int;

	/* vendor specific element*/
	struct sk_buff *vendor_skb_beacon;
	struct sk_buff *vendor_skb_probe_req;
	struct sk_buff *vendor_skb_probe_rsp;
	struct sk_buff *vendor_skb_assoc_req;

	/* work for removing vendor specific ie for wowlan pattern (AP) */
	struct delayed_work rm_vendor_ie_wowlan_pattern;

	/* CQM offload */
	struct timer_list bcn_mon_timer;
	unsigned long beacon_timeout;
	struct ieee80211_vif *associated_vif;
	bool is_bcn_timeout;

	/* for processing deauth when deepsleep */
	struct nrc_delayed_deauth d_deauth;

	/* WLAN module closing state - prevents RX processing during unregister */
	atomic_t hw_unregistering;

	/* set frag threshold by mac80211 */
	s32 frag_threshold;

	bool twt_requester;
	bool twt_responder;

	struct nrc_twt_sched *twt_sched;
	u64 twt_sched_interval;

	/* Module parameters structure (shared pointer) */
	struct nrc_params *params;

	/* Debug structure (shared pointer) */
	struct nrc_debug *debug;
	struct dentry *debugfs;
};

/* vif driver data structure */
struct nrc_vif {
	struct nrc *nw;
	int index;
	struct net_device *dev;

	/* scan */
	struct delayed_work scan_timeout;

	/* power save */
	bool ps_polling;

	/* MLME */
	spinlock_t preassoc_sta_lock;
	struct list_head preassoc_sta_list;

	/* inactivity */
	u32 max_idle_period;
	struct timer_list max_idle_timer;

#ifdef CONFIG_SUPPORT_AFTER_KERNEL_3_0_36
	/* P2p client NoA */
	struct ieee80211_noa_data noa;
#endif
};

#define to_ieee80211_vif(v) \
	container_of((void *)v, struct ieee80211_vif, drv_priv)

#define to_i_vif(v) ((struct nrc_vif *)(v)->drv_priv)

static inline int hw_vifindex(struct ieee80211_vif *vif)
{
	struct nrc_vif *i_vif;

	if (vif == NULL)
		return 0;

	i_vif = to_i_vif(vif);
	return i_vif->index;
}

struct tx_ba_session {
	struct nrc_sta *sta;
	u16 tid;
	enum ieee80211_tx_ba_state state;
	uint32_t ba_req_last_jiffies;
	struct work_struct ba_session_work;
};

struct rx_ba_session {
	bool started;
	u16 sn;
	u16 buf_size;
};

/* sta driver data structure */
struct nrc_sta {
	struct nrc *nw;
	struct ieee80211_vif *vif;
	/*struct ieee80211_sta *sta;*/

	enum ieee80211_sta_state state;
	struct list_head list;

	/* keys */
	struct ieee80211_key_conf *ptk;
	struct ieee80211_key_conf *gtk;

	/* period */
	uint16_t listen_interval;
	struct nrc_max_idle max_idle;

	/* Block Ack Session per TID */
	struct tx_ba_session tx_ba_session[NRC_MAX_TID];
	struct rx_ba_session rx_ba_session[NRC_MAX_TID];

	/* TWT */
	struct nrc_twt twt;
};

#define to_ieee80211_sta(s) \
	container_of((void *)s, struct ieee80211_sta, drv_priv)

#define to_i_sta(s) ((struct nrc_sta *)(s)->drv_priv)

/* EU countries (27) + GB, SA for S1G channel compatibility - defined in nrc-wlan-module.c */
extern const char *const eu_countries_cc[];
#endif
