/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC HAL Callback Implementation
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
#include <linux/spinlock.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-backend-hif-callback.h"
#include "nrc-hal-core-callback.h"
#include "nrc-hal-core-interface.h"
#include "nrc-backend-hif-interface.h"
#include "nrc-wim-types.h"

/* Local module headers */
#include "nrc-dump.h"
#include "wim.h"
#include "nrc-debug.h"
#include "nrc-ps.h"
#include "nrc-fw.h"
#include "hif.h"

/* Multi-Frontend Callback Registration Entry */
struct nrc_hal_callback_entry {
	enum nrc_frontend_type type;
	nrc_hal_callback_fn callback;
	bool registered;
};

/* Global callback management */
static struct {
	struct nrc_hal_callback_entry frontends[NRC_FRONTEND_MAX];
	spinlock_t lock;
	bool spi_registered;
} hal_callback_mgr = {
	.frontends =
		{
			[NRC_FRONTEND_WLAN] = {.type = NRC_FRONTEND_WLAN,
					       .callback = NULL,
					       .registered = false},
			[NRC_FRONTEND_MCP] = {.type = NRC_FRONTEND_MCP,
					      .callback = NULL,
					      .registered = false},
		},
	.lock = __SPIN_LOCK_UNLOCKED(hal_callback_mgr.lock),
	.spi_registered = false,
};

/* Forward declarations */
static int
nrc_hal_spi_callback_handler(struct nrc_spi_event_data *backend_event);
static bool nrc_hal_handle_rx_data(struct nrc_spi_event_data *backend_event,
				   struct nrc_hal_event_data *hal_event);
static bool nrc_hal_handle_wim_data(struct nrc_spi_event_data *backend_event,
				    struct nrc_hal_event_data *hal_event);
static bool nrc_hal_process_wim_request(struct sk_buff *skb, struct hif *hif,
					struct wim *wim,
					struct nrc_hal_event_data *hal_event);
static bool nrc_hal_process_wim_event(struct nrc_hif_device *hdev,
				      struct sk_buff *skb, struct hif *hif,
				      struct wim *wim,
				      struct nrc_hal_event_data *hal_event);
static bool nrc_hal_handle_wdt_expired(struct nrc_spi_event_data *backend_event,
				       struct nrc_hal_event_data *hal_event);
static bool
nrc_hal_handle_fw_ready_from_wdt(struct nrc_spi_event_data *backend,
				 struct nrc_hal_event_data *hal_event);
static bool
nrc_hal_handle_request_fw_download(struct nrc_spi_event_data *backend_event,
				   struct nrc_hal_event_data *hal_event);
static bool
nrc_hal_handle_fw_ready_from_ps(struct nrc_spi_event_data *backend_event,
				struct nrc_hal_event_data *hal_event);
static bool
nrc_hal_handle_failed_to_enter_ps(struct nrc_spi_event_data *backend_event,
				  struct nrc_hal_event_data *hal_event);
static bool nrc_hal_handle_loopback(struct sk_buff *skb, struct hif *hif);

/**
 * nrc_hal_forward_simple_event - Helper for simple event forwarding
 * @backend_event: Backend event data
 * @hal_event: HAL event to fill
 * @hal_type: HAL event type to set
 * @debug_name: Debug name for logging (can be NULL)
 *
 * Returns: true (always forward to frontend)
 */
static inline bool
nrc_hal_forward_simple_event(struct nrc_spi_event_data *backend_event,
			     struct nrc_hal_event_data *hal_event,
			     enum nrc_hal_event_type hal_type,
			     const char *debug_name)
{
	if (debug_name)
		DBG_STATE("Processing %s", debug_name);
	hal_event->type = hal_type;
	hal_event->data = backend_event->data;
	hal_event->data_len = backend_event->data_len;
	return true;
}

/**
 * nrc_hal_register_callback - Register frontend callback function
 * @type: Frontend type (WLAN, MCP)
 * @callback: Callback function to register
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_hal_register_callback(enum nrc_frontend_type type,
			      nrc_hal_callback_fn callback)
{
	unsigned long flags;

	if (!callback) {
		ERR_HAL("Invalid callback function");
		return -EINVAL;
	}

	if (type >= NRC_FRONTEND_MAX) {
		ERR_HAL("Invalid frontend type %d", type);
		return -EINVAL;
	}

	spin_lock_irqsave(&hal_callback_mgr.lock, flags);

	if (hal_callback_mgr.frontends[type].registered) {
		spin_unlock_irqrestore(&hal_callback_mgr.lock, flags);
		ERR_HAL("HAL callback for frontend type %d already registered",
			type);
		return -EBUSY;
	}

	hal_callback_mgr.frontends[type].callback = callback;
	hal_callback_mgr.frontends[type].registered = true;

	spin_unlock_irqrestore(&hal_callback_mgr.lock, flags);

	DBG_HIF("Registered HAL callback for frontend type %d", type);
	return 0;
}
EXPORT_SYMBOL(nrc_hal_register_callback);

/**
 * nrc_hal_unregister_callback - Unregister frontend callback function
 * @type: Frontend type (WLAN, MCP)
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_hal_unregister_callback(enum nrc_frontend_type type)
{
	unsigned long flags;

	if (type >= NRC_FRONTEND_MAX) {
		ERR_HAL("Invalid frontend type %d", type);
		return -EINVAL;
	}

	spin_lock_irqsave(&hal_callback_mgr.lock, flags);

	if (!hal_callback_mgr.frontends[type].registered) {
		spin_unlock_irqrestore(&hal_callback_mgr.lock, flags);
		ERR_HAL("No HAL callback registered for frontend type %d",
			type);
		return -ENOENT;
	}

	hal_callback_mgr.frontends[type].callback = NULL;
	hal_callback_mgr.frontends[type].registered = false;

	spin_unlock_irqrestore(&hal_callback_mgr.lock, flags);

	DBG_HIF("Unregistered HAL callback for frontend type %d", type);
	return 0;
}
EXPORT_SYMBOL(nrc_hal_unregister_callback);

/**
 * nrc_hal_trigger_event - Trigger callback for specific frontend type
 * @event: Event data to pass to callback (must include frontend_type)
 *
 * Calls the callback for the specific frontend type indicated in the event.
 * Returns: 0 on success, negative error code on failure
 */
int nrc_hal_trigger_event(struct nrc_hal_event_data *event)
{
	unsigned long flags;
	nrc_hal_callback_fn callback = NULL;
	bool is_registered = false;
	int ret = 0;

	if (!event) {
		ERR_HAL("Invalid HAL event data");
		return -EINVAL;
	}

	if (event->frontend_type >= NRC_FRONTEND_MAX) {
		ERR_HAL("Invalid frontend type %d in event",
			event->frontend_type);
		return -EINVAL;
	}

	/* Copy callback under lock to avoid holding lock during execution */
	spin_lock_irqsave(&hal_callback_mgr.lock, flags);

	if (hal_callback_mgr.frontends[event->frontend_type].registered &&
	    hal_callback_mgr.frontends[event->frontend_type].callback) {
		callback = hal_callback_mgr.frontends[event->frontend_type]
				   .callback;
		is_registered = true;
	}

	spin_unlock_irqrestore(&hal_callback_mgr.lock, flags);

	/* Execute callback outside of lock */
	if (is_registered && callback) {
		// DBG_HIF("Triggering HAL event type %d to frontend %d", event->type, event->frontend_type);
		ret = callback(event);
		if (ret < 0) {
			ERR_HAL("HAL callback for frontend %d returned error: %d",
				event->frontend_type, ret);
		}
	} else {
		// DBG_HIF("No HAL callback registered for frontend type %d", event->frontend_type);
		ret = -ENOENT;
	}

	return ret;
}

/**
 * nrc_hal_spi_callback_handler - Handle SPI events and forward to WLAN
 * @spi_event: SPI event data
 *
 * Returns: 0 on success, negative error code on failure
 */
static int
nrc_hal_spi_callback_handler(struct nrc_spi_event_data *backend_event)
{
	struct nrc_hal_event_data hal_event = {.frontend_type =
						       NRC_FRONTEND_WLAN,
					       .type = 0,
					       .data = NULL,
					       .data_len = 0};
	bool should_forward_to_frontend = false;
	int ret = 0;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!backend_event) {
		ERR_HAL("Invalid SPI event data");
		return -EINVAL;
	}

	if (!hdev) {
		ERR_HAL("No hdev");
		return -EINVAL;
	}

	//DBG_MON("HAL received SPI event type %d", backend_event->type);

	switch (backend_event->type) {
	case NRC_BACKEND_EVT_IRQ:
		hal_event.type = NRC_HAL_EVT_SPI_IRQ;
		hal_event.data = backend_event->data;
		hal_event.data_len = backend_event->data_len;
		should_forward_to_frontend = true;
		break;
	case NRC_BACKEND_EVT_RX_READY:
		should_forward_to_frontend =
			nrc_hal_handle_rx_data(backend_event, &hal_event);
		break;
	case NRC_BACKEND_EVT_TX_COMPLETE:
		hal_event.type = NRC_HAL_EVT_TX_COMPLETE;
		should_forward_to_frontend = true;
		break;
	case NRC_BACKEND_EVT_ERROR:
		hal_event.type = NRC_HAL_EVT_ERROR;
		should_forward_to_frontend = true;
		break;
	case NRC_BACKEND_EVT_RESET_TX:
		if (hdev) {
			nrc_wim_reset_hif_tx(hdev);
		} else {
			ERR_HAL("Invalid hdev pointer in RESET_TX event");
		}
		should_forward_to_frontend = false;
		break;
	case NRC_BACKEND_EVT_RESET_RX:
		if (hdev) {
			nrc_wim_reset_hif_rx(hdev);
		} else {
			ERR_HAL("Invalid hdev pointer in RESET_RX event");
		}
		should_forward_to_frontend = false;
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_WDT_EXPIRED:
		should_forward_to_frontend =
			nrc_hal_handle_wdt_expired(backend_event, &hal_event);
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_FW_READY_FROM_WDT:
		should_forward_to_frontend = nrc_hal_handle_fw_ready_from_wdt(
			backend_event, &hal_event);
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_W_DISABLE_ASSERTED:
		should_forward_to_frontend = nrc_hal_forward_simple_event(
			backend_event, &hal_event,
			NRC_HAL_EVT_TARGET_NOTI_W_DISABLE_ASSERTED,
			"TARGET_NOTI_W_DISABLE_ASSERTED");
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_REQUEST_FW_DOWNLOAD:
		should_forward_to_frontend = nrc_hal_handle_request_fw_download(
			backend_event, &hal_event);
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_FW_READY_FROM_PS:
		should_forward_to_frontend = nrc_hal_handle_fw_ready_from_ps(
			backend_event, &hal_event);
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_FAILED_TO_ENTER_PS:
		should_forward_to_frontend = nrc_hal_handle_failed_to_enter_ps(
			backend_event, &hal_event);
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_TWT_SERVICE:
		should_forward_to_frontend = nrc_hal_forward_simple_event(
			backend_event, &hal_event,
			NRC_HAL_EVT_TARGET_NOTI_TWT_SERVICE,
			"TARGET_NOTI_TWT_SERVICE");
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_TWT_QUIET:
		should_forward_to_frontend = nrc_hal_forward_simple_event(
			backend_event, &hal_event,
			NRC_HAL_EVT_TARGET_NOTI_TWT_QUIET,
			"TARGET_NOTI_TWT_QUIET");
		break;

	case NRC_BACKEND_EVT_TARGET_NOTI_BEACON_UPDATED:
	case NRC_BACKEND_EVT_TARGET_NOTI_FW_ENTER_TO_PS:
	case NRC_BACKEND_EVT_TARGET_NOTI_PS_READY:
		/* These events are handled in Backend layer only */
		should_forward_to_frontend = false;
		break;

	default:
		ERR_HAL("Unknown Backend event type %d", backend_event->type);
		return -EINVAL;
	}

	if (should_forward_to_frontend) {
		ret = nrc_hal_trigger_event(&hal_event);
	}

	return ret;
}

/**
 * nrc_hal_callback_init - Initialize HAL callback system
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_hal_callback_init(void)
{
	int ret;

	if (hal_callback_mgr.spi_registered) {
		DBG_HIF("HAL callback already registered, cleaning up first");
		nrc_hal_callback_cleanup();
	}

	ret = nrc_spi_register_callback(nrc_hal_spi_callback_handler);
	if (ret) {
		DBG_HIF("Failed to register SPI callback: %d", ret);
		return ret;
	}

	hal_callback_mgr.spi_registered = true;

	return 0;
}

/**
 * nrc_hal_callback_cleanup - Cleanup HAL callback system
 */
void nrc_hal_callback_cleanup(void)
{
	unsigned long flags;
	int i;

	if (hal_callback_mgr.spi_registered) {
		nrc_spi_unregister_callback();
		hal_callback_mgr.spi_registered = false;
	}

	spin_lock_irqsave(&hal_callback_mgr.lock, flags);
	for (i = 0; i < NRC_FRONTEND_MAX; i++) {
		hal_callback_mgr.frontends[i].callback = NULL;
		hal_callback_mgr.frontends[i].registered = false;
	}
	spin_unlock_irqrestore(&hal_callback_mgr.lock, flags);
}

/**
 * nrc_hal_handle_wdt_expired - Handle watchdog timer expired event
 * @backend_event: Backend event data
 * @hal_event: HAL event to be filled for forwarding
 *
 * Original driver behavior:
 * 1. If drv_state == PS: recover HSPI and disconnect
 * 2. Set drv_state = REBOOT
 * 3. Reset slot/credit (spi_config_fw equivalent)
 * 4. HIF cleanup, TX queue clean
 * 5. Wait for TARGET_NOTI_FW_READY_FROM_WDT
 *
 * Returns: true if event should be forwarded to frontend, false otherwise
 */
static bool nrc_hal_handle_wdt_expired(struct nrc_spi_event_data *backend_event,
				       struct nrc_hal_event_data *hal_event)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev) {
		ERR_FW("HAL: No hdev available for WDT_EXPIRED");
		return false;
	}

	WARN_HAL(">>> IRQ 0x7D: WDT EXPIRED >>>");

	/*
	 * If in PS state, need to recover HSPI first.
	 * Original driver calls nrc_hif_resume() and ieee80211_connection_loss()
	 * but we handle this through frontend callback.
	 */
	if (NRC_DRV_IS_ASLEEP(hdev)) {
		WARN_HAL("WDT during PS - will notify frontend for recovery");
	}

	/* Set driver state to REBOOT to indicate WDT recovery in progress */
	NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_REBOOT);

	/* Reset slot/credit - equivalent to spi_config_fw() in original driver */
	nrc_hif_reset_slot_credit();

	/* Clean up all pending WIM responses from before WDT reset */
	nrc_wim_cleanup_pending_responses(hdev);

	/*
	 * Forward to frontend for connection_loss handling if needed.
	 * Frontend will call ieee80211_connection_loss() for STA mode.
	 */
	hal_event->type = NRC_HAL_EVT_TARGET_NOTI_WDT_EXPIRED;
	hal_event->data = backend_event->data;
	hal_event->data_len = backend_event->data_len;
	return true;
}

/**
 * nrc_hal_handle_fw_ready_from_wdt - Handle firmware ready from WDT reset event
 * @backend_event: Backend event data
 * @hal_event: HAL event to be filled for forwarding
 *
 * Original driver behavior:
 * 1. Set PS state to WAKE
 * 2. HIF resume
 * 3. nrc_mac_restart -> ieee80211_restart_hw
 * 4. Set drv_state = RUNNING
 *
 * Returns: true if event should be forwarded to frontend, false otherwise
 */
static bool
nrc_hal_handle_fw_ready_from_wdt(struct nrc_spi_event_data *backend_event,
				 struct nrc_hal_event_data *hal_event)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev) {
		ERR_FW("HAL: No hdev available for FW_READY_FROM_WDT");
		return false;
	}

	INFO(">>> IRQ 0x9D: FW READY FROM WDT >>>");

	/* Update PS state to WAKE - FW is now running */
	nrc_ps_handle_fw_ready();

	/* Reset slot/credit again to ensure clean state */
	nrc_hif_reset_slot_credit();

	/* Set driver state to RUNNING - WDT recovery complete */
	NRC_HIF_SET_DRV_STATE(hdev, NRC_DRV_RUNNING);

	/*
	 * Notify frontends that FW is ready after WDT.
	 * Frontend will call ieee80211_restart_hw() to restart mac80211.
	 */
	hal_event->type = NRC_HAL_EVT_TARGET_NOTI_FW_READY_FROM_WDT;
	hal_event->data = backend_event->data;
	hal_event->data_len = backend_event->data_len;
	return true;
}

/**
 * nrc_hal_handle_request_fw_download - Handle firmware download request event
 * @backend_event: Backend event data
 * @hal_event: HAL event to be filled for forwarding
 *
 * Returns: true if event should be forwarded to frontend, false otherwise
 */
static bool
nrc_hal_handle_request_fw_download(struct nrc_spi_event_data *backend_event,
				   struct nrc_hal_event_data *hal_event)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct nrc_ps_event_data wake_event;

	if (!hdev) {
		ERR_HAL("Invalid HIF device for FW download request");
		return false;
	}

	/* Check if module is unloading */
	if (NRC_DRV_IS_CLOSING(hdev)) {
		DBG_PS("Module unloading, skip FW download");
		return false;
	}

	/* Check if FW structure is valid - critical for cleanup race condition
	 * If fw.priv is NULL, it means HAL cleanup has already freed FW resources */
	if (!hdev->fw.priv) {
		DBG_PS("FW structure not initialized, skip FW download (cleanup in progress?)");
		return false;
	}

	/* Skip if FW is already loading or active (duplicate event) */
	if (atomic_read(&hdev->fw.state) == NRC_FW_LOADING) {
		DBG_PS("FW download already in progress, ignoring duplicate request");
		return false;
	}

	/* SPI module already logged 0xDC IRQ reception */

	/* Check if this is passive wake (SLEEP state) - just transition to WAKING */
	if (NRC_PS_IS_ASLEEP(hdev)) {
		/* Transition to WAKING state - FW_READY interrupt will complete the wake */
		wake_event.event = NRC_PS_EVT_WAKE_REQ;
		wake_event.mode = NRC_PS_NONE;
		wake_event.reason = NRC_PS_REASON_TARGET_FW_READY;
		nrc_ps_handle_event(hdev, &wake_event);
	}

	/* nrc_fw_reload handles FW loading for PS wake scenarios */
	if (nrc_fw_reload(hdev) != 0) {
		ERR_PS("Failed to load firmware from host");
	}

	/* This event is handled in HAL, no need to forward */
	return false;
}

/**
 * nrc_hal_handle_fw_ready_from_ps - Handle firmware ready from power save event
 * @backend_event: Backend event data
 * @hal_event: HAL event to be filled for forwarding
 *
 * Returns: true if event should be forwarded to frontend, false otherwise
 */
static bool
nrc_hal_handle_fw_ready_from_ps(struct nrc_spi_event_data *backend_event,
				struct nrc_hal_event_data *hal_event)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();

	if (!hdev) {
		ERR_PS("HAL: No hdev available for FW_READY_FROM_PS");
		return false;
	}

	/* SPI module already logged 0xEC IRQ reception */

	/* HAL handles HIF-level operations */
	if (hdev->params->fw_name == NULL) {
		/* It's for CSPI RAM Mode (no FW download) */
		nrc_hif_reset_slot_credit();
	}

	/* Signal wake completion - this updates PS state machine via event */
	nrc_ps_handle_fw_ready();

	/* Notify frontends that wakeup is complete */
	hal_event->type = NRC_HAL_EVT_WAKE_DONE;
	hal_event->data = NULL;
	hal_event->data_len = 0;
	return true;
}

/**
 * nrc_hal_handle_failed_to_enter_ps - Handle failed to enter power save event
 * @backend_event: Backend event data
 * @hal_event: HAL event to be filled for forwarding
 *
 * HAL processes PS recovery operations (resume HIF, update state machine),
 * then notifies frontends for WLAN-specific actions (beacon monitoring, etc.)
 *
 * Returns: true if event should be forwarded to frontend, false otherwise
 */
static bool
nrc_hal_handle_failed_to_enter_ps(struct nrc_spi_event_data *backend_event,
				  struct nrc_hal_event_data *hal_event)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct nrc_ps_event_data event_data = {
		.event = NRC_PS_EVT_WAKE_REQ,
		.mode = NRC_PS_NONE,
		.timeout_ms = 0,
		.reason = NRC_PS_REASON_TARGET_FAILED_ENTER_PS,
	};

	if (!hdev) {
		ERR_PS("HAL: No hdev for FAILED_TO_ENTER_PS");
		return false;
	}

	DBG_PS("HAL: FAILED_TO_ENTER_PS - Processing recovery");

	/* HAL handles HIF resume */
	nrc_hif_ops_rx_thread_resume();

	/* Update PS state machine via event */
	nrc_ps_handle_event(hdev, &event_data);

	DBG_PS("HAL: PS recovery complete, notifying frontends");

	/* Notify frontends for WLAN-specific actions */
	hal_event->type = NRC_HAL_EVT_PS_ENTER_FAILED;
	hal_event->data = NULL;
	hal_event->data_len = 0;
	return true;
}

/**
 * nrc_hal_handle_rx_data - Handle RX data with intelligent routing based on HIF packet type
 * @backend_event: Backend event data containing the received packet
 * @hal_event: HAL event to be filled for forwarding to WLAN
 *
 * Analyzes incoming RX data packet types and determines intelligent routing:
 * - HIF_TYPE_FRAME: Always forward to WLAN (802.11 frames)
 * - HIF_TYPE_WIM: Forward to WLAN for WIM processing
 * - HIF_TYPE_LOG: Forward to WLAN for netlink interface
 * - HIF_TYPE_DUMP: Process in HAL for memory dump storage
 * - HIF_TYPE_LOOPBACK: Process in HAL for debug testing (DEBUG builds only)
 * - Other types: Forward to WLAN for appropriate handling
 *
 * Returns: true if packet should be forwarded to WLAN, false if handled in HAL
 */
static bool nrc_hal_handle_rx_data(struct nrc_spi_event_data *backend_event,
				   struct nrc_hal_event_data *hal_event)
{
	struct sk_buff *skb;
	struct hif *hif;
	struct nrc_hif_device *hdev;

	if (!backend_event || !backend_event->data || !hal_event) {
		ERR_HAL("Invalid parameters for RX data handling");
		return false;
	}

	skb = (struct sk_buff *)backend_event->data;
	if (skb->len < sizeof(struct hif)) {
		DBG_HIF("RX packet too small (%u bytes) for HIF header",
			skb->len);
		hal_event->type = NRC_HAL_EVT_RX_READY;
		return true;
	}

	hif = (struct hif *)skb->data;
	hdev = nrc_hal_core_get_hdev();
	// DBG_RX("HAL RX: Analyzing packet type=%u subtype=%u len=%u skb_len=%u",
	// 	hif->type, hif->subtype, hif->len, skb->len);

	WARN_ON(skb->len != hif->len + sizeof(*hif));
	if (skb->len != hif->len + sizeof(*hif)) {
		ERR_HAL("HIF length mismatch: skb->len=%u, hif->len=%u",
			skb->len, hif->len);
		hal_event->type = NRC_HAL_EVT_RX_READY;
		return true;
	}

	/* Set basic HAL event properties */
	hal_event->type = NRC_HAL_EVT_RX_READY;
	hal_event->data = skb;
	hal_event->data_len = skb->len;

	/* Track FRAME SKB queued for frontend processing (RX path) */
	NRC_SKB_TRACK_QUEUE(hdev, skb, hif->type, true);

	/* Route based on HIF packet type - following original hif_receive_skb logic */
	switch (hif->type) {
	case HIF_TYPE_FRAME:
		/* Check if this is MCP-specific data that should only go to MCP frontend */
		if (hif->subtype == HIF_FRAME_SUB_MCP_PROTOCOL ||
		    hif->subtype == HIF_FRAME_SUB_MCP_DATA) {
			u8 expected_checksum =
				HIF_HEADER_CALC_CHECKSUM(hif->type, hif->len);
			if (hif->flags != expected_checksum) {
				ERR_HIF("MCP data checksum mismatch: flags=0x%02x, expected=0x%02x type:%d len:%d",
					hif->flags, expected_checksum,
					hif->type, hif->len);
				NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true,
						   false);
				return false;
			}

			/* Set frontend type to MCP */
			hal_event->frontend_type = NRC_FRONTEND_MCP;
		}

		/* All other 802.11 frames (data, management, control) go to WLAN */
		return true;

	case HIF_TYPE_WIM:
		/* WIM (Wireless Interface Module) packets - process in HAL first */
		return nrc_hal_handle_wim_data(backend_event, hal_event);

	case HIF_TYPE_LOG:
		/* Log packets go to WLAN for netlink interface via nrc_netlink_rx() */
		return true;

	case HIF_TYPE_DUMP:
		/* Pull HIF header before processing dump data */
		skb_pull(skb, sizeof(*hif));
		nrc_dump_store((char *)skb->data, hif->len);
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
		return false; /* Processed in HAL, don't forward to WLAN */

	case HIF_TYPE_LOOPBACK:
		return nrc_hal_handle_loopback(skb, hif);

	case HIF_TYPE_NANOPB:
	case HIF_TYPE_UART_CMD:
	case HIF_TYPE_SSP_READYRX:
	case HIF_TYPE_SSP_PING:
	case HIF_TYPE_SSP_PONG:
	case HIF_TYPE_SSP_SKIP:
		// DBG_HIF("HIF packet type %u - forwarding to WLAN", hif->type);
		return true;

	default:
		ERR_HAL("Unknown HIF packet type %u - freeing packet",
			hif->type);
		print_hex_dump(KERN_DEBUG, "hif type err ", DUMP_PREFIX_NONE,
			       16, 1, skb->data, skb->len > 32 ? 32 : skb->len,
			       false);
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
		return false; /* Packet freed in HAL, don't forward to WLAN */
	}
}

/**
 * nrc_hal_handle_wim_data - Handle WIM packet processing in HAL
 * @spi_event: Original SPI event
 * @hal_event: HAL event to be configured for WLAN forwarding
 *
 * Process WIM packets in HAL first. HAL-only WIM commands are handled completely here.
 * WIM commands with WLAN dependencies are forwarded to WLAN via callback chain.
 *
 * Returns: true to forward to WLAN, false if processed completely in HAL
 */
static bool nrc_hal_handle_wim_data(struct nrc_spi_event_data *backend_event,
				    struct nrc_hal_event_data *hal_event)
{
	struct sk_buff *skb = (struct sk_buff *)backend_event->data;
	struct hif *hif = (struct hif *)skb->data;
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	enum nrc_hal_event_type wim_event_type;
	struct wim *wim;

	if (!hdev) {
		ERR_HAL("%s: No HIF device available, dropping WIM packet",
			__func__);
		/* Error drop: no hdev available, use hif type from header */
		NRC_SKB_TRACK_FREE(NULL, skb, hif->type, true, false);
		return false;
	}

	if (NRC_HIF_DRV_STATE(hdev) < NRC_DRV_START) {
		ERR_HAL("%s: DRV(%s) not START, dropping WIM packet", __func__,
			NRC_DRV_STATE_STR(hdev));
		/* Error drop: driver not ready, use hif type from header */
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
		return false;
	}

	/* Remove HIF header */
	skb_pull(skb, sizeof(*hif));

	wim = (struct wim *)skb->data;

	// DBG_WIM("Processing subtype %u(%s)", hif->subtype,
	// 	nrc_wim_subtype_str(hif->subtype));

	/* Determine WIM subtype and handle appropriately */
	switch (hif->subtype) {
	case HIF_WIM_SUB_REQUEST:
		nrc_hal_process_wim_request(skb, hif, wim, hal_event);
		/* Track WIM request free with parsed cmd/event */
		NRC_SKB_TRACK_WIM_FREE(hdev, skb, wim->cmd, wim->event, true,
				       false);
		return false;

	case HIF_WIM_SUB_RESPONSE:
		/* WIM RESPONSE is always handled in HAL, never forwarded to frontend
		 * If handler stored SKB (return 1), we don't free it here.
		 * If handler didn't store SKB (return 0 or error), we free it here.
		 */
		if (nrc_wim_response_handler(skb) != 1) {
			/* SKB not stored, free it */
			NRC_SKB_TRACK_WIM_FREE(hdev, skb, wim->cmd, wim->event,
					       true, false);
		}
		return false;

	case HIF_WIM_SUB_EVENT:
		if (nrc_hal_process_wim_event(hdev, skb, hif, wim, hal_event)) {
			/* Track WIM event free with parsed cmd/event */
			NRC_SKB_TRACK_WIM_FREE(hdev, skb, wim->cmd, wim->event,
					       true, false);
			return false;
		}
		wim_event_type = NRC_HAL_EVT_WIM_EVENT;
		break;

	default:
		ERR_WIM("%s: Unknown HIF subtype %u, dropping packet", __func__,
			hif->subtype);
		/* Error drop: unknown WIM subtype, use hif type */
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
		return false;
	}

	/* Restore HIF header */
	skb_push(skb, sizeof(*hif));

	/* Configure HAL event for frontend forwarding */
	hal_event->type = wim_event_type;
	hal_event->data = skb;
	hal_event->data_len = skb->len;

	return true; /* Forward to appropriate frontend */
}

/**
 * nrc_hal_process_wim_request - Process WIM request in HAL if possible
 * @skb: SKB with HIF header removed
 * @hif: HIF header info
 * @wim: WIM header info
 * @hal_event: HAL event structure to configure frontend routing
 *
 * Process WIM requests that can be handled entirely in HAL without WLAN dependencies.
 *
 * Architecture Design:
 * - WIM_CMD_FW_RELOAD: Requires WLAN layer processing (firmware management, device restart)
 * - Other WIM commands: Most need WLAN processing for IEEE80211 dependencies
 *
 * Returns: true if processed in HAL (packet consumed), false if needs WLAN processing
 */
static bool nrc_hal_process_wim_request(struct sk_buff *skb, struct hif *hif,
					struct wim *wim,
					struct nrc_hal_event_data *hal_event)
{
	bool consumed = false;

	DBG_WIM("Processing request cmd %d(%s)", wim->cmd,
		nrc_wim_cmd_str(wim->cmd));

	switch (wim->cmd) {
	case WIM_CMD_FW_RELOAD:
		nrc_wim_handle_fw_reload();
		consumed = true;
		break;
	default:
		ERR_HAL("%s: WIM request cmd 0x%x forwarding to frontend",
			__func__, wim->cmd);
		consumed = false; /* Forward unknown requests to frontend */
		break;
	}

	return consumed;
}

/**
 * nrc_hal_process_wim_event - Process WIM event in HAL if possible
 * @hdev: HIF device
 * @skb: SKB with HIF header removed
 * @hif: HIF header info
 * @wim: WIM header info
 * @hal_event: HAL event structure to configure frontend routing
 *
 * Process WIM events based on their type. HAL handles hardware/system level events,
 * while IEEE80211/MAC related events are forwarded to WLAN.
 *
 * Architecture Design:
 * HAL Processing:
 * - WIM_EVENT_CREDIT_REPORT: TX credit management
 * - WIM_EVENT_PS_*: Power save state management
 * - WIM_EVENT_KEEP_ALIVE: System keep-alive
 * - WIM_EVENT_LBT_*: Listen Before Talk events
 * - WIM_EVENT_READY: Firmware ready state
 *
 * WLAN Processing:
 * - WIM_EVENT_SCAN_COMPLETED: IEEE80211 scan management
 * - WIM_EVENT_REQ_DEAUTH: IEEE80211 deauth handling
 * - WIM_EVENT_CSA/CH_SWITCH: IEEE80211 channel switching
 * - WIN_EVENT_CLEAN_TXQ_STA: IEEE80211 TX queue management
 *
 * Returns: true if processed in HAL (packet consumed), false if needs WLAN processing
 */
static bool nrc_hal_process_wim_event(struct nrc_hif_device *hdev,
				      struct sk_buff *skb, struct hif *hif,
				      struct wim *wim,
				      struct nrc_hal_event_data *hal_event)
{
	bool consumed = false;
	u8 expected_checksum = 0;

	/* Use CREDIT mask for CREDIT_REPORT events, WIM mask for others */
	/* Note: For CREDIT_REPORT, logging is done in nrc_wim_update_tx_credit only when values change */
	if (wim->event != WIM_EVENT_CREDIT_REPORT) {
		DBG_WIM("Processing event %d(%s)", wim->event,
			nrc_wim_event_str(wim->event));
	}

	switch (wim->event) {
	case WIM_EVENT_READY:
		nrc_wim_handle_fw_ready(hdev);
		consumed = true;
		break;

	case WIM_EVENT_CREDIT_REPORT:
#ifdef CONFIG_USE_TXQ
		nrc_wim_update_tx_credit(hdev, wim);
#endif
		consumed = true;
		break;

	case WIM_EVENT_PS_READY:
		consumed = true;
		break;

	case WIM_EVENT_PS_WAKEUP:
		consumed = true;
		break;

	case WIM_EVENT_KEEP_ALIVE:
		consumed = true;
		break;

	case WIM_EVENT_LBT_ENABLED:
		consumed = true;
		break;

	case WIM_EVENT_LBT_DISABLED:
		consumed = true;
		break;

	case WIM_EVENT_SCAN_COMPLETED:
		consumed = false;
		break;

	case WIM_EVENT_REQ_DEAUTH:
		consumed = false;
		break;

	case WIM_EVENT_CSA:
		consumed = false;
		break;

	case WIM_EVENT_CH_SWITCH:
		consumed = false;
		break;

	case WIN_EVENT_CLEAN_TXQ_STA:
		consumed = false;
		break;

	/* MCP-specific events - forward to MCP frontend */
	case WIM_EVENT_MCP_NEXT_GROUP:
	case WIM_EVENT_MCP_KEEP_ALIVE:
	case WIM_EVENT_MCP_SCHEDULE_REPORT:
	case WIM_EVENT_MCP_IMMEDIATE_REPORT:
		expected_checksum =
			HIF_HEADER_CALC_CHECKSUM(hif->type, hif->len);
		if (hif->flags != expected_checksum) {
			ERR_HIF("MCP wim checksum mismatch: flags=0x%02x, expected=0x%02x type:%d len:%d event:%d",
				hif->flags, expected_checksum, hif->type,
				hif->len, wim->event);
			return true;
		}
		if (hal_event)
			hal_event->frontend_type = NRC_FRONTEND_MCP;
		consumed = false; /* Forward to MCP frontend */
		break;

	default:
		ERR_HAL("%s: Unknown WIM event 0x%x, forwarding to WLAN",
			__func__, wim->event);
		consumed = false; /* Forward unknown events to WLAN */
		break;
	}

	return consumed;
}

/**
 * nrc_hal_handle_loopback - Handle HIF_TYPE_LOOPBACK packets
 * @skb: SKB containing loopback data
 * @hif: HIF header info
 *
 * Process loopback test packets for performance measurement and debug testing.
 * This function handles timing measurements and test completion detection.
 *
 * Returns: false (processed completely in HAL, don't forward to WLAN)
 */
static bool nrc_hal_handle_loopback(struct sk_buff *skb, struct hif *hif)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct nrc_debug *debug;
	struct hif_lb_hdr *hif_new;
	ktime_t *t;
	u8 *str[] = {"LOOPBACK ", "", "DATA "};
	u8 *p;
	u32 *d;

	if (!hdev || !hdev->debug) {
		DBG_HIF("No HIF device or debug structure for loopback processing");
		/* Error drop: no hdev, use hif type */
		NRC_SKB_TRACK_FREE(NULL, skb, hif->type, true, false);
		return false;
	}

	debug = hdev->debug;

	/* Pull HIF header for loopback processing */
	skb_pull(skb, sizeof(*hif));

	/* Original loopback RX processing from hif_receive_skb */
	t = (ktime_t *)(skb->data + 36);
	hif_new = (struct hif_lb_hdr *)(skb->data - sizeof(struct hif));

	debug->rcv_time_last = ktime_to_us(ktime_get());
	if (hif_new->subtype == LOOPBACK_MODE_RX_ONLY) {
		if (debug->time_info_array) {
			(debug->time_info_array + hif_new->index)->_i =
				hif_new->index;
		}
	}
	if (hif_new->subtype != LOOPBACK_MODE_TX_ONLY) {
		if (debug->time_info_array) {
			(debug->time_info_array + hif_new->index)->_rxt =
				debug->rcv_time_last;
		}
	}
	if (hif_new->index == 0) {
		debug->rcv_time_first = debug->rcv_time_last;
		INFO("[Loopback Test] First frame received time: %llu",
		     debug->rcv_time_first);
	}

	if (hif_new->subtype == LOOPBACK_MODE_TX_ONLY) {
		d = (u32 *)(skb->data);
		debug->arv_time_first = *d;
		debug->arv_time_last = *(d + 2);
		INFO("[Loopback Test][TX only] -- test done --");
	} else {
		if (hif_new->index > hif_new->count - 4) {
			if (debug->lb_hexdump) {
				print_hex_dump(KERN_DEBUG, "HIF ",
					       DUMP_PREFIX_NONE, 16, 1,
					       skb->data - sizeof(struct hif),
					       8, false);
				p = skb->data + 36;
				print_hex_dump(KERN_DEBUG,
					       str[hif_new->subtype],
					       DUMP_PREFIX_NONE, 16, 1,
					       skb->data, skb->len, false);
			}
			d = (u32 *)(skb->data);
			debug->arv_time_first = *d;
			debug->arv_time_last = *(d + 2);
		}
		if (hif_new->index == hif_new->count - 1) {
			INFO("[Loopback Test] Last frame received time: %llu",
			     debug->rcv_time_last);
			if (hif_new->subtype == LOOPBACK_MODE_ROUNDTRIP) {
				INFO("[Loopback Test][Round-trip] -- test done --");
			} else if (hif_new->subtype == LOOPBACK_MODE_RX_ONLY) {
				INFO("[Loopback Test][RX only] -- test done --");
			}
		}
	}

	NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
	return false; /* Processed completely in HAL, don't forward to WLAN */
}
