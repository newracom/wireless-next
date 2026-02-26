/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC HAL Module Initialization
 * Handles module loading and unloading for HAL core
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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

/* Common directory headers - Interfaces */
#include "nrc-backend-hif-interface.h"
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"

/* Local module headers */
#include "nrc-hal-ops-impl.h"
#include "hif.h"
#include "nrc-init.h"
#include "nrc-debug.h"
#include "nrc-ps.h"

/* Global HAL operations */
struct nrc_hal_ops *g_hal_ops = NULL;
DEFINE_MUTEX(hal_ops_mutex);

/* Global network device (set by WLAN layer) */
struct nrc *g_nw_from_wlan = NULL;
DEFINE_MUTEX(nw_mutex);

/* Platform device for HAL */
struct platform_device *nrc_hal_device = NULL;

/**
 * Register HAL operations
 */
static int nrc_hal_register_ops(struct nrc_hal_ops *ops)
{
	int ret = 0;

	mutex_lock(&hal_ops_mutex);
	if (g_hal_ops) {
		ERR_HAL("HAL ops already registered");
		ret = -EEXIST;
	} else {
		g_hal_ops = ops;
		// pr_info("HAL ops registered successfully\n");
	}
	mutex_unlock(&hal_ops_mutex);

	return ret;
}

/**
 * Unregister HAL operations
 */
static void nrc_hal_unregister_ops(void)
{
	mutex_lock(&hal_ops_mutex);
	g_hal_ops = NULL;
	mutex_unlock(&hal_ops_mutex);
}

#define MAX_RETRY_CNT 3

/**
 * nrc_hal_hdev_init - Initialize hdev workqueues and core resources
 * @hdev: HIF device structure
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_hal_hdev_init(struct nrc_hif_device *hdev)
{
	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return -EINVAL;
	}

	/* WLAN frontend workqueue */
	hdev->workqueue =
		alloc_workqueue("nrc_wlan_wq", WQ_UNBOUND | WQ_HIGHPRI, 1);
	if (!hdev->workqueue) {
		ERR_HIF("Failed to create WLAN workqueue");
		return -ENOMEM;
	}

	/* MCP frontend workqueue */
	hdev->mcp_workqueue =
		alloc_workqueue("nrc_mcp_wq", WQ_UNBOUND | WQ_HIGHPRI, 1);
	if (!hdev->mcp_workqueue) {
		ERR_HIF("Failed to create MCP workqueue");
		destroy_workqueue(hdev->workqueue);
		hdev->workqueue = NULL;
		return -ENOMEM;
	}

	hdev->event_workqueue =
		alloc_workqueue("nrc_event_wq", WQ_UNBOUND | WQ_HIGHPRI, 1);
	if (!hdev->event_workqueue) {
		ERR_HIF("Failed to create event workqueue");
		destroy_workqueue(hdev->mcp_workqueue);
		destroy_workqueue(hdev->workqueue);
		hdev->mcp_workqueue = NULL;
		hdev->workqueue = NULL;
		return -ENOMEM;
	}

	hdev->restart_workqueue = create_singlethread_workqueue("nrc_restart");
	if (!hdev->restart_workqueue) {
		ERR_HIF("Failed to create restart workqueue");
		destroy_workqueue(hdev->event_workqueue);
		destroy_workqueue(hdev->mcp_workqueue);
		destroy_workqueue(hdev->workqueue);
		hdev->event_workqueue = NULL;
		hdev->mcp_workqueue = NULL;
		hdev->workqueue = NULL;
		return -ENOMEM;
	}

	/* Initialize additional hdev components */
	nrc_backend_set_hal_core_refs(hdev);
	nrc_core_init_debugfs(hdev);
	nrc_init_credit_queue(hdev);

	INFO("HIF device resources initialized successfully");
	return 0;
}

/**
 * nrc_hal_hdev_cleanup - Clean up hdev workqueues and core resources
 * @hdev: HIF device structure
 */
static void nrc_hal_hdev_cleanup(struct nrc_hif_device *hdev)
{
	if (!hdev) {
		ERR_HIF("Invalid HIF device");
		return;
	}

	/* Flush and destroy workqueues in reverse order */
	if (hdev->restart_workqueue) {
		flush_workqueue(hdev->restart_workqueue);
		destroy_workqueue(hdev->restart_workqueue);
		hdev->restart_workqueue = NULL;
	}

	if (hdev->event_workqueue) {
		flush_workqueue(hdev->event_workqueue);
		destroy_workqueue(hdev->event_workqueue);
		hdev->event_workqueue = NULL;
	}

	/* MCP frontend workqueue */
	if (hdev->mcp_workqueue) {
		flush_workqueue(hdev->mcp_workqueue);
		destroy_workqueue(hdev->mcp_workqueue);
		hdev->mcp_workqueue = NULL;
	}

	/* WLAN frontend workqueue */
	if (hdev->workqueue) {
		flush_workqueue(hdev->workqueue);
		destroy_workqueue(hdev->workqueue);
		hdev->workqueue = NULL;
	}

	/* Clean up additional hdev components */
	nrc_core_exit_debugfs();
	/* Note: nrc_backend_set_hal_core_refs and nrc_init_credit_queue don't need explicit cleanup */

	INFO("HIF device resources cleaned up");
}

/**
 * nrc_hal_probe_hif_device - Probe and initialize HIF device
 * @hdev: HIF device to probe
 *
 * Performs HIF device reset and probe with retry mechanism
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_hal_probe_hif_device(struct nrc_hif_device *hdev)
{
	int ret;
	int retry = 0;

try:
	nrc_hif_ops_reset_device();
	ret = nrc_hif_ops_probe();
	if (ret && retry < MAX_RETRY_CNT) {
		retry++;
		goto try;
	}

	if (ret) {
		ERR_HIF("Failed to nrc_hif_probe %d", ret);
		return -ENODEV;
	}

	return 0;
}

/**
 * Platform driver probe function
 * Called when a platform device is matched with this driver
 */
static int nrc_hal_probe(struct platform_device *pdev)
{
	int ret;
	struct nrc_spi_device_info *spi_info;
	struct nrc_hif_device *hdev;

	if (!nrc_spi_is_device_available()) {
		ERR_HIF("HIF backend module not found. Please load nrc_spi module first.");
		return -ENODEV;
	}

	/* Get SPI device information */
	spi_info = nrc_spi_get_device_info();
	if (!spi_info) {
		ERR_HIF("Failed to get SPI device information");
		return -ENODEV;
	}

	/* Set platform device parent to SPI device for proper device hierarchy */
	/* Temporarily disable parent setting to isolate runtime PM issue */
	if (spi_info->dev) {
		INFO("HIF device found: %s (parent setting disabled for testing)",
		     dev_name(spi_info->dev));
	} else {
		ERR_HIF("HIF device is NULL");
		return -ENODEV;
	}

	hdev = nrc_hif_alloc(spi_info->dev, spi_info->priv, spi_info->ops);
	if (IS_ERR(hdev)) {
		ERR_HIF("Failed to allocate HIF device: %ld", PTR_ERR(hdev));
		return PTR_ERR(hdev);
	}

	/* Set HIF device properties */
	hdev->dev = &pdev->dev;

	/* Update global platform device reference with actual probed device */
	nrc_hal_device = pdev;

	/* Initialize HAL components */
	ret = nrc_hal_callback_init();
	if (ret) {
		ERR_HIF("Failed to initialize HAL callback system: %d", ret);
		g_nw_from_wlan = NULL;
		return ret;
	}

	/* Initialize debug system with Platform device */
	nrc_dbg_init(&pdev->dev);

	/* Register default HAL operations */
	nrc_hal_register_ops(nrc_hal_get_default_ops());

	/* Store HIF device in platform data temporarily until WLAN initialization */
	platform_set_drvdata(pdev, hdev);

	/* Probe and initialize HIF device after platform setup */
	ret = nrc_hal_probe_hif_device(hdev);
	if (ret) {
		ERR_HIF("Failed to probe HIF device: %d", ret);
		platform_set_drvdata(pdev, NULL);
		nrc_hal_hdev_cleanup(hdev);
		nrc_hif_free(hdev);
		return ret;
	}

	/* Initialize hdev workqueues and core resources after HIF probe */
	ret = nrc_hal_hdev_init(hdev);
	if (ret) {
		ERR_HIF("Failed to initialize hdev resources: %d", ret);
		platform_set_drvdata(pdev, NULL);
		nrc_hal_hdev_cleanup(hdev);
		nrc_hif_free(hdev);
		return ret;
	}

	INFO("NRC HAL platform driver probed successfully\n");
	return 0;
}

/**
 * Platform driver remove function
 * Called when platform device is removed
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void nrc_hal_remove(struct platform_device *pdev)
#else
static int nrc_hal_remove(struct platform_device *pdev)
#endif
{
	struct nrc_hif_device *hdev = platform_get_drvdata(pdev);

	// DBG_STATE("HAL: Platform device remove called\n");

	/* Ensure device is awake before cleanup to prevent SPI errors */
	if (hdev && !NRC_PS_IS_AWAKE(hdev)) {
		DBG_PS("HAL remove: Device not awake, requesting wake before cleanup");
		nrc_ps_request_wake_sync(hdev, 2000,
					 NRC_PS_REASON_HAL_SHUTDOWN);
	}

	/* Clear network device reference */
	g_nw_from_wlan = NULL;

	/* Unregister HAL operations */
	nrc_hal_unregister_ops();

	/* Cleanup HAL callback system */
	nrc_hal_callback_cleanup();

	/* Perform device reset before module cleanup */
	nrc_hif_ops_reset_device();

	/* Clean up hdev resources and free HIF device
	 * Note: During this call, hdev is still valid and nrc_hal_core_get_hdev()
	 * will return the valid hdev pointer. This allows GPIO cleanup to work. */
	if (hdev) {
		nrc_hal_hdev_cleanup(hdev);
		nrc_hif_free(hdev);
	}

	/* Clear platform driver data AFTER freeing hdev
	 * This prevents use-after-free by making nrc_hal_core_get_hdev() return NULL
	 * after all cleanup is complete */
	platform_set_drvdata(pdev, NULL);

	INFO("NRC HAL platform driver removed\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

/* ===========================================================================
 * Module Parameters
 * =========================================================================== */

/* Debug level: 0=ERR, 1=WARN, 2=INFO, 3=DBG */
int debug_level = DEFAULT_NRC_DBG_LEVEL;
module_param(debug_level, int, 0600);
MODULE_PARM_DESC(debug_level, "Debug level (0=ERR, 1=WARN, 2=INFO, 3=DBG)");

/* Debug mask: bitmask for categories */
unsigned long debug_mask = DEFAULT_NRC_DBG_MASK;
module_param(debug_mask, ulong, 0600);
MODULE_PARM_DESC(
	debug_mask,
	"Debug category mask (BASIC=0x1, HIF=0x2, WIM=0x4, TX=0x8, RX=0x10, MAC=0x20, CAPI=0x40, PS=0x80, STATE=0x100, BD=0x200, FW=0x400, AMPDU=0x800, CREDIT=0x1000, SLOT=0x2000, BUS=0x4000, ALL=0xFFFFFFFF)");

/* Platform device ID table for matching multiple device names */
static struct platform_device_id hal_device_ids[] = {
	{"nrc-hal", 0},
	{"core", 0},
	{},
};
MODULE_DEVICE_TABLE(platform, hal_device_ids);

/* Platform driver structure */
struct platform_driver nrc_hal_driver = {
	.probe = nrc_hal_probe,
	.remove = nrc_hal_remove,
	.id_table = hal_device_ids,
	.driver =
		{
			.name = "nrc-hal",
		},
};

/**
 * nrc_core_init - Initialize NRC HAL core module
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init nrc_core_init(void)
{
	int ret;

	// INFO("NRC Core HAL Module loaded");

	/* Register platform driver */
	ret = platform_driver_register(&nrc_hal_driver);
	if (ret) {
		ERR_HIF("Failed to register HAL platform driver: %d", ret);
		return ret;
	}

	/* Create platform device to trigger probe */
	nrc_hal_device = platform_device_register_simple("core", -1, NULL, 0);
	if (IS_ERR(nrc_hal_device)) {
		ERR_HIF("Failed to register HAL platform device: %ld",
			PTR_ERR(nrc_hal_device));
		platform_driver_unregister(&nrc_hal_driver);
		return PTR_ERR(nrc_hal_device);
	}

	// INFO("NRC HAL platform driver and device registered");
	return 0;
}

/**
 * nrc_core_exit - Cleanup NRC HAL core module
 */
static void __exit nrc_core_exit(void)
{
	/* Unregister platform device */
	if (nrc_hal_device) {
		platform_device_unregister(nrc_hal_device);
		nrc_hal_device = NULL;
	}

	/* Unregister platform driver */
	platform_driver_unregister(&nrc_hal_driver);
}

module_init(nrc_core_init);
module_exit(nrc_core_exit);

MODULE_DESCRIPTION("Newracom NRC Core HAL Driver");
MODULE_AUTHOR("Newracom, Inc.");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");
