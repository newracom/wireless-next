/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Post-HAL Initialization
 * Handles WLAN initialization after HAL is ready
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

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"

/* Local module headers */
#include "nrc-wlan-post-init.h"
#include "nrc-mac80211.h"
#include "nrc-netlink.h"
#include "nrc-stats.h"
#include "nrc-twt-sched.h"
#include "nrc-wlan-hal-init.h"
#include "nrc-debug.h"
#include "nrc-wlan-params.h"
#include "nrc-hal-core-interface.h"
#include "nrc-debug.h"
#include "nrc-ps.h"

/**
 * nrc_wlan_post_hal_init - Initialize WLAN components after HAL is ready
 * @restart: whether this is a restart operation
 *
 * This function performs the WLAN initialization that was moved from HAL's nrc_nw_start:
 * - Netlink initialization
 * - Hardware registration with IEEE80211 subsystem
 * - Debug filesystem initialization
 * - Queue management
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_wlan_post_hal_init(bool restart)
{
	struct nrc *nw;
	struct nrc_hif_device *hdev;
	int ret;

	/* Get network device from WLAN layer (early HAL init) */
	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No network device available from WLAN early init");
		return -ENODEV;
	}

	// INFO("WLAN: Starting post-HAL initialization (restart=%d)", restart);

	/* Get HIF device allocated during HAL probe */
	hdev = nrc_hal_core_get_hdev();
	if (!hdev) {
		ERR_WLAN("HIF device not available from HAL");
		return -ENODEV;
	}

	/* Check HAL availability */
	if (!nrc_hal_core_get_ops()) {
		ERR_WLAN("WLAN: HAL operations not available");
		return -ENODEV;
	}

	/* Start network device through HAL ops */
	ret = nrc_hal_ops_nw_start(restart);
	if (ret) {
		ERR_WLAN("WLAN: Failed to start network device: %d", ret);
		/* GPIO cleanup is now handled in SPI stop function */
		return ret;
	}

	/* Initialize TX tasklet for IEEE80211 queue processing */
#ifdef CONFIG_USE_TXQ
#ifdef CONFIG_NEW_TASKLET_API
	tasklet_setup(&nw->tx_tasklet, nrc_tx_tasklet);
#else
	tasklet_init(&nw->tx_tasklet, nrc_tx_tasklet, (unsigned long)nw);
#endif
#endif

	/* Initialize CQM after IEEE80211 hardware registration */
	if (!nw->params->disable_cqm) {
		DBG_MAC("CQM is enabled");
		nw->beacon_timeout = 0;
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
		setup_timer(&nw->bcn_mon_timer, nrc_bcn_mon_timer,
			    (unsigned long)nw);
#else
		timer_setup(&nw->bcn_mon_timer, nrc_bcn_mon_timer, 0);
#endif
	} else {
		DBG_MAC("CQM is disabled");
	}

	/* Initialize statistics subsystem */
	ret = nrc_stats_init();
	if (ret) {
		ERR_WLAN("WLAN: Failed to initialize statistics");
		return ret;
	}

	/* Initialize netlink interface */
	ret = nrc_netlink_init(nw);
	if (ret) {
		ERR_WLAN("WLAN: Failed to initialize netlink");
		nrc_stats_deinit();
		return ret;
	}

	if (!restart) {
		/* Set wiphy parent device to HAL platform device */
		struct device *hal_pdev = nrc_hal_core_get_dev();
		if (hal_pdev) {
			nw->hw->wiphy->dev.parent = hal_pdev;
		} else {
			dev_warn(nw->dev,
				 "WLAN: HAL platform device not available\n");
		}

		/* Register hardware with IEEE80211 subsystem */
		ret = nrc_register_hw(nw, hdev);
		if (ret) {
			ERR_WLAN("WLAN: Failed to register hardware");
			goto err_netlink_cleanup;
		}

		/* Initialize debug filesystem (must be called after ieee80211_register_hw) */
		nrc_init_debugfs(nw);
	} else {
		/* Handle restart case */
		nrc_mac_restart(nw);
		ieee80211_restart_hw(nw->hw);
	}

	/* Wake up queues */
	ieee80211_wake_queues(nw->hw);

	/* Idle mode will be started in nrc_mac_start() when interface is brought up */

	// dev_debug(nw->dev, "WLAN: Post-HAL initialization complete\n");
	return 0;

err_netlink_cleanup:
	nrc_netlink_exit();
	return ret;
}

/**
 * nrc_wlan_post_hal_cleanup - Cleanup WLAN components
 * @restart: whether this is a restart operation
 */
void nrc_wlan_post_hal_cleanup(bool restart)
{
	struct nrc *nw;
	struct nrc_hif_device *hdev;

	/* Get network device from WLAN layer */
	nw = nrc_wlan_get_nw();
	if (!nw) {
		ERR_WLAN("No network device available for cleanup");
		return;
	}

	/* Wake up device if sleeping to ensure clean unload */
	hdev = nw->hdev;
	if (hdev && NRC_PS_IS_ASLEEP(hdev)) {
		INFO("WLAN: Waking device from sleep before unload");
		/* Use high-level PS function for proper state management
		 * Extended timeout (5000ms) for cleanup to handle:
		 * - Target wakeup and REQUEST_FW_DOWNLOAD IRQ processing
		 * - FW ready verification (up to 3000ms)
		 * - Any additional cleanup operations */
		nrc_ps_set_mode(nw, NRC_PS_NONE, 5000, NULL,
				NRC_PS_REASON_HAL_SHUTDOWN);
	}

	/* Disable idle mode before unregister to prevent re-entering sleep
	 * during mac_stop callback from ieee80211_unregister_hw */
	if (nrc_idle_mode_get_state(nw)) {
		INFO("WLAN: Disabling idle mode for clean unload");
		nrc_idle_mode_set_state(nw, 0);
	}

	/* Stop queues */
	if (nw->hw) {
		ieee80211_stop_queues(nw->hw);
	}

	nrc_exit_debugfs(nw);

	nrc_netlink_exit();

	nrc_stats_deinit();

	nrc_twt_sched_deinit(nw);

#ifdef CONFIG_USE_TXQ
	tasklet_kill(&nw->tx_tasklet);
#endif

	if (!restart) {
		nrc_unregister_hw(nw);
	}

	/* Cleanup synchronization primitives after unregistering hardware
	 * to ensure nrc_mac_stop (called by ieee80211_unregister_hw) can
	 * still use the mutex before it's destroyed */
	mutex_destroy(&nw->state_mtx);

	/* Power save GPIO cleanup is now handled in SPI stop function */
}
