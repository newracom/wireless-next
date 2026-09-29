// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC SPI GPIO Helper Functions Implementation
 * Kernel version compatibility for GPIO operations
 */

/* Linux kernel headers */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/device.h>

/* Common directory headers */
#include "nrc-debug-common.h"

/* Local headers */
#include "nrc-spi-gpio.h"

/* GPIO descriptor storage */
#define MAX_GPIO_DESCRIPTORS 20
static struct {
	unsigned gpio_num;
	struct gpio_desc *desc;
	bool in_use;
} gpio_desc_table[MAX_GPIO_DESCRIPTORS];

static struct gpio_desc *find_gpio_desc(unsigned gpio)
{
	int i;
	for (i = 0; i < MAX_GPIO_DESCRIPTORS; i++) {
		if (gpio_desc_table[i].in_use &&
		    gpio_desc_table[i].gpio_num == gpio) {
			return gpio_desc_table[i].desc;
		}
	}
	return NULL;
}

static int store_gpio_desc(unsigned gpio, struct gpio_desc *desc)
{
	int i;
	for (i = 0; i < MAX_GPIO_DESCRIPTORS; i++) {
		if (!gpio_desc_table[i].in_use) {
			gpio_desc_table[i].gpio_num = gpio;
			gpio_desc_table[i].desc = desc;
			gpio_desc_table[i].in_use = true;
			return 0;
		}
	}
	return -ENOMEM;
}

static void remove_gpio_desc(unsigned gpio)
{
	int i;
	for (i = 0; i < MAX_GPIO_DESCRIPTORS; i++) {
		if (gpio_desc_table[i].in_use &&
		    gpio_desc_table[i].gpio_num == gpio) {
			gpio_desc_table[i].in_use = false;
			gpio_desc_table[i].desc = NULL;
			gpio_desc_table[i].gpio_num = 0;
			break;
		}
	}
}

/**
 * nrc_gpio_request - Request a GPIO pin
 * @gpio: GPIO number (for legacy API)
 * @label: Label for the GPIO request
 *
 * Requests the GPIO through the legacy API and keeps its descriptor for the
 * gpiod_* calls.
 *
 * Returns: GPIO descriptor pointer on success, ERR_PTR() on failure
 */
struct gpio_desc *nrc_gpio_request(unsigned gpio, const char *label)
{
	struct gpio_desc *desc;
	int ret;

	/* First try to request using legacy GPIO API, then convert to descriptor */
	ret = gpio_request(gpio, label);
	if (ret < 0) {
		ERR_HIF("GPIO: Failed to request GPIO %d with label '%s': %d",
			gpio, label, ret);
		return ERR_PTR(ret);
	}

	/* Get GPIO descriptor using gpio number */
	desc = gpio_to_desc(gpio);
	if (!desc) {
		ERR_HIF("GPIO: Invalid GPIO %d", gpio);
		gpio_free(gpio);
		return ERR_PTR(-EINVAL);
	}

	/* Store the descriptor for later use */
	ret = store_gpio_desc(gpio, desc);
	if (ret < 0) {
		ERR_HIF("GPIO: Failed to store descriptor for GPIO %d", gpio);
		gpio_free(gpio);
		return ERR_PTR(ret);
	}

	DBG_HIF("GPIO: Successfully requested GPIO %d with label '%s'", gpio,
		label);

	return desc;
}

/**
 * nrc_gpio_free - Free a GPIO pin
 * @gpio: GPIO number
 */
void nrc_gpio_free(unsigned gpio)
{
	struct gpio_desc *desc = find_gpio_desc(gpio);
	if (desc && !IS_ERR(desc)) {
		/* Remove from our descriptor table first */
		remove_gpio_desc(gpio);
	}
	gpio_free(gpio);
	DBG_HIF("GPIO: Successfully freed GPIO %d", gpio);
}

/**
 * nrc_gpio_direction_output - Set GPIO as output
 * @gpio: GPIO number
 * @value: Initial output value
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_gpio_direction_output(unsigned gpio, int value)
{
	struct gpio_desc *desc = find_gpio_desc(gpio);
	if (!desc) {
		ERR_HIF("GPIO: GPIO %d descriptor not found", gpio);
		return -EINVAL;
	}
	return gpiod_direction_output(desc, value);
}

/**
 * nrc_gpio_set_value - Set GPIO output value
 * @gpio: GPIO number
 * @value: Output value (0 or 1)
 */
void nrc_gpio_set_value(unsigned gpio, int value)
{
	DBG_HIF("Set GPIO %d to %s", gpio, value ? "HIGH" : "LOW");

	struct gpio_desc *desc = find_gpio_desc(gpio);
	if (!desc) {
		ERR_HIF("GPIO: GPIO %d descriptor not found", gpio);
		return;
	}
	gpiod_set_value(desc, value);
}

/**
 * nrc_gpio_direction_input - Set GPIO as input
 * @gpio: GPIO number
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_gpio_direction_input(unsigned gpio)
{
	struct gpio_desc *desc = find_gpio_desc(gpio);
	if (!desc) {
		ERR_HIF("GPIO: GPIO %d descriptor not found", gpio);
		return -EINVAL;
	}
	return gpiod_direction_input(desc);
}
