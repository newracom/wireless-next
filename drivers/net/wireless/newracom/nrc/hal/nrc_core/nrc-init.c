/*
 * Copyright (c) 2016-2019 Newracom, Inc.
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
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* Assembly headers */
#include <asm/uaccess.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-backend-hif-callback.h"
#include "nrc-backend-hif-interface.h"
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"

/* Local module headers */
#include "nrc-fw.h"
#include "nrc-init.h"
#include "nrc-vendor.h"
#include "wim.h"
#include "nrc-debug.h"
#include "hif.h"
#include "nrc-ps.h"
#if defined(CONFIG_SUPPORT_BD)
#include "nrc-bd.h"
#endif
#include "nrc-tx.h"

/**
 * nrc_init_credit_queue - Initialize credit queue management in struct nrc
 * @hdev: pointer to struct nrc_hif_device
 *
 * Initializes the credit queue fields (front, rear, credit_max) based on chip ID.
 * This function replaces the SPI module's spi_set_default_credit function.
 */
void nrc_init_credit_queue(struct nrc_hif_device *hdev)
{
	int i;
	u16 chip_id;

	if (!hdev) {
		ERR_HIF("Invalid hdev pointer");
		return;
	}

	/* Initialize credit spinlock */
	spin_lock_init(&hdev->credit.lock);

	/* Initialize all arrays to zero */
	for (i = 0; i < CREDIT_QUEUE_MAX; i++) {
		hdev->credit.front[i] = 0;
		hdev->credit.rear[i] = 0;
		hdev->credit.credit_max[i] = 0;
	}

	/* Get chip ID from hdev structure if available, otherwise use default */
	if (hdev && hdev->chip_id != 0) {
		chip_id = hdev->chip_id;
	} else {
		ERR_HIF("%s: Warning - chip ID not available", __func__);
		return;
	}

	/* Set credit limits based on chip ID */
	switch (chip_id) {
	case 0x7292:
	case 0x7394:
		hdev->credit.credit_max[0] = CREDIT_AC0;
		hdev->credit.credit_max[1] = CREDIT_AC1_80;
		hdev->credit.credit_max[2] = CREDIT_AC2;
		hdev->credit.credit_max[3] = CREDIT_AC3;
		hdev->credit.credit_max[5] = CREDIT_AC1;

		hdev->credit.credit_max[6] = CREDIT_AC0;
		hdev->credit.credit_max[7] = CREDIT_AC1_80;
		hdev->credit.credit_max[8] = CREDIT_AC2;
		hdev->credit.credit_max[9] = CREDIT_AC3;
		break;

	case 0x7391:
	case 0x7392:
	case 0x4791:
	case 0x5291:
		hdev->credit.credit_max[0] = 4;
		hdev->credit.credit_max[1] = CREDIT_AC1_20;
		hdev->credit.credit_max[2] = 4;
		hdev->credit.credit_max[3] = 4;

		hdev->credit.credit_max[6] = 4;
		hdev->credit.credit_max[7] = CREDIT_AC1_20;
		hdev->credit.credit_max[8] = 4;
		hdev->credit.credit_max[9] = 4;
		break;

	default:
		ERR_HIF("Unknown chip ID 0x%04x, using default credit values",
			 chip_id);
		break;
	}

	/* Debug output */
	for (i = 0; i < CREDIT_QUEUE_MAX; i++) {
		if (hdev->credit.credit_max[i] > 0) {
			DBG_HIF("%s: credit[%2d] :%3d", __func__, i,
				hdev->credit.credit_max[i]);
		}
	}
}

int country_match(const char *const cc[], const char *const country)
{
	int i;

	if (country == NULL)
		return 0;
	for (i = 0; cc[i]; i++) {
		if (cc[i][0] == country[0] && cc[i][1] == country[1])
			return 1;
	}

	return 0;
}

#define MAX_FW_RETRY_CNT 30
int nrc_nw_start(bool restart)
{
	int ret;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	enum NRC_DRV_STATE current_state;

	if (!hdev) {
		ERR_HIF("Invalid HIF device or ops");
		return -EINVAL;
	}

	current_state = NRC_HIF_DRV_STATE(hdev);

	/* Check if firmware is already loaded by another frontend */
	if (current_state != NRC_DRV_INIT) {
		/* Firmware already downloaded and started by another frontend */
		if (current_state >= NRC_DRV_START) {
			INFO("Firmware already loaded by first frontend (state=%s). Skipping firmware download.",
			     nrc_drv_state_str(current_state));

			/* Note: fw_name and bd_name are already synchronized by frontend's sync_params
			 * (nrc_wlan_sync_params or nrc_mcp_sync_params) before calling this function.
			 * Second frontend will share hdev->params pointer with first frontend,
			 * so all FW-related parameters are automatically synchronized. */

			return 0; /* Success - firmware already loaded */
		} else {
			ERR_HIF("Invalid HIF state for nw_start: %s (%d)",
				 nrc_drv_state_str(current_state),
				 current_state);
			return -EINVAL;
		}
	}

	/* First frontend - perform full initialization */
	INFO("NRC start (first frontend)");

	/* Check if firmware is already loaded (module reload case) */
	if (hdev->fw.loaded) {
		INFO("Firmware already loaded (%s), skipping firmware download",
		     hdev->params->fw_name);

		/* Update state to START to indicate firmware is ready */
		NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_START);

		/* Skip BD check and FW download, but continue to HIF/FW start */
		goto skip_fw_download;
	}

	/* Check if HW is in bootloader mode (ready for FW download) */
	if (hdev->params->fw_name && !nrc_hif_ops_fw_is_boot()) {
		ERR_HIF("Target not in bootloader mode");
		return -EINVAL;
	}

	// 2nd Phase: Read Board file
#if defined(CONFIG_SUPPORT_BD)
	ret = nrc_check_bd(hdev);
	if (ret) {
		ERR_HIF("Failed to nrc_check_bd");
		return -EINVAL;
	}
#endif

	// 3rd Phase: FW download
	ret = nrc_fw_load(hdev);
	if (ret != 0) {
		return ret;
	}

	/* Mark firmware as loaded */
	hdev->fw.loaded = true;
	INFO("First frontend firmware loaded: %s", hdev->params->fw_name);

skip_fw_download:
	// 4th Phase: HAL start(Rx thread and IRQ thread)
	NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_START);
	ret = nrc_hal_start();
	if (ret) {
		ERR_HIF("Failed to start HAL device, err %d", ret);
		goto err_return;
	}

	// 5th Phase: FW Start
	ret = nrc_fw_start(hdev);
	if (ret) {
		ERR_HIF("Failed to nrc_fw_start");
		goto err_return;
	}

	/* HAL initialization complete - frontend will handle netlink/hw registration */
	return 0;

err_return:
	/* Cleanup on error */
	hdev->fw.loaded = false;
	nrc_hal_stop(hdev);
	nrc_hif_ops_reset_device();
	NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_INIT);
	return ret;
}

int nrc_nw_start_fusing(void)
{
	int ret;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (!hdev) {
		ERR_HIF("Invalid HIF device or ops");
		return -EINVAL;
	}

	INFO("NRC start fusing flash");

	if (!NRC_DRV_IS_INIT(hdev)) {
		ERR_HIF("Invalid DRV state (%s)", NRC_DRV_STATE_STR(hdev));
		return -EINVAL;
	}

	if (hdev->params->dl_name && !nrc_hif_ops_fw_is_boot()) {
		ERR_HIF("Target not in bootloader mode");
		return -EINVAL;
	}

	ret = nrc_fw_fusing(hdev);

	return ret;
}

/**
 * nrc_hal_has_active_frontends - Check if other frontends are still active
 *
 * Returns: true if frontend_count > 0, false otherwise
 */
static bool nrc_hal_has_active_frontends(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	int count;

	if (!hdev)
		return false;

	count = atomic_read(&hdev->frontend_count);
	return (count > 0);
}

/**
 * nrc_nw_stop - Stop network operation
 * @restart: If true, ignore frontend check (used by restart)
 *
 * Returns 0 on success.
 */
int nrc_nw_stop(bool restart)
{
	int counter = 0;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	bool has_other_frontends = false;
#ifdef CONFIG_USE_TXQ
	/* Send callback event to WLAN layer instead of direct function call */
	struct nrc_hal_event_data event = {
		.type = NRC_HAL_EVT_CLEANUP_TXQ_ALL,
	};
#endif

	if (!hdev) {
		ERR_HIF("Invalid HIF device or ops");
		return -EINVAL;
	}

	if (NRC_HIF_DRV_STATE(hdev) == NRC_DRV_INIT) {
		return 0;
	}

	/* Check if other frontends are still active (skip if restart) */
	if (!restart) {
		has_other_frontends = nrc_hal_has_active_frontends();

		if (has_other_frontends) {
			INFO("Other frontends still active, keeping HAL/firmware running");
			/* Don't reset device or change state if other frontends are active */
			return 0;
		}
	}

	INFO("Stopping HAL%s", restart ? " (restart)" : "");

	if (!!hdev->nw) {
		while (atomic_read(&hdev->nw->d_deauth.delayed_deauth)) {
			msleep(100);
			if (counter++ > 10) {
				atomic_set(&hdev->nw->d_deauth.delayed_deauth,
					   0);
				break;
			}
		}
	}

	NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_CLOSING);

#ifdef CONFIG_USE_TXQ
	nrc_hal_trigger_event(&event);
#endif

	nrc_hal_stop(hdev);
	nrc_tx_cleanup_queues();

	nrc_hif_ops_reset_device();

	NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_INIT);

	/* Clear fw.loaded flag on restart to force FW re-download
	 * (device reset clears FW from target)
	 * For normal stop (module unload), keep fw.loaded to allow
	 * module reload without re-downloading firmware */
	if (restart) {
		hdev->fw.loaded = false;
		INFO("FW loaded flag cleared for restart");
	}

	return 0;
}

void nrc_nw_restart(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (!hdev) {
		ERR_HIF("Invalid HIF device or ops");
		return;
	}

	queue_work(hdev->restart_workqueue, &hdev->restart_work);
}

int nrc_hal_fw_init(struct nrc_hif_device *hdev)
{
	bool fw_was_loaded;

	/* Check if fw_priv is already allocated to prevent double initialization */
	if (hdev->fw.priv) {
		dev_warn(hdev->dev,
			 "FW already initialized, skipping re-initialization");
		return 0;
	}

	NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_INIT);

	/* Save fw.loaded flag before clearing structure (needed for WLAN module reload) */
	fw_was_loaded = hdev->fw.loaded;

	/* Initialize firmware structure */
	memset(&hdev->fw, 0, sizeof(hdev->fw));

	hdev->fw.priv = nrc_fw_alloc();
	if (!hdev->fw.priv) {
		ERR_HIF("Failed to allocate FW private structure");
		return -ENOMEM;
	}

	/* If firmware was already loaded (WLAN module reload case), restore flag */
	if (fw_was_loaded) {
		hdev->fw.loaded = true;
	}

	/* Initialize firmware state atomics */
	atomic_set(&hdev->fw.state, NRC_FW_NONE);
	atomic_set(&hdev->fw.started, 0);
	atomic_set(&hdev->fw.tx, 0);
	atomic_set(&hdev->fw.rx, 0);

	/* Initialize firmware capabilities */
	hdev->fw.use_ext_lna = false;
	hdev->fw.recovery_wdt = NULL;

	return 0;
}

void nrc_hal_fw_cleanup(struct nrc_hif_device *hdev)
{
	bool fw_was_loaded;

	if (hdev && hdev->fw.priv) {
		/* Release firmware if loaded */
		if (hdev->fw.fw) {
			release_firmware(hdev->fw.fw);
			hdev->fw.fw = NULL;
		}

		/* Cleanup recovery watchdog if allocated */
		if (hdev->fw.recovery_wdt) {
			/* Note: recovery_wdt cleanup will be handled by its owner */
			hdev->fw.recovery_wdt = NULL;
		}

		/* Cleanup firmware private data */
		nrc_fw_cleanup(hdev->fw.priv);

		/* Save fw.loaded flag before clearing structure
		 * This flag must be preserved across WLAN module reload */
		fw_was_loaded = hdev->fw.loaded;

		/* Clear entire firmware structure */
		memset(&hdev->fw, 0, sizeof(hdev->fw));

		/* Restore fw.loaded flag to allow module reload without re-downloading firmware */
		hdev->fw.loaded = fw_was_loaded;
	}

	/* Note: fw.loaded flag is preserved above to allow module reload
	 * without re-downloading firmware. It will be cleaned up when HAL module
	 * is unloaded or device is removed. */
}

/**
 * nrc_params_alloc - Allocate and initialize nrc_params structure
 *
 * Returns: Pointer to allocated nrc_params structure, or NULL on failure
 */
struct nrc_params *nrc_params_alloc(void)
{
	struct nrc_params *params;

	params = kzalloc(sizeof(struct nrc_params), GFP_KERNEL);
	if (!params) {
		ERR_HIF("Failed to allocate nrc_params structure");
		return NULL;
	}

	/* Initialize default values if needed */
	// params->power_save = 0;
	// params->sw_enc = 0;
	// params->ampdu_mode = 1;
	// params->support_ch_width = 0;

	return params;
}

/**
 * nrc_params_free - Free nrc_params structure
 * @params: Pointer to nrc_params structure to free
 *
 * Frees all dynamically allocated strings within the structure,
 * then frees the structure itself.
 */
void nrc_params_free(struct nrc_params *params)
{
	if (params) {
		/* Free all dynamically allocated string parameters */
		if (params->fw_name) {
			kfree(params->fw_name);
			params->fw_name = NULL;
		}
		if (params->bd_name) {
			kfree(params->bd_name);
			params->bd_name = NULL;
		}
		if (params->macaddr) {
			kfree(params->macaddr);
			params->macaddr = NULL;
		}
		if (params->fw_update_name) {
			kfree(params->fw_update_name);
			params->fw_update_name = NULL;
		}
		if (params->dl_name) {
			kfree(params->dl_name);
			params->dl_name = NULL;
		}
		if (params->bl_name) {
			kfree(params->bl_name);
			params->bl_name = NULL;
		}

		/* Free the structure itself */
		kfree(params);
	}
}

/**
 * nrc_debug_alloc - Allocate and initialize nrc_debug structure
 *
 * Returns: Pointer to allocated nrc_debug structure, or NULL on failure
 */
struct nrc_debug *nrc_debug_alloc(void)
{
	struct nrc_debug *debug;

	debug = kzalloc(sizeof(struct nrc_debug), GFP_KERNEL);
	if (!debug) {
		ERR_HIF("Failed to allocate nrc_debug structure");
		return NULL;
	}

	return debug;
}

/**
 * nrc_debug_free - Free nrc_debug structure
 * @debug: Pointer to nrc_debug structure to free
 */
void nrc_debug_free(struct nrc_debug *debug)
{
	if (debug) {
		kfree(debug);
	}
}
