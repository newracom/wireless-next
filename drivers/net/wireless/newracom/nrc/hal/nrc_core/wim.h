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

#ifndef _WIM_H_
#define _WIM_H_

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/if_ether.h>
#include <net/mac80211.h>
#ifdef CONFIG_SUPPORT_AFTER_KERNEL_3_0_36
#else
#include "nrc-mac80211.h"
#endif

#include "nrc-wim-types.h"

/**
 * nrc_wim_alloc_skb - allocate a sk_buff for wim message transmission
 * @cmd: wim command (enum WIM_CMD_ID)
 * @size: wim payload size in byte (excluding wim header)
 *
 * This function allocates a sk_buff,  prepends a wim header, and fills
 * a couple of wim header fields. A pointer to the allocated sk_buff is
 * returned.
 */
struct sk_buff *nrc_wim_alloc_skb(u16 cmd, int size);

struct sk_buff *nrc_wim_alloc_skb_vif(struct ieee80211_vif *vif, u16 cmd,
				      int size);

/**
 * nrc_wim_response_init - initialize wim response
 * @hdev: pointer to nrc_hif_device hdev
 */
int nrc_wim_response_init(struct nrc_hif_device *hdev);

/**
 * nrc_wim_response_deinit - deinitialize wim response
 */
int nrc_wim_response_deinit(struct nrc_hif_device *hdev);

/**
 * nrc_wim_skb_add_tlv - append a TLV parameter
 * @skb: buffer to use.
 * @T: type of the parameter to add.
 * @L: length of the parameter to add.
 * @V: value of the parameter.
 *
 * This function appends a TLV parameter to @skb. If @V is non-NULL,
 * @L bytes are copied from the buffer pointed to by it. Ohterwise,
 * the copy does not take place. A pointer to the first byte of V in
 * @skb is returned.
 */
void *nrc_wim_skb_add_tlv(struct sk_buff *skb, u16 T, u16 L, void *V);

/*
 * WLAN-specific WIM functions have been moved to frontend/nrc_wlan/nrc-wim-wlan.h:
 * - nrc_wim_wlan_change_sta, nrc_wim_wlan_hw_scan, nrc_wim_wlan_install_key,
 *   nrc_wim_wlan_ampdu_action, nrc_wim_wlan_get_tsf, nrc_wim_wlan_apf_*, etc.
 * Core module retains only infrastructure and PS/credit functions.
 */

/* Cipher type conversion - used by hif.c for TX encryption */
enum wim_cipher_type nrc_to_wim_cipher_type(u32 cipher);

/* HIF reset functions - used by SPI callback for error recovery */
bool nrc_wim_reset_hif_tx(struct nrc_hif_device *hdev);
bool nrc_wim_reset_hif_rx(struct nrc_hif_device *hdev);

/* PS management */
int nrc_wim_set_ps(struct nrc_hif_device *hdev, enum NRC_PS_MODE mode,
		   u64 timeout, struct cfg80211_wowlan *wowlan);
int nrc_wim_set_ps_sync(struct nrc_hif_device *hdev, enum NRC_PS_MODE mode,
			u64 timeout, struct cfg80211_wowlan *wowlan);

/* WIM response handling */
int nrc_wim_response_handler(struct sk_buff *skb);
void nrc_wim_cleanup_pending_responses(struct nrc_hif_device *hdev);
void nrc_wim_handle_fw_ready(struct nrc_hif_device *hdev);
int nrc_wim_update_tx_credit(struct nrc_hif_device *hdev, struct wim *wim);
void nrc_wim_handle_fw_reload(void);

/* WIM request/response handling */
int nrc_wim_request(struct sk_buff *skb, u16 cmd, int timeout,
		    bool use_mcp_path, struct sk_buff **skb_resp);

#endif /* _WIM_H_ */
