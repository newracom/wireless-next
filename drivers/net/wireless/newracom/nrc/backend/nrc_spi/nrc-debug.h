/*
 * Copyright (c) 2016-2025 Newracom, Inc.
 *
 * NRC SPI Backend Debug Functions Header
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

#ifndef _NRC_SPI_DEBUG_H_
#define _NRC_SPI_DEBUG_H_

#include <linux/spi/spi.h>

/* Include common debug interface */
#include "nrc-debug-common.h"

/* SPI-specific debug macros */
#define INFO_SPI(fmt, ...) INFo("Spi", fmt, ##__VA_ARGS__)
#define WARN_SPI(fmt, ...) WARn("Spi", fmt, ##__VA_ARGS__)
#define ERR_SPI(fmt, ...) ERR("Spi", fmt, ##__VA_ARGS__)

/* ===========================================================================
 * SPI Debug Function Prototypes
 * =========================================================================== */

/* Debug functions */
void nrc_spi_init_debugfs(struct spi_device *spi);
void nrc_spi_exit_debugfs(void);
void nrc_spi_debug_info(struct spi_device *spi);

#endif /* _NRC_SPI_DEBUG_H_ */
