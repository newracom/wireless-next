/*
 *
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC SPI Hardware Interface Operations Implementation
 * Complete HIF operations structure and function implementations for SPI backend
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
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/spi/spi.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#include <linux/gpio/consumer.h>
#endif

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Common directory headers - Interfaces */
#include "nrc-backend-hif-callback.h"
#include "nrc-wim-types.h"

/* Local module headers */
#include "nrc-hif-cspi.h"
#include "nrc-spi-hif-ops.h"
#include "nrc-spi-params.h"
#include "nrc-spi-gpio.h"

/* IRQ debug macro with multi-category mask for better visibility across contexts */
#define VBS_IRQ(fmt, ...) VBS(CAT(TX) | CAT(RX) | CAT(BUS), fmt, ##__VA_ARGS__)

/* Forward declaration */
static void spi_host_irq_disable(struct nrc_hif_device *hdev);
static void spi_host_irq_enable(struct nrc_hif_device *hdev);
static void spi_hif_reset_device(struct nrc_hif_device *hdev);

/* ===========================================================================
 * Device Management Operations
 * =========================================================================== */

static int spi_hif_probe(struct nrc_hif_device *hdev)
{
	static const unsigned int windows_ms[] = NRC_PROBE_BOOT_WINDOWS_MS;
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();
	struct spi_sys_reg *sys;
	ktime_t probe_start, attempt_start;
	unsigned int window_ms;
	bool need_boot;
	int attempt;

	if (!priv) {
		ERR("SPI not initialized");
		return -EINVAL;
	}
	sys = &priv->hw.sys;

	/* ROM bootloader is required only when the host downloads firmware. */
	need_boot = (hdev->params->fw_name != NULL);

	probe_start = ktime_get();

	for (attempt = 0; attempt < ARRAY_SIZE(windows_ms); attempt++) {
		window_ms = windows_ms[attempt];

		/*
		 * Reset before each attempt after the first. A reset restarts
		 * ROM boot, so the window that follows has to be long enough
		 * for the chip to finish booting; otherwise the retry is spent
		 * interrupting the very boot it is waiting for.
		 */
		if (attempt > 0) {
			WRN("Target not ready (sw_id=0x%x status=0x%x) within %u ms, SPI reset and retry %d/%zu with a %u ms window...",
			    sys->sw_id, sys->status, windows_ms[attempt - 1],
			    attempt, ARRAY_SIZE(windows_ms) - 1, window_ms);
			spi_hif_reset_device(hdev);
		}

		attempt_start = ktime_get();

		/* Poll for readiness after reset (ROM boot if downloading FW). */
		if (spi_hif_wait_rom_boot(spi, sys, window_ms, need_boot) != 0)
			continue;

		/*
		 * Report how long the chip took, so a slow cold power-up can be
		 * told apart from a warm reset and the budget can be revisited
		 * with field measurements instead of guesswork.
		 */
		INFO("SPI probe: target ready in %lld ms (attempt %d, window %u ms, total %lld ms, rom boot %s)",
		     ktime_ms_delta(ktime_get(), attempt_start), attempt + 1,
		     window_ms, ktime_ms_delta(ktime_get(), probe_start),
		     need_boot ? "required" : "not required");

		DBG_HIF("probe: chip_id=%04x modem_id=%08x sw_id=%08x status=%d",
			sys->chip_id, sys->modem_id, sys->sw_id, sys->status);

		switch (sys->chip_id) {
		case 0x4791:
		case 0x7292:
		case 0x7392:
		case 0x7394:
			c_spi_config(priv, hdev);
			if (hdev->chip_id != sys->chip_id)
				nrc_hif_set_model_conf(hdev, sys->chip_id);
			return 0;
		default:
			ERR("Invalid target chip %04x", sys->chip_id);
			return -ENODEV;
		}
	}

	ERR_HIF("Probe failed: target not ready after %zu attempt(s), %lld ms total (budget %u ms, sw_id=0x%x status=0x%x)",
		ARRAY_SIZE(windows_ms),
		ktime_ms_delta(ktime_get(), probe_start),
		NRC_PROBE_BOOT_BUDGET_MS, sys->sw_id, sys->status);
	return -1;
}

static int spi_hif_start(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();
	struct task_struct *kthread;
	int ret = 0;

	/* Power save GPIO allocation moved to HAL layer */

	/*
	 * Set known-good initial slot values before the first status read.
	 * After FW download, nrc_hif_reset_slot_credit() zeros all slots.
	 * If the FW has already set EIRQ_STATUS_DEVICE_READY by the time
	 * we call spi_update_status() (common during restart — warm boot
	 * is fast), spi_process_device_status() returns 1 and the slot
	 * update is SKIPPED, leaving head=0, tail=0 → zero TX slots.
	 * The old "Restore last state" code made this worse by overwriting
	 * TX_SLOT.tail with msg[3] (which holds a TARGET_NOTI value, not
	 * a slot pointer, when DEVICE_READY is set).
	 * spi_reset_slots() sets the same initial values used by WDT
	 * recovery (head=32, tail=-1 → 33 available TX slots), ensuring
	 * the first WIM command can always be sent regardless of EIRQ state.
	 */
	spi_reset_slots(hdev);

	ret = spi_update_status(hdev);

	DBG_HIF("spi_hif_start: after init TX(h=%u t=%u avail=%u) "
		"RX(h=%u t=%u avail=%u) ret=%d",
		hdev->slot[TX_SLOT].head, hdev->slot[TX_SLOT].tail,
		(u16)(hdev->slot[TX_SLOT].head - hdev->slot[TX_SLOT].tail),
		hdev->slot[RX_SLOT].head, hdev->slot[RX_SLOT].tail,
		(u16)(hdev->slot[RX_SLOT].head - hdev->slot[RX_SLOT].tail),
		ret);

	/* Start rx thread */
	kthread = kthread_run(spi_rx_thread, hdev, "nrc-spi-rx");
	if (IS_ERR(kthread)) {
		ERR("kthread_run() is failed");
		priv->kthread = NULL;
		return PTR_ERR(priv->kthread);
	}
	priv->kthread = kthread;
	atomic_set(&priv->rx_thread_parked, 0);

	/* Enable interrupt or polling */
	if (priv->polling_interval > 0) {
		priv->polling_kthread =
			kthread_run(spi_poll_thread, hdev, "spi-poll");
		if (IS_ERR(priv->polling_kthread)) {
			ERR("polling kthread_run failed");
			ret = PTR_ERR(priv->polling_kthread);
			priv->polling_kthread = NULL;
			goto kill_kthread;
		}
	} else if (spi->irq >= 0) {
#ifdef CONFIG_SPI_USE_DT
		/* DT provides IRQ trigger configuration */
		unsigned long irq_flags = IRQF_ONESHOT;
#else
		/* Non-DT: specify trigger type explicitly */
		unsigned long irq_flags = IRQF_TRIGGER_HIGH | IRQF_ONESHOT;
#endif
#ifdef CONFIG_SUPPORT_THREADED_IRQ
		if (!priv->irq_requested) {
			ret = request_threaded_irq(spi->irq, NULL, spi_irq,
						   irq_flags, "nrc-spi-irq",
						   hdev);
		}
#else
		if (!priv->irq_requested) {
			ret = request_irq(spi->irq, spi_irq, irq_flags,
					  "nrc-spi-irq", hdev);
		}
#endif

		if (ret < 0) {
#ifdef CONFIG_SUPPORT_THREADED_IRQ
			ERR("request_irq() is failed");
#else
			ERR("request_threaded_irq() is failed");
#endif
			priv->irq_requested = false;
			priv->irq_dev_id = NULL;
			goto kill_kthread;
		} else {
			priv->irq_requested = true;
			priv->irq_dev_id = hdev; /* save for free_irq in remove */
		}
		/* IRQ is now enabled and stays enabled until free_irq() in spi_stop() */
	} else {
		ERR("invalid module parameters: spi_gpio_irq < 0 && spi_gpio_poll <= 0 && spi_regs_poll <= 0");
		goto kill_kthread;
	}

	c_spi_enable_irq(priv->spi, false,
			 CSPI_EIRQ_A_ENABLE); /* cleanup shadow reg */
	c_spi_enable_irq(spi, spi->irq >= 0 ? true : false, CSPI_EIRQ_A_ENABLE);

	return ret;

kill_kthread:
	kthread_stop(priv->kthread);
	priv->kthread = NULL;

	return ret;
}

static int spi_hif_stop(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();

	/* Note: Wake state check is now handled by HAL in nrc_hif_stop() */

	hdev->slot[TX_SLOT].count = 999;
	if (priv->polling_kthread) {
		kthread_stop(priv->polling_kthread);
		priv->polling_kthread = NULL;
	}

	/* Unpark kthread before stopping to avoid issues */
	if (priv->kthread && atomic_read(&priv->rx_thread_parked)) {
		kthread_unpark(priv->kthread);
		atomic_set(&priv->rx_thread_parked, 0);
	}
	if (priv->kthread) {
		kthread_stop(priv->kthread);
		/* prevent false "leaked" warning in nrc_cspi_remove() */
		priv->kthread = NULL;
	}

	/* Wake up both TX and RX threads for cleanup */
	wake_up_interruptible(&priv->tx_wait);
	wake_up_interruptible(&priv->rx_wait);

	cancel_delayed_work(&priv->work);

	nrc_spi_free_irq(priv);

	return 0;
}

void spi_hif_close(struct nrc_hif_device *hdev)
{
	return;
}

static int spi_hif_rx_thread_suspend(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();
	int ret;

	/* Park RX thread for deep sleep */
	if (priv->kthread && !atomic_read(&priv->rx_thread_parked)) {
		/**
		 * Deadlock prevention: Wake up threads from wait_event_interruptible()
		 * before parking. If thread is sleeping in wait queue, kthread_park()
		 * will block forever waiting for thread to reach park point.
		 */
		wake_up_interruptible(&priv->tx_wait);
		wake_up_interruptible(&priv->rx_wait);

		ret = kthread_park(priv->kthread);
		if (ret == 0) {
			atomic_set(&priv->rx_thread_parked, 1);
			VBS_PS("%s: RX thread parked", __func__);
		} else {
			ERR_PS("kthread_park failed: %d", ret);
			return ret;
		}
	}

	/*
	 * Signal deep sleep state to device by clearing EIRQ A_ENABLE bits
	 * (RegHIF_DEVICE_HST_STATS & 0xF → 0).
	 *
	 * On wake, ucode calls nrc_ps_force_eirq_and_wait() which checks:
	 *   if (RegHIF_DEVICE_HST_STATS & 0xF) != 0xF → EIRQ handshake
	 *   else                                       → return -1 ("Failed DEEPSLEEP")
	 *
	 * Without this call, A_ENABLE bits remain 0xF (normal operating state),
	 * causing ucode to skip the handshake and print "Failed DEEPSLEEP" on
	 * every NonTIM/TIM deep sleep wake cycle.
	 *
	 * The hardware IRQ remains enabled so the host can still receive the
	 * EIRQ GPIO30 assertion from ucode and respond with BIT1 via
	 * spi_process_device_status() → c_spi_enable_irq(true).
	 */
	c_spi_enable_irq(spi, false, CSPI_EIRQ_A_ENABLE);
	priv->data_irq_disabled = true;

	/* Synchronize IRQ to ensure no pending handlers */
	if (spi->irq >= 0 && priv->polling_interval <= 0) {
		synchronize_irq(spi->irq);
		/**
		 * The SPI IRQ will be never disabled because it needs to be asserted
		 * by sending FW ready WIM message from target when uCode received a TIM
		 * and it has to notify wake-up to the host.
		 */
	}

	return 0;
}

int spi_hif_rx_thread_resume(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();

	if (priv->kthread && atomic_read(&priv->rx_thread_parked)) {
		kthread_unpark(priv->kthread);
		atomic_set(&priv->rx_thread_parked, 0);
		VBS_PS("%s: RX thread unparked", __func__);
	}

	return 0;
}

/* ===========================================================================
 * Data Transmission Operations
 * =========================================================================== */

static int spi_hif_xmit(struct nrc_hif_device *hdev, struct sk_buff *skb)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();

	int ret, nr_slot = DIV_ROUND_UP(skb->len, hdev->slot[TX_SLOT].size);
	int avail = 0;
#ifdef CONFIG_TRX_BACKOFF
	int backoff;
#endif
	struct hif *hif;
	struct frame_hdr *fh;

	/* Validate HIF header before SPI hardware transmission */
	VALIDATE_HIF_HEADER(skb, "spi_xmit");

	hif = (void *)skb->data;
	fh = (void *)(hif + 1);

	DBG_TX("HIF %s(%s) len=%u nr_slot=%d drv=%s ps=%s",
	       nrc_hif_type_str(hif->type),
	       nrc_hif_subtype_str(hif->type, hif->subtype), skb->len, nr_slot,
	       NRC_DRV_STATE_STR(hdev), NRC_PS_STATE_STR(hdev));

	if (NRC_HIF_DRV_STATE(hdev) <= NRC_DRV_STOP || hdev->params->loopback) {
		DBG_TX("Skipping drv_state=%s(%d) loopback=%d",
		       NRC_DRV_STATE_STR(hdev), NRC_HIF_DRV_STATE(hdev),
		       hdev->params->loopback);
		return 0;
	}

#ifdef CONFIG_TRX_BACKOFF
	if (!hdev->ampdu_supported) {
		backoff = atomic_inc_return(&priv->trx_backoff);

		if ((backoff % 3) == 0) {
			DBG_HIF("xmit: trx_backoff=%d", backoff);
			usleep_range(800, 1000);
		}
	}
#endif

	/*
	 * SLOT_SYNC_LOCK protects only the critical section:
	 * 1. slot[TX_SLOT].tail update
	 * 2. Actual SPI write operation
	 *
	 * Keep lock scope minimal to avoid blocking other SPI operations.
	 */
	SLOT_SYNC_LOCK();

	/* Check available credits to xmit by nr_slot */
	if ((hif->type == HIF_TYPE_FRAME) &&
	    ((hif->subtype == HIF_FRAME_SUB_DATA_BE) ||
	     (hif->subtype == HIF_FRAME_SUB_MGMT))) {
		u8 ac = fh->flags.tx.ac;
		if (ac < CREDIT_QUEUE_MAX) {
			unsigned long flags;
			u8 f, r, max;
			int room;

			CREDIT_LOCK(hdev, flags);
			f = hdev->credit.front[ac];
			r = hdev->credit.rear[ac];
			max = hdev->credit.credit_max[ac];
			CREDIT_UNLOCK(hdev, flags);

			room = (f >= r) ? (f - r) : (255 - r + f);
			avail = max - room;

			if (avail < nr_slot) {
				SLOT_SYNC_UNLOCK();
				VBS_HIF("TX Credit Shortage: ac=%d need=%d avail=%d front=%d rear=%d max=%d",
					ac, nr_slot, avail, f, r, max);
				return HIF_TX_FAILED;
			}
		}
	}

	/* Re-validate slot availability inside lock to prevent race condition.
	 * Multiple threads may pass wait_for_xmit() check simultaneously,
	 * but only one can actually use the slots.
	 */
	if (c_spi_num_slots(hdev, TX_SLOT) < nr_slot) {
		SLOT_SYNC_UNLOCK();
		WRN("TX slot exhausted: need=%d avail=%d head=%d tail=%d",
		    nr_slot, c_spi_num_slots(hdev, TX_SLOT),
		    hdev->slot[TX_SLOT].head, hdev->slot[TX_SLOT].tail);
		return HIF_TX_FAILED;
	}

	DBG_SLOT("TX TAIL: %d -> %d (HEAD=%d, avail=%d/%d)",
		 hdev->slot[TX_SLOT].tail, hdev->slot[TX_SLOT].tail + nr_slot,
		 hdev->slot[TX_SLOT].head,
		 c_spi_num_slots(hdev, TX_SLOT) - nr_slot,
		 hdev->slot[TX_SLOT].count);
	hdev->slot[TX_SLOT].tail += nr_slot;

	ret = c_spi_xmit(priv->spi, skb->data, skb->len);

	if (ret != skb->len) {
		/* SPI write failed - rollback tail to allow requeue */
		hdev->slot[TX_SLOT].tail -= nr_slot;
		SLOT_SYNC_UNLOCK();

		/*
		 * If failure is -EIO in deep sleep mode (TIM or NonTIM), it's a known
		 * PS transition race where the target enters sleep or wakes via 0xDC.
		 * Both modes use the same FW-reload wake path; suppress to Verbose.
		 */
		if (ret == -EIO && NRC_PS_IS_DEEPSLEEP(hdev)) {
			VBS_BUS("SPI xmit desync (-EIO) during deep sleep transition (ps=%s)",
				NRC_PS_STATE_STR(hdev));
		} else {
			ERR("SPI xmit failed - expected %u bytes, wrote %zd (ps=%s, drv=%s)",
			    skb->len, ret, NRC_PS_STATE_STR(hdev),
			    NRC_DRV_STATE_STR(hdev));
		}
		return HIF_TX_FAILED;
	}

	/*
	 * Increment credit.front AFTER successful SPI write.
	 * This prevents credit leak when SPI write fails, as front
	 * would be permanently advanced without a corresponding
	 * rear increment from FW.
	 */
	if (avail >= nr_slot) {
		unsigned long flags;
		CREDIT_LOCK(hdev, flags);
		hdev->credit.front[fh->flags.tx.ac] += nr_slot;
		CREDIT_UNLOCK(hdev, flags);
	}

	SLOT_SYNC_UNLOCK();

	if (fh->flags.tx.ac < CREDIT_QUEUE_MAX) {
		DBG_TX("xmit: ac=%d slot=%d(%d/%d) fwpend=%d/%d qlen=%d",
		       fh->flags.tx.ac, nr_slot, hdev->slot[TX_SLOT].head,
		       hdev->slot[TX_SLOT].tail,
		       hdev->credit.front[fh->flags.tx.ac],
		       hdev->credit.rear[fh->flags.tx.ac],
		       skb_queue_len(&hdev->queue[0]));
	}

	return HIF_TX_COMPLETE;
}

static int spi_hif_wait_for_xmit(struct nrc_hif_device *hdev,
				 struct sk_buff *skb)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	int nr_slot = DIV_ROUND_UP(skb->len, hdev->slot[TX_SLOT].size);
	int ret;

	if (c_spi_num_slots(hdev, TX_SLOT) >= nr_slot)
		return 0;

	/* Skip SPI access during WAKING state (waiting for FW_READY) */
	if (!NRC_PS_IS_WAKING(hdev)) {
		spi_update_status(hdev);
	} else {
		DBG_PS("[%s] Skip spi_update_status during WAKING", __func__);
	}

	if (c_spi_num_slots(hdev, TX_SLOT) < hdev->max_slot_num) {
		spi_enable_data_interrupt(priv->spi, priv, "TX slot available");
	}

	if (c_spi_num_slots(hdev, TX_SLOT) >= nr_slot) {
		wake_up_interruptible(&priv->tx_wait);
		return 0;
	}

	ret = wait_event_interruptible_timeout(
		priv->tx_wait,
		(c_spi_num_slots(hdev, TX_SLOT) >= nr_slot) ||
			kthread_should_stop(),
		5 * HZ);
	if (ret == 0) { /* Timeout */
		ERR_HIF("xmit timeout: TX(h=%u t=%u avail=%u) need=%d ps=%s drv=%s",
			hdev->slot[TX_SLOT].head, hdev->slot[TX_SLOT].tail,
			(u16)(hdev->slot[TX_SLOT].head -
			      hdev->slot[TX_SLOT].tail),
			nr_slot, NRC_PS_STATE_STR(hdev),
			NRC_DRV_STATE_STR(hdev));
		return -1;
	}
	if (ret < 0)
		return ret;

	return 0;
}

static int spi_hif_raw_write(struct nrc_hif_device *hdev, const u8 *data,
			     const u32 len)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	ssize_t ret;

	ret = c_spi_write(priv->spi, (u8 *)data, (u32)len);
	if (ret < 0)
		return HIF_TX_FAILED;
	return HIF_TX_COMPLETE;
}

static int spi_hif_raw_read(struct nrc_hif_device *hdev, const u8 *data,
			    const u32 len)
{
	struct nrc_spi_priv *priv = hdev->priv;

	SLOT_SYNC_LOCK();
	c_spi_read(priv->spi, (u8 *)data, (u32)len);
	SLOT_SYNC_UNLOCK();
	return 0;
}

static int spi_hif_wait_rxq_slot(struct nrc_hif_device *hdev, u8 *data, u32 len)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_status_reg status;
	int ret;

	do {
		SLOT_SYNC_LOCK();
		ret = c_spi_read_regs(priv->spi, C_SPI_EIRQ_MODE,
				      (void *)&status, sizeof(status));
		SLOT_SYNC_UNLOCK();
		if (ret < 0) {
			ERR("wait_rxq_slot: read regs failed");
			return ret;
		}
	} while ((status.rxq_status[1] & RXQ_SLOT_COUNT) < 1);
	return 0;
}

/* ===========================================================================
 * Device Control Operations
 *
 * Two-tier reset design — see context rules in nrc-spi-hif-ops.h.
 * =========================================================================== */

static void spi_hif_reset_device(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();
	int i;

	if (enable_hspi_init) {
		for (i = 0; i < 180; i++)
			_c_spi_write_dummy(spi);
	}

	/*
	 * Reset through the hardware line when the board has one. A soft reset
	 * has to travel over C-SPI, so it cannot reach a target that has
	 * stopped answering - exactly the case this reset is asked to recover.
	 */
	nrc_cspi_reset(priv, spi);
}

void spi_hif_reset_rx(struct nrc_hif_device *hdev)
{
	struct spi_device *spi = nrc_spi_get_device();
	struct nrc_spi_event_data event_data;

	/*
	 * Must be called from process context only.
	 * See context rules in nrc-spi-hif-ops.h.
	 */
	might_sleep();

	WARN_HIF("Reset SPI RX");

	spi_host_irq_disable(hdev);
	c_spi_enable_irq(spi, false, CSPI_EIRQ_A_ENABLE);
	hdev->slot[RX_SLOT].tail = hdev->slot[RX_SLOT].head = 0;

	/* Trigger HAL callback event for TX reset instead of direct function call */
	memset(&event_data, 0, sizeof(event_data));
	event_data.type = NRC_BACKEND_EVT_RESET_TX;
	nrc_spi_trigger_event(&event_data);

	spi_host_irq_enable(hdev);
	c_spi_enable_irq(spi, true, CSPI_EIRQ_A_ENABLE);
}

void spi_hif_reset_tx(struct nrc_hif_device *hdev)
{
	struct spi_device *spi = nrc_spi_get_device();
	struct nrc_spi_event_data event_data;

	/*
	 * Must be called from process context only.
	 * See context rules in nrc-spi-hif-ops.h.
	 */
	might_sleep();

	WARN_HIF("Reset SPI TX");

	spi_host_irq_disable(hdev);
	c_spi_enable_irq(spi, false, CSPI_EIRQ_A_ENABLE);

	/* Trigger HAL callback event for RX reset instead of direct function call */
	memset(&event_data, 0, sizeof(event_data));
	event_data.type = NRC_BACKEND_EVT_RESET_RX;
	nrc_spi_trigger_event(&event_data);

	/* wim itself is tx frame, so set -1, not 0 */
	hdev->slot[TX_SLOT].tail = -1;
	hdev->slot[TX_SLOT].head = 32;

	spi_host_irq_enable(hdev);
	c_spi_enable_irq(spi, true, CSPI_EIRQ_A_ENABLE);
}

/*
 * spi_reset_slot_tx / spi_reset_slot_rx - IRQ-thread-safe slot reset
 *
 * Resets only the local host-side slot counters to canonical initial values.
 * Does NOT touch IRQ control and does NOT send any WIM command to firmware.
 *
 * Safe to call from the threaded IRQ handler (spi_irq → spi_update_status).
 * Used when slot desync is detected inside spi_update_status(): the full reset
 * (spi_hif_reset_tx/rx) cannot be used there because disable_irq() →
 * synchronize_irq() would deadlock waiting for the IRQ thread itself.
 *
 * The firmware-side sync (WIM_CMD_RESET_HIF_TX/RX) is intentionally skipped
 * here because: (a) during early boot FW may not yet be ready to receive WIM,
 * and (b) normal slot operation resumes naturally once FW is ready.
 */
void spi_reset_slot_tx(struct nrc_hif_device *hdev)
{
	if (!hdev)
		return;
	/* Canonical initial TX slot state */
	hdev->slot[TX_SLOT].tail = -1;
	hdev->slot[TX_SLOT].head = 32;
}

void spi_reset_slot_rx(struct nrc_hif_device *hdev)
{
	if (!hdev)
		return;
	/* Canonical initial RX slot state */
	hdev->slot[RX_SLOT].tail = hdev->slot[RX_SLOT].head = 0;
}

/* ===========================================================================
 * Synchronization Operations
 * =========================================================================== */

/* ===========================================================================
 * Interrupt Management Operations
 *
 * IRQ lifecycle is fully managed by SPI module:
 * - request_irq() in spi_start() — IRQ enabled
 * - free_irq() in spi_stop()     — IRQ disabled
 *
 * spi_host_irq_disable/enable are used only inside spi_hif_reset_tx/rx(),
 * which are process-context-only functions. They MUST NOT be called from
 * the threaded IRQ handler. See context rules in nrc-spi-hif-ops.h.
 * =========================================================================== */

/*
 * spi_host_irq_disable/enable - Host IRQ gate for full HIF reset
 *
 * Called only from spi_hif_reset_tx/rx() (process context).
 * Uses kernel IRQ depth counting — disable/enable must be balanced.
 */
static void spi_host_irq_disable(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();

	if (!priv || !spi || spi->irq < 0)
		return;

	if (priv->polling_interval <= 0)
		disable_irq(spi->irq); /* sync to avoid race in PS/reset path */
}

static void spi_host_irq_enable(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();

	if (!priv || !spi || spi->irq < 0)
		return;

	if (priv->polling_interval <= 0)
		enable_irq(spi->irq);
}

int spi_hif_status_irq(struct nrc_hif_device *hdev)
{
	//struct nrc_spi_priv *priv = nrc_spi_get_priv();
	//struct spi_device *spi = nrc_spi_get_device();
	int irq = gpio_get_value(spi_gpio_irq);

	VBS_IRQ("[%s] gpio_value=%d ps_state=%s", __func__, irq,
		NRC_PS_STATE_STR(hdev));

	return irq;
}

void spi_hif_clear_irq(struct nrc_hif_device *hdev)
{
	// struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();

	VBS_IRQ("[%s] ps_state=%s", __func__, NRC_PS_STATE_STR(hdev));

	spi_read_status(spi);
}

/* ===========================================================================
 * Status and GPIO Operations
 * =========================================================================== */

static int spi_hif_check_target(struct nrc_hif_device *hdev, u8 reg)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_device *spi = nrc_spi_get_device();
	struct spi_status_reg *status = &priv->hw.status;
	int ret;

	SLOT_SYNC_LOCK();
	ret = c_spi_read_regs(spi, C_SPI_EIRQ_MODE, (void *)status,
			      sizeof(*status));
	SLOT_SYNC_UNLOCK();

	if (ret < 0)
		return ret;

	return (int)(*(((u8 *)status) + reg - C_SPI_EIRQ_MODE));
}

/**
 * spi_hif_fw_is_boot - Check if target is in bootloader mode
 * @hdev: HIF device structure
 *
 * Reads the SW_ID register to determine if the target chip is in bootloader
 * mode (waiting for firmware download) or has firmware already loaded.
 *
 * Return: true if in bootloader mode, false otherwise
 */
static bool spi_hif_fw_is_boot(struct nrc_hif_device *hdev)
{
	struct nrc_spi_priv *priv = nrc_spi_get_priv();
	struct spi_sys_reg sys;
	int ret;

	SLOT_SYNC_LOCK();
	ret = spi_read_sys_reg(priv->spi, &sys);
	SLOT_SYNC_UNLOCK();

	if (ret < 0) {
		ERR("failed to read register 0x0");
		return false;
	}
	DBG_FW("fw_is_boot: sw_id=0x%x, result=%d", sys.sw_id,
	       sys.sw_id == SW_MAGIC_FOR_BOOT);
	return (sys.sw_id == SW_MAGIC_FOR_BOOT);
}

/**
 * spi_hif_fw_is_loaded - Check if firmware is loaded and running
 * @hdev: HIF device structure
 *
 * Verifies that firmware is properly loaded by checking SW_ID register:
 * - Device status bit must be set
 * - SW_ID must match expected NRC_SW_ID (not bootloader magic)
 * - Build chip ID must match actual chip ID
 *
 * Return: true if firmware is loaded and running, false if in bootloader or error
 */
static bool spi_hif_fw_is_loaded(struct nrc_hif_device *hdev)
{
	struct spi_device *spi = nrc_spi_get_device();
	struct spi_sys_reg sys;
	int ret = 0;

	SLOT_SYNC_LOCK();
	ret = spi_read_sys_reg(spi, &sys);
	SLOT_SYNC_UNLOCK();

	if (ret < 0)
		return false;

	if (!(sys.status & 0x1)) {
		return false;
	}
	if ((sys.sw_id & 0xFFFF) != NRC_SW_ID) { /* Low 2byte: build sw id */
		return false;
	}
	if (((sys.sw_id >> 16) & 0xFFFF) !=
	    sys.chip_id) { /* High 2byte: build chip id */
		return false;
	}

	DBG_FW("fw_is_loaded: chip=%04x sw_id=%u/%u status=%d", sys.chip_id,
	       sys.sw_id & 0xFFFF, NRC_SW_ID, sys.status);

	return true;
}

/**
 * spi_hif_check_sleep - Check if target has entered sleep mode
 * @hdev: HIF device structure
 *
 * This function polls the device status register to confirm sleep entry.
 *
 * Returns:
 *   0: PS not ready yet (device still awake and responsive)
 *   1: PS ready (TARGET_NOTI_PS_READY detected) - sleep confirmed
 *   ret < 0: SPI read failure - typical for deep sleep entry (sleep confirmed)
 *   3, 4: FW download request detected (unexpected during sleep entry)
 */
static int spi_hif_check_sleep(struct nrc_hif_device *hdev)
{
	struct spi_device *spi = nrc_spi_get_device();
	struct spi_status_reg status;
	int ret;
	u32 target_noti;

	/* Read status with lock held (minimal lock duration) */
	SLOT_SYNC_LOCK();
	memset(&status, 0x00, sizeof(status));
	ret = c_spi_read_regs(spi, C_SPI_EIRQ_MODE, (void *)&status,
			      sizeof(status));
	SLOT_SYNC_UNLOCK();

	if (ret != 0) {
		/*
		 * SPI read failure during deep sleep entry means the firmware has
		 * shut down the SPI bus — this IS the sleep confirmation signal.
		 */
		DBG_PS("check_sleep: SPI read failed (ret=%d), assuming device asleep",
		       ret);
		return ret;
	}

	target_noti = status.msg[3] & 0xffff;

	/*
	 * On newer chips and updated firmware libraries, firmware uses
	 * nrc_ps_force_eirq_and_wait() during deep sleep entry.
	 */
	if (status.eirq.status == EIRQ_STATUS_DEVICE_ROM) {
		c_spi_enable_irq(spi, false,
				 CSPI_EIRQ_A_ENABLE); /* cleanup shadow reg */
		c_spi_enable_irq(spi, true, CSPI_EIRQ_A_ENABLE);
	}

	if (target_noti == TARGET_NOTI_PS_READY)
		return 1;

	if (target_noti == TARGET_NOTI_REQUEST_FW_DOWNLOAD) {
		if (status.eirq.status & EIRQ_STATUS_DEVICE_READY)
			return 4;
		else
			return 3;
	}

	return 0;
}

/**
 * spi_hif_fw_state - Read firmware hardware state from EIRQ_STATUS
 * @hdev: HIF device structure
 *
 * Returns: NRC_FW_STATE enum value
 */
static enum NRC_FW_STATE spi_hif_fw_state(struct nrc_hif_device *hdev)
{
	struct spi_device *spi = nrc_spi_get_device();
	struct spi_status_reg status;
	int ret;

	memset(&status, 0, sizeof(status));
	SLOT_SYNC_LOCK();
	ret = c_spi_read_regs(spi, C_SPI_EIRQ_MODE, (void *)&status,
			      sizeof(status));
	SLOT_SYNC_UNLOCK();
	if (ret < 0)
		return NRC_FW_STATE_ROM;

	/* Convert EIRQ status to NRC_FW_STATE */
	if (status.eirq.status & EIRQ_STATUS_DEVICE_SLEEP)
		return NRC_FW_STATE_SLEEP;
	if (status.eirq.status & EIRQ_STATUS_DEVICE_READY)
		return NRC_FW_STATE_READY;
	return NRC_FW_STATE_ROM;
}

/* ===========================================================================
 * Generic GPIO Operations
 * =========================================================================== */

/**
 * spi_hif_gpio_alloc - Allocate GPIO resource
 * @hdev: HIF device structure
 * @gpio_num: GPIO number to allocate
 * @label: Label for GPIO (e.g., "nrc-ps-gpio", "nrc-reset")
 *
 * Returns: 0 on success, negative error code on failure
 */
static int spi_hif_gpio_alloc(struct nrc_hif_device *hdev, int gpio_num,
			      const char *label)
{
	struct gpio_desc *desc = NULL;

	if (!hdev) {
		ERR_HIF("GPIO: Invalid hdev pointer for allocation");
		return -EINVAL;
	}

	if (gpio_num <= 0) {
		ERR_HIF("GPIO: Invalid GPIO number: %d", gpio_num);
		return -EINVAL;
	}

	INFO("GPIO: Allocating GPIO#%d with label '%s'", gpio_num,
	     label ? label : "unknown");

	/* Request GPIO */
	desc = nrc_gpio_request(gpio_num, label ? label : "nrc-gpio");
	if (IS_ERR(desc)) {
		int err = PTR_ERR(desc);
		/* -EBUSY means GPIO already in use - could be from module reload */
		if (err == -EBUSY || err == -EPROBE_DEFER) {
			INFO("GPIO: GPIO %d already in use (expected during reload), continuing",
			     gpio_num);
			/* Reinitialize to known state */
			nrc_gpio_direction_output(gpio_num, 1);
			return 0; /* Not a failure */
		} else {
			ERR_HIF("GPIO: Failed to request GPIO %d: %d", gpio_num,
				err);
			return err;
		}
	}

	/* Set GPIO direction to output with initial LOW state */
	INFO("GPIO: Setting GPIO %d to output mode (initial LOW)", gpio_num);
	nrc_gpio_direction_output(gpio_num, 0);

	return 0;
}

/**
 * spi_hif_gpio_free - Free GPIO resource
 * @hdev: HIF device structure
 * @gpio_num: GPIO number to free
 */
static void spi_hif_gpio_free(struct nrc_hif_device *hdev, int gpio_num)
{
	if (!hdev) {
		ERR_HIF("GPIO: Invalid hdev pointer for free");
		return;
	}

	if (gpio_num <= 0) {
		/* GPIO 0 or negative is often intentional (not configured) */
		return;
	}

	nrc_gpio_free(gpio_num);
}

/**
 * spi_hif_gpio_set - Set GPIO value
 * @hdev: HIF device structure
 * @gpio_num: GPIO number to control
 * @value: GPIO value (0 or 1)
 */
static void spi_hif_gpio_set(struct nrc_hif_device *hdev, int gpio_num,
			     int value)
{
	if (!hdev) {
		ERR_HIF("GPIO: Invalid hdev for control");
		return;
	}

	if (gpio_num <= 0) {
		ERR_HIF("GPIO: Invalid GPIO number: %d", gpio_num);
		return;
	}

	nrc_gpio_set_value(gpio_num, value);
}

/* ===========================================================================
 * HIF Operations Structure Definition
 * =========================================================================== */

struct nrc_hif_ops spi_ops = {
	.probe = spi_hif_probe,
	.start = spi_hif_start,
	.stop = spi_hif_stop,
	.xmit = spi_hif_xmit,
	.wait_for_xmit = spi_hif_wait_for_xmit,
	.write = spi_hif_raw_write,
	.read = spi_hif_raw_read,
	.wait_rxq_slot = spi_hif_wait_rxq_slot,
	.rx_thread_suspend = spi_hif_rx_thread_suspend,
	.rx_thread_resume = spi_hif_rx_thread_resume,
	.reset_device = spi_hif_reset_device,
	.update = spi_update_status,
	.check_target = spi_hif_check_target,
	.check_sleep = spi_hif_check_sleep,
	.fw_is_boot = spi_hif_fw_is_boot,
	.fw_is_loaded = spi_hif_fw_is_loaded,
	.fw_state = spi_hif_fw_state,
	.gpio_alloc = spi_hif_gpio_alloc,
	.gpio_free = spi_hif_gpio_free,
	.gpio_set = spi_hif_gpio_set,
};

/* ===========================================================================
 * HIF Operations Export
 * =========================================================================== */

/**
 * nrc_spi_get_hif_ops - Get SPI HIF operations structure
 *
 * Returns: Pointer to SPI HIF operations structure for HAL registration
 */
struct nrc_hif_ops *nrc_spi_get_hif_ops(void)
{
	return &spi_ops;
}
EXPORT_SYMBOL(nrc_spi_get_hif_ops);
