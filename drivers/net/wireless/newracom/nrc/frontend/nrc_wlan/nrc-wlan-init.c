/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Frontend Module Init
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

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"
#include "nrc-ps-common.h"
#include "nrc-hif.h"

/* Local module headers */
#include "nrc-wlan-post-init.h"
#include "nrc-mac80211.h"
#include "nrc-wlan-callback.h"
#include "nrc-wlan-hal-init.h"

static int nrc_wlan_module_init(void)
{
	int ret;

	// INFO("NRC WLAN Frontend subsystem initializing...");

	/* Early HAL initialization from WLAN layer */
	ret = nrc_wlan_hal_early_init();
	if (ret) {
		ERR_WLAN("Failed to perform early HAL initialization: %d", ret);
		return ret;
	}

	/* Initialize WLAN callback system */
	ret = nrc_wlan_callback_init();
	if (ret) {
		ERR_WLAN("Failed to initialize WLAN callback system: %d", ret);
		nrc_wlan_hal_early_cleanup();
		return ret;
	}

	/* Get network device from WLAN layer and initialize frontend components */
	ret = nrc_wlan_post_hal_init(false);
	if (ret) {
		ERR_WLAN("Failed to initialize frontend components: %d", ret);
		nrc_wlan_callback_cleanup();
		nrc_wlan_hal_early_cleanup();
		return ret;
	}

	INFO("NRC WLAN Frontend subsystem initialized");
	return 0;
}

static void nrc_wlan_module_exit(void)
{
	struct nrc_hif_device *hdev;

	// INFO("NRC WLAN Frontend subsystem exiting...");

	/* Ensure device is awake before cleanup */
	hdev = nrc_hal_core_get_hdev();
	if (hdev && !NRC_PS_IS_AWAKE(hdev)) {
		DBG_PS("WLAN exit: Device not awake, requesting wake");
		nrc_hal_ops_ps_request_wake(2000, NRC_PS_REASON_HAL_SHUTDOWN);
	}

	nrc_wlan_post_hal_cleanup(false);

	nrc_wlan_callback_cleanup();

	nrc_wlan_hal_early_cleanup();

	pr_info("nrc_wlan: NRC WLAN Frontend subsystem cleaned up\n");
}

/**
 * nrc_wlan_init - Initialize NRC WLAN frontend module
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init nrc_wlan_init(void)
{
	// INFO("NRC WLAN Frontend Module loaded");
	return nrc_wlan_module_init();
}

/**
 * nrc_wlan_exit - Cleanup NRC WLAN frontend module
 */
static void __exit nrc_wlan_exit(void)
{
	// INFO("NRC WLAN Frontend Module unloaded");
	nrc_wlan_module_exit();
}

module_init(nrc_wlan_init);
module_exit(nrc_wlan_exit);

MODULE_DESCRIPTION("Newracom NRC WLAN Frontend Driver");
MODULE_AUTHOR("Newracom, Inc.");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");
