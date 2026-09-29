/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 */

#ifndef _NRC_MAC80211_H_
#define _NRC_MAC80211_H_

#include "nrc.h"
#include "mac80211-ext.h"

/* WLAN-specific headers (moved from common/) */
#include "nrc-radiotap.h"
#include "nrc-trx-handler.h"
#include "nrc-sta-handler.h"

#define NRC_MAC80211_MAX_SCANSSID (32)
#define NRC_MAC80211_MAX_SCANIELEN (IEEE80211_MAX_DATA_LEN)
#define NRC_MAC80211_ROC_DURATION (1000)
#define NRC_MAC80211_RCU_LOCK_THRESHOLD (1000)

struct wim_event_work {
	struct work_struct work;
	struct nrc *nw;
	struct ieee80211_vif *vif;
	void *data;
};

typedef void (*wim_event_handler_t)(struct work_struct *work);

int nrc_mac80211_init(struct nrc *nr);
void nrc_mac80211_exit(struct nrc *nr);

void nrc_free_hw(struct nrc *nw);

struct ieee80211_hw *nrc_mac_alloc_hw(size_t priv_data_len,
				      const char *req_name);
void nrc_mac_free_hw(struct ieee80211_hw *hw);

int nrc_register_hw(struct nrc *nw, struct nrc_hif_device *hdev);
void nrc_unregister_hw(struct nrc *nw);

void nrc_ampdu_mon_init(void);
void nrc_ampdu_mon_deinit(void);

void nrc_kick_txq(struct nrc *nw);
int nrc_handle_frame(struct nrc *nw, struct sk_buff *skb);
void nrc_mac_cancel_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
bool nrc_cancel_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
void nrc_mac_scan_completed_work_handler(struct work_struct *work);
void beacon_loss_check_work_handler(struct work_struct *work);

struct net_device *nrc_get_intf_by_name(const char *intf_name);

void nrc_mac_tx_process(struct ieee80211_hw *hw,
			struct ieee80211_tx_control *control,
			struct sk_buff *skb, bool from_mac80211);
int nrc_mac_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    unsigned int link_id, u16 ac,
		    const struct ieee80211_tx_queue_params *params);
void nrc_mac_bss_info_changed(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_bss_conf *info,
			      u64 changed);
void nrc_mac_add_tlv_channel(struct sk_buff *skb,
			     struct cfg80211_chan_def *chandef);
int nrc_mac_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta);
void nrc_mac_stop(struct ieee80211_hw *hw);

int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb);
void dump_mgmt_frame(struct ieee80211_hdr *hdr, int direction);
void nrc_mac_trx_init(struct nrc *nw);

bool nrc_access_vif(struct nrc *nw);

void nrc_free_vif_index(struct nrc *nw, struct ieee80211_vif *vif);

#ifdef CONFIG_S1G_CHANNEL
void init_s1g_channels(struct nrc *nw);
#endif /* #ifdef CONFIG_S1G_CHANNEL */

/**
 * nrc_restore_reg_domain - Re-send country code (and board data) to FW.
 *
 * After a cold FW reboot (restart_wlan, WDT recovery, idle-mode wakeup) the
 * freshly loaded firmware has no country code or channel table.  This helper
 * replicates the regulatory notification that cfg80211 sends automatically at
 * ieee80211_register_hw() time but does NOT repeat on subsequent restarts.
 *
 * Call this whenever the FW has been restarted and needs its regulatory state
 * re-initialized before any channel or VIF configuration WIM commands arrive.
 */
void nrc_restore_reg_domain(struct nrc *nw);

void nrc_mac_clean_txq(struct nrc *nw);
void nrc_mac_flush_txq(struct nrc *nw);
void nrc_send_beacon_loss(struct nrc *nw);

void nrc_mac_roc_finish(struct work_struct *work);
void nrc_rm_vendor_ie_wowlan_pattern(struct work_struct *work);
void nrc_vcmd_backup_set_wdt_flag(u8 vif_id);

void nrc_probe_timer(struct timer_list *t);
void nrc_bcn_mon_timer(struct timer_list *t);
void nrc_tx_tasklet(struct tasklet_struct *t);
void nrc_cleanup_txq_all(struct nrc *nw);
void nrc_cleanup_txq(struct nrc *nw, struct ieee80211_txq *txq);
void nrc_cleanup_txq_by_macaddr(struct nrc *nw, struct ieee80211_vif *vif,
				uint8_t *macaddr);

void nrc_cleanup_ba_session_sta(void *data, struct ieee80211_sta *sta);
void nrc_cleanup_ba_session_vif(struct nrc *nw, struct ieee80211_vif *vif);
void nrc_cleanup_ba_session_all(struct nrc *nw);

int nrc_mac_restart(struct nrc *nw);
int nrc_nw_restart_wlan(struct nrc *nw);

/**
 * nrc_mac_bd_invalidate - Mark BD as not loaded in FW.
 *
 * Call this before any FW restart (WDT recovery, module reload) to ensure
 * nrc_mac_start(), nrc_mac_add_interface(), and nrc_mac_start_ap() block
 * all WLAN operations until nrc_restore_reg_domain() re-sends the BD.
 */
void nrc_mac_bd_invalidate(void);

bool nrc_idle_mode_get_state(struct nrc *nw);
void nrc_idle_mode_set_state(struct nrc *nw, bool enable);
char *nrc_idle_mode_get_state_str(struct nrc *nw);

void nrc_mac_sched_scan_completed(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif);
void nrc_mac_sched_scan_results(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif);
void nrc_mac_sched_scan_results_work_handler(struct work_struct *work);

const char *nrc_mac_get_scan_status_str(struct nrc *nw);
#endif
