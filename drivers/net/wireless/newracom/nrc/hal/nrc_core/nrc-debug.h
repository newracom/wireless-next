/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC Debug Header - HAL Core debug interface
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

#ifndef _NRC_DEBUG_H_
#define _NRC_DEBUG_H_

#include <linux/skbuff.h>

/* Common debug macros and definitions */
#include "nrc-debug-common.h"

/* HAL Core-specific debug macros */
#define INFO_HAL(fmt, ...) INFo("Hal", fmt, ##__VA_ARGS__)
#define WARN_HAL(fmt, ...) WARn("Hal", fmt, ##__VA_ARGS__)
#define ERR_HAL(fmt, ...) ERR("Hal", fmt, ##__VA_ARGS__)

struct nrc_hif_device;
struct nrc_debug;

/* Core Debugfs Functions
 *
 * Core module provides centralized debugfs at /sys/kernel/debug/nrc_core/
 * with the following entries (shared by all frontends):
 *
 * Common entries:
 *   - nrc_credit: TX credit queue status monitoring
 *   - nrc_debug: Debug message mask control
 *   - nrc_cspi: SPI interface status testing
 *   - nrc_hif: HIF loopback test (frame transmission test)
 *   - nrc_reset: Device hardware reset trigger
 *   - nrc_restart: Network stack restart trigger
 *
 * Hardware device (hdev) entries:
 *   - skb_stats: SKB memory statistics (RX/TX allocation/free/leak by type)
 */
void nrc_core_init_debugfs(struct nrc_hif_device *hdev);
void nrc_core_exit_debugfs(void);
struct nrc_debug *nrc_debug_alloc(void);
void nrc_debug_free(struct nrc_debug *debug);

/* WIM Debug Functions */
const char *nrc_wim_cmd_str(int cmd);
const char *nrc_wim_event_str(int event);
const char *nrc_wim_subtype_str(u8 subtype);
void nrc_dump_wim(struct sk_buff *skb);

/* HAL Debug Functions */
void nrc_hal_debug_send(struct sk_buff *skb);

/* Dump Functions */
void nrc_dump_store(char *src, int len);

#endif /* _NRC_DEBUG_H_ */