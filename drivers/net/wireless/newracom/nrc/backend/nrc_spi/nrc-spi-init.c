/*
 *
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC SPI Module Initialization
 * Handles module loading and unloading for SPI backend
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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/completion.h>
#if defined(ANDROID) && defined(CONFIG_PM)
#include <linux/pm_wakeirq.h>
#include <linux/pm_runtime.h>
#endif

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Callback */
#include "nrc-backend-hif-callback.h"

/* Local module headers */
#include "nrc-hif-cspi.h"
#include "nrc-spi-params.h"
#include "nrc-spi-device.h"
#include "nrc-debug.h"
#include "nrc-spi-hif-ops.h"
#include "nrc-spi-gpio.h"

/* External declarations */
static DECLARE_COMPLETION(spi_probe_done);

#ifndef CONFIG_SPI_USE_DT
struct spi_device *nrc_create_spi_device(void);
#endif

#ifndef CONFIG_SPI_USE_DT
static struct spi_device *g_spi_dev;
#endif

/* SPI device ID table */
static const struct spi_device_id nrc_spi_id[] = {{NRC_DRIVER_NAME, 0}, {}};
MODULE_DEVICE_TABLE(spi, nrc_spi_id);

/*
 * Reset the chip once at probe so that it starts ROM boot from a known state.
 * nrc_cspi_reset() picks the hardware line when the board has one and falls
 * back to the soft reset otherwise. Readiness is only sampled here; the real
 * wait, with escalating windows, happens in spi_hif_probe().
 */
static int nrc_cspi_device_hw_reset(struct nrc_spi_priv *priv)
{
	if (!priv) {
		ERR_SPI("Invalid priv pointer");
		return -EINVAL;
	}

	INFO("Resetting device");
	nrc_cspi_reset(priv, priv->spi);

	msleep(NRC_HW_RESET_SETTLE_MS);

	if (spi_hif_wait_rom_boot(priv->spi, &priv->hw.sys,
				  NRC_HW_RESET_READY_TIMEOUT_MS, false))
		WARN_SPI("Device not responding %dms after reset; probe will keep waiting",
			 NRC_HW_RESET_SETTLE_MS +
				 NRC_HW_RESET_READY_TIMEOUT_MS);
	else
		INFO("Device reset completed");
	return 0;
}

/**
 * nrc_cspi_probe - SPI device probe function
 * @spi: SPI device to probe
 *
 * Returns: 0 on success, negative error code on failure
 */
static int nrc_cspi_probe(struct spi_device *spi)
{
	struct nrc_spi_priv *priv;
	int ret = 0;

	INFO("Probe nrc spi device");

	nrc_dbg_init(&spi->dev);

#ifdef ANDROID
#ifdef CONFIG_PM
	INFO("[%s,L%d] device_init_wakeup spi->irq:%d", __func__, __LINE__,
	     spi->irq);
	device_init_wakeup(&spi->dev, true);
	dev_pm_set_wake_irq(&spi->dev, spi->irq);
#ifdef ANDROID_SP01
	pm_runtime_disable(spi->controller->dev.parent);

#ifdef CONFIG_PM_SLEEP
	spi_master_suspend(spi->controller);
	pm_generic_resume(spi->controller->dev.parent);
#endif

	pm_generic_runtime_resume(spi->controller->dev.parent);
	spi->controller->auto_runtime_pm = false;
#endif
	spi->controller->rt = true;
#endif
#endif

	priv = nrc_cspi_alloc(spi);
	if (IS_ERR(priv)) {
		ERR("Failed to nrc_cspi_alloc");
		complete(&spi_probe_done);
		return PTR_ERR(priv);
	}

	ret = nrc_cspi_gpio_alloc(spi);
	if (ret) {
		ERR("Failed to nrc_cspi_gpio_alloc");
		goto err_cspi_free;
	}

	/*
	 * Publish priv before the reset. spi_read_sys_reg() and
	 * spi_hif_wait_rom_boot() reach priv through spi_get_drvdata(), and
	 * they use priv->boot_poll to suppress the read failures that are
	 * expected while the target comes up. Setting it afterwards left that
	 * suppression inoperative, so the readiness poll flooded the log with
	 * rate-limited errors and the real poll count became unknowable.
	 */
	spi_set_drvdata(spi, priv);

	nrc_cspi_device_hw_reset(priv);

	/* Register SPI device for HAL layer to discover */
	ret = nrc_spi_register_device(spi, priv, nrc_spi_get_hif_ops());
	if (ret) {
		ERR("Failed to register SPI device for HAL layer");
		goto err_gpio_free;
	}

	g_spi_priv = priv; /* Set global reference */

	/* Initialize SPI debugfs */
	nrc_spi_init_debugfs(spi);

	INFO("NRC SPI device registered successfully for HAL layer");
	complete(&spi_probe_done);
	return 0;

err_gpio_free:
	nrc_cspi_gpio_free(spi);
err_cspi_free:
	nrc_cspi_free(priv);

	spi_set_drvdata(spi, NULL);
#ifdef ANDROID
#ifdef CONFIG_PM
	dev_pm_clear_wake_irq(&spi->dev);
	device_init_wakeup(&spi->dev, false);
#endif
#endif
	complete(&spi_probe_done);
	return ret;
}

/**
 * nrc_cspi_remove - SPI device remove function
 * @spi: SPI device to remove
 *
 * Returns: 0 on older kernels, void on newer kernels
 */
#if NRC_TARGET_KERNEL_VERSION < KERNEL_VERSION(5, 18, 0)
static int nrc_cspi_remove(struct spi_device *spi)
#else
static void nrc_cspi_remove(struct spi_device *spi)
#endif
{
	struct nrc_spi_priv *priv;

#ifdef ANDROID
#ifdef CONFIG_PM
	dev_pm_clear_wake_irq(&spi->dev);
	device_init_wakeup(&spi->dev, false);
#endif
#endif
	INFO("Remove nrc spi device");

	/* Cleanup SPI debugfs */
	nrc_spi_exit_debugfs();

	priv = spi_get_drvdata(spi);
	if (!priv) {
		WARN_SPI("SPI device data is NULL");
#if NRC_TARGET_KERNEL_VERSION < KERNEL_VERSION(5, 18, 0)
		return 0;
#else
		return;
#endif
	}

	/* Force IRQ cleanup if the frontend was not stopped first (e.g. SPI
	 * device removed on reboot while the driver is still up). */
	nrc_spi_free_irq(priv);

	/* Cancel any pending work - this is safe */
	cancel_delayed_work_sync(&priv->work);

	/* Only warn about leftover threads - never call kthread_stop() to avoid use-after-free */
	if (priv->polling_kthread) {
		WARN_SPI("polling_kthread still exists during module unload - leaked");
	}
	if (priv->kthread) {
		WARN_SPI("kthread still exists during module unload - leaked");
	}
	/* Unregister SPI device from HAL layer */
	nrc_spi_unregister_device(spi);

	/* Cleanup power save GPIO if it was allocated */
	if (priv->power_save_gpio_allocated &&
	    priv->power_save_gpio_number > 0) {
		INFO("SPI: Freeing power save GPIO %d during module unload",
		     priv->power_save_gpio_number);
		nrc_gpio_free(priv->power_save_gpio_number);
		priv->power_save_gpio_allocated = false;
		priv->power_save_gpio_number = -1;
	}

	spi_set_drvdata(spi, NULL);
	g_spi_priv = NULL; /* Clear global reference */

	nrc_cspi_gpio_free(spi);
	nrc_cspi_free(priv);

	INFO("NRC SPI device removed successfully");
#if NRC_TARGET_KERNEL_VERSION < KERNEL_VERSION(5, 18, 0)
	return 0;
#endif
}

/* For reboot or halt */
/* after shutdown, remove is called, so simply implemented for only ps */
/* Note: Wake handling is now done by HAL platform driver shutdown callback */
static void nrc_cspi_shutdown(struct spi_device *spi)
{
	INFO("Shutdown nrc spi device");
	/* HAL handles wake during system shutdown via its own shutdown callback */
}

/* SPI driver structure */
static struct spi_driver nrc_cspi_driver = {
	.id_table = nrc_spi_id,
	.probe = nrc_cspi_probe,
	.remove = nrc_cspi_remove,
	.shutdown = nrc_cspi_shutdown,
	.driver =
		{
			.name = NRC_DRIVER_NAME,
		},
};

/**
 * nrc_cspi_init - Initialize NRC SPI module
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init nrc_cspi_init(void)
{
#ifndef CONFIG_SPI_USE_DT
	struct spi_device *spi;
#endif
	int ret = 0;

	/* Initialize SPI parameters */
	nrc_spi_params_init();

#ifndef CONFIG_SPI_USE_DT
	spi = nrc_create_spi_device();
	if (IS_ERR(spi)) {
		ERR("Failed to nrc_create_spi_dev");
		goto out;
	}
	g_spi_dev = spi;
#endif

	ret = spi_register_driver(&nrc_cspi_driver);
	if (ret) {
		ERR("Failed to register SPI driver(%s): %d",
		    nrc_cspi_driver.driver.name, ret);
		goto unregister_device;
	}

	/* Wait for SPI device probe to complete.
	 * The SPI core probes devices asynchronously after driver registration.
	 * Other modules (nrc_core, nrc_wlan) depend on the SPI device being
	 * fully probed, so we must wait here before returning from module_init.
	 * This ensures modprobe can load the entire dependency chain
	 * (nrc_spi → nrc_core → nrc_wlan) in one shot without timing issues.
	 */
	if (!wait_for_completion_timeout(&spi_probe_done,
					 msecs_to_jiffies(5000))) {
		ERR("SPI device probe timed out — no matching device found");
		spi_unregister_driver(&nrc_cspi_driver);
		ret = -ENODEV;
		goto unregister_device;
	}

	if (!g_spi_priv) {
		ERR("SPI device probe failed");
		spi_unregister_driver(&nrc_cspi_driver);
		ret = -ENODEV;
		goto unregister_device;
	}

	return 0;

unregister_device:
#ifndef CONFIG_SPI_USE_DT
	spi_unregister_device(spi);
out:
#endif
	return ret;
}

/**
 * nrc_cspi_exit - Cleanup NRC SPI module
 */
static void __exit nrc_cspi_exit(void)
{
#ifndef CONFIG_SPI_USE_DT
	spi_unregister_device(g_spi_dev);
#endif
	spi_unregister_driver(&nrc_cspi_driver);
}

module_init(nrc_cspi_init);
module_exit(nrc_cspi_exit);

MODULE_AUTHOR("Newracom, Inc.(http://www.newracom.com)");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Newracom 802.11 driver");
#if KERNEL_VERSION(5, 12, 0) > NRC_TARGET_KERNEL_VERSION
MODULE_SUPPORTED_DEVICE("Newracom 802.11 devices");
#endif
