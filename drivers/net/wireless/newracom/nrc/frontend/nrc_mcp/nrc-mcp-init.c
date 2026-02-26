/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Module Initialization
 * Handles module loading and unloading for MCP module
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
#include <linux/init.h>
#include <linux/module.h>

/* Common directory headers */
#include "nrc.h"
#include "mcp.h"
#include "nrc-hif.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"

/* Local module headers */
#include "nrc-mcp-init.h"
#include "nrc-mcp-params.h"
#include "nrc-mcp-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-netlink-driver.h"
#include "nrc-debug.h"

/* Global MCP device */
static struct mcp_priv *g_mcp_dev = NULL;

/* Virtual device for MCP module logging */
static struct device *g_mcp_virtual_dev = NULL;

static struct mcp_priv *nrc_mcp_alloc(struct device *dev,
				      struct nrc_hif_device *hdev)
{
	struct mcp_priv *mcp;

	if (!dev || !hdev) {
		ERR_MCP("Invalid device or HIF device");
		return NULL;
	}

	mcp = kzalloc(sizeof(struct mcp_priv), GFP_KERNEL);
	if (!mcp) {
		ERR_MCP("Failed to allocate MCP structure");
		return NULL;
	}

	mcp->dev = dev;
	mcp->hdev = hdev;

	mcp->params = hdev->params;
	mcp->debug = hdev->debug;

	nrc_mcp_sync_params(mcp);

	return mcp;
}

static void nrc_mcp_free(struct mcp_priv *mcp)
{
	if (!mcp)
		return;

	mcp->params = NULL;
	mcp->debug = NULL;

	kfree(mcp);
}

struct mcp_priv *nrc_mcp_get_device(void)
{
	return g_mcp_dev;
}

bool nrc_mcp_is_initialized(void)
{
	return g_mcp_dev != NULL;
}

static int nrc_mcp_module_init(void)
{
	struct nrc_hal_ops *hal_ops;
	struct nrc_hif_device *hdev;
	int ret;

	// pr_info("NRC MCP Module initializing...\n");

	/* Initialize debug system early */
	nrc_dbg_init(NULL); /* Device will be set later when available */

	/* Check if HAL is available */
	if (!nrc_hal_core_is_init()) {
		ERR_MCP("HAL core not initialized");
		return -ENODEV;
	}

	/* Get HAL operations */
	hal_ops = nrc_hal_core_get_ops();
	if (!hal_ops) {
		ERR_MCP("HAL operations not available");
		return -ENODEV;
	}

	/* Get HIF device from HAL */
	hdev = nrc_hal_core_get_hdev();
	if (!hdev) {
		ERR_MCP("HIF device not available");
		return -ENODEV;
	}

	/* Create virtual device for MCP logging */
	g_mcp_virtual_dev = root_device_register("nrc-mcp");
	if (IS_ERR(g_mcp_virtual_dev)) {
		ERR_MCP("Failed to register virtual device");
		g_mcp_virtual_dev = NULL;
		/* Continue without device - will use pr_info */
	}

	/* 1. Allocate MCP device structure */
	g_mcp_dev = nrc_mcp_alloc(nrc_hal_core_get_dev(), hdev);
	if (!g_mcp_dev) {
		ERR_MCP("Failed to allocate MCP device");
		ret = -ENOMEM;
		goto err_unregister_dev;
	}

	/* Update debug system to use MCP virtual device */
	g_dev = g_mcp_virtual_dev;

	/* Initialize MCP callback system */
	ret = nrc_mcp_callback_init();
	if (ret) {
		ERR_MCP("Failed to initialize callback system: %d", ret);
		goto err_free_mcp;
	}

	/* 2. Initialize HAL core with MCP device (allocates hdev->fw.priv if needed) */
	ret = nrc_hal_core_nw_init(NULL, hdev);
	if (ret) {
		ERR_MCP("Failed to initialize HAL core: %d", ret);
		goto err_cleanup_callback;
	}

	/* 3. Initialize netlink interface */
	ret = netlink_driver_init(hdev);
	if (ret) {
		ERR_MCP("Failed to initialize netlink interface: %d", ret);
		goto err_cleanup_hal;
	}

	/* 4. Start network device through HAL ops wrapper */
	if (!hdev->started) {
		ret = nrc_hal_ops_nw_start(false);
		if (ret) {
			ERR_MCP("Failed to start network device: %d", ret);
			goto err_cleanup_netlink;
		}
	}

	/* 5. Initialize debugfs for SKB monitoring */
	nrc_init_debugfs(g_mcp_dev);

	INFO("NRC MCP Module initialized successfully");
	return 0;

err_cleanup_netlink:
	netlink_driver_exit();
err_cleanup_hal:
	nrc_hal_core_nw_cleanup(hdev, NULL);
err_cleanup_callback:
	nrc_mcp_callback_cleanup();
err_free_mcp:
	nrc_mcp_free(g_mcp_dev);
	g_mcp_dev = NULL;
err_unregister_dev:
	if (g_mcp_virtual_dev) {
		root_device_unregister(g_mcp_virtual_dev);
		g_mcp_virtual_dev = NULL;
	}
	return ret;
}

static void nrc_mcp_module_exit(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	// pr_info("NRC MCP Module exiting...\n");

	/* Cleanup debugfs */
	if (g_mcp_dev) {
		nrc_exit_debugfs(g_mcp_dev);
	}

	/* Cleanup netlink interface */
	netlink_driver_exit();

	/* Cleanup HAL core resources */
	if (hdev) {
		nrc_hal_core_nw_cleanup(hdev, NULL);
	}

	/* Cleanup callback system */
	nrc_mcp_callback_cleanup();

	/* Free MCP device */
	if (g_mcp_dev) {
		nrc_mcp_free(g_mcp_dev);
		g_mcp_dev = NULL;
	}

	/* Log cleanup before unregistering device (INFO uses device pointer) */
	INFO("NRC MCP Module cleaned up");

	/* Unregister virtual device */
	if (g_mcp_virtual_dev) {
		root_device_unregister(g_mcp_virtual_dev);
		g_mcp_virtual_dev = NULL;
	}
}

/**
 * nrc_mcp_init - Initialize NRC MCP module
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init nrc_mcp_init(void)
{
	return nrc_mcp_module_init();
}

/**
 * nrc_mcp_exit - Cleanup NRC MCP module
 */
static void __exit nrc_mcp_exit(void)
{
	nrc_mcp_module_exit();
}

module_init(nrc_mcp_init);
module_exit(nrc_mcp_exit);

MODULE_AUTHOR("Newracom, Inc.(http://www.newracom.com)");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Newracom MCP driver");
