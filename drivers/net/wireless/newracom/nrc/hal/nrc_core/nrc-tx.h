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

#ifndef __NRC_TX_H__
#define __NRC_TX_H__

#include <linux/workqueue.h>

/* Forward declarations */
struct nrc;
struct sk_buff;

/**
 * nrc_hif_wlan_work - WLAN frontend HIF transmission work handler
 * @work: work structure
 *
 * Handles transmission of WLAN frames and WIM commands through the HIF layer.
 * Processes queues in priority order with special handling for deauth frames.
 */
void nrc_hif_wlan_work(struct work_struct *work);

/**
 * nrc_hif_mcp_work - MCP frontend HIF transmission work handler
 * @work: work structure
 *
 * Handles transmission of MCP frames and commands through the HIF layer.
 */
void nrc_hif_mcp_work(struct work_struct *work);

void nrc_tx_flush_wq(struct nrc_hif_device *hdev);
void nrc_tx_cleanup_queues(void);

#endif /* __NRC_TX_H__ */
