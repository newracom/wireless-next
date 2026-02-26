/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN HAL Early Initialization
 * Handles early HAL initialization from WLAN layer
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
#include <linux/platform_device.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"

/* Local module headers */
#include "nrc-mac80211.h"
#include "nrc-wlan-hal-init.h"
#include "nrc-wlan-params.h"
#include "nrc-ps.h"

/* Global variables for network device and HIF device */
static struct nrc *g_nw = NULL;
static struct ieee80211_hw *g_hw = NULL;

/* Forward declarations for helper functions */
static int nrc_wlan_workqueue_init(struct nrc *nw);
static void nrc_wlan_workqueue_deinit(struct nrc *nw);

/**
 * nrc_wlan_hal_early_init - Get network device from HAL and perform HIF probe
 *
 * This function gets the network device already allocated by HAL
 * and performs HIF probe operations.
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_hal_early_init(void)
{
	int ret;
	static struct nrc_hif_device *hdev;

	// INFO("WLAN: Starting HAL initialization");

	/* Check if HAL core is initialized and ready */
	if (!nrc_hal_core_is_init()) {
		ERR_WLAN(
			"HAL Core is not initialized. Please load nrc_core module first.");
		return -ENODEV;
	}

	/* Get HIF device allocated during HAL probe */
	hdev = nrc_hal_core_get_hdev();
	if (!hdev) {
		ERR_WLAN("HIF device not available from HAL");
		return -ENODEV;
	}

	/* Allocate MAC80211 hardware in WLAN layer */
	g_hw = nrc_mac_alloc_hw(sizeof(struct nrc), NRC_DRIVER_NAME);
	if (!g_hw) {
		ERR_WLAN("Failed to allocate MAC80211 hardware");
		return -ENOMEM;
	}

	g_nw = g_hw->priv;
	g_nw->hw = g_hw;
	g_nw->dev = hdev->dev;
	g_nw->params = hdev->params;
	g_nw->debug = hdev->debug;

	/* Initialize synchronization primitives and basic fields immediately after allocation */
	mutex_init(&g_nw->state_mtx);
	spin_lock_init(&g_nw->vif_lock);
	atomic_set(&g_nw->scan_mode, NRC_SCAN_MODE_IDLE);
	g_nw->twt_sched = NULL;

	/* Initialize WLAN-specific components first - better for parameter dependency */
	ret = nrc_wlan_nw_init(g_nw);
	if (ret) {
		ERR_WLAN("WLAN initialization failed: %d", ret);
		nrc_wlan_nw_deinit(g_nw);
		nrc_mac_free_hw(g_hw);
		g_nw = NULL;
		g_hw = NULL;
		return ret;
	}

	/* Initialize HAL with network device after WLAN preparation */
	ret = nrc_hal_core_nw_init(g_nw, hdev);
	if (ret) {
		ERR_WLAN("HAL initialization failed: %d", ret);
		nrc_hal_core_nw_cleanup(hdev, g_nw);
		nrc_wlan_nw_deinit(g_nw);
		nrc_mac_free_hw(g_hw);
		g_nw = NULL;
		g_hw = NULL;
		return ret;
	}

	return 0;
}

/**
 * nrc_wlan_hal_early_cleanup - Cleanup HAL initialization
 */
void nrc_wlan_hal_early_cleanup(void)
{
	// INFO("WLAN: Starting HAL cleanup");

	/* Cleanup in reverse order: HAL first, then WLAN */
	if (g_nw) {
		/* HAL cleanup with hdev and nw parameters */
		if (g_nw->hdev) {
			nrc_hal_core_nw_cleanup(g_nw->hdev, g_nw);
		}

		/* Cleanup WLAN-specific components */
		nrc_wlan_nw_deinit(g_nw);
		g_nw = NULL;
	}

	/* Free MAC80211 hardware allocated in WLAN layer */
	if (g_hw) {
		nrc_mac_free_hw(g_hw);
		g_hw = NULL;
	}

	// INFO("WLAN: HAL cleanup completed");
}

/**
 * nrc_wlan_get_nw - Get network device (shorter alias)
 *
 * Returns: network device pointer or NULL
 */
struct nrc *nrc_wlan_get_nw(void)
{
	return g_nw;
}

/**
 * nrc_wlan_get_hw - Get MAC80211 hardware structure
 *
 * Returns: IEEE80211 hardware pointer or NULL
 */
struct ieee80211_hw *nrc_wlan_get_hw(void)
{
	return g_hw;
}

/**
 * nrc_wlan_is_initialized - Check if WLAN module is initialized
 *
 * Returns: true if initialized, false otherwise
 */
bool nrc_wlan_is_initialized(void)
{
	return (g_nw != NULL && g_hw != NULL);
}

/**
 * nrc_wlan_get_device - Get device pointer from network device
 *
 * Returns: device pointer or NULL
 */
struct device *nrc_wlan_get_device(void)
{
	if (!g_nw)
		return NULL;

	return g_nw->dev;
}

/**
 * nrc_wlan_nw_init - Initialize WLAN-specific network device components
 * @nw: Network device
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_nw_init(struct nrc *nw)
{
	int ret;

	if (!nw) {
		ERR_WLAN("Invalid network device for WLAN initialization");
		return -EINVAL;
	}

	// DBG_STATE("WLAN: Starting WLAN-specific initialization");

	/* Synchronize WLAN module parameters with nw structure */
	nrc_wlan_sync_params(nw);

	/* Initialize vendor SKB pointers */
	nw->vendor_skb_beacon = NULL;
	nw->vendor_skb_probe_req = NULL;
	nw->vendor_skb_probe_rsp = NULL;
	nw->vendor_skb_assoc_req = NULL;

	/* Initialize WLAN-specific workqueues if needed */
	ret = nrc_wlan_workqueue_init(nw);
	if (ret) {
		ERR_WLAN("Failed to initialize WLAN workqueues: %d", ret);
		return ret;
	}

	return 0;
}

/**
 * nrc_wlan_nw_deinit - Cleanup WLAN-specific network device components
 * @nw: Network device
 */
void nrc_wlan_nw_deinit(struct nrc *nw)
{
	if (!nw) {
		ERR_WLAN("Invalid network device for WLAN cleanup");
		return;
	}

	// DBG_STATE("WLAN: Starting WLAN-specific cleanup");

	/* Free vendor SKBs */
	if (nw->vendor_skb_beacon) {
		NRC_SKB_TRACK_FREE(nw->hdev, nw->vendor_skb_beacon,
				   HIF_TYPE_FRAME, false, false);
		nw->vendor_skb_beacon = NULL;
	}

	if (nw->vendor_skb_probe_req) {
		NRC_SKB_TRACK_FREE(nw->hdev, nw->vendor_skb_probe_req,
				   HIF_TYPE_FRAME, false, false);
		nw->vendor_skb_probe_req = NULL;
	}

	if (nw->vendor_skb_probe_rsp) {
		NRC_SKB_TRACK_FREE(nw->hdev, nw->vendor_skb_probe_rsp,
				   HIF_TYPE_FRAME, false, false);
		nw->vendor_skb_probe_rsp = NULL;
	}

	if (nw->vendor_skb_assoc_req) {
		NRC_SKB_TRACK_FREE(nw->hdev, nw->vendor_skb_assoc_req,
				   HIF_TYPE_FRAME, false, false);
		nw->vendor_skb_assoc_req = NULL;
	}

	/* Cleanup WLAN-specific workqueues */
	nrc_wlan_workqueue_deinit(nw);

	// DBG_STATE("WLAN: WLAN-specific cleanup completed");
}

/* Helper functions for WLAN-specific initialization */

/**
 * nrc_wlan_workqueue_init - Initialize WLAN-specific workqueues
 * @nw: Network device
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_wlan_workqueue_init(struct nrc *nw)
{
	/* Initialize WLAN-specific delayed work items */
	INIT_DELAYED_WORK(&nw->roc_finish, nrc_mac_roc_finish);
	INIT_DELAYED_WORK(&nw->rm_vendor_ie_wowlan_pattern,
			  nrc_rm_vendor_ie_wowlan_pattern);
	INIT_DELAYED_WORK(&nw->idle_work, nrc_ps_set_idle_mode_work_handler);
	INIT_DELAYED_WORK(&nw->beacon_loss_work,
			  beacon_loss_check_work_handler);

	return 0;
}

/**
 * nrc_wlan_workqueue_deinit - Cleanup WLAN-specific workqueues
 * @nw: Network device
 */
static void nrc_wlan_workqueue_deinit(struct nrc *nw)
{
	/* Cancel and flush WLAN-specific delayed work items */
	cancel_delayed_work_sync(&nw->roc_finish);
	cancel_delayed_work_sync(&nw->rm_vendor_ie_wowlan_pattern);
	cancel_delayed_work_sync(&nw->idle_work);
	cancel_delayed_work_sync(&nw->beacon_loss_work);
}
