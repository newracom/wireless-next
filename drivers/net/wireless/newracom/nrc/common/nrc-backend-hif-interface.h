/*
 *
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC Backend HIF Interface - HAL uses these to access SPI HIF operations
 */

#ifndef _NRC_BACKEND_HIF_INTERFACE_H
#define _NRC_BACKEND_HIF_INTERFACE_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include "nrc-hif.h"

/* SPI device information structure for HAL layer */
struct nrc_spi_device_info {
	struct spi_device *spi;
	struct device *dev;
	void *priv; /* SPI private data */
	struct nrc_hif_ops *ops; /* HIF operations */
	bool initialized; /* Device initialization status */
};

/* SPI Device Registration Interface */
/* These functions are implemented in SPI backend and exported for HAL usage */
extern struct nrc_spi_device_info *nrc_spi_get_device_info(void);
extern bool nrc_spi_is_device_available(void);

/* Backend Core References Interface */
/* Function to set HAL core references in backend module - called by HAL */
extern void nrc_backend_set_hal_core_refs(struct nrc_hif_device *hdev);

/* SPI Device Information Access Interface */
/* Function to get chip ID from SPI device - called by HAL */
extern u16 nrc_spi_get_chip_id(void);

/* Forward declaration */
extern struct nrc_hif_device *nrc_hal_core_get_hdev(void);

/*
 * ==================================================================
 * HIF Operations Inline Wrapper Functions
 * ==================================================================
 */

/* Basic HIF Operations */
static inline int nrc_hif_ops_write(u8 *data, u32 len)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->write) ?
		       hdev->hif_ops->write(hdev, data, len) :
		       -EOPNOTSUPP;
}

static inline int nrc_hif_ops_read(const u8 *data, u32 len)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->read) ?
		       hdev->hif_ops->read(hdev, data, len) :
		       -EOPNOTSUPP;
}

static inline int nrc_hif_ops_wait_rxq_slot(u8 *data, u32 len)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->wait_rxq_slot) ?
		       hdev->hif_ops->wait_rxq_slot(hdev, data, len) :
		       -EOPNOTSUPP;
}

/* Status and Control Operations */
static inline int nrc_hif_ops_check_sleep(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->check_sleep) ?
		       hdev->hif_ops->check_sleep(hdev) :
		       -EOPNOTSUPP;
}

static inline int nrc_hif_ops_xmit(struct sk_buff *skb)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->xmit) ?
		       hdev->hif_ops->xmit(hdev, skb) :
		       -EOPNOTSUPP;
}

static inline int nrc_hif_ops_wait_for_xmit(struct sk_buff *skb)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->wait_for_xmit) ?
		       hdev->hif_ops->wait_for_xmit(hdev, skb) :
		       -EOPNOTSUPP;
}

/* Configuration Operations */
static inline int nrc_hif_ops_update(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->update)
		return hdev->hif_ops->update(hdev);
	return 0;
}

/* Target Status Operations */
static inline int nrc_hif_ops_check_target(u8 reg)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->check_target) ?
		       hdev->hif_ops->check_target(hdev, reg) :
		       -EOPNOTSUPP;
}

static inline bool nrc_hif_ops_fw_is_loaded(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->fw_is_loaded) ?
		       hdev->hif_ops->fw_is_loaded(hdev) :
		       true;
}

static inline bool nrc_hif_ops_fw_is_boot(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->fw_is_boot) ?
		       hdev->hif_ops->fw_is_boot(hdev) :
		       false;
}

/* Generic GPIO Operations */
static inline int nrc_hif_ops_gpio_alloc(int gpio_num, const char *label)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->gpio_alloc) ?
		       hdev->hif_ops->gpio_alloc(hdev, gpio_num, label) :
		       0; /* Success if not implemented */
}

static inline void nrc_hif_ops_gpio_free(int gpio_num)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->gpio_free)
		hdev->hif_ops->gpio_free(hdev, gpio_num);
}

static inline void nrc_hif_ops_gpio_set(int gpio_num, int value)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->gpio_set)
		hdev->hif_ops->gpio_set(hdev, gpio_num, value);
}

static inline int nrc_hif_ops_reset_device(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->reset_device) {
		hdev->hif_ops->reset_device(hdev);
		return 0;
	}
	return -EOPNOTSUPP;
}

static inline int nrc_hif_ops_probe(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->probe) {
		/*
		 * Propagate the backend result. Returning 0 unconditionally
		 * made the caller's reset-and-retry loop unreachable, so a
		 * target that was merely slow to reach the ROM bootloader was
		 * reported once and then failed the bootloader-mode check.
		 */
		return hdev->hif_ops->probe(hdev);
	}
	return -EOPNOTSUPP;
}

static inline int nrc_hif_ops_start(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->start) {
		return hdev->hif_ops->start(hdev);
	}
	return -EOPNOTSUPP;
}

static inline int nrc_hif_ops_stop(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->stop) {
		hdev->hif_ops->stop(hdev);
		return 0;
	}
	return -EOPNOTSUPP;
}

static inline enum NRC_FW_STATE nrc_hif_ops_fw_state(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	return (hdev && hdev->hif_ops && hdev->hif_ops->fw_state) ?
		       hdev->hif_ops->fw_state(hdev) :
		       NRC_FW_STATE_ROM;
}

static inline int nrc_hif_ops_rx_thread_suspend(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->rx_thread_suspend)
		return hdev->hif_ops->rx_thread_suspend(hdev);
	return -EOPNOTSUPP;
}

static inline int nrc_hif_ops_rx_thread_resume(void)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	if (hdev && hdev->hif_ops && hdev->hif_ops->rx_thread_resume)
		return hdev->hif_ops->rx_thread_resume(hdev);
	return -EOPNOTSUPP;
}

#endif /* _NRC_BACKEND_HIF_INTERFACE_H */
