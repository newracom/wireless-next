/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC SPI Backend Debug Implementation
 */

/* Linux kernel headers */
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/math64.h>
#include <linux/spi/spi.h>
#include <linux/version.h>
#include <linux/debugfs.h>

/* Local module headers */
#include "nrc-debug-common.h"
#include "nrc-hif-cspi.h"
#include "nrc-debug.h"

/* Global debug variables - defined as module parameters in nrc-spi-params.c */
extern unsigned long debug_mask;
extern int debug_level;
struct device *g_dev;

#ifdef CONFIG_DEBUG_FS
/* SPI debugfs root */
static struct dentry *nrc_spi_debugfs_root;

/* SPI-specific debug functions now use common debug system */

/* SPI module debug mask control */
static int nrc_spi_debugfs_debug_read(void *data, u64 *val)
{
	*val = debug_mask;
	return 0;
}

static int nrc_spi_debugfs_debug_write(void *data, u64 val)
{
	debug_mask = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_spi_debugfs_debug_fops, nrc_spi_debugfs_debug_read,
			nrc_spi_debugfs_debug_write, "%llu\n");

/* SPI module debug level control */
static int nrc_spi_debugfs_level_read(void *data, u64 *val)
{
	*val = debug_level;
	return 0;
}

static int nrc_spi_debugfs_level_write(void *data, u64 val)
{
	if (val < NRC_DBG_LEVEL_MAX)
		debug_level = (enum NRC_DEBUG_LEVEL)val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_spi_debugfs_level_fops, nrc_spi_debugfs_level_read,
			nrc_spi_debugfs_level_write, "%llu\n");

/* CSPI status test - read SPI system register to verify communication */
static int nrc_spi_debugfs_cspi_read(void *data, u64 *val)
{
	struct nrc_spi_priv *priv = (struct nrc_spi_priv *)data;
	struct spi_sys_reg sys;
	int ret;

	if (!priv || !priv->spi)
		return -ENODEV;

	/* Read SPI system register to test communication */
	ret = c_spi_read_regs(priv->spi, C_SPI_SYS_REG, (void *)&sys,
			      sizeof(struct spi_sys_reg));
	if (ret) {
		*val = ret;
		return 0;
	}

	/* Return chip_id as success indicator */
	*val = sys.chip_id;
	return 0;
}

static int nrc_spi_debugfs_cspi_write(void *data, u64 val)
{
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_spi_debugfs_cspi_fops, nrc_spi_debugfs_cspi_read,
			nrc_spi_debugfs_cspi_write, "%llu\n");

/* Reset device */
static int nrc_spi_debugfs_reset_read(void *data, u64 *val)
{
	*val = 0;
	return 0;
}

static int nrc_spi_debugfs_reset_write(void *data, u64 val)
{
	struct nrc_spi_priv *priv = (struct nrc_spi_priv *)data;

	if (!priv || !priv->hdev)
		return -ENODEV;

	/* Call HIF ops reset function via hdev->hif_ops */
	if (priv->hdev->hif_ops && priv->hdev->hif_ops->reset_device)
		priv->hdev->hif_ops->reset_device(priv->hdev);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_spi_debugfs_reset_fops, nrc_spi_debugfs_reset_read,
			nrc_spi_debugfs_reset_write, "%llu\n");
#endif /* CONFIG_DEBUG_FS */

/* Forward declarations to avoid missing prototype warnings */
void nrc_spi_init_debugfs(struct spi_device *spi);
void nrc_spi_exit_debugfs(void);
void nrc_spi_debug_info(struct spi_device *spi);

void nrc_spi_init_debugfs(struct spi_device *spi)
{
#ifdef CONFIG_DEBUG_FS
	struct nrc_spi_priv *priv = spi_get_drvdata(spi);

	/* Create SPI debugfs root directory */
	nrc_spi_debugfs_root = debugfs_create_dir("nrc_spi", NULL);
	if (!nrc_spi_debugfs_root) {
		ERR_HIF("Failed to create nrc_spi debugfs directory");
		return;
	}

	/* Create SPI module debug mask control */
	debugfs_create_file("debug_mask", 0664, nrc_spi_debugfs_root, NULL,
			    &nrc_spi_debugfs_debug_fops);

	/* Create SPI module debug level control */
	debugfs_create_file("debug_level", 0664, nrc_spi_debugfs_root, NULL,
			    &nrc_spi_debugfs_level_fops);

	/* Create CSPI status test */
	debugfs_create_file("cspi", 0664, nrc_spi_debugfs_root, priv,
			    &nrc_spi_debugfs_cspi_fops);

	/* Create device reset control */
	debugfs_create_file("reset", 0664, nrc_spi_debugfs_root, priv,
			    &nrc_spi_debugfs_reset_fops);

	INFO("SPI debugfs initialized at /sys/kernel/debug/nrc_spi/");
#endif
}

void nrc_spi_exit_debugfs(void)
{
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(nrc_spi_debugfs_root);
	nrc_spi_debugfs_root = NULL;
#endif
}

void nrc_spi_debug_info(struct spi_device *spi)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	DBG_HIF("SPI device: bus=%d, cs=%d, max_speed=%u Hz",
		spi->controller->bus_num, spi->chip_select, spi->max_speed_hz);
#else
	DBG_HIF("SPI device: bus=%d, cs=%d, max_speed=%u Hz",
		spi->master->bus_num, spi->chip_select, spi->max_speed_hz);
#endif
}
