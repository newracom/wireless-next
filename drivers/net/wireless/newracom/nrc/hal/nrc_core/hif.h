/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC HIF Header - HAL Core HIF interface
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

#ifndef _HIF_H_
#define _HIF_H_

#include <linux/skbuff.h>

/* HIF header checksum calculation: XOR of type and length bytes */
#define HIF_HEADER_CALC_CHECKSUM(type, length) \
	((uint8_t)((type) ^ ((length) & 0xFF) ^ (((length) >> 8) & 0xFF)))

/* Forward declarations */
struct nrc_hif_device;
struct nrc;
struct ieee80211_vif;
struct ieee80211_sta;

/* HIF Device Management Functions */
struct nrc_hif_device *nrc_hif_alloc(struct device *dev, void *priv,
				     struct nrc_hif_ops *ops);
void nrc_hif_free(struct nrc_hif_device *hdev);
void nrc_hif_reset_slot_credit(void);
void nrc_hif_free_skb(struct nrc_hif_device *hdev, struct sk_buff *skb);

/* HAL Operations Functions (not direct HIF ops) */
int nrc_hal_start(void);
int nrc_hal_stop(struct nrc_hif_device *hdev);
void nrc_hal_debug_send(struct sk_buff *skb);

/* HIF Queue Management Functions */
int skb_change_ac(struct nrc_hif_device *hdev, struct sk_buff *skb, uint8_t ac);
int hif_enqueue_skb(struct nrc_hif_device *hdev, struct sk_buff *skb);
int hif_enqueue_mcp_skb(struct nrc_hif_device *hdev, struct sk_buff *skb);

/* WLAN frontend transmit function */
int nrc_xmit_wlan_frame(s8 vif_index, u16 aid, struct sk_buff *skb);
int nrc_xmit_injected_frame(struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta, struct sk_buff *skb);

/* MCP frontend transmit function */
int nrc_xmit_mcp_frame(int subtype, struct sk_buff *skb,
		       bool hif_header_included);

/* HIF WIM Functions */
int nrc_xmit_wim_request(struct sk_buff *skb);

/* HIF Slot Management Functions */
void nrc_hif_set_slot_index(struct nrc_hif_device *hdev, int slot_type,
			    int index_type, int value);
void nrc_hif_inc_slot_index(struct nrc_hif_device *hdev, int slot_type,
			    int index_type);

#endif /* _HIF_H_ */
