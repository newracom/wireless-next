// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Debug Implementation
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

/* Debug device for this layer; level and mask are module parameters in nrc-mcp-params.c */
struct device *nrc_debug_dev;

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
	*val = nrc_debug_mask;
	return 0;
}

static int nrc_mcp_debugfs_debug_write(void *data, u64 val)
{
	nrc_debug_mask = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_mcp_debugfs_debug_fops,
			nrc_mcp_debugfs_debug_read,
			nrc_mcp_debugfs_debug_write, "%llu\n");

/* MCP module-specific debug level control */
static int nrc_mcp_debugfs_level_read(void *data, u64 *val)
{
	*val = nrc_debug_level;
	return 0;
}

static int nrc_mcp_debugfs_level_write(void *data, u64 val)
{
	if (val < NRC_DBG_LEVEL_MAX)
		nrc_debug_level = (enum NRC_DEBUG_LEVEL)val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_mcp_debugfs_level_fops,
			nrc_mcp_debugfs_level_read,
			nrc_mcp_debugfs_level_write, "%llu\n");
#endif /* CONFIG_DEBUG_FS */

/**
 * nrc_mcp_init_debugfs - Initialize MCP debugfs interface
 * @mcp: MCP device structure
 *
 * Note: Common debugfs entries (nrc_credit, nrc_debug, nrc_cspi, nrc_reset, nrc_restart)
 * are now centralized in nrc_core module at /sys/kernel/debug/nrc_core/
 * This avoids duplication between WLAN and MCP frontends.
 *
 * MCP-specific debugfs entries (if any) would be created here.
 */
void nrc_mcp_init_debugfs(struct mcp_priv *mcp)
{
#ifdef CONFIG_DEBUG_FS
	if (!mcp) {
		ERR("Invalid MCP device for debugfs");
		return;
	}

	/* Create MCP debugfs root directory for future MCP-specific entries */
	mcp_debugfs_root = debugfs_create_dir("nrc_mcp", NULL);
	if (!mcp_debugfs_root) {
		ERR("Failed to create MCP debugfs root");
		return;
	}

	/* MCP module-specific debug mask and level */
	debugfs_create_file("debug_mask", 0600, mcp_debugfs_root, mcp,
			    &nrc_mcp_debugfs_debug_fops);
	debugfs_create_file("debug_level", 0600, mcp_debugfs_root, mcp,
			    &nrc_mcp_debugfs_level_fops);

	/* MCP-specific debugfs entries would be created here if needed */
	/* Common entries are now in /sys/kernel/debug/nrc_core/ */

	INFO("MCP debugfs initialized at /sys/kernel/debug/nrc_mcp (common entries in nrc_core)");
#endif
}

/**
 * nrc_mcp_exit_debugfs - Cleanup MCP debugfs interface
 * @mcp: MCP device structure
 *
 * Removes all MCP debugfs entries.
 */
void nrc_mcp_exit_debugfs(struct mcp_priv *mcp)
{
#ifdef CONFIG_DEBUG_FS
	if (mcp_debugfs_root) {
		debugfs_remove_recursive(mcp_debugfs_root);
		mcp_debugfs_root = NULL;
	}
#endif
}
