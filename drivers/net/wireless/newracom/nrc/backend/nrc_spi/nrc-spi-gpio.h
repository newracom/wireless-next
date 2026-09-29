/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC SPI GPIO Helper Functions Header
 * Kernel version compatibility for GPIO operations
 */

#ifndef _NRC_SPI_GPIO_H_
#define _NRC_SPI_GPIO_H_

#include <linux/gpio/consumer.h>

/**
 * nrc_gpio_request - Request a GPIO pin
 * @gpio: GPIO number (for legacy API)
 * @label: Label for the GPIO request
 *
 * Returns: GPIO descriptor pointer on success, ERR_PTR() on failure
 */
struct gpio_desc *nrc_gpio_request(unsigned gpio, const char *label);

/**
 * nrc_gpio_free - Free a GPIO pin
 * @gpio: GPIO number
 */
void nrc_gpio_free(unsigned gpio);

/**
 * nrc_gpio_direction_output - Set GPIO as output
 * @gpio: GPIO number
 * @value: Initial output value
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_gpio_direction_output(unsigned gpio, int value);

/**
 * nrc_gpio_set_value - Set GPIO output value
 * @gpio: GPIO number
 * @value: Output value (0 or 1)
 */
void nrc_gpio_set_value(unsigned gpio, int value);

/**
 * nrc_gpio_direction_input - Set GPIO as input
 * @gpio: GPIO number
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_gpio_direction_input(unsigned gpio);

#endif /* _NRC_SPI_GPIO_H_ */
