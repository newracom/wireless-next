/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Callback Implementation
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
#include <linux/kernel.h>
#include <linux/module.h>

/* Linux networking headers */
#include <net/mac80211.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"
#include "mcp.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-wim-types.h"

/* Local module headers */
#include "nrc-mcp-init.h"
#include "nrc-mcp-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-mcp-transmit.h"
#include "nrc-netlink-driver.h"

/* Forward declarations */
static int nrc_mcp_hal_callback_handler(struct nrc_hal_event_data *hal_event);
static int nrc_mcp_handle_wim_event(struct nrc_hal_event_data *event);

/* Helper function to convert MCP WIM event type to string */
static const char *mcp_event_to_str(int event_type)
{
	switch (event_type) {
	case WIM_EVENT_MCP_NEXT_GROUP:
		return "NEXT_GROUP";
	case WIM_EVENT_MCP_KEEP_ALIVE:
		return "KEEP_ALIVE";
	case WIM_EVENT_MCP_SCHEDULE_REPORT:
		return "SCHEDULE_REPORT";
	case WIM_EVENT_MCP_IMMEDIATE_REPORT:
		return "IMMEDIATE_REPORT";
	default:
		return "UNKNOWN";
	}
}

/**
 * nrc_mcp_handle_spi_irq - Handle SPI interrupt event
 * @event: HAL event data containing SPI interrupt information
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_mcp_handle_spi_irq(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR_MCP("Invalid event data for SPI IRQ");
		return -EINVAL;
	}

	DBG_AMPDU("MCP: Processing SPI interrupt event");

	return 0;
}

/**
 * nrc_mcp_handle_rx_ready - Handle RX ready event from HAL
 * @event: HAL event data containing RX ready information
 *
 * Processes RX data packets that were forwarded from HAL intelligent routing.
 * HAL has already filtered the packets, so we only receive packets that
 * need MCP processing (HIF_TYPE_FRAME, HIF_TYPE_WIM, HIF_TYPE_LOG, etc.)
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_mcp_handle_rx_ready(struct nrc_hal_event_data *event)
{
	struct sk_buff *skb;
	struct nrc_hif_device *hdev;
	struct mcp_priv *mcp;
	struct hif *hif;
	struct frame_hdr *fh;
	int id = -1;

	mcp = nrc_mcp_get_device();
	if (!mcp) {
		ERR_MCP("Device not available");
		return -EINVAL;
	}
	hdev = mcp->hdev;
	if (!hdev) {
		ERR_MCP("MCP: HIF device not available");
		return -EINVAL;
	}

	if (!event || !event->data) {
		ERR_MCP("Invalid event data for RX ready");
		return -EINVAL;
	}

	skb = (struct sk_buff *)event->data;
	if (!skb || skb->len < sizeof(struct hif)) {
		ERR_MCP("Invalid SKB in RX ready event: skb=%p, data=%p, len=%d",
			skb, skb ? skb->data : NULL, skb ? skb->len : 0);
		return -EINVAL;
	}

	hif = (struct hif *)skb->data;
	WARN_ON(skb->len != hif->len + sizeof(*hif));

	DBG_RX("MCP HIF type=%s(%u) subtype=%s(%u) len=%u skb_len=%u",
	       nrc_hif_type_str(hif->type), hif->type,
	       nrc_hif_subtype_str(hif->type, hif->subtype), hif->subtype,
	       hif->len, skb->len);

	skb_pull(skb, sizeof(*hif));

	if (hif->type == HIF_TYPE_FRAME &&
	    hif->subtype == HIF_FRAME_SUB_MCP_PROTOCOL) {
		id = CHAN_ID_PROTOCOL_F2H;
	}

	fh = (void *)skb->data; /* frame header */

	/* Peel off frame header */
	// skb_pull(skb, hdev->fw.info.rx_head_size - sizeof(struct hif));
	skb_pull(skb, 16);

	/* Forward to appropriate netlink channel based on HIF type/subtype */
	if (id >= 0) {
		/* send_to_netlink will free the SKB */
		send_to_netlink(id, skb, hdev, hif->type, true);
	} else {
		ERR_MCP("MCP RX: Cannot determine netlink channel ID for type=%s(%u), subtype=%s(%u)",
			nrc_hif_type_str(hif->type), hif->type,
			nrc_hif_subtype_str(hif->type, hif->subtype),
			hif->subtype);
		/* Track SKB free (RX path from SPI) */
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
	}
	return 0;
}

/**
 * nrc_mcp_handle_tx_complete - Handle TX complete event
 * @event: HAL event data containing TX complete information
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_mcp_handle_tx_complete(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR_MCP("Invalid event data for TX complete");
		return -EINVAL;
	}

	return 0;
}

/**
 * nrc_mcp_handle_error - Handle error event
 * @event: HAL event data containing error information
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_mcp_handle_error(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR_MCP("Invalid event data for error");
		return -EINVAL;
	}

	return 0;
}

/**
 * nrc_mcp_handle_kick_txq - Handle TX queue kick request from HAL
 * @event: HAL event data containing network device pointer
 */
static int __maybe_unused
nrc_mcp_handle_kick_txq(struct nrc_hal_event_data *event)
{
	if (!event) {
		ERR_MCP("Invalid event data for error");
		return -EINVAL;
	}

	return 0;
}

/**
 * nrc_mcp_hal_callback_handler - Handle HAL events
 * @hal_event: HAL event data
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_mcp_hal_callback_handler(struct nrc_hal_event_data *hal_event)
{
	int ret = 0;

	if (!hal_event) {
		ERR_MCP("Invalid HAL event data");
		return -EINVAL;
	}

	/* Dispatch to appropriate handler based on event type */
	switch (hal_event->type) {
	case NRC_HAL_EVT_SPI_IRQ:
		ret = nrc_mcp_handle_spi_irq(hal_event);
		break;
	case NRC_HAL_EVT_RX_READY:
		ret = nrc_mcp_handle_rx_ready(hal_event);
		break;
	case NRC_HAL_EVT_ERROR:
		ret = nrc_mcp_handle_error(hal_event);
		break;
	case NRC_HAL_EVT_WIM_EVENT:
		ret = nrc_mcp_handle_wim_event(hal_event);
		break;
	default:
		DBG_MAC("Unknown HAL event type %d", hal_event->type);
		ret = -EINVAL;
		break;
	}

	return ret;
}

/**
 * nrc_mcp_handle_wim_event - Handle WIM event forwarded from HAL
 * @hal_event: HAL event data containing WIM event
 *
 * Process WIM events that require MCP layer handling. HAL processes local events
 * and forwards events with MCP layer dependencies to MCP.
 * MCP-specific events are processed and then forwarded to netlink for user space.
 *
 * Architecture Design:
 * - MCP-specific events need both kernel and user space processing
 * - Process MCP events in kernel, then forward to user space via netlink
 * - SKB is forwarded to netlink after kernel processing
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_mcp_handle_wim_event(struct nrc_hal_event_data *hal_event)
{
	struct sk_buff *skb = (struct sk_buff *)hal_event->data;
	struct hif *hif;
	struct wim *wim;
	struct nrc_hif_device *hdev;
	struct mcp_priv *mcp;
	int hif_type;
	int event_type;
	int id = -1;

	if (!skb || skb->len < sizeof(struct hif) + sizeof(struct wim)) {
		ERR_MCP("Invalid WIM event SKB");
		if (skb) {
			struct mcp_priv *mcp = nrc_mcp_get_device();
			/* Track WIM event SKB free */
			NRC_SKB_TRACK_FREE(mcp ? mcp->hdev : NULL, skb,
					   HIF_TYPE_WIM, true, false);
		}
		return -EINVAL;
	}

	hif = (struct hif *)skb->data;
	hif_type = hif->type;

	mcp = nrc_mcp_get_device();
	if (!mcp) {
		ERR_MCP("Device not available");
		/* Error drop: MCP device not available, use parsed hif type */
		NRC_SKB_TRACK_FREE(NULL, skb, hif_type, true, false);
		return -ENODEV;
	}
	hdev = mcp->hdev;

	skb_pull(skb, sizeof(*hif));
	wim = (struct wim *)skb->data;

	event_type = wim->event;

	/* Process MCP-specific events */
	switch (event_type) {
	case WIM_EVENT_MCP_NEXT_GROUP:
		id = CHAN_ID_EVENT;
		break;

	case WIM_EVENT_MCP_KEEP_ALIVE:
		id = CHAN_ID_EVENT;
		break;

	case WIM_EVENT_MCP_SCHEDULE_REPORT:
		id = CHAN_ID_PROTOCOL_EVENT;
		break;

	case WIM_EVENT_MCP_IMMEDIATE_REPORT:
		id = CHAN_ID_PROTOCOL_EVENT;
		break;

	default:
		break;
	}

	/* Forward to netlink for user space processing */
	if (id >= 0) {
		DBG_WIM("Forwarding event %s(%d) to channel %s(%d)",
			mcp_event_to_str(event_type), event_type,
			channel_id_to_str(id), id);
		skb_pull(skb, sizeof(*wim));
		/* send_to_netlink will free the SKB */
		send_to_netlink(id, skb, hdev, HIF_TYPE_WIM, true);
	} else {
		ERR_MCP("Unknown WIM event %s(%d), cannot determine netlink channel ID",
			mcp_event_to_str(event_type), event_type);
		skb_push(skb, sizeof(*hif));
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, true, false);
	}

	return 0;
}

/**
 * nrc_mcp_callback_init - Initialize MCP callback system
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_mcp_callback_init(void)
{
	int ret;

	ret = nrc_hal_register_callback(NRC_FRONTEND_MCP,
					nrc_mcp_hal_callback_handler);
	if (ret) {
		ERR_MCP("Failed to register HAL callback: %d", ret);
		return ret;
	}

	return 0;
}

/**
 * nrc_mcp_callback_cleanup - Cleanup MCP callback system
 */
void nrc_mcp_callback_cleanup(void)
{
	nrc_hal_unregister_callback(NRC_FRONTEND_MCP);
}
