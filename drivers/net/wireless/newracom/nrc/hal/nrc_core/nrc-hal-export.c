/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC HAL Export Functions
 * Exports HAL functions and variables for other modules
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
#include <linux/mutex.h>
#include <linux/platform_device.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-wim-types.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Common directory headers - Interfaces */
#include "nrc-backend-hif-interface.h"
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"

/* Local module headers */
#include "nrc-hal-ops-impl.h"
#include "nrc-init.h"
#include "wim.h"

/* Global HAL operations */
extern struct nrc_hal_ops *g_hal_ops;
extern struct mutex hal_ops_mutex;

/* Global network device (set by WLAN layer) */
extern struct nrc *g_nw_from_wlan;
extern struct mutex nw_mutex;

/* Platform device for HAL */
extern struct platform_device *nrc_hal_device;
extern struct platform_driver nrc_hal_driver;

/**
 * nrc_hal_core_get_ops - Get HAL operations
 *
 * Returns: HAL operations structure or NULL
 */
struct nrc_hal_ops *nrc_hal_core_get_ops(void)
{
	return READ_ONCE(g_hal_ops);
}
EXPORT_SYMBOL(nrc_hal_core_get_ops);

/**
 * nrc_hal_core_is_init - Check if HAL core is initialized
 *
 * Returns: true if initialized, false otherwise
 */
bool nrc_hal_core_is_init(void)
{
	return (nrc_hal_device != NULL && nrc_spi_get_device_info() != NULL);
}
EXPORT_SYMBOL(nrc_hal_core_is_init);

/**
 * nrc_hal_core_get_dev - Get HAL platform device
 *
 * Returns: platform device pointer or NULL
 */
struct device *nrc_hal_core_get_dev(void)
{
	if (nrc_hal_device)
		return &nrc_hal_device->dev;
	return NULL;
}
EXPORT_SYMBOL(nrc_hal_core_get_dev);

/**
 * nrc_hal_core_get_nw - Get network device from WLAN layer
 *
 * Returns: network device pointer or NULL
 */
struct nrc *nrc_hal_core_get_nw(void)
{
	return READ_ONCE(g_nw_from_wlan);
}
EXPORT_SYMBOL(nrc_hal_core_get_nw);

/**
 * nrc_hal_core_get_hdev - Get HIF device for WLAN early initialization
 *
 * Returns: HIF device pointer or NULL
 */
struct nrc_hif_device *nrc_hal_core_get_hdev(void)
{
	if (nrc_hal_device)
		return platform_get_drvdata(nrc_hal_device);
	return NULL;
}
EXPORT_SYMBOL(nrc_hal_core_get_hdev);

/**
 * nrc_hal_core_nw_init - Initialize HAL with network device from WLAN
 * @nw: Network device allocated by WLAN layer
 * @hdev: HIF device for initialization
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_hal_core_nw_init(struct nrc *nw, struct nrc_hif_device *hdev)
{
	int ret;

	if (!hdev) {
		ERR_HAL("Invalid parameters for HAL-CORE initialization");
		return -EINVAL;
	}

	// pr_info("HAL: Initializing HAL with network device from WLAN layer\n");

	if (nw) {
		/* Set network device reference for HAL access */
		mutex_lock(&nw_mutex);
		g_nw_from_wlan = nw;
		mutex_unlock(&nw_mutex);

		/* Link network device with HIF device */
		nw->hdev = hdev;
		hdev->nw = nw;

		/* Share params and debug pointers from hdev */
		nw->params = hdev->params;
		nw->debug = hdev->debug;

		/* Note: fw_name and bd_name are already set by frontend's sync_params */
	}

	/* Initialize HAL firmware */
	ret = nrc_hal_fw_init(hdev);
	if (ret) {
		ERR_HAL("Failed to initialize HAL firmware: %d", ret);
		mutex_lock(&nw_mutex);
		g_nw_from_wlan = NULL;
		mutex_unlock(&nw_mutex);
		return ret;
	}

	/* Increment frontend count */
	atomic_inc(&hdev->frontend_count);

	return 0;
}
EXPORT_SYMBOL(nrc_hal_core_nw_init);

/**
 * nrc_hal_core_nw_cleanup - Cleanup HAL resources
 * @hdev: HIF device for cleanup
 * @nw: Network device reference being cleaned up
 *
 * Note: Uses reference counting to determine when to perform actual cleanup.
 * Only performs cleanup when the last frontend is unloaded (frontend_count reaches 0).
 */
void nrc_hal_core_nw_cleanup(struct nrc_hif_device *hdev, struct nrc *nw)
{
	int count;

	// pr_info("HAL: Cleaning up HAL resources\n");

	if (!hdev) {
		return;
	}

	/* Clear network device reference if it matches the one being cleaned up */
	if (nw && hdev->nw == nw) {
		DBG_HIF("Clearing network device reference (WLAN frontend unloading)");
		hdev->nw = NULL;

		mutex_lock(&nw_mutex);
		if (g_nw_from_wlan == nw) {
			g_nw_from_wlan = NULL;
		}
		mutex_unlock(&nw_mutex);
	}

	/* Decrement frontend count and check if this is the last frontend */
	count = atomic_dec_return(&hdev->frontend_count);

	if (count < 0) {
		/* Should not happen - reset to 0 */
		ERR_HAL("frontend_count went negative, resetting to 0");
		atomic_set(&hdev->frontend_count, 0);
		return;
	}

	if (count == 0) {
		/* Last frontend unloading - send WIM_CMD_STOP to firmware */
		/* 
		 * Send WIM_CMD_STOP before cleanup to gracefully stop firmware.
		 * Use no-response mode (timeout=0) to avoid blocking during cleanup.
		 * 
		 * SAFETY: Check wim_resp validity immediately before use to prevent
		 * use-after-free during concurrent cleanup operations.
		 */
		if (NRC_FW_IS_STARTED(hdev)) {
			if (hdev->wim_resp && hdev->workqueue) {
				DBG_HIF("Last frontend exiting, sending WIM_CMD_STOP");
				/*
				 * Critical: Send WIM_CMD_STOP BEFORE any cleanup that might
				 * free wim_resp. The nrc_wim_request internally checks hdev->wim_resp
				 * again, but we verify here to catch obvious invalid states early.
				 */
				nrc_wim_request(NULL, WIM_CMD_STOP, 0, false, NULL);
			} else {
				DBG_HIF("Last frontend exiting, skipping WIM_CMD_STOP (HIF resources unavailable)");
			}
			NRC_FW_CLEAR_STARTED(hdev);
		} else if (NRC_FW_IS_STARTED(hdev)) {
			DBG_HIF("Last frontend exiting, skipping WIM_CMD_STOP (HIF resources already cleaned)");
			NRC_FW_CLEAR_STARTED(hdev);
		}

		/* Perform full cleanup */
		nrc_hal_fw_cleanup(hdev);

		/* Clear remaining cross-references */
		if (hdev->nw) {
			hdev->nw->hdev = NULL;
			hdev->nw = NULL;
		}

		mutex_lock(&nw_mutex);
		g_nw_from_wlan = NULL;
		mutex_unlock(&nw_mutex);

		DBG_HIF("HAL cleanup completed (last frontend unloaded)");
	} else {
		DBG_HIF("Cleanup skipped (%d frontend(s) still active)", count);
	}
}
EXPORT_SYMBOL(nrc_hal_core_nw_cleanup);
