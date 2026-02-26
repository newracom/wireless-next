/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Module Parameters
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
#include <linux/string.h>
#include <linux/slab.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "mcp.h"
#include "nrc-build-config.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-vendor.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Local module headers */
#include "nrc-log.h"
#include "nrc-mcp-init.h"
#include "nrc-mcp-params.h"

char *fw_name = "sample_nrc7394.bin";
module_param(fw_name, charp, 0444);
MODULE_PARM_DESC(fw_name, "Firmware file name");

char *bd_name = "bd.dat";
module_param(bd_name, charp, 0600);
MODULE_PARM_DESC(bd_name, "Board Data file name");

bool mcp_priority = false;
module_param(mcp_priority, bool, 0644);
MODULE_PARM_DESC(
	mcp_priority,
	"Enable MCP priority: suspend WLAN TX when MCP is transmitting (default: false)");

/* Debug level: 0=ERR, 1=WARN, 2=INFO, 3=DBG */
int debug_level = DEFAULT_NRC_DBG_LEVEL;
module_param(debug_level, int, 0600);
MODULE_PARM_DESC(debug_level, "Debug level (0=ERR, 1=WARN, 2=INFO, 3=DBG)");

/* Debug mask: bitmask for categories */
unsigned long debug_mask = DEFAULT_NRC_DBG_MASK;
module_param(debug_mask, ulong, 0600);
MODULE_PARM_DESC(debug_mask, "Debug category mask (BASIC=0x1, HIF=0x2, WIM=0x4, TX=0x8, RX=0x10, MAC=0x20, CAPI=0x40, PS=0x80, STATS=0x100, STATE=0x200, BD=0x400, FW=0x800, AMPDU=0x1000, CREDIT=0x2000, SLOT=0x4000, BUS=0x8000, ALL=0xFFFFFFFF)");

/* ===========================================================================
 * Parameter Synchronization Functions
 * =========================================================================== */

/**
 * nrc_mcp_sync_params - Synchronize MCP parameters to mcp_priv structure
 * @mcp: MCP device structure
 *
 * This function copies MCP module parameters to the mcp_priv params structure
 * for organized access by MCP layer. Allocates memory for strings to avoid
 * dangling pointers when module is unloaded.
 */
void nrc_mcp_sync_params(struct mcp_priv *mcp)
{
	struct nrc_params *params = mcp->params;

	if (!mcp || !mcp->params) {
		ERR_MCP("Invalid mcp structure for parameter synchronization");
		return;
	}

	/* Set fw_name only if not already set by first frontend */
	if (!params->fw_name) {
		if (fw_name) {
			params->fw_name = kstrdup(fw_name, GFP_KERNEL);
			if (!params->fw_name) {
				ERR_MCP("Failed to allocate memory for fw_name");
				return;
			}
		}
	} else if (fw_name && strcmp(fw_name, params->fw_name) != 0) {
		WARN_MCP(
			"MCP fw_name ('%s') differs from first frontend ('%s') - using first frontend's FW",
			fw_name, params->fw_name);
	}

	/* Set bd_name only if not already set by first frontend */
	if (!params->bd_name) {
		if (bd_name) {
			params->bd_name = kstrdup(bd_name, GFP_KERNEL);
			if (!params->bd_name) {
				ERR_MCP("Failed to allocate memory for bd_name");
				/* Clean up fw_name if bd_name allocation fails */
				if (params->fw_name) {
					kfree(params->fw_name);
					params->fw_name = NULL;
				}
				return;
			}
		}
	} else if (bd_name && strcmp(bd_name, params->bd_name) != 0) {
		WARN_MCP(
			"MCP bd_name ('%s') differs from first frontend ('%s') - using first frontend's BD",
			bd_name, params->bd_name);
	}

	/* MCP priority can be set independently */
	params->mcp_priority = mcp_priority;
}
