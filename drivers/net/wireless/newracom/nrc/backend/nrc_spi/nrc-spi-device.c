/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC SPI Device Registration Implementation
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
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>

/* Common directory headers - Interfaces */
#include "nrc-hif.h"
#include "nrc-backend-hif-interface.h"

/* Local module headers */
#include "nrc-debug.h"
#include "nrc-spi-device.h"
#include "nrc-hif-cspi.h"

static struct nrc_spi_device_info g_spi_device_info;
static DEFINE_MUTEX(spi_device_mutex);

/**
 * nrc_spi_register_device - Register SPI device for HAL layer
 * @spi: SPI device
 * @priv: SPI private data
 * @ops: HIF operations structure
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_spi_register_device(struct spi_device *spi, void *priv,
			    struct nrc_hif_ops *ops)
{
	mutex_lock(&spi_device_mutex);

	if (g_spi_device_info.initialized) {
		mutex_unlock(&spi_device_mutex);
		ERR_SPI("SPI device already registered");
		return -EEXIST;
	}

	g_spi_device_info.spi = spi;
	g_spi_device_info.dev = &spi->dev;
	g_spi_device_info.priv = priv;
	g_spi_device_info.ops = ops;
	g_spi_device_info.initialized = true;

	mutex_unlock(&spi_device_mutex);

	return 0;
}

/**
 * nrc_spi_unregister_device - Unregister SPI device
 * @spi: SPI device to unregister
 */
void nrc_spi_unregister_device(struct spi_device *spi)
{
	mutex_lock(&spi_device_mutex);

	if (g_spi_device_info.initialized && g_spi_device_info.spi == spi) {
		memset(&g_spi_device_info, 0, sizeof(g_spi_device_info));
	}

	mutex_unlock(&spi_device_mutex);
}

/**
 * nrc_spi_get_device_info - Get registered SPI device information
 *
 * Returns: SPI device info structure or NULL if not available
 */
struct nrc_spi_device_info *nrc_spi_get_device_info(void)
{
	mutex_lock(&spi_device_mutex);

	if (!g_spi_device_info.initialized) {
		mutex_unlock(&spi_device_mutex);
		return NULL;
	}

	mutex_unlock(&spi_device_mutex);
	return &g_spi_device_info;
}
EXPORT_SYMBOL(nrc_spi_get_device_info);

/**
 * nrc_spi_is_device_available - Check if SPI device is available
 *
 * Returns: true if device is available, false otherwise
 */
bool nrc_spi_is_device_available(void)
{
	bool available;

	mutex_lock(&spi_device_mutex);
	available = g_spi_device_info.initialized;
	mutex_unlock(&spi_device_mutex);

	return available;
}
EXPORT_SYMBOL(nrc_spi_is_device_available);

/**
 * nrc_spi_get_chip_id - Get chip ID from registered SPI device
 *
 * Returns: chip ID or 0 if device not available
 */
u16 nrc_spi_get_chip_id(void)
{
	struct nrc_spi_priv *priv;
	u16 chip_id = 0;

	mutex_lock(&spi_device_mutex);

	if (!g_spi_device_info.initialized || !g_spi_device_info.priv) {
		mutex_unlock(&spi_device_mutex);
		return 0;
	}

	priv = (struct nrc_spi_priv *)g_spi_device_info.priv;
	chip_id = priv->hw.sys.chip_id;

	mutex_unlock(&spi_device_mutex);

	return chip_id;
}
EXPORT_SYMBOL(nrc_spi_get_chip_id);
