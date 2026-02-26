/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC SPI Module Parameters Implementation
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
#include <linux/stat.h>

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers */
#include "nrc-spi-params.h"

/* SPI Configuration Parameters */
int spi_bus_num;
module_param(spi_bus_num, int, 0600);
MODULE_PARM_DESC(spi_bus_num, "SPI controller bus number");

int spi_cs_num;
module_param(spi_cs_num, int, 0600);
MODULE_PARM_DESC(spi_cs_num, "SPI chip select number");

int spi_gpio_irq = -1;
module_param(spi_gpio_irq, int, 0600);
MODULE_PARM_DESC(spi_gpio_irq, "SPI gpio irq");

int spi_polling_interval = 0;
module_param(spi_polling_interval, int, 0600);
MODULE_PARM_DESC(spi_polling_interval, "SPI polling interval (msec)");

int spi_gdma_irq = 6;
module_param(spi_gdma_irq, int, 0600);
MODULE_PARM_DESC(spi_gdma_irq, "SPI gdma irq");

bool enable_hspi_init = false;
module_param(enable_hspi_init, bool, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(enable_hspi_init, "Enable HSPI Initialization");

/**
 * default port name
 */
#if defined(CONFIG_ARM)
#if defined(CONFIG_NRC_HIF_UART)
char *hifport = "/dev/ttyAMA0";
#else
char *hifport = "/dev/ttyUSB0";
#endif
#else
char *hifport = "/dev/ttyUSB0";
#endif
module_param(hifport, charp, 0600);
MODULE_PARM_DESC(hifport, "HIF port device name");

/**
 * default port speed
 */
#if defined(CONFIG_NRC_HIF_CSPI)
int hifspeed = (20 * 1000 * 1000);
#elif defined(CONFIG_NRC_HIF_SSP)
int hifspeed = (1300 * 1000);
#elif defined(CONFIG_NRC_HIF_UART)
int hifspeed = 115200;
#else
int hifspeed = 115200;
#endif
module_param(hifspeed, int, 0600);
MODULE_PARM_DESC(hifspeed, "HIF port speed");

/**
 * gpio for power save (default Host_output(GP20) --> Target_input(GP11))
 */
int power_save_gpio[3] = {
	20, 11,
	1}; /* HOST_GPIO_FOR_TARGET_WAKEUP, TARGET_GPIO_FOR_WAKEUP, TARGET_WAKEUP_ACTIVE_HIGH */
module_param_array(power_save_gpio, int, NULL, 0600);
MODULE_PARM_DESC(power_save_gpio, "gpio for power save");

/* Debug level: 0=ERR, 1=WARN, 2=INFO, 3=DBG */
int debug_level = DEFAULT_NRC_DBG_LEVEL;
module_param(debug_level, int, 0600);
MODULE_PARM_DESC(debug_level, "Debug level (0=ERR, 1=WARN, 2=INFO, 3=DBG)");

/* Debug mask: bitmask for categories */
unsigned long debug_mask = DEFAULT_NRC_DBG_MASK;
module_param(debug_mask, ulong, 0600);
MODULE_PARM_DESC(debug_mask, "Debug category mask (BASIC=0x1, HIF=0x2, WIM=0x4, TX=0x8, RX=0x10, MAC=0x20, CAPI=0x40, PS=0x80, STATS=0x100, STATE=0x200, BD=0x400, FW=0x800, AMPDU=0x1000, CREDIT=0x2000, SLOT=0x4000, BUS=0x8000, ALL=0xFFFFFFFF)");

/* SPI-specific parameters only - power management and board data are handled by HAL layer */

/**
 * nrc_spi_params_init - Initialize SPI parameters with default values
 */
void nrc_spi_params_init(void)
{
	/* Set default values if not specified */
	// if (spi_gpio_irq == -1) {
	// 	ERR_SPI("SPI: Using default GPIO IRQ configuration");
	// }
}
