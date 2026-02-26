/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC HAL Core Interface - Frontend modules use these APIs to access HAL
 */

#ifndef _NRC_HAL_CORE_INTERFACE_H
#define _NRC_HAL_CORE_INTERFACE_H

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include "nrc-debug-common.h"
#include "nrc-bd-common.h"
#include "nrc-ps-common.h"

/* Forward declarations */
struct ieee80211_vif;
struct ieee80211_sta;
struct cfg80211_wowlan;
struct nrc;
struct nrc_hif_device;
struct nrc_hif_ops;

/* Core device access */
extern bool nrc_hal_core_is_init(void);
extern struct nrc *nrc_hal_core_get_nw(void);
extern struct device *nrc_hal_core_get_dev(void);
extern struct nrc_hif_device *nrc_hal_core_get_hdev(void);

/* HAL initialization */
extern int nrc_hal_core_nw_init(struct nrc *nw, struct nrc_hif_device *hdev);
extern void nrc_hal_core_nw_cleanup(struct nrc_hif_device *hdev, struct nrc *nw);

/**
 * struct nrc_hal_ops - HAL operations for frontend modules
 *
 * @nw_start:           Start network device
 * @nw_stop:            Stop network device
 * @nw_restart:         Restart network device
 * @xmit_wlan_frame:    Transmit WLAN frame
 * @xmit_mcp_frame:     Transmit MCP frame
 * @xmit_injected_frame: Transmit injected frame (monitor mode)
 * @wim_alloc_skb:      Allocate SKB for WIM command
 * @wim_alloc_skb_vif:  Allocate SKB for WIM command with VIF
 * @wim_skb_add_tlv:    Add TLV to WIM SKB
 * @wim_request:        Send WIM request (caller frees response SKB)
 * @bd_get_tx_pwr:      Get TX power from board data
 * @bd_get_supp_ch_list: Get supported channel list
 * @tx_cleanup_queues:  Cleanup TX queues
 * @ps_request_sleep:   Request power save sleep
 * @ps_request_wake:    Request power save wake
 */
struct nrc_hal_ops {
	/* Network control */
	int (*nw_start)(bool restart);
	int (*nw_stop)(bool restart);
	void (*nw_restart)(void);

	/* Frame TX */
	int (*xmit_wlan_frame)(s8 vif_index, u16 aid, struct sk_buff *skb);
	int (*xmit_mcp_frame)(int subtype, struct sk_buff *skb,
			      bool hif_header_included);
	int (*xmit_injected_frame)(struct ieee80211_vif *vif,
				   struct ieee80211_sta *sta,
				   struct sk_buff *skb);

	/* WIM operations */
	struct sk_buff *(*wim_alloc_skb)(u16 cmd, int size);
	struct sk_buff *(*wim_alloc_skb_vif)(struct ieee80211_vif *vif, u16 cmd,
					     int size);
	void *(*wim_skb_add_tlv)(struct sk_buff *skb, u16 type, u16 len,
				 void *tlv);
	int (*wim_request)(struct sk_buff *skb, u16 cmd, int timeout,
			   bool use_mcp_path, struct sk_buff **skb_resp);

	/* Board data */
	struct wim_bd_param *(*bd_get_tx_pwr)(u8 *cc);
	struct bd_supp_param *(*bd_get_supp_ch_list)(void);

	/* TX control */
	void (*tx_cleanup_queues)(void);

	/* Power save */
	int (*ps_request_sleep)(enum NRC_PS_MODE mode, u64 timeout,
				struct cfg80211_wowlan *wowlan,
				enum NRC_PS_REASON reason);
	int (*ps_request_wake)(int timeout_ms, enum NRC_PS_REASON reason);
};

extern struct nrc_hal_ops *nrc_hal_core_get_ops(void);

/*
 * HAL Operations Inline Wrappers
 */

/* Network control */
static inline int nrc_hal_ops_nw_start(bool restart)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->nw_start ? ops->nw_start(restart) : -ENODEV;
}

static inline int nrc_hal_ops_nw_stop(bool restart)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->nw_stop ? ops->nw_stop(restart) : -ENODEV;
}

static inline void nrc_hal_ops_nw_restart(void)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	if (ops && ops->nw_restart)
		ops->nw_restart();
}

/* Frame TX */
static inline int nrc_hal_ops_xmit_wlan_frame(s8 vif_index, u16 aid,
					      struct sk_buff *skb)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->xmit_wlan_frame ?
		       ops->xmit_wlan_frame(vif_index, aid, skb) :
		       -ENODEV;
}

static inline int nrc_hal_ops_xmit_mcp_frame(int subtype, struct sk_buff *skb,
					     bool hif_header_included)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->xmit_mcp_frame ?
		       ops->xmit_mcp_frame(subtype, skb, hif_header_included) :
		       -ENODEV;
}

static inline int nrc_hal_ops_xmit_injected_frame(struct ieee80211_vif *vif,
						  struct ieee80211_sta *sta,
						  struct sk_buff *skb)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->xmit_injected_frame ?
		       ops->xmit_injected_frame(vif, sta, skb) :
		       -ENODEV;
}

/* WIM operations */
static inline struct sk_buff *nrc_hal_ops_wim_alloc_skb(u16 cmd, int size)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->wim_alloc_skb ? ops->wim_alloc_skb(cmd, size) : NULL;
}

static inline struct sk_buff *
nrc_hal_ops_wim_alloc_skb_vif(struct ieee80211_vif *vif, u16 cmd, int size)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->wim_alloc_skb_vif ?
		       ops->wim_alloc_skb_vif(vif, cmd, size) :
		       NULL;
}

static inline void *nrc_hal_ops_wim_skb_add_tlv(struct sk_buff *skb, u16 type,
						u16 len, void *tlv)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->wim_skb_add_tlv ?
		       ops->wim_skb_add_tlv(skb, type, len, tlv) :
		       NULL;
}

/* Caller must free response SKB with NRC_SKB_TRACK_WIM_FREE() */
static inline int nrc_hal_ops_wim_request(struct sk_buff *skb, u16 cmd,
					  int timeout, bool use_mcp_path,
					  struct sk_buff **skb_resp)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->wim_request ?
		       ops->wim_request(skb, cmd, timeout, use_mcp_path,
					skb_resp) :
		       -ENODEV;
}

/* Board data */
static inline struct wim_bd_param *nrc_hal_ops_bd_get_tx_pwr(u8 *cc)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->bd_get_tx_pwr ? ops->bd_get_tx_pwr(cc) : NULL;
}

static inline struct bd_supp_param *nrc_hal_ops_bd_get_supp_ch_list(void)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->bd_get_supp_ch_list ? ops->bd_get_supp_ch_list() :
						 NULL;
}

/* TX control */
static inline void nrc_hal_ops_tx_cleanup_queues(void)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	if (ops && ops->tx_cleanup_queues)
		ops->tx_cleanup_queues();
}

/* Power save */
static inline int nrc_hal_ops_ps_request_sleep(enum NRC_PS_MODE mode,
					       u64 timeout,
					       struct cfg80211_wowlan *wowlan,
					       enum NRC_PS_REASON reason)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->ps_request_sleep ?
		       ops->ps_request_sleep(mode, timeout, wowlan, reason) :
		       -ENODEV;
}

static inline int nrc_hal_ops_ps_request_wake(int timeout_ms,
					      enum NRC_PS_REASON reason)
{
	struct nrc_hal_ops *ops = nrc_hal_core_get_ops();
	return ops && ops->ps_request_wake ?
		       ops->ps_request_wake(timeout_ms, reason) :
		       -ENODEV;
}

#endif /* _NRC_HAL_CORE_INTERFACE_H */
