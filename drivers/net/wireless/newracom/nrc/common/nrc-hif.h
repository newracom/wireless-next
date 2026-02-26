/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC HIF Definitions - Host Interface structures and operations
 */

#ifndef _NRC_HIF_H_
#define _NRC_HIF_H_

#include <net/mac80211.h>

#include "nrc.h"
#include "nrc-debug-common.h"
#include "nrc-ps-common.h"
#include "nrc-wim-types.h"

/**
 * struct nrc_hif_ops - Host Interface operations
 *
 * This structure contains callbacks from transport layer to the host layer.
 * The host interface handles various operations including device control,
 * frame transmission, and power management.
 *
 * @probe: Initialize host device
 * @start: Start host device operations
 * @stop: Stop host device operations
 * @write: Write data to device
 * @read: Read data from device
 * @rx_thread_suspend: Suspend RX thread
 * @rx_thread_resume: Resume RX thread
 * @wait_rxq_slot: Wait for RX queue slot availability
 * @xmit: Transmit frame
 * @wait_for_xmit: Wait for transmission completion
 * @reset_device: Reset the device
 * @test: Test interface functionality
 * @update: Update device status
 * @check_target: Check target status
 * @ps_status: Get power save status from target
 * @gpio_alloc: Allocate GPIO resource
 * @gpio_free: Free GPIO resource
 * @gpio_set: Set GPIO value
 * @fw_is_boot: Check if target is in bootloader mode (waiting for download)
 * @fw_is_loaded: Check if firmware is loaded and running (SW_ID matches)
 * @fw_state: Read firmware hardware state from EIRQ status register
 */
struct nrc_hif_ops {
	int (*probe)(struct nrc_hif_device *dev);
	int (*start)(struct nrc_hif_device *dev);
	int (*stop)(struct nrc_hif_device *dev);
	int (*write)(struct nrc_hif_device *dev, const u8 *data, const u32 len);
	int (*read)(struct nrc_hif_device *dev, const u8 *data, const u32 len);
	int (*rx_thread_suspend)(struct nrc_hif_device *dev);
	int (*rx_thread_resume)(struct nrc_hif_device *dev);
	int (*wait_rxq_slot)(struct nrc_hif_device *dev, u8 *data, u32 len);
	int (*xmit)(struct nrc_hif_device *dev, struct sk_buff *skb);
	int (*wait_for_xmit)(struct nrc_hif_device *dev, struct sk_buff *skb);
	void (*reset_device)(struct nrc_hif_device *dev);
	int (*update)(struct nrc_hif_device *dev);
	int (*check_target)(struct nrc_hif_device *dev, u8 reg);
	int (*ps_status)(struct nrc_hif_device *dev);
	bool (*fw_is_boot)(struct nrc_hif_device *dev);
	bool (*fw_is_loaded)(struct nrc_hif_device *dev);
	enum NRC_FW_STATE (*fw_state)(struct nrc_hif_device *dev);
	int (*gpio_alloc)(struct nrc_hif_device *dev, int gpio_num,
			  const char *label);
	void (*gpio_free)(struct nrc_hif_device *dev, int gpio_num);
	void (*gpio_set)(struct nrc_hif_device *dev, int gpio_num, int value);
};

/**
 * @front: Host-side frame transmission counter per AC
 * @rear: Target-side frame completion counter per AC
 * @credit_max: Maximum credit limit per AC based on chip ID
 * @tx_credit: Available credit for transmission per AC
 * @tx_pend: Pending frames count per AC
 * @lock: Spinlock protecting front[] and rear[] array access
 */
typedef struct tx_credit {
	u8 front[CREDIT_QUEUE_MAX]; /* VIF0(AC0~AC3), BCN, CONC, VIF1(AC0~AC3) */
	u8 rear[CREDIT_QUEUE_MAX];
	u8 credit_max[CREDIT_QUEUE_MAX];

	atomic_t tx_credit[IEEE80211_NUM_ACS *
			   3]; /* AC_BK, AC_BE, AC_VI, AC_VO */
	atomic_t tx_pend[IEEE80211_NUM_ACS * 3];

	spinlock_t lock; /* Protect front[] and rear[] concurrent access */
} tx_credit_t;

/**
 * struct nrc_slot_info - HIF slot management structure
 * @head: Head index for slot queue
 * @tail: Tail index for slot queue
 * @size: Size of each slot in bytes
 * @count: Number of available slots
 */
typedef struct nrc_slot_info {
	u16 head;
	u16 tail;
	u16 size;
	u16 count;
} nrc_slot_info_t;

/**
 * struct nrc_fw - Firmware management structure
 * @fw: Firmware binary data
 * @priv: Firmware private data
 * @info: Firmware information
 * @state: Firmware state (loading, active, etc.)
 * @started: WIM_CMD_START completed successfully (cleared on WIM_CMD_STOP)
 * @loaded: Firmware loaded flag (for module reload detection)
 * @tx: Firmware TX counter
 * @rx: Firmware RX counter
 * @use_ext_lna: External LNA usage flag
 * @recovery_wdt: Firmware recovery watchdog
 */
typedef struct nrc_fw {
	struct firmware *fw;
	struct nrc_fw_priv *priv;
	struct fwinfo_t info;

	atomic_t state;
	atomic_t started;
	bool loaded;
	atomic_t tx;
	atomic_t rx;

	bool use_ext_lna;
	struct nrc_recovery_wdt *recovery_wdt;
} nrc_fw_t;

/**
 * struct nrc_hif_device - Host interface device structure
 * @dev: Device pointer
 * @nw: Network device pointer
 * @hif_ops: Host interface operations
 * @started: HIF start state flag
 * @suspended: HIF suspend state flag
 * @priv: Private data pointer
 * @queue: SKB queues (frame, wim)
 * @queue_pending: Pending queue counter
 * @work: Main work structure
 * @ps_work: Power save work structure
 * @restart_work: Restart work structure
 * @workqueue: Main workqueue
 * @event_workqueue: Event workqueue
 * @ps_wq: Power save workqueue
 * @restart_workqueue: Restart workqueue
 * @wake_work: Wake work structure
 * @wake_recover_work: Wake recovery work structure
 * @ps_time: Power save timestamp (if CONFIG_DELAY_WAKE_TARGET)
 * @wake_done: Wake completion
 * @sleep_done: Sleep completion (if TEST_BLOCK_TX)
 * @params: Shared module parameters structure
 * @debug: Shared debug structure
 * @drv_state: Driver state management (atomic_t, enum NRC_DRV_STATE values)
 * @wim_seqno: WIM sequence number
 * @chip_id: Hardware chip ID
 * @has_macaddr: MAC address availability per VIF
 * @mac_addr: MAC addresses per VIF
 * @credit: TX credit queue management
 * @hw_queues: Number of supported hardware queues
 * @slot: HIF slot management (TX/RX slots)
 * @max_slot_num: Maximum number of slots
 * @wowlan_pattern_num: Number of supported WoWLAN patterns
 * @wowlan_enabled: WoWLAN enable state
 * @ampdu_supported: AMPDU support flag
 * @ps: Power save management structure
 * @fw: Firmware management structure
 * @wim_resp: WIM response structure (each includes its own lock)
 * @cap: Device capabilities
 */

/* SKB Statistics Structure - RX/TX separated */
struct nrc_skb_stats {
	/* Overall counters */
	atomic_t total_alloc;
	atomic_t total_free;
	atomic_t total_queued;
	atomic_t total_err; /* Error drops (freed due to errors) */

	/* RX counters */
	atomic_t rx_alloc;
	atomic_t rx_free;
	atomic_t rx_queued;
	atomic_t rx_err; /* RX error drops */
	atomic_t rx_alloc_by_type[HIF_TYPE_MAX];
	atomic_t rx_free_by_type[HIF_TYPE_MAX];
	atomic_t rx_queued_by_type[HIF_TYPE_MAX];

	/* TX counters */
	atomic_t tx_alloc;
	atomic_t tx_free;
	atomic_t tx_queued;
	atomic_t tx_err; /* TX error drops */
	atomic_t tx_alloc_by_type[HIF_TYPE_MAX];
	atomic_t tx_free_by_type[HIF_TYPE_MAX];
	atomic_t tx_queued_by_type[HIF_TYPE_MAX];
};

/* Forward declaration for Board Data structure (defined in hal/nrc_core/nrc-bd.h) */
struct BDF;

struct nrc_hif_device {
	struct device *dev;
	struct nrc *nw;
	struct nrc_hif_ops *hif_ops;
	bool started;
	void *priv;
	atomic_t frontend_count; /* Number of active frontends */

	/* WLAN frontend TX path */
	struct sk_buff_head queue[2]; /* 0: frame, 1: wim */
	atomic_t queue_pending;
	struct work_struct work; /* WLAN TX work */
	struct workqueue_struct *workqueue; /* WLAN TX workqueue */

	/* MCP frontend TX path */
	struct sk_buff_head mcp_queue[2]; /* 0: frame, 1: wim */
	atomic_t mcp_queue_pending;
	struct work_struct mcp_work; /* MCP TX work */
	struct workqueue_struct *mcp_workqueue; /* MCP TX workqueue */
	atomic_t mcp_active; /* MCP is actively transmitting (for priority control) */

	/* Common workqueues and PS state machine */
	struct work_struct restart_work;
	struct workqueue_struct *event_workqueue;
	struct workqueue_struct *restart_workqueue;
#ifdef CONFIG_DELAY_WAKE_TARGET
	ktime_t ps_time;
#endif
	struct completion wake_done;
#ifdef TEST_BLOCK_TX
	struct completion sleep_done;
#endif
	struct nrc_params *params; /* Shared module parameters */
	struct nrc_debug *debug; /* Shared debug structure */
	atomic_t drv_state; /* Driver state management (atomic) */
	u8 wim_seqno;
	u32 chip_id;
	bool has_macaddr[NR_NRC_VIF];
	struct mac_address mac_addr[NR_NRC_VIF];
	tx_credit_t credit;
	int hw_queues;

	/* HIF Slot management - moved from SPI private */
	nrc_slot_info_t slot[2]; /* 0: TX_SLOT, 1: RX_SLOT */
	int max_slot_num;

	int wowlan_pattern_num;
	bool wowlan_enabled;
	bool ampdu_supported;
	nrc_ps_t ps;
	nrc_fw_t fw;
	struct wim_response *wim_resp;
	struct nrc_capabilities cap;

	/* Board Data (HAL managed) */
	struct BDF *bd;

	/* SKB Debug control and statistics */
	bool skb_debug_enabled; /* Runtime control via debugfs */
	struct nrc_skb_stats skb_stats;
};

#define NRC_HIF_DRV_STATE(dev) atomic_read(&(dev)->drv_state)
#define NRC_HIF_SET_DRV_STATE(dev, val) atomic_set(&(dev)->drv_state, (val))

/* Driver State Check Macros (atomic operations) */
#define NRC_DRV_IS_RUNNING(dev) \
	(atomic_read(&(dev)->drv_state) == NRC_DRV_RUNNING)
#define NRC_DRV_IS_NOT_RUNNING(dev) \
	(atomic_read(&(dev)->drv_state) != NRC_DRV_RUNNING)
#define NRC_DRV_IS_READY(dev) \
	(atomic_read(&(dev)->drv_state) >= NRC_DRV_RUNNING)
#define NRC_DRV_IS_STARTED(dev) \
	(atomic_read(&(dev)->drv_state) >= NRC_DRV_START)
#define NRC_DRV_IS_REBOOT(dev) \
	(atomic_read(&(dev)->drv_state) == NRC_DRV_REBOOT)
#define NRC_DRV_IS_CLOSING(dev) \
	(atomic_read(&(dev)->drv_state) == NRC_DRV_CLOSING)
#define NRC_DRV_IS_INIT(dev) (atomic_read(&(dev)->drv_state) == NRC_DRV_INIT)
#define NRC_DRV_IS_STARTING(dev) \
	(atomic_read(&(dev)->drv_state) == NRC_DRV_START)
#define NRC_DRV_IS_STOPPED(dev) (atomic_read(&(dev)->drv_state) == NRC_DRV_STOP)
#define NRC_DRV_IS_ASLEEP(dev) (atomic_read(&(dev)->drv_state) == NRC_DRV_PS)

/* Firmware Started State Macros (WIM_CMD_START completed) */
#define NRC_FW_IS_STARTED(dev) (atomic_read(&(dev)->fw.started) != 0)
#define NRC_FW_SET_STARTED(dev) atomic_set(&(dev)->fw.started, 1)
#define NRC_FW_CLEAR_STARTED(dev) atomic_set(&(dev)->fw.started, 0)

/* Driver State String Macro */
#define NRC_DRV_STATE_STR(dev) nrc_drv_state_str(NRC_HIF_DRV_STATE(dev))

/* Power Save State Macros */
#define NRC_PS_STATE_STR(dev) nrc_ps_state_str(nrc_ps_get_state(&(dev)->ps))
#define NRC_PS_IS_ASLEEP(dev) \
	(nrc_ps_get_state(&(dev)->ps) == NRC_PS_STATE_SLEEP)
#define NRC_PS_IS_SLEEPING(dev) \
	(nrc_ps_get_state(&(dev)->ps) == NRC_PS_STATE_SLEEPING)
#define NRC_PS_IS_AWAKE(dev) (nrc_ps_get_state(&(dev)->ps) == NRC_PS_STATE_WAKE)
#define NRC_PS_IS_WAKING(dev) \
	(nrc_ps_get_state(&(dev)->ps) == NRC_PS_STATE_WAKING)

#define NRC_WIM_RESP_LOCK(hdev, cmd)                               \
	do {                                                       \
		if (!in_atomic())                                  \
			mutex_lock(&(hdev)->wim_resp[(cmd)].lock); \
	} while (0)

#define NRC_WIM_RESP_UNLOCK(hdev, cmd)                               \
	do {                                                         \
		if (!in_atomic())                                    \
			mutex_unlock(&(hdev)->wim_resp[(cmd)].lock); \
	} while (0)

/**
 * struct nrc_hif_rx_info - RX additional information
 * @offset: Data offset
 * @need_free: Memory free responsibility flag
 * @in_interrupt: IRQ context flag
 * @band: Frequency band
 * @freq: Frequency
 */
struct nrc_hif_rx_info {
	int offset;
	bool need_free;
	bool in_interrupt;
	u8 band;
	u16 freq;
};

/**
 * nrc_hif_init - Initialize host interface
 * @nw: NRC device pointer
 * Must be called before any other host functions.
 * Return: nrc_hif_device pointer on success, PTR_ERR on failure
 */
struct nrc_hif_device *nrc_hif_init(struct nrc *nw);

/**
 * nrc_hif_exit - Free host interface descriptor
 * @dev: Host interface descriptor pointer
 * Return: 0 on success, error code otherwise
 */
int nrc_hif_exit(struct nrc_hif_device *dev);

/**
 * struct hif - Host Interface Frame header
 * @type: Frame type
 * @subtype: Frame subtype
 * @flags: Frame flags
 * @vifindex: Virtual interface index
 * @len: Frame length
 * @tlv_len: TLV length
 * @payload: Frame payload
 */
struct hif {
	u8 type;
	u8 subtype;
	u8 flags;
	s8 vifindex;
	u16 len;
	u16 tlv_len;
	u8 payload[0];
} __packed;

/* Credit lock macros - Protect credit.front[] and credit.rear[] concurrent access */
#define CREDIT_LOCK(hdev, flags) spin_lock_irqsave(&(hdev)->credit.lock, flags)

#define CREDIT_UNLOCK(hdev, flags) \
	spin_unlock_irqrestore(&(hdev)->credit.lock, flags)

/* Queue length check macros */
#define NRC_WIM_QUEUE_LEN(hdev) skb_queue_len(&(hdev)->queue[1])
#define NRC_FRAME_QUEUE_LEN(hdev) skb_queue_len(&(hdev)->queue[0])
#define NRC_QUEUE_LEN(hdev, type) skb_queue_len(&(hdev)->queue[type])
#define NRC_QUEUE_HAS_DATA(hdev)                  \
	(skb_queue_len(&(hdev)->queue[0]) != 0 || \
	 skb_queue_len(&(hdev)->queue[1]) != 0)

#define NRC_MCP_WIM_QUEUE_LEN(hdev) skb_queue_len(&(hdev)->mcp_queue[1])
#define NRC_MCP_FRAME_QUEUE_LEN(hdev) skb_queue_len(&(hdev)->mcp_queue[0])
#define NRC_MCP_QUEUE_LEN(hdev, type) skb_queue_len(&(hdev)->mcp_queue[type])
#define NRC_MCP_QUEUE_HAS_DATA(hdev)                  \
	(skb_queue_len(&(hdev)->mcp_queue[0]) != 0 || \
	 skb_queue_len(&(hdev)->mcp_queue[1]) != 0)

/* Parameter Access Macros */
#define NRC_PARAM_MCP_PRIORITY(hdev) \
	((hdev)->params && (hdev)->params->mcp_priority)
#define NRC_PARAM_POWER_SAVE(hdev) ((hdev)->params->power_save)
#define NRC_PARAM_POWER_SAVE_GPIO(hdev, idx) \
	((hdev)->params->power_save_gpio[(idx)])
#define NRC_PARAM_FW_NAME(hdev) ((hdev)->params->fw_name)
#define NRC_PARAM_BD_NAME(hdev) ((hdev)->params->bd_name)
#define NRC_PARAM_FLASH_FW(hdev) ((hdev)->params->flash_fw)
#define NRC_PARAM_IDLE_MODE(hdev) ((hdev)->params->idle_mode)
#define NRC_PARAM_FW_UPDATE_NAME(hdev) ((hdev)->params->fw_update_name)

/* HIF TX Status codes */
#define HIF_TX_COMPLETE 0
#define HIF_TX_QUEUED 1
#define HIF_TX_FAILED 2
#define HIF_TX_PASSOVER 3

/* SKB Debug Macros - Runtime control via debugfs in hdev */

/* Explicitly zero cb for defense-in-depth, though dev_alloc_skb()
 * already clears it via memset(skb, 0, offsetof(sk_buff, tail)).
 * Ensures clean state for ieee80211_tx_info in all code paths. */
#define NRC_SKB_CB_INIT(skb)                                     \
	do {                                                     \
		if (skb)                                         \
			memset((skb)->cb, 0, sizeof((skb)->cb)); \
	} while (0)

#define NRC_SKB_TRACK_ALLOC(hdev, skb, hif_type, is_rx_path, count_only)                   \
	do {                                                                               \
		struct sk_buff *_skb_tmp = (skb);                                          \
		void *_hdev_tmp = (void *)(hdev);                                          \
		bool _count_only = (count_only);                                           \
		if (_count_only || _skb_tmp) {                                             \
			if (_hdev_tmp) {                                                   \
				struct nrc_hif_device *_hdev =                             \
					(struct nrc_hif_device *)_hdev_tmp;                \
				if (!_hdev->skb_debug_enabled)                             \
					break;                                             \
				atomic_inc(&_hdev->skb_stats.total_alloc);                 \
				if (is_rx_path) {                                          \
					atomic_inc(                                        \
						&_hdev->skb_stats.rx_alloc);               \
					if ((hif_type) < HIF_TYPE_MAX) {                   \
						atomic_inc(                                \
							&_hdev->skb_stats.rx_alloc_by_type \
								 [hif_type]);              \
					}                                                  \
				} else {                                                   \
					atomic_inc(                                        \
						&_hdev->skb_stats.tx_alloc);               \
					if ((hif_type) < HIF_TYPE_MAX) {                   \
						atomic_inc(                                \
							&_hdev->skb_stats.tx_alloc_by_type \
								 [hif_type]);              \
					}                                                  \
				}                                                          \
			}                                                                  \
		}                                                                          \
	} while (0)

#define NRC_SKB_TRACK_FREE(hdev, skb, hif_type, is_rx_path, count_only)                                                         \
	do {                                                                                                                    \
		struct sk_buff *_skb_tmp = (skb);                                                                               \
		void *_hdev_tmp = (void *)(hdev);                                                                               \
		bool _count_only = (count_only);                                                                                \
		int _hif_type = (hif_type);                                                                                     \
		bool _skb_freed = false;                                                                                        \
		/* Step 1: Always free SKB if it exists */                                                                      \
		if (_skb_tmp) {                                                                                                 \
			dev_kfree_skb_any(_skb_tmp);                                                                            \
			_skb_freed = true;                                                                                      \
		}                                                                                                               \
		/* Step 2: Update counters only if hdev exists and debug enabled */                                             \
		if (_hdev_tmp) {                                                                                                \
			struct nrc_hif_device *_hdev =                                                                          \
				(struct nrc_hif_device *)_hdev_tmp;                                                             \
			if (!_hdev->skb_debug_enabled)                                                                          \
				break;                                                                                          \
			/* Count if SKB was freed OR count_only mode */                                                         \
			if (_skb_freed || _count_only) {                                                                        \
				/* Error drop counter - increment error but NOT free counter */                                 \
				if (_hif_type == -1) {                                                                          \
					atomic_inc(                                                                             \
						&_hdev->skb_stats.total_err);                                                   \
					if (is_rx_path) {                                                                       \
						atomic_inc(&_hdev->skb_stats                                                    \
								    .rx_err);                                                   \
					} else {                                                                                \
						atomic_inc(&_hdev->skb_stats                                                    \
								    .tx_err);                                                   \
					}                                                                                       \
				} else {                                                                                        \
					/* Normal free - increment free counter */                                              \
					atomic_inc(                                                                             \
						&_hdev->skb_stats.total_free);                                                  \
					/* RX/TX specific counters */                                                           \
					if (is_rx_path) {                                                                       \
						atomic_inc(&_hdev->skb_stats                                                    \
								    .rx_free);                                                  \
						if (_hif_type >= 0 &&                                                           \
						    _hif_type <                                                                 \
							    HIF_TYPE_MAX) {                                                     \
							atomic_inc(                                                             \
								&_hdev->skb_stats                                               \
									 .rx_free_by_type                                       \
										 [_hif_type]);                                  \
						} else if (_hif_type >=                                                         \
							   HIF_TYPE_MAX) {                                                      \
							pr_warn("SKB_TRACK_FREE: RX HIF type out of range at %s:%d, type=%d\n", \
								__func__,                                                       \
								__LINE__,                                                       \
								_hif_type);                                                     \
						}                                                                               \
					} else {                                                                                \
						atomic_inc(&_hdev->skb_stats                                                    \
								    .tx_free);                                                  \
						if (_hif_type >= 0 &&                                                           \
						    _hif_type <                                                                 \
							    HIF_TYPE_MAX) {                                                     \
							atomic_inc(                                                             \
								&_hdev->skb_stats                                               \
									 .tx_free_by_type                                       \
										 [_hif_type]);                                  \
						} else if (_hif_type >=                                                         \
							   HIF_TYPE_MAX) {                                                      \
							pr_warn("SKB_TRACK_FREE: TX HIF type out of range at %s:%d, type=%d\n", \
								__func__,                                                       \
								__LINE__,                                                       \
								_hif_type);                                                     \
						}                                                                               \
					}                                                                                       \
				}                                                                                               \
			}                                                                                                       \
		}                                                                                                               \
	} while (0)

#define NRC_SKB_TRACK_QUEUE(hdev, skb, hif_type, is_rx_path)                  \
	do {                                                                  \
		void *_hdev_tmp = (void *)(hdev);                             \
		if (_hdev_tmp) {                                              \
			struct nrc_hif_device *_hdev =                        \
				(struct nrc_hif_device *)_hdev_tmp;           \
			if (!_hdev->skb_debug_enabled)                        \
				break;                                        \
			if (is_rx_path) {                                     \
				atomic_inc(&_hdev->skb_stats.rx_queued);      \
				if ((hif_type) < HIF_TYPE_MAX) {              \
					atomic_inc(                           \
						&_hdev->skb_stats             \
							 .rx_queued_by_type   \
								 [hif_type]); \
				}                                             \
			} else {                                              \
				atomic_inc(&_hdev->skb_stats.tx_queued);      \
				if ((hif_type) < HIF_TYPE_MAX) {              \
					atomic_inc(                           \
						&_hdev->skb_stats             \
							 .tx_queued_by_type   \
								 [hif_type]); \
				}                                             \
			}                                                     \
			atomic_inc(&_hdev->skb_stats.total_queued);           \
		}                                                             \
		(void)(skb);                                                  \
	} while (0)

#define NRC_SKB_TRACK_WIM_FREE(hdev, skb, wim_cmd, wim_event, is_rx_path, \
			       count_only)                                \
	NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, is_rx_path, count_only)

/**
 * nrc_mac_is_s1g - Check if device supports S1G mode
 * @hdev: HIF device structure
 *
 * Return: true if S1G mode is supported, false otherwise
 */
static inline bool nrc_mac_is_s1g(struct nrc_hif_device *hdev)
{
#ifdef CONFIG_S1G_CHANNEL
	return true;
#else
	return (hdev->fw.info.version != WIM_SYSTEM_VER_11N);
#endif
}

/**
 * nrc_hif_type_str - Convert HIF type to string
 * @type: HIF type value
 *
 * Return: String representation of HIF type
 */
static inline const char *nrc_hif_type_str(u8 type)
{
	switch (type) {
	case HIF_TYPE_FRAME:
		return "FRAME";
	case HIF_TYPE_WIM:
		return "WIM";
	case HIF_TYPE_ND_WIM:
		return "ND_WIM";
	case HIF_TYPE_NANOPB:
		return "NANOPB";
	case HIF_TYPE_UART_CMD:
		return "UART_CMD";
	case HIF_TYPE_SSP_READYRX:
		return "SSP_READYRX";
	case HIF_TYPE_SSP_PING:
		return "SSP_PING";
	case HIF_TYPE_SSP_PONG:
		return "SSP_PONG";
	case HIF_TYPE_SSP_SKIP:
		return "SSP_SKIP";
	case HIF_TYPE_LOG:
		return "LOG";
	case HIF_TYPE_LOOPBACK:
		return "LOOPBACK";
	case HIF_TYPE_DUMP:
		return "DUMP";
	default:
		return "UNKNOWN";
	}
}

/**
 * nrc_hif_subtype_str - Convert HIF subtype to string
 * @type: HIF type value (context for subtype)
 * @subtype: HIF subtype value
 *
 * Return: String representation of HIF subtype
 */
static inline const char *nrc_hif_subtype_str(u8 type, u8 subtype)
{
	switch (type) {
	case HIF_TYPE_FRAME:
		switch (subtype) {
		case HIF_FRAME_SUB_DATA_BE:
			return "DATA_BE";
		case HIF_FRAME_SUB_DATA_BK:
			return "DATA_BK";
		case HIF_FRAME_SUB_DATA_VO:
			return "DATA_VO";
		case HIF_FRAME_SUB_DATA_VI:
			return "DATA_VI";
		case HIF_FRAME_SUB_MGMT:
			return "MGMT";
		case HIF_FRAME_SUB_BEACON:
			return "BEACON";
		case HIF_FRAME_SUB_PROBE_RESP:
			return "PROBE_RESP";
		case HIF_FRAME_SUB_802_3:
			return "802_3";
		case HIF_FRAME_SUB_CTRL:
			return "CTRL";
		case HIF_FRAME_SUB_RESPONSE:
			return "RESPONSE";
		case HIF_FRAME_SUB_MCP_PROTOCOL:
			return "MCP_PROTOCOL";
		case HIF_FRAME_SUB_MCP_DATA:
			return "MCP_DATA";
		default:
			return "UNKNOWN";
		}
	case HIF_TYPE_WIM:
	case HIF_TYPE_ND_WIM:
		switch (subtype) {
		case HIF_WIM_SUB_REQUEST:
			return "REQUEST";
		case HIF_WIM_SUB_RESPONSE:
			return "RESPONSE";
		case HIF_WIM_SUB_EVENT:
			return "EVENT";
		default:
			return "UNKNOWN";
		}
	case HIF_TYPE_LOG:
		switch (subtype) {
		case HIF_LOG_SUB_VB:
			return "VB";
		case HIF_LOG_SUB_INFO:
			return "INFO";
		case HIF_LOG_SUB_ERROR:
			return "ERROR";
		case HIF_LOG_SUB_ASSERT:
			return "ASSERT";
		case HIF_LOG_SUB_NONE:
			return "NONE";
		default:
			return "UNKNOWN";
		}
	default:
		return "-";
	}
}

/* HIF Header Validation Macro - DEBUG mode only */
#if defined(DEBUG)
#define VALIDATE_HIF_HEADER(skb, location)                                    \
	do {                                                                  \
		struct hif *__hdr;                                            \
		if (!(skb) || !(skb)->data) {                                 \
			ERR_HIF("%s: NULL SKB or data!", location);           \
			break;                                                \
		}                                                             \
		if ((skb)->len < sizeof(struct hif)) {                        \
			ERR_HIF("%s: SKB too short! len=%u", location,        \
				(skb)->len);                                  \
			break;                                                \
		}                                                             \
		__hdr = (struct hif *)(skb)->data;                            \
		if (__hdr->type >= HIF_TYPE_MAX) {                            \
			ERR_HIF("%s: CORRUPTED HIF! type=%d", location,       \
				__hdr->type);                                 \
			print_hex_dump(KERN_ERR, "HIF: ", DUMP_PREFIX_OFFSET, \
				       16, 1, (skb)->data,                    \
				       min_t(int, 64, (skb)->len), true);     \
		}                                                             \
	} while (0)
#else
#define VALIDATE_HIF_HEADER(skb, location) \
	do {                               \
	} while (0)
#endif

#endif /* _NRC_HIF_H_ */
