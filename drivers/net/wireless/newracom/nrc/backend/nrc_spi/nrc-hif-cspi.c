/*
 * Copyright (c) 2016-2019 Newracom, Inc.
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
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/spi/spi.h>
#include <linux/timekeeping.h>
#include <linux/wait.h>
#include <linux/version.h>

/* Assembly headers - compatibility for different kernel versions */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-build-config.h"
#include "nrc-hif.h"
#include "nrc-vendor.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* SPI module trace system - disabled due to module loading order */
/* Trace symbols are created by frontend module, but SPI loads first */
#if defined(CONFIG_NRC_TRACING) && 0
#include "nrc-trace.h"
#else
/* Provide empty trace function stubs for SPI module - using void* to avoid forward declaration issues */
static inline void trace_nrc_hif_rx_slot(void *priv, int dir, const char *msg)
{
}
static inline void trace_nrc_hif_tx_slot(void *priv, int dir, const char *msg)
{
}
#endif

/* Common directory headers - Interfaces */
#include "nrc-backend-hif-callback.h"
#include "nrc-backend-hif-interface.h"
#include "nrc-wim-types.h"

/* Global SPI private data for module-wide access */
struct nrc_spi_priv *g_spi_priv = NULL;
static DEFINE_SPINLOCK(g_spi_priv_lock);

/* Local module headers */
#include "nrc-hif-cspi.h"
#include "nrc-spi-device.h"
#include "nrc-spi-hif-ops.h"
#include "nrc-spi-params.h"
#include "nrc-spi-gpio.h"
#include "nrc-debug.h"

/* Forward declarations */
static inline void spi_forward_rx_packet(struct nrc_hif_device *hdev,
					 struct sk_buff *skb,
					 const char *caller);

/**
 * spi_check_core_refs - Check if core module references are valid
 * @priv: SPI private data pointer
 * @caller: calling function name for error reporting
 *
 * Returns: true if both nw and hdev are valid, false otherwise
 */
static bool spi_check_core_refs(struct nrc_spi_priv *priv, const char *caller)
{
	if (!priv) {
		ERR_SPI("SPI: %s - Invalid priv pointer", caller);
		return false;
	}

	/* Enhanced pointer validation */
	if (!priv->hdev) {
		ERR_SPI("%s: Core module hdev is NULL", caller);
		return false;
	}

	/* Check for invalid low-memory addresses */
	if ((unsigned long)priv->hdev < PAGE_SIZE) {
		ERR_SPI("%s: Core module hdev in invalid low memory (hdev=%p)",
			caller, priv->hdev);
		return false;
	}

	/* Validate kernel virtual address range */
	if (!virt_addr_valid(priv->hdev)) {
		ERR_SPI("%s: Core module hdev not in valid kernel address space (hdev=%p)",
			caller, priv->hdev);
		return false;
	}

	return true;
}

/**
 * spi_trigger_hal_event - Trigger HAL layer event
 * @priv: SPI private data
 * @event_type: Type of event to trigger
 * @caller: calling function name
 */
static void spi_trigger_hal_event(struct nrc_spi_priv *priv,
				  struct nrc_spi_event_data *event,
				  const char *caller)
{
	struct nrc_spi_event_data event_copy;

	if (!spi_check_core_refs(priv, caller)) {
		return;
	}

	/* Make a local copy to ensure event data persists through callback chain */
	event_copy = *event;
	nrc_spi_trigger_event(&event_copy);
}

static bool once;

static u8 crc7(u8 seed, u8 data)
{
	int i;
	const u8 g = 0x89;

	seed ^= data;
	for (i = 0; i < 8; i++) {
		if (seed & 0x80)
			seed ^= g;
		seed <<= 1;
	}
	return seed;
}

static u8 compute_crc7(const u8 *data, ssize_t len)
{
	int i;
	u8 crc = 0;

	for (i = 0; i < len; i++)
		crc = crc7(crc, data[i]);

	return crc >> 1;
}

static inline void spi_set_transfer(struct spi_transfer *xfer, void *tx,
				    void *rx, int len)
{
	xfer->tx_buf = tx;
	xfer->rx_buf = rx;
	xfer->len = len;
}

#ifdef CONFIG_SUPPORT_SPI_SYNC_TRANSFER
#else
static inline void spi_message_init_with_transfers(struct spi_message *m,
						   struct spi_transfer *xfers,
						   unsigned int num_xfers)
{
	unsigned int i;

	spi_message_init(m);
	for (i = 0; i < num_xfers; ++i)
		spi_message_add_tail(&xfers[i], m);
}

static inline int spi_sync_transfer(struct spi_device *spi,
				    struct spi_transfer *xfers,
				    unsigned int num_xfers)
{
	struct spi_message msg;

	spi_message_init_with_transfers(&msg, xfers, num_xfers);

	return spi_sync(spi, &msg);
}
#endif

int _c_spi_write_dummy(struct spi_device *spi)
{
	struct spi_transfer xfer[2] = {
		{0},
	};
	u32 dummy = 0xffffffff;
	u8 tx[8];
#ifndef CONFIG_SPI_HALF_DUPLEX
	u8 rx[8];
#endif
	ssize_t status;

	memset(tx, 0xff, sizeof(tx));
#ifndef CONFIG_SPI_HALF_DUPLEX
	spi_set_transfer(&xfer[0], tx, rx, 8);
#else
	spi_set_transfer(&xfer[0], tx, NULL, 8);
#endif
	dummy = 0xffffffff;
	spi_set_transfer(&xfer[1], &dummy, NULL, sizeof(dummy));

	status = spi_sync_transfer(spi, xfer, 2);

	return 0;
}

static int _c_spi_read_regs(struct spi_device *spi, u8 addr, u8 *buf,
			    ssize_t size)
{
	struct spi_transfer xfer[4] = {
		{0},
	};
	u32 cmd, crc, dummy;
	u8 tx[8];
#ifndef CONFIG_SPI_HALF_DUPLEX
	u8 rx[8];
#endif
	ssize_t status;
	int arr_len;
	struct nrc_spi_priv *priv = spi_get_drvdata(spi);

	if (size == 0 || buf == NULL || size > (1024 * 1024)) /* Max 1MB */
		return -EINVAL;

	cmd = C_SPI_READ | C_SPI_ADDR(addr);
	if (size > 1)
		cmd |= C_SPI_BURST | C_SPI_LEN(size);
	else
		cmd |= C_SPI_FIXED | C_SPI_LEN(0);

	put_unaligned_be32(cmd, (u32 *)tx);
	tx[4] = (compute_crc7(tx, 4) << 1) | 0x1;

#ifndef CONFIG_SPI_HALF_DUPLEX
	spi_set_transfer(&xfer[0], tx, rx, 8);
#else
	spi_set_transfer(&xfer[0], tx, NULL, 8);
#endif
	spi_set_transfer(&xfer[1], NULL, buf, size);
	spi_set_transfer(&xfer[2], NULL, &crc, sizeof(crc));

	dummy = 0xffffffff;
	if (size > 1)
		spi_set_transfer(&xfer[3], &dummy, NULL, sizeof(dummy));
	else
		spi_set_transfer(&xfer[1], &dummy, NULL, sizeof(dummy));

	arr_len = (size > 1) ? ARRAY_SIZE(xfer) : 2;
	status = spi_sync_transfer(spi, xfer, arr_len);
	if (status < 0) {
		ERR_SPI("[%s] reading spi failed(%zd).", __func__, status);
		return status;
	}

#ifndef CONFIG_SPI_HALF_DUPLEX
	if (rx[7] != C_SPI_ACK) {
		if (priv && priv->hdev && !NRC_PS_IS_ASLEEP(priv->hdev) &&
		    !NRC_PS_IS_SLEEPING(priv->hdev)) {
			WARN_ON_ONCE(1);
			ERR_SPI("SPI ACK is invalid (PS state: %s)",
				NRC_PS_STATE_STR(priv->hdev));
		}
		/* During sleep polling (SLEEPING state), invalid ACK is expected */
		return -EIO;
	}
#endif

#ifndef CONFIG_SPI_HALF_DUPLEX
	if (size == 1)
		buf[0] = rx[6];
#else
	/* Half duplex can't handle size 1 and 2 */
	if (size <= 2)
		INFO("[%s] Half duplex can't handle size 1 and 2...", __func__);
#endif
	return 0;
}

static int _c_spi_write_reg(struct spi_device *spi, u8 addr, u8 data)
{
	struct spi_transfer xfer[2] = {
		{0},
	};
	u32 cmd, dummy;
	u8 tx[8];
#ifndef CONFIG_SPI_HALF_DUPLEX
	u8 rx[8];
#endif
	ssize_t status;
	struct nrc_spi_priv *priv = spi_get_drvdata(spi);

	cmd = C_SPI_WRITE | C_SPI_FIXED | C_SPI_ADDR(addr) | C_SPI_WDATA(data);
	put_unaligned_be32(cmd, (u32 *)tx);
	tx[4] = (compute_crc7(tx, 4) << 1) | 0x1;
#ifndef CONFIG_SPI_HALF_DUPLEX
	spi_set_transfer(&xfer[0], tx, rx, 8);
#else
	spi_set_transfer(&xfer[0], tx, NULL, 8);
#endif
	dummy = 0xffffffff;
	spi_set_transfer(&xfer[1], &dummy, NULL, sizeof(dummy));

	status = spi_sync_transfer(spi, xfer, 2);
	if (status < 0) {
		ERR_SPI("[%s] writing spi failed(%zd).", __func__, status);
		return status;
	}

#ifndef CONFIG_SPI_HALF_DUPLEX
	/* In case of spi reset, skip a process for confirming spi ack */
	if (C_SPI_WDATA(data) != 0xC8) {
		if (rx[7] != C_SPI_ACK) {
			WARN_ON_ONCE(1);
			if (priv && priv->hdev) {
				ERR_SPI("SPI ACK is invalid (PS state: %s)",
					NRC_PS_STATE_STR(priv->hdev));
			} else {
				ERR_SPI("SPI ACK is invalid");
			}
			return -EIO;
		}
	}
#endif

	return 0;
}

static ssize_t _c_spi_read(struct spi_device *spi, u8 *buf, ssize_t size)
{
	struct spi_transfer xfer[4] = {
		{0},
	};
	u32 cmd, crc, dummy;
	u8 tx[8];
#ifndef CONFIG_SPI_HALF_DUPLEX
	u8 rx[8];
#endif
	ssize_t status;
	u8 *aligned_buf = NULL;
	u8 *original_buf = buf;
	u8 *aligned_buf_start = NULL;

	if (size == 0 || buf == NULL || size > (1024 * 1024)) /* Max 1MB */
		return -EINVAL;

	if ((uintptr_t)buf % 4 != 0) {
		/* Check for integer overflow in allocation size */
		if (size > SIZE_MAX - 4) {
			return -EINVAL;
		}

		aligned_buf = (u8 *)kmalloc(
			size + 4,
			GFP_KERNEL); // Allocate memory in kernel space
		if (!aligned_buf) {
			return -ENOMEM;
		}

		// Align the buffer to 4-byte boundary
		aligned_buf_start = (u8 *)(((uintptr_t)aligned_buf + 3) &
					   ~3); // Align to 4-byte boundary
		buf = aligned_buf_start; // Use the aligned buffer for SPI transfer
	}

	cmd = C_SPI_READ | C_SPI_BURST | C_SPI_FIXED;
	cmd |= C_SPI_ADDR(C_SPI_TXQ_WINDOW) | C_SPI_LEN(size);
	put_unaligned_be32(cmd, (u32 *)tx);
	tx[4] = (compute_crc7(tx, 4) << 1) | 0x1;
	tx[5] = 0xff;

#ifndef CONFIG_SPI_HALF_DUPLEX
	spi_set_transfer(&xfer[0], tx, rx, 8);
#else
	spi_set_transfer(&xfer[0], tx, NULL, 8);
#endif
	spi_set_transfer(&xfer[1], NULL, buf, size);
	spi_set_transfer(&xfer[2], NULL, &crc, sizeof(crc));

	dummy = 0xffffffff;
	spi_set_transfer(&xfer[3], &dummy, NULL, sizeof(dummy));
	status = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));
	if (status < 0) {
		ERR_SPI("[%s] reading spi failed(%zd).", __func__, status);
		goto error_cleanup;
	}

#ifndef CONFIG_SPI_HALF_DUPLEX
	if (rx[7] != C_SPI_ACK) {
		WARN_ON_ONCE(1);
		ERR_SPI("SPI ACK is invalid");
		status = -EIO;
		goto error_cleanup;
	}
#endif

	/* Copy data from aligned buffer back to original buffer if needed */
	if (aligned_buf && aligned_buf_start) {
		memcpy(original_buf, aligned_buf_start, size);
		kfree(aligned_buf);
	}

	return size;

error_cleanup:
	/* Clean up allocated buffer on error */
	if (aligned_buf) {
		kfree(aligned_buf);
	}
	return status;
}

static ssize_t _c_spi_write(struct spi_device *spi, u8 *buf, ssize_t size)
{
	struct spi_transfer xfer[4] = {
		{0},
	};
	u32 cmd, dummy = 0xffffffff;
	u8 tx[8];
#ifndef CONFIG_SPI_HALF_DUPLEX
	u8 rx[8];
#endif
	ssize_t status;
	u8 *aligned_buf = NULL; // Declare aligned buffer pointer
	u8 *aligned_buf_start = NULL;

	if (size == 0 || buf == NULL || size > (1024 * 1024)) /* Max 1MB */
		return -EINVAL;

	if ((uintptr_t)buf % 4 != 0) {
		/* Check for integer overflow in allocation size */
		if (size > SIZE_MAX - 4) {
			return -EINVAL;
		}

		aligned_buf = (u8 *)kmalloc(
			size + 4,
			GFP_KERNEL); // Allocate memory in kernel space
		if (!aligned_buf) {
			return -ENOMEM;
		}

		// Align the buffer to 4-byte boundary
		aligned_buf_start = (u8 *)(((uintptr_t)aligned_buf + 3) &
					   ~3); // Align to 4-byte boundary
		memcpy(aligned_buf_start, buf, size);
		buf = aligned_buf_start; // Use the aligned buffer for SPI transfer
	}

	cmd = C_SPI_WRITE | C_SPI_BURST | C_SPI_FIXED;
	cmd |= C_SPI_ADDR(C_SPI_RXQ_WINDOW) | C_SPI_LEN(size);
	put_unaligned_be32(cmd, (u32 *)tx);
	tx[4] = (compute_crc7(tx, 4) << 1) | 0x1;
	tx[5] = 0xff;

#ifndef CONFIG_SPI_HALF_DUPLEX
	spi_set_transfer(&xfer[0], tx, rx, 8);
#else
	spi_set_transfer(&xfer[0], tx, NULL, 8);
#endif
	spi_set_transfer(&xfer[1], buf, NULL, size);
	spi_set_transfer(&xfer[2], &dummy, NULL, sizeof(dummy));
	dummy = 0xffffffff;
	spi_set_transfer(&xfer[3], &dummy, NULL, sizeof(dummy));

	status = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));
	if (status < 0) {
		ERR_SPI("[%s] writing spi failed(%zd).", __func__, status);
		goto error_cleanup;
	}

#ifndef CONFIG_SPI_HALF_DUPLEX
	if (WARN_ON_ONCE(rx[7] != C_SPI_ACK)) {
		// INFO("[%s] try to read register but SPI ACK is invalid", __func__);
		status = -EIO;
		goto error_cleanup;
	}
#endif

	/* Free the aligned buffer if it was allocated */
	if (aligned_buf) {
		kfree(aligned_buf);
	}

	return size;

error_cleanup:
	/* Clean up allocated buffer on error */
	if (aligned_buf) {
		kfree(aligned_buf);
	}
	return status;
}

/* ops structure removed - using direct function calls */

int c_spi_read_regs(struct spi_device *spi, u8 addr, u8 *buf, ssize_t size)
{
	return _c_spi_read_regs(spi, addr, buf, size);
}

int c_spi_write_reg(struct spi_device *spi, u8 addr, u8 data)
{
	return _c_spi_write_reg(spi, addr, data);
}

ssize_t c_spi_read(struct spi_device *spi, u8 *buf, ssize_t size)
{
	return _c_spi_read(spi, buf, size);
}

ssize_t c_spi_write(struct spi_device *spi, u8 *buf, ssize_t size)
{
	return _c_spi_write(spi, buf, size);
}

/**
 * spi_rx_skb - fetch a single hif packet from the target
 */

static struct sk_buff *spi_rx_skb(struct spi_device *spi,
				  struct nrc_spi_priv *priv,
				  struct nrc_hif_device *hdev)
{
	struct sk_buff *skb = NULL;
	struct hif *hif;
	int hif_type = -1; /* Initialize to invalid type for error cases */
	ssize_t size;
	u32 nr_slot;
	int ret;
	u32 second_length = 0;
#ifdef CONFIG_TRX_BACKOFF
	int backoff;
#endif
	static uint cnt1 = 0;
	static uint cnt2 = 0;

	/* Check if core module references are valid */
	if (!spi_check_core_refs(priv, __func__)) {
		ERR_SPI("Core module references invalid, abort RX");
		goto fail;
	}

	/* During SLEEP or WAKING, RX thread is parked - abort RX processing */
	if (atomic_read(&priv->rx_thread_parked)) {
		DBG_PS("RX thread parked (ps_state=%s), abort RX",
		       NRC_PS_STATE_STR(hdev));
		goto fail;
	}

	/* Don't call spi_update_status during WAKING - wait for FW_REQUEST IRQ first */
	if (c_spi_num_slots(hdev, RX_SLOT) == 0 && !NRC_PS_IS_WAKING(hdev))
		spi_update_status(hdev);

	if (c_spi_num_slots(hdev, RX_SLOT) < hdev->max_slot_num) {
		trace_nrc_hif_rx_slot(priv, RX_SLOT, "enable irq");
		c_spi_enable_irq(spi, true, CSPI_EIRQ_S_ENABLE);
	}

	trace_nrc_hif_rx_slot(priv, RX_SLOT, "wait rx header");

	/* Wait until at least one rx slot is non-empty */
	ret = wait_event_interruptible(priv->rx_wait,
				       ((c_spi_num_slots(hdev, RX_SLOT) > 0) ||
					kthread_should_stop() ||
					kthread_should_park()));
	if (ret < 0) {
		ERR_SPI("wait_event_interruptible error (%d)", ret);
		goto fail;
	}

	if (kthread_should_stop() || kthread_should_park()) {
		// DBG_HIF("kthread should stop or park");
		goto fail;
	}

	/* During WAKING state, only process FW_READY interrupt, not normal RX */
	if (NRC_PS_IS_WAKING(hdev)) {
		DBG_PS("PS state is WAKING, skip normal RX processing (waiting for FW_READY)");
		goto fail;
	}

#ifdef CONFIG_TRX_BACKOFF
	if (!hdev->ampdu_supported) {
		backoff = atomic_inc_return(&priv->trx_backoff);

		if ((backoff % 3) != 0) {
			DBG_HIF_RX("rx-irq: trx_backoff=%d", backoff);
			usleep_range(800, 1000);
		}
	}
#endif
	SLOT_SYNC_LOCK();

	trace_nrc_hif_rx_slot(priv, RX_SLOT, "before rx");

	if (c_spi_num_slots(hdev, RX_SLOT) > 32) {
		SLOT_SYNC_UNLOCK();
		if (cnt1++ < 10) {
			ERR_SPI("!!!!! garbage rx data");
			spi_hif_reset_rx(hdev);
		}
		ERR_SPI("rxslot:(h=%d,t=%d)", hdev->slot[RX_SLOT].head,
			hdev->slot[RX_SLOT].tail);
		goto fail;
	}
	cnt1 = 0;

	/* Allocate SKB right before reading data */
	skb = dev_alloc_skb(hdev->slot[RX_SLOT].size * hdev->max_slot_num);
	if (!skb) {
		SLOT_SYNC_UNLOCK();
		ERR_SPI("Failed to allocate RX SKB");
		goto fail;
	}

	/*
	 * For the first time, the slot should be read entirely
	 * since we cannot know the data size until the hif->len is checked.
	 * And, the current RX_SLOT size is already word aligned(456 bytes).
	 */
	DBG_SLOT("RX TAIL: %d -> %d (HEAD=%d, readable=%d->%d/%d)",
		 hdev->slot[RX_SLOT].tail, hdev->slot[RX_SLOT].tail + 1,
		 hdev->slot[RX_SLOT].head, c_spi_num_slots(hdev, RX_SLOT),
		 c_spi_num_slots(hdev, RX_SLOT) - 1, hdev->slot[RX_SLOT].count);
	hdev->slot[RX_SLOT].tail++;
	size = c_spi_read(spi, skb->data, hdev->slot[RX_SLOT].size);
	SLOT_SYNC_UNLOCK();
	if (size < 0) {
		hdev->slot[RX_SLOT].tail--;
		ERR_SPI("Failed to read first slot");
		goto fail;
	}

	/* Calculate how many more slot to read for this hif packet */
	hif = (void *)skb->data;

	if (hif->type >= HIF_TYPE_MAX || hif->len == 0) {
		ERR_SPI("rxslot:(h=%d,t=%d)", hdev->slot[RX_SLOT].head,
			hdev->slot[RX_SLOT].tail);
		print_hex_dump(KERN_DEBUG, "rxskb ", DUMP_PREFIX_NONE, 16, 1,
			       skb->data, 480, false);
		spi_hif_reset_rx(hdev);
		goto fail;
	}
	hif_type = hif->type;
	NRC_SKB_TRACK_ALLOC(hdev, skb, hif_type, true, false);

	nr_slot =
		DIV_ROUND_UP(sizeof(*hif) + hif->len, hdev->slot[RX_SLOT].size);

	if (nr_slot >= hdev->max_slot_num) {
		struct sk_buff *skb2 =
			dev_alloc_skb(hdev->slot[RX_SLOT].size * (nr_slot + 1));

		if (!skb2) {
			ERR_SPI("Failed to allocate larger RX SKB (nr_slot=%d)",
				nr_slot);
			goto fail;
		}

		/* Track new larger RX SKB allocation (reallocation for larger packet) */
		NRC_SKB_TRACK_ALLOC(hdev, skb2, hif_type, true, false);

		memcpy(skb2->data, skb->data, hdev->slot[RX_SLOT].size);
		NRC_SKB_TRACK_FREE(hdev, skb, hif_type, true, false);
		skb = skb2;
		hif = (void *)skb->data;
	}

	nr_slot--;

	if (nr_slot == 0)
		goto out;

	if (c_spi_num_slots(hdev, RX_SLOT) < nr_slot)
		spi_update_status(hdev);

	/*
	 * Wait until priv->nr_rx_slot >= nr_slot or 100ms.
	 * If nr_cnt is too large, it could be a hif error.
	 */
	trace_nrc_hif_rx_slot(priv, RX_SLOT, "wait rx body");

	ret = wait_event_interruptible_timeout(
		priv->rx_wait,
		(c_spi_num_slots(hdev, RX_SLOT) >= nr_slot) ||
			kthread_should_stop() || kthread_should_park(),
		msecs_to_jiffies(100));

	if (ret == 0) { /* Timeout */
		ERR_HIF("wait_event_interruptible timeout (nr_slot:%d gap:%d)",
			nr_slot, c_spi_num_slots(hdev, RX_SLOT));
		goto fail;
	}
	if (ret < 0) {
		ERR_HIF("wait_event_interruptible error (%d)", ret);
		goto fail;
	}

	if (kthread_should_stop() || kthread_should_park()) {
		ERR_HIF("kthread should stop or park");
		goto fail;
	}

	DBG_SLOT("RX TAIL: %d -> %d (HEAD=%d, readable=%d->%d/%d)",
		 hdev->slot[RX_SLOT].tail, hdev->slot[RX_SLOT].tail + nr_slot,
		 hdev->slot[RX_SLOT].head, c_spi_num_slots(hdev, RX_SLOT),
		 c_spi_num_slots(hdev, RX_SLOT) - nr_slot,
		 hdev->slot[RX_SLOT].count);
	hdev->slot[RX_SLOT].tail += nr_slot;

	second_length = hif->len + sizeof(*hif) - hdev->slot[RX_SLOT].size;
	/*
	 * If it's necessary to read more data over other slots,
	 * the size to read must be a multiple of 4.
	 * because the HSPI HW stores the data length as a word unit.
	 */
	if (second_length & 0x3) {
		second_length = (second_length + 4) & 0xFFFFFFFC;
	}

	SLOT_SYNC_LOCK();
	if (c_spi_num_slots(hdev, RX_SLOT) > 32) {
		SLOT_SYNC_UNLOCK();
		// if (cnt2++ < 10) {
		// 	ERR_SPI("@@@@@@ garbage rx data");
		// }
		spi_hif_reset_rx(hdev);
		ERR_HIF("rxslot:(h=%d,t=%d)", hdev->slot[RX_SLOT].head,
			hdev->slot[RX_SLOT].tail);
		goto fail;
	}
	cnt2 = 0;
	size = c_spi_read(spi, skb->data + hdev->slot[RX_SLOT].size,
			  second_length);
	SLOT_SYNC_UNLOCK();

	if (size < 0) {
		ERR_HIF("Failed to read second length");
		goto fail;
	}

out:
	trace_nrc_hif_rx_slot(priv, RX_SLOT, "after rx out");

	skb_put(skb, sizeof(*hif) + hif->len);
	return skb;

fail:
	trace_nrc_hif_rx_slot(priv, RX_SLOT, "after rx fail");
	/* count_only=true to suppress NULL SKB warning, but still count error */
	NRC_SKB_TRACK_FREE(hdev, skb, hif_type, true, false);
	return NULL;
}

/* Use this function in other function */
int spi_read_sys_reg(struct spi_device *spi, struct spi_sys_reg *sys)
{
	int ret;

	ret = c_spi_read_regs(spi, C_SPI_SYS_REG, (void *)sys,
			      sizeof(struct spi_sys_reg));

	if (ret) {
		ERR_SPI("Fail to c_spi_read_regs");
		return -1;
	}

	sys->chip_id = be16_to_cpu(sys->chip_id);
	sys->modem_id = be32_to_cpu(sys->modem_id);
	sys->sw_id = be32_to_cpu(sys->sw_id);
	sys->board_id = be32_to_cpu(sys->board_id);

	return 0;
}

/* Credit queue management moved to HAL module */

static void spi_credit_skb(struct spi_device *spi, struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = spi_get_drvdata(spi);
	struct sk_buff *skb;
	struct hif *hif;
	struct wim *wim;
	struct wim_credit_report *cr;
	u8 *p;
	int i;
	int size = sizeof(*hif) + sizeof(*wim) + sizeof(*cr);

	/* Check if core module references are valid */
	if (!spi_check_core_refs(priv, __func__)) {
		ERR_SPI("Invalid core module references");
		return;
	}

	if (!once) {
		DBG_CREDIT("Credit skb not sent yet");
		once = true;
		return;
	}

	skb = dev_alloc_skb(size);
	if (!skb) {
		ERR_SPI("Failed to allocate credit skb");
		return;
	}
	NRC_SKB_TRACK_ALLOC(hdev, skb, HIF_TYPE_WIM, true, false);

	p = skb->data;
	hif = (void *)p;
	hif->type = HIF_TYPE_WIM;
	hif->subtype = HIF_WIM_SUB_EVENT;
	hif->vifindex = 0;
	hif->len = sizeof(*wim) + sizeof(*cr);

	p += sizeof(*hif);
	wim = (void *)p;
	wim->event = WIM_EVENT_CREDIT_REPORT;

	p += sizeof(*wim);
	cr = (void *)p;
	cr->h.type = WIM_TLV_AC_CREDIT_REPORT;
	cr->h.len = sizeof(struct wim_credit_report_param);

	cr->v.change_index = 0;

	/* Protect credit front/rear array access */
	{
		unsigned long flags;
		CREDIT_LOCK(hdev, flags);

		for (i = 0; i < CREDIT_QUEUE_MAX && i < ARRAY_SIZE(cr->v.ac);
		     i++) {
			u8 room = 0;

			/* Validate array bounds before access */
			if (i >= ARRAY_SIZE(hdev->credit.front) ||
			    i >= ARRAY_SIZE(hdev->credit.rear) ||
			    i >= ARRAY_SIZE(hdev->credit.credit_max)) {
				ERR_SPI("Credit index %d out of bounds", i);
				break;
			}

			if (hdev->credit.front[i] >= hdev->credit.rear[i]) {
				room = hdev->credit.front[i] -
				       hdev->credit.rear[i];
			} else {
				room = (255 - hdev->credit.rear[i]) +
				       hdev->credit.front[i];
			}

			room = min(hdev->credit.credit_max[i], room);
			cr->v.ac[i] = hdev->credit.credit_max[i] - room;

			DBG_CREDIT("credit[%d]=%d f=%d r=%d", i, cr->v.ac[i],
				   hdev->credit.front[i], hdev->credit.rear[i]);
		}

		CREDIT_UNLOCK(hdev, flags);
	}

	skb_put(skb, hif->len + sizeof(*hif));

	/* Forward credit packet to HAL using unified function */
	spi_forward_rx_packet(hdev, skb, __func__);
}

/**
 * spi_loopback - fetch a single hif packet from the target
 *					and send it back
 */

static unsigned long get_tod_usec(void)
{
#if KERNEL_VERSION(5, 0, 0) > LINUX_VERSION_CODE
	struct timeval tv;

	do_gettimeofday(&tv);
	return tv.tv_usec;
#else
	return (unsigned long)ktime_to_us(ktime_get());
#endif
}

static int spi_loopback(struct nrc_hif_device *hdev, struct spi_device *spi,
			struct nrc_spi_priv *priv, int lb_cnt)
{
	struct sk_buff *skb;
	ssize_t size;
	u32 nr_slot;
	u32 second_length = 0;
	int ret = 0;
	unsigned long t1, t2;
	struct hif *hif;

	if (priv->loopback_total_cnt == 0) {
		priv->loopback_last_jiffies = jiffies;
		priv->loopback_read_usec = 0;
		priv->loopback_write_usec = 0;
		priv->loopback_measure_cnt = 0;
	}

	skb = dev_alloc_skb(hdev->slot[RX_SLOT].size * lb_cnt);
	if (!skb) {
		ERR_SPI("Failed to allocate credit skb");
		goto end;
	}

	t1 = get_tod_usec();

	/* Wait until at least one rx slot is non-empty */
	ret = wait_event_interruptible(
		priv->rx_wait, (c_spi_num_slots(hdev, RX_SLOT) >= lb_cnt ||
				kthread_should_stop()));

	if (ret < 0)
		goto end;

	if (kthread_should_stop())
		goto end;

	DBG_SLOT("RX TAIL: %d -> %d (HEAD=%d, readable=%d->%d/%d)",
		 hdev->slot[RX_SLOT].tail, hdev->slot[RX_SLOT].tail + 1,
		 hdev->slot[RX_SLOT].head, c_spi_num_slots(hdev, RX_SLOT),
		 c_spi_num_slots(hdev, RX_SLOT) - 1, hdev->slot[RX_SLOT].count);
	hdev->slot[RX_SLOT].tail++;
	size = c_spi_read(spi, skb->data, hdev->slot[RX_SLOT].size + 4);
	if (size < 0) {
		hdev->slot[RX_SLOT].tail--;
		goto end;
	}

	hif = (void *)skb->data;

	nr_slot =
		DIV_ROUND_UP(sizeof(*hif) + hif->len, hdev->slot[RX_SLOT].size);

	t2 = get_tod_usec();

	if (t2 > t1)
		priv->loopback_read_usec += (t2 - t1);

	t1 = get_tod_usec();

	nr_slot--;
	if (nr_slot == 0)
		goto loopback_tx;

	DBG_SLOT("RX TAIL: %d -> %d (HEAD=%d, readable=%d->%d/%d)",
		 hdev->slot[RX_SLOT].tail, hdev->slot[RX_SLOT].tail + nr_slot,
		 hdev->slot[RX_SLOT].head, c_spi_num_slots(hdev, RX_SLOT),
		 c_spi_num_slots(hdev, RX_SLOT) - nr_slot,
		 hdev->slot[RX_SLOT].count);
	hdev->slot[RX_SLOT].tail += nr_slot;
	second_length = hif->len + sizeof(*hif) - hdev->slot[RX_SLOT].size;
	/* README: align with 4bytes dummy data */
	second_length = (second_length + 4) & 0xFFFFFFFC;
	size = c_spi_read(spi, skb->data + hdev->slot[RX_SLOT].size,
			  second_length);

loopback_tx:
	schedule_delayed_work(&priv->work, msecs_to_jiffies(5));
	ret = wait_event_interruptible(
		priv->tx_wait, c_spi_num_slots(hdev, TX_SLOT) >= lb_cnt);
	if (ret < 0)
		goto end;

	cancel_delayed_work_sync(&priv->work);
	hdev->slot[TX_SLOT].tail += lb_cnt;

	ret = c_spi_write(priv->spi, skb->data,
			  (hdev->slot[TX_SLOT].size * lb_cnt));
	if (ret < 0)
		goto end;

	t2 = get_tod_usec();

	if (t2 > t1)
		priv->loopback_write_usec += (t2 - t1);

	dev_kfree_skb(skb);
	priv->loopback_total_cnt += ((hdev->slot[TX_SLOT].size * lb_cnt) * 2);
	priv->loopback_measure_cnt++;
	if (time_after(jiffies, (priv->loopback_last_jiffies +
				 msecs_to_jiffies(1000)))) {
		unsigned long kilo_bits;

		kilo_bits =
			((priv->loopback_total_cnt - priv->loopback_prev_cnt) *
			 8);
		kilo_bits = (kilo_bits / 1024);
		priv->loopback_read_usec /= priv->loopback_measure_cnt;
		priv->loopback_write_usec /= priv->loopback_measure_cnt;
		DBG_HIF("loopback: throughput=%d kbps @ %d Hz", kilo_bits,
			spi->max_speed_hz);

		priv->loopback_last_jiffies = jiffies;
		priv->loopback_prev_cnt = priv->loopback_total_cnt;
		priv->loopback_measure_cnt = 0;
	}
end:
	if (skb)
		dev_kfree_skb(skb);

	return ret;
}

/**
 * spi_forward_rx_packet() - Forward RX packet to HAL layer
 * @priv: SPI private structure
 * @skb: Socket buffer containing the packet
 * @caller: Caller function name for debugging
 *
 * Forwards the received packet to HAL layer via callback chain.
 * If core refs are not ready, drops the packet.
 */
static inline void spi_forward_rx_packet(struct nrc_hif_device *hdev,
					 struct sk_buff *skb,
					 const char *caller)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct nrc_spi_event_data rx_event = {.type = NRC_BACKEND_EVT_RX_READY,
					      .data = skb,
					      .data_len = skb->len};

	if (spi_check_core_refs(priv, caller)) {
		spi_trigger_hal_event(priv, &rx_event, caller);
	} else {
		struct hif *hif = (struct hif *)skb->data;

		ERR_SPI("[RX] Core refs not ready, dropping packet (%s)",
			caller);
		/* Error drop: use tracking free to match allocation */
		NRC_SKB_TRACK_FREE(hdev, skb, hif->type, true, false);
	}
}

/**
 * spi_rx_thread
 *
 */
int spi_rx_thread(void *data)
{
	struct nrc_hif_device *hdev = data;
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();
	struct sk_buff *skb;
	struct hif *hif;
	int ret;

	while (!kthread_should_stop()) {
		if (hdev->params->loopback) {
			ret = spi_loopback(hdev, spi, priv,
					   hdev->params->lb_count);
			if (ret <= 0)
				ERR_SPI("loopback error: %d", ret);
			continue;
		}

		if (!kthread_should_park()) {
			skb = spi_rx_skb(spi, priv, hdev);
			if (!skb)
				continue;

			hif = (void *)skb->data;
			DBG_RX("SPI HIF type=%s(%u) subtype=%s(%u) len=%u",
			       nrc_hif_type_str(hif->type), hif->type,
			       nrc_hif_subtype_str(hif->type, hif->subtype),
			       hif->subtype, hif->len);

			/* Note: suspend check removed - kthread_should_park() is sufficient
			 * RX thread continues running until kthread_park() is called, so packets
			 * received during suspend flag set should still be forwarded to prevent loss.
			 * The thread will naturally park on next loop iteration.
			 */
			/* Forward packet to HAL via callback chain */
			spi_forward_rx_packet(hdev, skb, __func__);
		} else {
			// DBG_HIF("[RX] spi_rx_thread parked.");
			atomic_set(&priv->rx_thread_parked, 1);
			kthread_parkme();
			atomic_set(&priv->rx_thread_parked, 0);
			/*
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			*/
		}
	}
	return 0;
}

/**
 * spi_eirq_status_str - Convert EIRQ status to text representation
 * @status: EIRQ status value
 *
 * Returns: String representation of EIRQ status bits
 */
static const char *spi_eirq_status_str(u8 status)
{
	static char buf[32];
	char *p = buf;
	int len = 0;

	/* Build compact string with active bits */
	if (status & EIRQ_STATUS_DEVICE_SLEEP)
		len += snprintf(p + len, sizeof(buf) - len, "SLEEP ");
	if (status & EIRQ_STATUS_DEVICE_READY)
		len += snprintf(p + len, sizeof(buf) - len, "READY ");
	if (status & EIRQ_STATUS_RXQUE_EIRQ)
		len += snprintf(p + len, sizeof(buf) - len, "RX ");
	if (status & EIRQ_STATUS_TXQUE_EIRQ)
		len += snprintf(p + len, sizeof(buf) - len, "TX ");
	if (status == EIRQ_STATUS_DEVICE_ROM)
		len += snprintf(p + len, sizeof(buf) - len, "ROM ");

	/* Remove trailing space */
	if (len > 0 && buf[len - 1] == ' ')
		buf[len - 1] = '\0';
	else if (len == 0)
		snprintf(buf, sizeof(buf), "NONE");

	return buf;
}

/**
 * spi_eirq_mode_str - Convert EIRQ mode to text representation
 * @mode: EIRQ mode value
 *
 * Returns: String representation of EIRQ mode
 */
static const char *spi_eirq_mode_str(u8 mode)
{
	/* CSPI_EIRQ_MODE = 0x05 is the standard mode */
	if (mode == CSPI_EIRQ_MODE)
		return "CSPI";
	else if (mode == 0)
		return "OFF";
	else
		return "?";
}

/**
 * spi_target_noti_str - Convert target notification msg[3] to text
 * @msg3: msg[3] value (can be bitmask of multiple notifications)
 *
 * Returns: String representation of all set notification bits
 */
static const char *spi_target_noti_str(u32 msg3)
{
	static char buf[128];
	int len = 0;

	if (msg3 == 0)
		return "-";

	/* Check each known notification bit/value */
	if ((msg3 & 0xFFFF) == TARGET_NOTI_REQUEST_FW_DOWNLOAD)
		len += snprintf(buf + len, sizeof(buf) - len, "FW_DOWNLOAD ");
	if ((msg3 & 0xFFFF) == TARGET_NOTI_FW_READY_FROM_PS)
		len += snprintf(buf + len, sizeof(buf) - len, "FW_READY_PS ");
	if ((msg3 & 0xFFFF) == TARGET_NOTI_FAILED_TO_ENTER_PS)
		len += snprintf(buf + len, sizeof(buf) - len, "FAIL_ENTER_PS ");
	if ((msg3 & 0xFFFF) == TARGET_NOTI_FW_ENTER_TO_PS)
		len += snprintf(buf + len, sizeof(buf) - len, "FW_ENTER_PS ");
	if ((msg3 & 0xFFFF) == TARGET_NOTI_FW_READY_FROM_WDT)
		len += snprintf(buf + len, sizeof(buf) - len, "FW_READY_WDT ");
	if ((msg3 & 0xFFFF) == TARGET_NOTI_BEACON_UPDATED)
		len += snprintf(buf + len, sizeof(buf) - len, "BEACON_UPD ");

	/* Show upper 16 bits if present */
	if ((msg3 >> 16) != 0)
		len += snprintf(buf + len, sizeof(buf) - len, "[upper:0x%04X] ",
				(msg3 >> 16));

	/* Remove trailing space */
	if (len > 0 && buf[len - 1] == ' ')
		buf[len - 1] = '\0';
	else if (len == 0)
		snprintf(buf, sizeof(buf), "?");

	return buf;
}

int spi_read_status(struct spi_device *spi)
{
	struct spi_status_reg status;
	int ret;
	//struct spi_status_reg *priv = &status;

	ret = c_spi_read_regs(spi, C_SPI_EIRQ_MODE, (void *)&status,
			      sizeof(status));
	if (ret == 0) {
		DBG_BUS("[spi_read_status] status:0x%02x mode:0x%02x enable:0x%02x msg[3]:0x%08X",
			status.eirq.status, status.eirq.mode,
			status.eirq.enable, status.msg[3]);
	}
	//spi_print_status(priv);

	return ret;
}

/**
 * spi_process_device_status - Process device status and handle special conditions
 * @hdev: NRC HIF device
 * @spi: SPI device
 * @priv: SPI private data
 * @status: Status register data
 * @debug: Debug structure
 *
 * Returns: 0 to continue with normal processing, 1 to skip slot/credit updates
 */
static int spi_process_device_status(struct nrc_hif_device *hdev,
				     struct spi_device *spi,
				     struct nrc_spi_priv *priv,
				     struct spi_status_reg *status,
				     struct nrc_debug *debug)
{
	u32 target_noti;
	struct nrc_spi_event_data event;

	if ((status->eirq.status & EIRQ_STATUS_DEVICE_READY) &&
	    ((status->msg[3] & 0xffff) == TARGET_NOTI_FAILED_TO_ENTER_PS))
		goto DEVICE_READY;

	/* update */
	if (status->eirq.status & EIRQ_STATUS_DEVICE_SLEEP) {
		if ((status->msg[3] & 0xffff) ==
		    TARGET_NOTI_REQUEST_FW_DOWNLOAD) {
			/*
			 * SPI communication is working, which means device is already awake.
			 * This can happen when:
			 * 1. Firmware crashed/reset during wakeup sequence
			 * 2. FW sends FW_DOWNLOAD instead of FW_READY_FROM_PS
			 *
			 * Solution: Treat as FW_READY_FROM_WDT (firmware recovered)
			 * instead of ignoring, since SPI is clearly functional.
			 */
			if (NRC_PS_IS_WAKING(hdev)) {
				ERR_PS("FW DOWNLOAD during wakeup - treating as firmware recovery (SPI is working)");

				/* Trigger FW_READY_FROM_WDT event to complete wakeup */
				{
					struct nrc_spi_event_data event = {
						.type = NRC_BACKEND_EVT_TARGET_NOTI_FW_READY_FROM_WDT,
					};
					nrc_spi_trigger_event(&event);
				}
			} else {
				/* Normal SLEEP state - FW download request */
				ERR_PS("FW DOWNLOAD request during SLEEP drv=%s ps=%s status:0x%02x msg[3]=0x%08X",
				       NRC_DRV_STATE_STR(hdev),
				       NRC_PS_STATE_STR(hdev),
				       status->eirq.status, status->msg[3]);
			}
			/* Don't update slot and credit in either case */
			return 1;
		}
	}

	/* 7292 : update, 7393/7394 : check WDT/FWDW and update */
	if (status->eirq.status == EIRQ_STATUS_DEVICE_ROM) {
		if (priv->hw.sys.chip_id == 0x7394) {
			/*
			 * Device is in ROM mode - need to enable IRQ for target
			 * to proceed with WDT notification sequence.
			 *
			 * WDT sequence on target (nrc_ps_force_eirq_and_wait):
			 * 1. Target raises GPIO30 (EIRQ) to wake host
			 * 2. Target waits for host to set RegHIF_DEVICE_HST_STATS & BIT1
			 * 3. Host must call c_spi_enable_irq to set this bit
			 * 4. Then target calls system_notify_host_msg(WDT_EXPIRED)
			 *
			 * If we don't enable IRQ here, target keeps retrying ("RETRY EIRQ")
			 * and never sends the WDT_EXPIRED notification.
			 */
			c_spi_enable_irq(
				spi, false,
				CSPI_EIRQ_A_ENABLE); /* cleanup shadow reg */
			c_spi_enable_irq(spi, true, CSPI_EIRQ_A_ENABLE);

			if ((status->msg[3] & 0xFFFF) ==
			    TARGET_NOTI_WDT_EXPIRED) {
				WARN_HIF("WDT expired: msg=0x%X status=0x%X",
					 status->msg[3], status->eirq.status);
				return 1; /* skip updates */
			}
		}

		/*
		* This part handles the exception case where the firmware is reset during PS operation.
		* It is equivalent to the download handling logic below.
		*/
		if ((status->msg[3] & 0xFFFF) ==
		    TARGET_NOTI_REQUEST_FW_DOWNLOAD) {
			{
				struct nrc_spi_event_data event = {
					.type = NRC_BACKEND_EVT_TARGET_NOTI_REQUEST_FW_DOWNLOAD,
				};
				nrc_spi_trigger_event(&event);
			}
			/* don't update slot and credit */
			return 1; /* skip updates */
		}
		return 0; /* continue with normal update */
	}

DEVICE_READY:
	/* Device Ready after FW download, WDT Reset, or Deep sleep */
	if (status->eirq.status & EIRQ_STATUS_DEVICE_READY) {
		target_noti = status->msg[3] & 0xffff;

		/*
		 * Unpark RX thread when device is fully ready, BUT:
		 * - Skip for REQUEST_FW_DOWNLOAD (0xDC): Target is in WAKING state,
		 *   FW download in progress, not ready for normal SPI communication
		 * - For FW_READY_FROM_PS (0xEC): Device fully awake, ready for operation
		 * - For FW_READY_FROM_WDT: Device reset complete, ready for operation
		 */
		if (target_noti != TARGET_NOTI_REQUEST_FW_DOWNLOAD) {
			if (priv->kthread &&
			    atomic_read(&priv->rx_thread_parked)) {
				kthread_unpark(priv->kthread);
				atomic_set(&priv->rx_thread_parked, 0);
				DBG_PS("RX thread unparked on DEVICE_READY (noti=0x%04X)",
				       target_noti);
			}
		} else {
			DBG_PS("DEVICE_READY with REQUEST_FW_DOWNLOAD (0xDC)");
		}

		if (nrc_spi_target_noti_to_event(target_noti, &event.type)) {
			spi_trigger_hal_event(priv, &event, __func__);
		} else if (target_noti != 0) {
			/* Unknown non-zero target notification - log error */
			ERR_HIF("Unknown TARGET_NOTI: 0x%04X", target_noti);
		}

		/* Handle specific target notifications in SPI before forwarding to HAL */
		switch (status->msg[3] & 0xffff) {
		case TARGET_NOTI_BEACON_UPDATED:
			debug->g_nrc_beacon_updated++;
			//DBG_STATE("TARGET_NOTI_BEACON_UPDATED");
			break;
		case TARGET_NOTI_WDT_EXPIRED:
			/*
			 * WDT expired - target is rebooting.
			 * HAL callback will handle:
			 * 1. If drv_state == PS: set fw_state to ACTIVE
			 * 2. Set drv_state = REBOOT
			 * 3. Reset slot/credit (spi_config_fw equivalent)
			 * 4. WIM cleanup
			 * 5. Forward to frontend for connection_loss if needed
			 */
			WARN_HIF("TARGET_NOTI_WDT_EXPIRED - target rebooting");
			break;
		case TARGET_NOTI_FW_READY_FROM_PS:
			spi_hif_rx_thread_resume(hdev);
			/* IRQ handler already logged FW_READY_FROM_PS */
			return 0; /* continue with normal update */
		case TARGET_NOTI_FAILED_TO_ENTER_PS:
			DBG_PS("Wake-up by FAILED_TO_ENTER_PS");
			return 0; /* continue with normal update */
		case TARGET_NOTI_FW_READY_FROM_WDT:
			/*
			 * FW ready after WDT - HAL callback will handle:
			 * 1. PS state to WAKE
			 * 2. Reset slot/credit
			 * 3. Set drv_state = RUNNING
			 * 4. Forward to frontend for ieee80211_restart_hw
			 */
			spi_hif_rx_thread_resume(hdev);
			DBG_STATE("FW ready from WDT - RX thread resumed");
			return 0; /* continue with normal update */
		case TARGET_NOTI_FW_ENTER_TO_PS:
			/*
			 * FW enters to sleep silently
			 */
			/* notify target int is received */
			c_spi_enable_irq(
				spi, true,
				CSPI_EIRQ_A_ENABLE); /* cleanup shadow reg */
			c_spi_enable_irq(spi, false, CSPI_EIRQ_A_ENABLE);
			/* FW entered PS - keep fw.state as ACTIVE (FW still running) */
			/* Note: Android PM (pm_relax) is handled in core module PS state machine */
			DBG_PS("Wake-up by FW_ENTER_TO_PS");
			break;
		}
		/* don't update slot and credit */
		return 1; /* skip updates */
	}

	return 0; /* continue with normal update */
}

int spi_update_status(struct nrc_hif_device *hdev)
{
	struct spi_device *spi = nrc_spi_get_device();
	struct nrc_spi_priv *priv = spi_get_drvdata(spi);
	struct spi_status_reg *status = &priv->hw.status;
	struct nrc_debug *debug;
	int ret, ac = 0;
	u32 rear;

	if (!spi_check_core_refs(priv, __func__) || !hdev) {
		return -EINVAL;
	}

	debug = hdev->debug;

	/* Note: Continue update_status even when RX thread is parked (SLEEP/WAKING)
	 * - Need to read status register to clear IRQ (prevent IRQ storm)
	 * - Need to detect FW_READY and other device notifications
	 * - RX packet processing will be skipped anyway (RX thread is parked)
	 */

	if (priv->hw.sys.chip_id == 0x7394) {
		if (NRC_DRV_IS_ASLEEP(hdev)) {
			c_spi_enable_irq(
				spi, false,
				CSPI_EIRQ_A_ENABLE); /* cleanup shadow reg */
			c_spi_enable_irq(spi, true, CSPI_EIRQ_A_ENABLE);
			/* Note: Android PM (pm_stay_awake) is handled in core module PS state machine */
			mdelay(10);
		}
	}

	SLOT_SYNC_LOCK();
	ret = c_spi_read_regs(spi, C_SPI_EIRQ_MODE, (void *)status,
			      sizeof(*status));
	if (ret < 0) {
		SLOT_SYNC_UNLOCK();
		return ret;
	}

	DBG_BUS("EIRQ status:0x%02x(%s) mode:0x%02x(%s) enable:0x%02x",
		status->eirq.status, spi_eirq_status_str(status->eirq.status),
		status->eirq.mode, spi_eirq_mode_str(status->eirq.mode),
		status->eirq.enable);
	DBG_BUS("     msg[3]:0x%08X(%s)", status->msg[3],
		spi_target_noti_str(status->msg[3]));

	/* Process device status and check if we should skip slot/credit updates */
	if (spi_process_device_status(hdev, spi, priv, status, debug)) {
		SLOT_SYNC_UNLOCK();
		goto done;
	}

	/* update slot according to register set by target */
	hdev->slot[TX_SLOT].count = status->rxq_status[1] & RXQ_SLOT_COUNT;
	hdev->slot[RX_SLOT].count = status->txq_status[1] & TXQ_SLOT_COUNT;

	/* Update slot heads and log only when changed */
	{
		int new_rx_head = __be32_to_cpu(status->msg[0]) & 0xffff;
		int new_tx_head = __be32_to_cpu(status->msg[0]) >> 16;

		if (hdev->slot[RX_SLOT].head != new_rx_head) {
			int old_readable = hdev->slot[RX_SLOT].head -
					   hdev->slot[RX_SLOT].tail;
			int old_head = hdev->slot[RX_SLOT].head;
			hdev->slot[RX_SLOT].head = new_rx_head;
			DBG_SLOT(
				"Update RX HEAD: %d -> %d (TAIL=%d, readable=%d->%d/%d)",
				old_head, new_rx_head, hdev->slot[RX_SLOT].tail,
				old_readable, c_spi_num_slots(hdev, RX_SLOT),
				hdev->slot[RX_SLOT].count);
		}

		if (hdev->slot[TX_SLOT].head != new_tx_head) {
			int old_avail = hdev->slot[TX_SLOT].head -
					hdev->slot[TX_SLOT].tail;
			int old_head = hdev->slot[TX_SLOT].head;
			hdev->slot[TX_SLOT].head = new_tx_head;
			DBG_SLOT(
				"Update TX HEAD: %d -> %d (TAIL=%d, avail=%d->%d/%d)",
				old_head, new_tx_head, hdev->slot[TX_SLOT].tail,
				old_avail, c_spi_num_slots(hdev, TX_SLOT),
				hdev->slot[TX_SLOT].count);
		}
	}

	trace_nrc_hif_rx_slot(priv, RX_SLOT, "update");
	trace_nrc_hif_tx_slot(priv, TX_SLOT, "update");

	if (c_spi_num_slots(hdev, TX_SLOT) > 32) {
		WARN_SPI("TX_gap:%u head:%u vs tail:%u",
			 c_spi_num_slots(hdev, TX_SLOT),
			 hdev->slot[TX_SLOT].head, hdev->slot[TX_SLOT].tail);
		if (priv->hw.sys.chip_id == 0x7394 && NRC_PS_IS_AWAKE(hdev)) {
			spi_hif_reset_tx(hdev);
		}
	}

	if (c_spi_num_slots(hdev, RX_SLOT) > 33) {
		WARN_SPI("RX_gap:%u head:%u vs tail:%u",
			 c_spi_num_slots(hdev, RX_SLOT),
			 hdev->slot[RX_SLOT].head, hdev->slot[RX_SLOT].tail);
		//hdev->slot[RX_SLOT].tail = hdev->slot[RX_SLOT].head = 0;
		if (priv->hw.sys.chip_id == 0x7394 && NRC_PS_IS_AWAKE(hdev)) {
			spi_hif_reset_rx(hdev);
		}
	}

	SLOT_SYNC_UNLOCK();

	/* no need to update credit while loopback test */
	if (hdev->params->loopback) {
		goto done;
	}

	/* Update VIF0 credit */
	rear = __be32_to_cpu(status->msg[1]);

	/* Protect credit rear array updates */
	{
		unsigned long flags;
		CREDIT_LOCK(hdev, flags);

		for (ac = 0; ac < 4 && ac < ARRAY_SIZE(hdev->credit.rear);
		     ac++) {
			hdev->credit.rear[ac] = (rear >> 8 * ac) & 0xff;
		}

		/* Update VIF1 credit */
		rear = __be32_to_cpu(status->msg[2]);
		if (hdev->hw_queues == 6) {
			for (ac = 0;
			     ac < 4 && (4 + ac) < ARRAY_SIZE(hdev->credit.rear);
			     ac++)
				hdev->credit.rear[4 + ac] =
					(rear >> 8 * ac) &
					0xff; /* Actually rear[5] is used for GP */
		} else if (hdev->hw_queues == 11) {
			for (ac = 0;
			     ac < 4 && (6 + ac) < ARRAY_SIZE(hdev->credit.rear);
			     ac++)
				hdev->credit.rear[6 + ac] = (rear >> 8 * ac) &
							    0xff;
		} else {
			ERR_SPI("Invalid queue (%d)", hdev->hw_queues);
			//BUG();
		}

		CREDIT_UNLOCK(hdev, flags);
	}

	/* For flow control debug */
	if (hdev->params->dbg_flow_control) {
		DBG_SLOT("RX slot: t_snt=%d h_rcv=%d diff=%d",
			 hdev->slot[RX_SLOT].head, hdev->slot[RX_SLOT].tail,
			 hdev->slot[RX_SLOT].head - hdev->slot[RX_SLOT].tail);

		DBG_SLOT("TX slot: t_rdy=%d h_snt=%d diff=%d",
			 hdev->slot[TX_SLOT].head, hdev->slot[TX_SLOT].tail,
			 hdev->slot[TX_SLOT].head - hdev->slot[TX_SLOT].tail);

		for (ac = 0; ac < hdev->hw_queues; ac++) {
			DBG_CREDIT("AC%d: h_snt=%d t_rcv=%d credit=%d pend=%d",
				   ac, hdev->credit.front[ac],
				   hdev->credit.rear[ac],
				   hdev->credit.tx_credit[ac],
				   hdev->credit.tx_pend[ac]);
		}
	}

	spi_credit_skb(spi, hdev);

/* To address the tendency of interrupts to occur slowly on low-spec CPUs,
   we prioritize giving the rx_thread an opportunity to process first
*/
#define EXTRA_SLOT 1
	if (c_spi_num_slots(hdev, RX_SLOT) >=
		    (hdev->max_slot_num + EXTRA_SLOT) &&
	    c_spi_num_slots(hdev, TX_SLOT) >= hdev->max_slot_num) {
		trace_nrc_hif_tx_slot(priv, TX_SLOT, "disable irq");
		trace_nrc_hif_rx_slot(priv, RX_SLOT, "disable irq");
		c_spi_enable_irq(spi, false, CSPI_EIRQ_S_ENABLE);
	}

	/* Wake up appropriate threads based on slot availability */
	if (c_spi_num_slots(hdev, RX_SLOT) > 0)
		wake_up_interruptible(&priv->rx_wait);
	if (c_spi_num_slots(hdev, TX_SLOT) > 0)
		wake_up_interruptible(&priv->tx_wait);

done:
	return 0;
}

/**
 * spi_irq_handler - Common IRQ handler for both threaded and workqueue modes
 * @hdev: HIF device structure (must not be NULL)
 *
 * Processes SPI interrupts by updating device status.
 * spi_update_status() will wake appropriate threads based on slot availability.
 * Called condition: hdev must be non-NULL and core references must be ready.
 */
static void spi_irq_handler(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();

	if (!hdev || !priv) {
		return;
	}

	DBG_BUS("%s", __func__);

	/* Update device status and wake appropriate threads */
	spi_update_status(hdev);
}

#ifdef CONFIG_SUPPORT_THREADED_IRQ
/* Threaded IRQ handler - runs in thread context */
irqreturn_t spi_irq(int irq, void *data)
{
	struct nrc_hif_device *hdev = data;

	if (hdev) {
		spi_irq_handler(hdev);
	}

	return IRQ_HANDLED;
}
#else
/* Hard IRQ handler - queues work for processing */
static irqreturn_t spi_irq(int irq, void *data)
{
	struct nrc_hif_device *hdev = data;
	struct nrc_spi_priv *priv = nrc_spi_get_priv();

	if (hdev && priv) {
		queue_work(priv->irq_wq, &priv->irq_work);
	}

	return IRQ_HANDLED;
}

/* Workqueue worker - processes interrupt in process context */
static void irq_worker(struct work_struct *work)
{
	struct nrc_spi_priv *priv =
		container_of(work, struct nrc_spi_priv, irq_work);

	DBG_BUS("%s", __func__);

	/* Check core references before proceeding */
	if (!spi_check_core_refs(priv, __func__)) {
		return;
	}

	/* Call common handler if hdev is valid */
	if (priv->hdev) {
		spi_irq_handler(priv->hdev);
	}
}
#endif

static void spi_poll_status(struct work_struct *work)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();

	if (!priv || !priv->hdev)
		return;

	/* If RX thread is parked (SLEEP/WAKING), skip work processing */
	if (atomic_read(&priv->rx_thread_parked))
		return;

	/* Update device status and wake appropriate threads */
	spi_update_status(priv->hdev);
}

int spi_poll_thread(void *data)
{
	struct nrc_hif_device *hdev = (struct nrc_hif_device *)data;
	struct nrc_spi_priv *priv = (struct nrc_spi_priv *)hdev->priv;
	int gpio = spi_gpio_irq;
	int interval = priv->polling_interval;
	int ret;

	DBG_HIF("poll_thread: gpio=%d interval=%d", gpio, interval);

	if (WARN_ON(interval <= 0))
		return -1;

	interval *= 1000;

	while (!kthread_should_stop()) {
		if (gpio < 0) {
			/* spi_irq calls spi_update_status which wakes appropriate threads */
			spi_irq(-1, hdev);
		} else {
			ret = gpio_get_value_cansleep(gpio);

			if (ret < 0)
				ERR_SPI("%s: gpio_get_value_cansleep() failed, ret=%d",
					__func__, ret);
			else if (ret == !!(CSPI_EIRQ_MODE & 1))
				spi_irq(gpio, hdev);
		}

		usleep_range(interval, interval + 100);
	}

	return 0;
}

/* spi_ops structure moved to nrc-spi-ops.c for cleaner separation */

#define MAX_ENABLE_IRQ_RETRY 3
#define MAX_ENABLE_IRQ_DELAY 5

static DEFINE_MUTEX(irq_mutex);

void c_spi_enable_irq(struct spi_device *spi, bool enable, u8 mask)
{
	int ret = 0, retry = 0;
	u8 m, e = 0x00;
	static u8 shadow = 0;
	u8 tmp;

	mutex_lock(&irq_mutex);

	if (mask == CSPI_EIRQ_A_ENABLE) {
		//printk("EIRQ ENABLE");
		if (enable) {
			m = CSPI_EIRQ_MODE;
		}
		for (retry = 0; retry < MAX_ENABLE_IRQ_RETRY; retry++) {
			ret = c_spi_write_reg(spi, C_SPI_EIRQ_MODE, m);
			if (ret) {
				DBG_HIF("enable_irq: mode write retry %d",
					retry + 1);
				mdelay(MAX_ENABLE_IRQ_DELAY);
				continue;
			}
			break;
		}
	}

	tmp = enable ? (shadow | mask) : (shadow & ~mask);
	if (tmp == shadow) {
		//printk("SKIP:(%d)", enable);
		goto skip;
	}

	for (retry = 0; retry < MAX_ENABLE_IRQ_RETRY; retry++) {
		ret = c_spi_read_regs(spi, C_SPI_EIRQ_ENABLE, &e, sizeof(u8));
		if (ret) {
			DBG_HIF("enable_irq: read retry %d", retry + 1);
			mdelay(MAX_ENABLE_IRQ_DELAY);
			continue;
		}
		break;
	}

	e = enable ? (e | mask) : (e & ~mask);

	/* uCode Wake-up --> uCode Interrupt --> Host IRQ Handler --> HIF Enabled case */

	for (retry = 0; retry < MAX_ENABLE_IRQ_RETRY; retry++) {
		ret = c_spi_write_reg(spi, C_SPI_EIRQ_ENABLE, e);
		if (ret) {
			DBG_HIF("enable_irq: enable write retry %d", retry + 1);
			mdelay(MAX_ENABLE_IRQ_DELAY);
			continue;
		}
		break;
	}

	shadow = e;
skip:
	mutex_unlock(&irq_mutex);
}

void c_spi_config(struct nrc_spi_priv *priv, struct nrc_hif_device *hdev)
{
	struct spi_sys_reg *sys = &priv->hw.sys;
	static u16 last_chip_id = 0;
	static u32 last_sw_id = 0;
	static u32 last_board_id = 0;
	bool config_changed = false;

	/* Initialize slot sizes in hdev instead of priv */
	hdev->slot[TX_SLOT].size = TX_SLOT_SIZE;
	hdev->slot[RX_SLOT].size = RX_SLOT_SIZE;

	switch (sys->chip_id) {
	case 0x7391:
	case 0x7292:
	case 0x7392:
	case 0x4791:
	case 0x5291:
	case 0x7394:
		/* These chips require software slot synchronization */
		priv->slot_sync_auto = false;
		break;
	case 0x6201:
		/* This chip supports hardware slot synchronization */
		priv->slot_sync_auto = true;
		break;
	default:
		ERR_SPI("Unknown chipset %04x", sys->chip_id);
		BUG();
	}

	/* maybe 4, 32 is for batman-adv, see hw->max_mtu in nrc-mac80211.c */
	hdev->max_slot_num =
		DIV_ROUND_UP(sizeof(struct hif) + ETH_DATA_LEN + 32,
			     hdev->slot[RX_SLOT].size);

	/* Check if configuration changed */
	if (last_chip_id != sys->chip_id || last_sw_id != sys->sw_id ||
	    last_board_id != sys->board_id) {
		config_changed = true;
		last_chip_id = sys->chip_id;
		last_sw_id = sys->sw_id;
		last_board_id = sys->board_id;
	}

	/* Print chip info only on first config or when values change */
	if (config_changed) {
		INFO("Newracom IEEE802.11 C-SPI: chipid=%04x, sw_id=%04x, board_id=%04X",
		     sys->chip_id, sys->sw_id, sys->board_id);
		if (sys->sw_id == SW_MAGIC_FOR_BOOT)
			INFO("Boot loader");
		else if (sys->sw_id == SW_MAGIC_FOR_FW)
			INFO("Firmware");
	}

	c_spi_enable_irq(priv->spi, false,
			 CSPI_EIRQ_A_ENABLE); /* cleanup shadow reg */
	c_spi_enable_irq(priv->spi, spi_gpio_irq >= 0 ? true : false,
			 CSPI_EIRQ_A_ENABLE);
}

int nrc_cspi_gpio_alloc(struct spi_device *spi)
{
#if defined(SPI_DBG)
	/* Claim gpio used for debugging */
	if (nrc_gpio_request(SPI_DBG, "nrc-spi-dgb") < 0) {
		ERR_SPI("[Error] gpio_reqeust() is failed");
		goto err;
	}
	nrc_gpio_direction_output(SPI_DBG, 1);
#endif

#if defined(ENABLE_HW_RESET)
#if defined(CONFIG_SPI_USE_DT)
	((struct nrc_spi_priv *)(spi->dev.platform_data))->reset_gpio =
		devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(((struct nrc_spi_priv *)(spi->dev.platform_data))
			   ->reset_gpio)) {
		ERR_SPI("[Error] gpio_reqeust(nrc-reset) is failed");
		goto err_dbg_irq_free;
	}
#else
	if (nrc_gpio_request(HOST_GPIO_FOR_TARGET_RST, "nrc-reset") < 0) {
		ERR_SPI("[Error] gpio_reqeust(nrc-reset) is failed");
		goto err_dbg_irq_free;
	}
	nrc_gpio_direction_output(HOST_GPIO_FOR_TARGET_RST, 1);
#endif
#endif

	/* Power save GPIO allocation moved to HAL initialization -
	 * will be handled when power_save parameter is available via nw structure */

#ifndef CONFIG_SPI_USE_DT /* spi->irq is real irq number, not gpio number by setting in dts */
	if (spi_gpio_irq >= 0) {
		/* Claim gpio used for irq */
		if (nrc_gpio_request(spi_gpio_irq, "nrc-spi-irq") < 0) {
			ERR_SPI("[Error] gpio_reqeust() is failed (%d)",
				spi_gpio_irq);
			goto err_rst_free;
		}
		nrc_gpio_direction_input(spi_gpio_irq);
	}
#endif

	return 0;

#ifndef CONFIG_SPI_USE_DT
err_rst_free:
#endif
#if defined(ENABLE_HW_RESET)
#if !defined(CONFIG_SPI_USE_DT)
	nrc_gpio_free(HOST_GPIO_FOR_TARGET_RST);
#endif
err_dbg_irq_free:
#endif
#if defined(SPI_DBG)
	nrc_gpio_free(SPI_DBG);
err:
#endif
	return -EINVAL;
}

void nrc_cspi_gpio_free(struct spi_device *spi)
{
#if defined(SPI_DBG)
	nrc_gpio_free(SPI_DBG);
#endif

#if defined(ENABLE_HW_RESET)
#if !defined(CONFIG_SPI_USE_DT)
	nrc_gpio_set_value(HOST_GPIO_FOR_TARGET_RST, 0);
	msleep(10);
	nrc_gpio_set_value(HOST_GPIO_FOR_TARGET_RST, 1);
	nrc_gpio_free(HOST_GPIO_FOR_TARGET_RST);
#endif
#endif

	/* Power save GPIO cleanup moved to HAL cleanup */

#ifndef CONFIG_SPI_USE_DT /* spi->irq is real irq number, not gpio number by setting in dts */
	if (spi_gpio_irq >= 0) {
		nrc_gpio_free(spi_gpio_irq);
	}
#endif
}

struct nrc_spi_priv *nrc_cspi_alloc(struct spi_device *dev)
{
	struct nrc_spi_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		return NULL;
	}

	dev->dev.platform_data =
		priv; /* spi_device can refer to nrc_spi_priv */
	/* some api still uses dev_get_platdata */
	priv->spi = dev;

	init_waitqueue_head(&priv->tx_wait);
	init_waitqueue_head(&priv->rx_wait);

	/* Initialize slot synchronization lock */
	mutex_init(&priv->slot_sync_lock);
	priv->slot_sync_lock_initialized = true;

#if !defined(CONFIG_SUPPORT_THREADED_IRQ)
	priv->irq_wq = create_singlethread_workqueue("nrc_cspi_irq");
	INIT_WORK(&priv->irq_work, irq_worker);
#endif
	INIT_DELAYED_WORK(&priv->work, spi_poll_status);

	priv->polling_interval = spi_polling_interval; /* from module param */
	priv->power_save_gpio_allocated = false; /* GPIO resource tracking */
	priv->power_save_gpio_number = -1; /* No GPIO allocated initially */

	return priv;
}

void nrc_cspi_free(struct nrc_spi_priv *priv)
{
	/* IRQ is freed in spi_stop() function, not here to avoid double free */

#if !defined(CONFIG_SUPPORT_THREADED_IRQ)
	flush_workqueue(priv->irq_wq);
	destroy_workqueue(priv->irq_wq);
#endif

	priv->spi->dev.platform_data = NULL;
	kfree(priv);
}

/* SPI probe, remove functions and device ID table moved to nrc-spi-init.c */

/* spi_driver structure moved to nrc-spi-init.c */

#ifndef CONFIG_SPI_USE_DT
static struct spi_board_info bi = {
	.modalias = NRC_DRIVER_NAME,
	//	.chip_select = 0,
	.mode = SPI_MODE_0,
};
#endif

#ifdef CONFIG_SPI_USE_FUNC

static int __spi_controller_match(struct device *dev, const void *data)
{
	struct spi_controller *ctlr;
	const u16 *bus_num = data;

	ctlr = container_of(dev, struct spi_controller, dev);

	if (!ctlr) {
		return 0;
	}

	return ctlr->bus_num == *bus_num;
}

static struct spi_controller *spi_busnum_to_master(u16 bus_num)
{
	struct platform_device *pdev = NULL;
	struct spi_master *master = NULL;
	struct spi_controller *ctlr = NULL;
	struct device *dev = NULL;

	pdev = platform_device_alloc("pdev", PLATFORM_DEVID_NONE);
	pdev->num_resources = 0;
	platform_device_add(pdev);

	master = spi_alloc_master(&pdev->dev, sizeof(void *));
	if (!master) {
		ERR_SPI("Error: failed to allocate SPI master device");
		platform_device_unregister(pdev);
		return NULL;
	}

	dev = class_find_device(master->dev.class, NULL, &bus_num,
				__spi_controller_match);
	if (dev) {
		ctlr = container_of(dev, struct spi_controller, dev);
		/* class_find_device takes a reference, release it */
		put_device(dev);
	}

	spi_master_put(master);
	platform_device_unregister(pdev);

	return ctlr;
}
#endif

#ifndef CONFIG_SPI_USE_DT
struct spi_device *nrc_create_spi_device(void)
{
	struct spi_master *master;
	struct spi_device *spi;

	/* Apply module parameters */
	bi.bus_num = spi_bus_num;
	bi.chip_select = spi_cs_num;
	bi.irq = (spi_gpio_irq >= 0 && spi_polling_interval <= 0) ?
			 gpio_to_irq(spi_gpio_irq) :
			 -1;
	bi.max_speed_hz = hifspeed;

	/* Find the spi master that our device is attached to */
	master = spi_busnum_to_master(spi_bus_num);
	if (!master) {
		ERR_SPI("Could not find spi master with the bus number %d.",
			spi_bus_num);
		return NULL;
	}

	/* Instantiate and add a spi device */
	spi = spi_new_device(master, &bi);
	if (!spi) {
		ERR_SPI("Failed to instantiate a new spi device.");
		return NULL;
	}
	/*
	 * In kernel version 6.8 or higher, multiple cs is supporting.
	 * Since we do not support multiple cs, we can use chip_select index 0.
	 * However, it appears that the function below may need to be used in the future.
	 * example) int cs = spi_get_chipselect(spi, 0);
	 */
	INFO("SPI Device Created (bus_num:%d, cs_num:%d, irq_num:%d, max_speed:%d",
	     spi->master->bus_num,
#if KERNEL_VERSION(6, 8, 0) <= NRC_TARGET_KERNEL_VERSION
	     spi->chip_select[0],
#else
	     spi->chip_select,
#endif
	     spi->irq, spi->max_speed_hz);
	return spi;
}
#endif

int nrc_hif_set_model_conf(struct nrc_hif_device *hdev, u16 chip_id)
{
	hdev->chip_id = chip_id;
	INFO("Configuration of H/W Dependent Setting : %04x", hdev->chip_id);

	/**
	 * Config List
	 * - hw_queues
	 * - wowlan_pattern_num
	 */
	switch (hdev->chip_id) {
	case 0x7292:
		hdev->hw_queues = 6;
		hdev->wowlan_pattern_num = 1;
		break;
	case 0x7394:
		hdev->hw_queues = 11;
		hdev->wowlan_pattern_num = 2;
		break;
	default:
		ERR_SPI("Unknown Newracom IEEE80211 chipset %04x",
			hdev->chip_id);
		BUG();
	}

	INFO("- HW_QUEUES: %d", hdev->hw_queues);
	INFO("- WoWLAN Pattern num: %d", hdev->wowlan_pattern_num);

	return 0;
}

/* Module initialization and cleanup moved to nrc-spi-init.c */

/**
 * nrc_backend_set_hal_core_refs - Set HAL core module references in backend private data
 * @hdev: pointer to struct nrc_hif_device from HAL module
 *
 * This function allows the HAL module to pass its core structures to the
 * backend module so that backend can access HAL core module data when needed.
 */
void nrc_backend_set_hal_core_refs(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv;

	if (!hdev) {
		ERR_SPI("SPI: Invalid parameters: hdev=%p", hdev);
		return;
	}

	if (!hdev->priv) {
		ERR_SPI("SPI: HIF device private data is NULL");
		return;
	}

	priv = hdev->priv;

	if (priv->hdev && priv->hdev != hdev) {
		ERR_SPI("Warning - overwriting existing hdev reference (%p -> %p)",
			priv->hdev, hdev);
	}

	priv->hdev = hdev;

	/* Synchronize SPI module parameter to nw structure */
	memcpy(hdev->params->power_save_gpio, power_save_gpio,
	       sizeof(power_save_gpio));
	// DBG_STATE("HIF device module: Power save GPIO synchronized - [%d, %d, %d]",
	// 		hdev->params->power_save_gpio[0], hdev->params->power_save_gpio[1], hdev->params->power_save_gpio[2]);

	// INFO("SPI module: Core references set - nw=%p hdev=%p", nw, hdev);
}
EXPORT_SYMBOL(nrc_backend_set_hal_core_refs);

/**
 * nrc_spi_get_priv - Get global SPI private data
 *
 * Returns: SPI private data pointer or NULL if not initialized
 */
struct nrc_spi_priv *nrc_spi_get_priv(void)
{
	struct nrc_spi_priv *priv;
	unsigned long flags;

	spin_lock_irqsave(&g_spi_priv_lock, flags);
	priv = g_spi_priv;
	spin_unlock_irqrestore(&g_spi_priv_lock, flags);

	return priv;
}

/**
 * nrc_spi_is_initialized - Check if SPI module is initialized
 *
 * Returns: true if initialized, false otherwise
 */
bool nrc_spi_is_initialized(void)
{
	bool initialized;
	unsigned long flags;

	spin_lock_irqsave(&g_spi_priv_lock, flags);
	initialized = (g_spi_priv != NULL);
	spin_unlock_irqrestore(&g_spi_priv_lock, flags);

	return initialized;
}

/**
 * nrc_spi_get_device - Get SPI device from global private data
 *
 * Returns: SPI device pointer or NULL if not initialized
 */
struct spi_device *nrc_spi_get_device(void)
{
	struct spi_device *spi = NULL;
	unsigned long flags;

	spin_lock_irqsave(&g_spi_priv_lock, flags);
	if (g_spi_priv)
		spi = g_spi_priv->spi;
	spin_unlock_irqrestore(&g_spi_priv_lock, flags);

	return spi;
}
