/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Debug Header - WLAN Frontend debug interface
 */

#ifndef _NRC_WLAN_DEBUG_H_
#define _NRC_WLAN_DEBUG_H_

#include <linux/device.h>

/* Include common debug interface */
#include "nrc-debug-common.h"

/* WLAN layer aliases */
#ifndef ERR_WLAN
#define ERR_WLAN(fmt, ...) ERR(fmt, ##__VA_ARGS__)
#endif
#ifndef WARN_WLAN
#define WARN_WLAN(fmt, ...) WRN(fmt, ##__VA_ARGS__)
#endif
#ifndef INFO_WLAN
#define INFO_WLAN(fmt, ...) INFO(fmt, ##__VA_ARGS__)
#endif
#ifndef DBG_WLAN
#define DBG_WLAN(fmt, ...) DBG(CAT(BASIC), fmt, ##__VA_ARGS__)
#endif
#ifndef VBS_WLAN
#define VBS_WLAN(fmt, ...) VBS(CAT(BASIC), fmt, ##__VA_ARGS__)
#endif


/* Forward declarations */
struct nrc;

/* WLAN debug macros:
 * Module identity is provided by the kernel device prefix (e.g., "ieee80211 nrc80211:")
 * Use generic INFO/WARN/ERR from nrc-debug-common.h directly.
 */

/* WLAN Debug Functions */
void nrc_init_debugfs(struct nrc *nw);
void nrc_exit_debugfs(struct nrc *nw);

#endif /* _NRC_WLAN_DEBUG_H_ */
