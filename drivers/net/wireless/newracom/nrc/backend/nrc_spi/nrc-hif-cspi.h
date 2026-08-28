/*
 *
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

#ifndef _NRC_HIF_CSPI_H_
#define _NRC_HIF_CSPI_H_

/* Linux kernel headers */
#include <linux/mutex.h>
#include <linux/spi/spi.h>

/* Common directory headers */
#include "nrc.h"
#include "nrc-hif.h"

struct spi_sys_reg {
	u8 wakeup; /* 0x0 */
	u8 status; /* 0x1 */
	u16 chip_id; /* 0x2-0x3 */
	u32 modem_id; /* 0x4-0x7 */
	u32 sw_id; /* 0x8-0xb */
	u32 board_id; /* 0xc-0xf */
} __packed;

struct spi_status_reg {
	struct {
		u8 mode;
		u8 enable;
		u8 latched_status;
		u8 status;
	} eirq;
	u8 txq_status[6];
	u8 rxq_status[6];
	u32 msg[4];

#define EIRQ_IO_ENABLE (1 << 2)
#define EIRQ_EDGE (1 << 1)
#define EIRQ_ACTIVE_LO (1 << 0)

#define EIRQ_DEV_SLEEP (1 << 3)
#define EIRQ_DEV_READY (1 << 2)
#define EIRQ_RXQ (1 << 1)
#define EIRQ_TXQ (1 << 0)

#define TXQ_ERROR (1 << 7)
#define TXQ_SLOT_COUNT (0x7F)
#define RXQ_SLOT_COUNT (0x7F)

} __packed;

#define SPI_BUFFER_SIZE (496 - 20)

/* C-SPI command
 *
 * [31:24]: start byte (0x50)
 * [23:23]: burst (0: single, 1: burst)
 * [22:22]: direction (0: read, 1: write)
 * [21:21]: fixed (0: incremental, 1: fixed)
 * [20:13]: address
 * [12:0]: length (for multi-word transfer)
 * [7:0]: wdata (for single write)
 */
#define C_SPI_READ 0x50000000
#define C_SPI_WRITE 0x50400000
#define C_SPI_BURST 0x00800000
#define C_SPI_FIXED 0x00200000
#define C_SPI_ADDR(x) (((x) & 0xff) << 13)
#define C_SPI_LEN(x) ((x) & 0x1fff)
#define C_SPI_WDATA(x) ((x) & 0xff)
#define C_SPI_ACK 0x47

#define TX_SLOT 0
#define RX_SLOT 1

/* N-SPI host-side memory map */
#define C_SPI_SYS_REG 0x0
#define C_SPI_WAKE_UP 0x0
#define C_SPI_DEVICE_STATUS 0x1
#define C_SPI_CHIP_ID_HIGH 0x2
#define C_SPI_CHIP_ID_LOW 0x3
#define C_SPI_MODEM_ID 0x4
#define C_SPI_SOFTWARE_VERSION 0x8
#define C_SPI_BOARD_ID 0xc
#define C_SPI_EIRQ_MODE 0x10
#define C_SPI_EIRQ_ENABLE 0x11
#define C_SPI_EIRQ_STATUS_LATCH 0x12
#define C_SPI_EIRQ_STATUS 0x13
#define C_SPI_QUEUE_STATUS 0x14 /* 0x1f */
#define C_SPI_MESSAGE 0x20 /* 0x2f */

#define C_SPI_RXQ_THRESHOLD 0x30
#define C_SPI_RXQ_WINDOW 0x31

#define C_SPI_TXQ_THRESHOLD 0x40
#define C_SPI_TXQ_WINDOW 0x41

#define SW_MAGIC_FOR_BOOT (0x01020716)
#define SW_MAGIC_FOR_FW (0x01210630)

/* Bounded poll timeouts used instead of fixed post-reset delays. */
/* Initial settle before polling after HW reset. */
#define NRC_HW_RESET_SETTLE_MS 100
/* Poll window for the chip to return on the bus after HW reset. */
#define NRC_HW_RESET_READY_TIMEOUT_MS 300
/*
 * Wait for ROM boot per probe attempt.
 *
 * The windows escalate because a cold power-up needs markedly longer to reach
 * the ROM bootloader than a reset of an already powered chip: a warm target
 * answers in roughly 300 ms, while a cold power-up has been observed to need
 * more than 800 ms after reset. Each retry resets the chip again, which
 * restarts ROM boot, so a fixed short window can never catch a slow chip no
 * matter how many retries are allowed - the window itself has to grow.
 *
 * The first window is unchanged so a healthy target probes as fast as before.
 * The cumulative worst case is NRC_PROBE_BOOT_BUDGET_MS.
 */
#define NRC_PROBE_BOOT_TIMEOUT_MS 500
#define NRC_PROBE_BOOT_WINDOWS_MS { 500, 1500, 3000 }
#define NRC_PROBE_BOOT_BUDGET_MS 5000

#define CSPI_EIRQ_MODE 0x05
#define CSPI_EIRQ_Q_ENABLE 0x3
#define CSPI_EIRQ_R_ENABLE 0x4
#define CSPI_EIRQ_S_ENABLE 0x8
#define CSPI_EIRQ_A_ENABLE \
	(CSPI_EIRQ_Q_ENABLE | CSPI_EIRQ_R_ENABLE | CSPI_EIRQ_S_ENABLE)
/*#define CSPI_EIRQ_ENABLE 0x16*/ /* disable tx/rx que */

/* Object prepended to strut nrc_hif_device */
struct nrc_spi_priv {
	struct spi_device *spi;
#if defined(CONFIG_SPI_USE_DT)
	struct gpio_desc *reset_gpio;
#endif
	/* Set while polling the target for readiness after a reset. Read
	 * failures (invalid ACK / SYS read fail) are expected during that
	 * window, so they are not logged to avoid flooding the console. */
	bool boot_poll;

	/* work, kthread, ... */
	struct delayed_work work;
	struct task_struct *kthread;
	wait_queue_head_t tx_wait; /* TX slot wait queue */
	wait_queue_head_t rx_wait; /* RX data wait queue */

#if !defined(CONFIG_SUPPORT_THREADED_IRQ)
	struct workqueue_struct *irq_wq;
	struct work_struct irq_work;
#endif

	struct {
		struct spi_sys_reg sys;
		struct spi_status_reg status;
	} hw;

	spinlock_t lock;

#ifdef CONFIG_TRX_BACKOFF
	atomic_t trx_backoff;
#endif
	unsigned long loopback_prev_cnt;
	unsigned long loopback_total_cnt;
	unsigned long loopback_last_jiffies;
	unsigned long loopback_read_usec;
	unsigned long loopback_write_usec;
	unsigned long loopback_measure_cnt;

	/* Slot synchronization - protects slot counter operations */
	struct mutex slot_sync_lock;
	bool slot_sync_lock_initialized;
	bool slot_sync_auto; /* Hardware slot synchronization (chip 6201) */

	int polling_interval;
	struct task_struct *polling_kthread;
	bool irq_requested;
	void *irq_dev_id; /* saved dev_id for free_irq (priv->hdev may be freed first) */

	/* Core module references */
	/*
	 * WARNING: hdev member usage restriction
	 * This hdev pointer should ONLY be accessed in spi_poll_status() function.
	 * Do NOT use this member in any other SPI functions. Use proper layer
	 * separation through callback system or HIF ops interface instead.
	 */
	struct nrc_hif_device *hdev;

	/* GPIO resource tracking for proper cleanup */
	bool power_save_gpio_allocated;
	int power_save_gpio_number;

	/* Dummy buffer for TX slot padding (prevents OOB access in c_spi_xmit) */
	u8 *dummy_slot;

	/* RX kthread park state tracking - atomic for multi-context safety */
	atomic_t rx_thread_parked;

	/* Data IRQ throttle state - tracks whether CSPI_EIRQ_S_ENABLE is disabled */
	bool data_irq_disabled;
};

static inline u16 c_spi_num_slots(struct nrc_hif_device *hdev, int dir)
{
	if (!hdev)
		return 0;

	return (hdev->slot[dir].head - hdev->slot[dir].tail);
}

void c_spi_config(struct nrc_spi_priv *priv, struct nrc_hif_device *hdev);
int spi_rx_thread(void *data);
int spi_poll_thread(void *data);
irqreturn_t spi_irq(int irq, void *data);
int spi_update_status(struct nrc_hif_device *hdev);
void spi_reset_slots(struct nrc_hif_device *hdev);
int spi_read_status(struct spi_device *spi);
int c_spi_write_reg(struct spi_device *spi, u8 addr, u8 data);
int _c_spi_write_dummy(struct spi_device *spi);
void c_spi_enable_irq(struct spi_device *spi, bool enable, u8 mask);
ssize_t c_spi_read(struct spi_device *spi, u8 *buf, ssize_t size);
ssize_t c_spi_write(struct spi_device *spi, u8 *buf, ssize_t size);
ssize_t c_spi_xmit(struct spi_device *spi, u8 *buf, ssize_t size);
int c_spi_read_regs(struct spi_device *spi, u8 addr, u8 *buf, ssize_t size);
int spi_read_sys_reg(struct spi_device *spi, struct spi_sys_reg *sys);
int spi_hif_wait_rom_boot(struct spi_device *spi, struct spi_sys_reg *sys,
			  unsigned int timeout_ms, bool need_boot);
void nrc_spi_free_irq(struct nrc_spi_priv *priv);
int nrc_cspi_hw_reset(struct nrc_spi_priv *priv);
void nrc_cspi_sw_reset(struct spi_device *spi);
void nrc_cspi_reset(struct nrc_spi_priv *priv, struct spi_device *spi);
int nrc_cspi_gpio_alloc(struct spi_device *spi);
void nrc_cspi_gpio_free(struct spi_device *spi);
struct nrc_spi_priv *nrc_cspi_alloc(struct spi_device *spi);
void nrc_cspi_free(struct nrc_spi_priv *priv);
int nrc_hif_set_model_conf(struct nrc_hif_device *hdev, u16 chip_id);
void spi_enable_data_interrupt(struct spi_device *spi,
			       struct nrc_spi_priv *priv, const char *reason);
void spi_disable_data_interrupt(struct spi_device *spi,
				struct nrc_spi_priv *priv, const char *reason);

/* Global SPI private data access */
extern struct nrc_spi_priv *g_spi_priv;

/* Helper functions for global SPI private data access */
struct nrc_spi_priv *nrc_spi_get_priv(void);
bool nrc_spi_is_initialized(void);
struct spi_device *nrc_spi_get_device(void);

/*
 * Slot synchronization macros
 *
 * These macros protect the critical section that includes:
 * 1. Checking available slot count (head - tail)
 * 2. Updating slot tail pointer
 * 3. Actual SPI read/write operation
 *
 * Without this lock, RX and TX threads could race and corrupt slot state.
 * Some chips (6201) support hardware slot synchronization (slot_sync_auto=true).
 */
#define SLOT_SYNC_LOCK()                                         \
	do {                                                     \
		struct nrc_spi_priv *_priv = nrc_spi_get_priv(); \
		if (_priv && !_priv->slot_sync_auto &&           \
		    _priv->slot_sync_lock_initialized)           \
			mutex_lock(&_priv->slot_sync_lock);      \
	} while (0)

#define SLOT_SYNC_UNLOCK()                                       \
	do {                                                     \
		struct nrc_spi_priv *_priv = nrc_spi_get_priv(); \
		if (_priv && !_priv->slot_sync_auto &&           \
		    _priv->slot_sync_lock_initialized)           \
			mutex_unlock(&_priv->slot_sync_lock);    \
	} while (0)

#endif
