/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Debug Header - WLAN Frontend debug interface
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

#ifndef _NRC_WLAN_DEBUG_H_
#define _NRC_WLAN_DEBUG_H_

#include <linux/device.h>

/* Include common debug interface */
#include "nrc-debug-common.h"

/* Forward declarations */
struct nrc;

/* Global device pointer */
extern struct device *g_dev;

/* WLAN-specific debug macros */
#define INFO_WLAN(fmt, ...) INFo("Wlan", fmt, ##__VA_ARGS__)
#define WARN_WLAN(fmt, ...) WARn("Wlan", fmt, ##__VA_ARGS__)
#define ERR_WLAN(fmt, ...) ERR("Wlan", fmt, ##__VA_ARGS__)

/* WLAN Debug Functions */
void nrc_init_debugfs(struct nrc *nw);
void nrc_exit_debugfs(struct nrc *nw);

#endif /* _NRC_WLAN_DEBUG_H_ */