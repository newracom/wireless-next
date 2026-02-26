/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Debug Implementation
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
#include <linux/debugfs.h>
#include <linux/math64.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"
#include "mcp.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-wim-types.h"

/* Local module headers */
#include "nrc-hal-core-interface.h"
#include "nrc-mcp-init.h"
#include "nrc-debug.h"

/* Global debug variables - defined as module parameters in nrc-mcp-params.c */
extern unsigned long debug_mask;
extern int debug_level;
struct device *g_dev;

#ifdef CONFIG_DEBUG_FS
/* MCP debugfs root directory */
static struct dentry *mcp_debugfs_root;

/* Note: Common debugfs entries (nrc_credit, nrc_debug, nrc_cspi, nrc_reset, nrc_restart)
 * have been moved to nrc_core module at /sys/kernel/debug/nrc_core/
 * to avoid duplication between WLAN and MCP frontends.
 */

/* MCP module-specific debug mask control */
static int nrc_mcp_debugfs_debug_read(void *data, u64 *val)
{
	*val = debug_mask;
	return 0;
}

static int nrc_mcp_debugfs_debug_write(void *data, u64 val)
{
	debug_mask = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_mcp_debugfs_debug_fops,
			nrc_mcp_debugfs_debug_read,
			nrc_mcp_debugfs_debug_write, "%llu\n");

/* MCP module-specific debug level control */
static int nrc_mcp_debugfs_level_read(void *data, u64 *val)
{
	*val = debug_level;
	return 0;
}

static int nrc_mcp_debugfs_level_write(void *data, u64 val)
{
	if (val < NRC_DBG_LEVEL_MAX)
		debug_level = (enum NRC_DEBUG_LEVEL)val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_mcp_debugfs_level_fops,
			nrc_mcp_debugfs_level_read,
			nrc_mcp_debugfs_level_write, "%llu\n");
#endif /* CONFIG_DEBUG_FS */

/**
 * nrc_init_debugfs - Initialize MCP debugfs interface
 * @mcp: MCP device structure
 *
 * Note: Common debugfs entries (nrc_credit, nrc_debug, nrc_cspi, nrc_reset, nrc_restart)
 * are now centralized in nrc_core module at /sys/kernel/debug/nrc_core/
 * This avoids duplication between WLAN and MCP frontends.
 *
 * MCP-specific debugfs entries (if any) would be created here.
 */
void nrc_init_debugfs(struct mcp_priv *mcp)
{
#ifdef CONFIG_DEBUG_FS
	if (!mcp) {
		ERR_MCP("Invalid MCP device for debugfs");
		return;
	}

	/* Create MCP debugfs root directory for future MCP-specific entries */
	mcp_debugfs_root = debugfs_create_dir("nrc_mcp", NULL);
	if (!mcp_debugfs_root) {
		ERR_MCP("Failed to create MCP debugfs root");
		return;
	}

	/* MCP module-specific debug mask and level */
	debugfs_create_file("debug_mask", 0664, mcp_debugfs_root, mcp,
			    &nrc_mcp_debugfs_debug_fops);
	debugfs_create_file("debug_level", 0664, mcp_debugfs_root, mcp,
			    &nrc_mcp_debugfs_level_fops);

	/* MCP-specific debugfs entries would be created here if needed */
	/* Common entries are now in /sys/kernel/debug/nrc_core/ */

	INFO("MCP debugfs initialized at /sys/kernel/debug/nrc_mcp (common entries in nrc_core)");
#endif
}

/**
 * nrc_exit_debugfs - Cleanup MCP debugfs interface
 * @mcp: MCP device structure
 *
 * Removes all MCP debugfs entries.
 */
void nrc_exit_debugfs(struct mcp_priv *mcp)
{
#ifdef CONFIG_DEBUG_FS
	if (mcp_debugfs_root) {
		debugfs_remove_recursive(mcp_debugfs_root);
		mcp_debugfs_root = NULL;
	}
#endif
}