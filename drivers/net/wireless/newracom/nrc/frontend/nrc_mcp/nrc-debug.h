/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Debug Header - MCP Frontend debug interface
 */

#ifndef _NRC_MCP_DEBUG_H_
#define _NRC_MCP_DEBUG_H_

#include <linux/device.h>

/* Include common debug interface */
#include "nrc-debug-common.h"

/* MCP layer aliases */
#ifndef ERR_MCP
#define ERR_MCP(fmt, ...) ERR(fmt, ##__VA_ARGS__)
#endif
#ifndef WARN_MCP
#define WARN_MCP(fmt, ...) WRN(fmt, ##__VA_ARGS__)
#endif
#ifndef INFO_MCP
#define INFO_MCP(fmt, ...) INFO(fmt, ##__VA_ARGS__)
#endif
#ifndef DBG_MCP
#define DBG_MCP(fmt, ...) DBG(CAT(BASIC), fmt, ##__VA_ARGS__)
#endif
#ifndef VBS_MCP
#define VBS_MCP(fmt, ...) VBS(CAT(BASIC), fmt, ##__VA_ARGS__)
#endif


/* Forward declarations */
struct mcp_priv;

/* MCP debug macros:
 * Module identity is provided by the kernel device prefix (e.g., "nrc-mcp:")
 * Use generic INFO/WARN/ERR from nrc-debug-common.h directly.
 */

/* MCP Debug Functions */
void nrc_mcp_init_debugfs(struct mcp_priv *mcp);
void nrc_mcp_exit_debugfs(struct mcp_priv *mcp);

#endif /* _NRC_MCP_DEBUG_H_ */
