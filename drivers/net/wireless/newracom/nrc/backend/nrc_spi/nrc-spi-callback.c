/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC SPI Callback Implementation
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

/* Common directory headers */
#include "nrc-hif.h"
#include "nrc-backend-hif-callback.h"
#include "nrc-debug-common.h"

/* Global callback management */
static struct {
	nrc_spi_callback_fn callback;
	spinlock_t lock;
	bool registered;
} spi_callback_mgr = {
	.callback = NULL,
	.lock = __SPIN_LOCK_UNLOCKED(spi_callback_mgr.lock),
	.registered = false,
};

/**
 * nrc_spi_register_callback - Register HAL callback function
 * @callback: Callback function to register
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_spi_register_callback(nrc_spi_callback_fn callback)
{
	unsigned long flags;

	if (!callback) {
		DBG_HIF("Invalid callback function");
		return -EINVAL;
	}

	spin_lock_irqsave(&spi_callback_mgr.lock, flags);

	if (spi_callback_mgr.registered) {
		spin_unlock_irqrestore(&spi_callback_mgr.lock, flags);
		DBG_HIF("Callback already registered");
		return -EBUSY;
	}

	spi_callback_mgr.callback = callback;
	spi_callback_mgr.registered = true;

	spin_unlock_irqrestore(&spi_callback_mgr.lock, flags);

	DBG_HIF("SPI callback registered successfully");
	return 0;
}
EXPORT_SYMBOL(nrc_spi_register_callback);

/**
 * nrc_spi_unregister_callback - Unregister HAL callback function
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_spi_unregister_callback(void)
{
	unsigned long flags;

	spin_lock_irqsave(&spi_callback_mgr.lock, flags);

	if (!spi_callback_mgr.registered) {
		spin_unlock_irqrestore(&spi_callback_mgr.lock, flags);
		DBG_HIF("No callback registered");
		return -ENOENT;
	}

	spi_callback_mgr.callback = NULL;
	spi_callback_mgr.registered = false;

	spin_unlock_irqrestore(&spi_callback_mgr.lock, flags);

	DBG_HIF("SPI callback unregistered successfully");
	return 0;
}
EXPORT_SYMBOL(nrc_spi_unregister_callback);

/**
 * nrc_spi_trigger_event - Trigger registered callback with event data
 * @event: Event data to pass to callback
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_spi_trigger_event(struct nrc_spi_event_data *event)
{
	unsigned long flags;
	nrc_spi_callback_fn callback = NULL;
	int ret = 0;

	if (!event) {
		DBG_HIF("Invalid event data");
		return -EINVAL;
	}

	spin_lock_irqsave(&spi_callback_mgr.lock, flags);

	if (spi_callback_mgr.registered && spi_callback_mgr.callback) {
		callback = spi_callback_mgr.callback;
	}

	spin_unlock_irqrestore(&spi_callback_mgr.lock, flags);

	if (callback) {
		ret = callback(event);
		if (ret < 0) {
			ERR_BUS("Callback returned error: %d", ret);
		}
	} else {
		ERR_BUS("No callback registered for event type %d",
			event->type);
		ret = -ENOENT;
	}

	return ret;
}
