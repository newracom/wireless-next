/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC Power Save Definitions - PS state machine and types
 */

#ifndef _NRC_PS_COMMON_H_
#define _NRC_PS_COMMON_H_

#include <linux/spinlock.h>

struct nrc_hif_device;

enum NRC_PS_MODE {
	NRC_PS_NONE,
	NRC_PS_MODEMSLEEP,
	NRC_PS_DEEPSLEEP_TIM,
	NRC_PS_DEEPSLEEP_NONTIM,
	NRC_PS_MAX
};

enum NRC_PS_STATE {
	NRC_PS_STATE_WAKE,
	NRC_PS_STATE_SLEEPING,
	NRC_PS_STATE_SLEEP,
	NRC_PS_STATE_WAKING,
	NRC_PS_STATE_MAX,
};

/**
 * enum NRC_PS_REASON - Power save mode change reason codes
 *
 * Naming convention:
 * - MAC_*  : Requested by mac80211 subsystem (external)
 * - DRV_*  : Decided by driver logic (internal)
 * - HAL_*  : Triggered by HAL/Backend layer
 * - USER_* : User space command
 * - SYS_*  : System event
 */
enum NRC_PS_REASON {
	/* mac80211 subsystem requests */
	NRC_PS_REASON_MAC_CONFIG_PS_ENABLED = 0, /* mac80211: PS enabled */
	NRC_PS_REASON_MAC_CONFIG_PS_DISABLED, /* mac80211: PS disabled */
	NRC_PS_REASON_MAC_IDLE_ENTER, /* mac80211: Enter idle */
	NRC_PS_REASON_MAC_IDLE_EXIT, /* mac80211: Exit idle */

	/* Firmware/Target initiated */
	NRC_PS_REASON_TARGET_FW_READY, /* Target: Firmware ready interrupt from PS */
	NRC_PS_REASON_TARGET_FAILED_ENTER_PS, /* Target: Failed to enter PS, recovery needed */
	NRC_PS_REASON_TARGET_TWT_SERVICE, /* Target: TWT service period started (stay awake) */
	NRC_PS_REASON_TARGET_TWT_QUIET,   /* Target: TWT quiet period started (enter sleep) */

	/* Driver-initiated actions */
	NRC_PS_REASON_DRV_TX_WAKEUP = 10, /* Driver: TX queue wakeup */
	NRC_PS_REASON_DRV_RX_WAKEUP, /* Driver: RX packet processing wakeup */
	NRC_PS_REASON_DRV_BSS_CONFIG, /* Driver: Prepare for BSS operations (start, config, scan) */
	NRC_PS_REASON_DRV_STA_ADD, /* Driver: Station addition */
	NRC_PS_REASON_DRV_STA_REMOVE, /* Driver: Station removal */
	NRC_PS_REASON_DRV_SCAN_START, /* Driver: Start scan */
	NRC_PS_REASON_DRV_SCAN_ABORT, /* Driver: Abort scan */
	NRC_PS_REASON_DRV_ROC_START, /* Driver: ROC start */
	NRC_PS_REASON_DRV_ROC_CANCEL, /* Driver: ROC cancel */
	NRC_PS_REASON_DRV_APF_CONFIG, /* Driver: APF filter config */
	NRC_PS_REASON_DRV_POST_INIT, /* Driver: Post init sleep */
	NRC_PS_REASON_DRV_DYNAMIC_PS, /* Driver: Dynamic PS timer */

	/* HAL/Backend layer triggers */
	NRC_PS_REASON_HAL_CALLBACK =
		30, /* HAL: Generic callback (deprecated) */
	NRC_PS_REASON_HAL_PS_DYNAMIC, /* HAL: Dynamic PS work */
	NRC_PS_REASON_HAL_TX_TIMEOUT, /* HAL: TX timeout recovery */
	NRC_PS_REASON_HAL_TX_WAKEUP, /* HAL: TX data in deepsleep */
	NRC_PS_REASON_HAL_PS_RECONFIG, /* HAL: PS mode/timeout reconfig */
	NRC_PS_REASON_HAL_SHUTDOWN, /* HAL: SPI shutdown */

	/* User space commands */
	NRC_PS_REASON_USER_NETLINK_CMD = 40, /* User: Netlink command */
	NRC_PS_REASON_USER_DEBUG_WAKE, /* User: Debug wakeup */
	NRC_PS_REASON_USER_DEBUG_SLEEP, /* User: Debug sleep */

	/* System events */
	NRC_PS_REASON_SYS_SUSPEND = 50, /* System: Suspend/wowlan */

	NRC_PS_REASON_MAX
};

/* Power Save Mode Event Data */
struct nrc_ps_mode_data {
	int mode; /* Power save mode (e.g., NRC_PS_NONE) */
	int timeout; /* Timeout value in milliseconds */
	int reason; /* Reason code for PS mode change (enum NRC_PS_REASON) */
};

/**
 * enum NRC_PS_EVENT - Power save state machine events
 *
 * Events that trigger state transitions in the PS state machine.
 * All state transitions are handled through nrc_ps_handle_event().
 */
enum NRC_PS_EVENT {
	NRC_PS_EVT_SLEEP_REQ, /* Request to enter sleep mode */
	NRC_PS_EVT_WAKE_REQ, /* Request to wake up (TX/RX) */
	NRC_PS_EVT_FW_READY, /* FW signaled ready after wake */
	NRC_PS_EVT_SLEEP_DONE, /* FW entered sleep successfully */
	NRC_PS_EVT_SLEEP_FAIL, /* FW failed to enter sleep */
	NRC_PS_EVT_TIMEOUT, /* Timeout occurred */
	NRC_PS_EVT_MAX
};

/**
 * struct nrc_ps_event_data - Event data for state machine
 */
struct nrc_ps_event_data {
	enum NRC_PS_EVENT event;
	enum NRC_PS_MODE mode; /* Target mode for SLEEP_REQ */
	int timeout_ms; /* Timeout for operations */
	enum NRC_PS_REASON reason; /* Reason for state change */
};

/* PS Timing Event Structure for debugfs monitoring */
#define NRC_PS_HISTORY_SIZE 16

struct nrc_ps_timing_event {
	ktime_t timestamp; /* Event timestamp */
	enum NRC_PS_STATE state; /* State after event */
	enum NRC_PS_MODE mode; /* Mode after event */
	enum NRC_PS_REASON reason; /* Reason for event */
	u64 timeout_ms; /* Timeout value (for sleep) */
	bool is_sleep; /* true=sleep, false=wake */
};

typedef struct {
	spinlock_t lock;
	enum NRC_PS_MODE mode;
	enum NRC_PS_STATE state;
	/* Legacy power management fields (moved from hdev <- nw) */
	bool modem_enabled;
	int timeout; // hw->conf.dynamic_ps_timeout
	/*
	 * Driver-managed dynamic PS flag (mirrors ieee80211 SUPPORTS_DYNAMIC_PS)
	 * Set for every PS mode (NRC_PS_PER_MODE_DYN restricts it to deep-sleep
	 * modes).
	 * When set, driver uses its own timer for PS management instead of mac80211.
	 */
	bool supports_dynamic_ps;
	/* SLEEPING state timeout detection */
	unsigned long sleeping_start_jiffies;
	/* State machine pending wake flag for atomic context */
	bool wake_pending;
	/* Pending wake reason for tracking user-initiated wakes */
	enum NRC_PS_REASON pending_wake_reason;

	/* PS Timing tracking for debugfs */
	struct nrc_ps_timing_event history[NRC_PS_HISTORY_SIZE];
	int history_idx; /* Next write index (circular) */
	int history_count; /* Total events recorded */
	ktime_t last_sleep_time; /* Last sleep request time */
	ktime_t last_wake_time; /* Last wake complete time */
	u64 last_sleep_timeout_ms; /* Last sleep timeout value */
	enum NRC_PS_REASON last_sleep_reason;
	enum NRC_PS_REASON last_wake_reason;
} nrc_ps_t;

static inline const char *nrc_ps_event_str(enum NRC_PS_EVENT event)
{
	static const char *const str[] = {"SLEEP_REQ",	"WAKE_REQ",
					  "FW_READY",	"SLEEP_DONE",
					  "SLEEP_FAIL", "TIMEOUT"};
	if (event >= NRC_PS_EVT_MAX)
		return "UNKNOWN";
	return str[event];
}

static inline void nrc_ps_lock_init(nrc_ps_t *ps)
{
	if (ps) {
		spin_lock_init(&ps->lock);
	}
}

/*
 * Locking rules for ps->lock:
 *
 * ALL acquisitions of ps->lock MUST use spin_lock_irqsave() /
 * spin_unlock_irqrestore(). Do NOT use spin_lock_bh() on this lock.
 *
 * nrc_ps_handle_event() and nrc_ps_record_event() can be called from
 * softirq (BH) context. If spin_lock_bh() were used on the same lock
 * object while spin_lock_irqsave() is also used, a deadlock results
 * when one CPU holds the lock with spin_lock_bh() and a (soft)IRQ fires
 * on that same CPU trying to acquire with spin_lock_irqsave().
 *
 * For read-only state queries use READ_ONCE(ps->state) directly; the
 * state field is word-sized and naturally atomic on all supported arches.
 */

/* String conversion functions */
static inline const char *nrc_ps_mode_str(enum NRC_PS_MODE mode)
{
	static const char *const str[] = {"NONE", "MODEMSLEEP", "DEEPSLEEP_TIM",
					  "DEEPSLEEP_NONTIM"};
	if (mode >= NRC_PS_MAX)
		return "UNKNOWN";
	return str[mode];
}

static inline const char *nrc_ps_state_str(enum NRC_PS_STATE state)
{
	static const char *const str[] = {"WAKE", "SLEEPING", "SLEEP",
					  "WAKING"};
	if (state >= NRC_PS_STATE_MAX)
		return "UNKNOWN";
	return str[state];
}

static inline const char *nrc_ps_reason_str(enum NRC_PS_REASON reason)
{
	switch (reason) {
	case NRC_PS_REASON_MAC_CONFIG_PS_ENABLED:
		return "MAC_CONFIG_PS_ENABLED";
	case NRC_PS_REASON_MAC_CONFIG_PS_DISABLED:
		return "MAC_CONFIG_PS_DISABLED";
	case NRC_PS_REASON_MAC_IDLE_ENTER:
		return "MAC_IDLE_ENTER";
	case NRC_PS_REASON_MAC_IDLE_EXIT:
		return "MAC_IDLE_EXIT";
	case NRC_PS_REASON_TARGET_FW_READY:
		return "TARGET_FW_READY";
	case NRC_PS_REASON_TARGET_FAILED_ENTER_PS:
		return "TARGET_FAILED_ENTER_PS";
	case NRC_PS_REASON_TARGET_TWT_SERVICE:
		return "TARGET_TWT_SERVICE";
	case NRC_PS_REASON_TARGET_TWT_QUIET:
		return "TARGET_TWT_QUIET";
	case NRC_PS_REASON_DRV_TX_WAKEUP:
		return "DRV_TX_WAKEUP";
	case NRC_PS_REASON_DRV_RX_WAKEUP:
		return "DRV_RX_WAKEUP";
	case NRC_PS_REASON_DRV_BSS_CONFIG:
		return "DRV_BSS_CONFIG";
	case NRC_PS_REASON_DRV_STA_ADD:
		return "DRV_STA_ADD";
	case NRC_PS_REASON_DRV_STA_REMOVE:
		return "DRV_STA_REMOVE";
	case NRC_PS_REASON_DRV_SCAN_START:
		return "DRV_SCAN_START";
	case NRC_PS_REASON_DRV_SCAN_ABORT:
		return "DRV_SCAN_ABORT";
	case NRC_PS_REASON_DRV_ROC_START:
		return "DRV_ROC_START";
	case NRC_PS_REASON_DRV_ROC_CANCEL:
		return "DRV_ROC_CANCEL";
	case NRC_PS_REASON_DRV_APF_CONFIG:
		return "DRV_APF_CONFIG";
	case NRC_PS_REASON_DRV_POST_INIT:
		return "DRV_POST_INIT";
	case NRC_PS_REASON_DRV_DYNAMIC_PS:
		return "DRV_DYNAMIC_PS";
	case NRC_PS_REASON_HAL_CALLBACK:
		return "HAL_CALLBACK";
	case NRC_PS_REASON_HAL_PS_DYNAMIC:
		return "HAL_PS_DYNAMIC";
	case NRC_PS_REASON_HAL_TX_TIMEOUT:
		return "HAL_TX_TIMEOUT";
	case NRC_PS_REASON_HAL_TX_WAKEUP:
		return "HAL_TX_WAKEUP";
	case NRC_PS_REASON_HAL_PS_RECONFIG:
		return "HAL_PS_RECONFIG";
	case NRC_PS_REASON_HAL_SHUTDOWN:
		return "HAL_SHUTDOWN";
	case NRC_PS_REASON_USER_NETLINK_CMD:
		return "USER_NETLINK_CMD";
	case NRC_PS_REASON_USER_DEBUG_WAKE:
		return "USER_DEBUG_WAKE";
	case NRC_PS_REASON_USER_DEBUG_SLEEP:
		return "USER_DEBUG_SLEEP";
	case NRC_PS_REASON_SYS_SUSPEND:
		return "SYS_SUSPEND";
	default:
		return "UNKNOWN";
	}
}

/**
 * nrc_ps_get_state - Get current power save state (lockless)
 * @ps: Power save structure pointer
 *
 * Returns the current PS state using READ_ONCE() for a safe, lockless
 * snapshot. The state field is word-sized and naturally atomic on all
 * supported architectures.  Callers must not assume the value remains
 * stable after return; they should re-check inside the lock if they need
 * to act on an exact state.
 */
static inline enum NRC_PS_STATE nrc_ps_get_state(nrc_ps_t *ps)
{
	if (ps)
		return READ_ONCE(ps->state);
	return NRC_PS_STATE_WAKE;
}

/**
 * nrc_ps_set_state - Set power save state (IRQ-safe)
 * @ps: Power save structure pointer
 * @state: New power save state
 *
 * Acquires ps->lock with spin_lock_irqsave() to be safe for call sites
 * that may run in softirq or process context.  Must NOT be called while
 * ps->lock is already held — use WRITE_ONCE(ps->state, ...) directly
 * inside the already-locked region instead.
 */
static inline void nrc_ps_set_state(nrc_ps_t *ps, enum NRC_PS_STATE state)
{
	unsigned long flags;

	if (ps) {
		spin_lock_irqsave(&ps->lock, flags);
		ps->state = state;
		if (state == NRC_PS_STATE_WAKE) {
			/* from interrupt wake up, not gpio */
			ps->mode = NRC_PS_NONE;
		} else if (state == NRC_PS_STATE_SLEEPING) {
			/* Record timestamp when entering SLEEPING state */
			ps->sleeping_start_jiffies = jiffies;
		}
		spin_unlock_irqrestore(&ps->lock, flags);
	}
}

/**
 * nrc_ps_check_sleeping_timeout - Check if SLEEPING state timeout has occurred
 * @ps: Power save structure pointer
 * @timeout_ms: Timeout value in milliseconds
 *
 * Returns: true if timeout occurred and state was reset, false otherwise
 *
 * Acquires ps->lock with spin_lock_irqsave() — consistent with
 * nrc_ps_handle_event() — so it is safe to call from any context.
 */
static inline bool nrc_ps_check_sleeping_timeout(nrc_ps_t *ps,
						 unsigned int timeout_ms)
{
	unsigned long flags;
	bool timeout_occurred = false;

	if (ps) {
		spin_lock_irqsave(&ps->lock, flags);
		if (ps->state == NRC_PS_STATE_SLEEPING) {
			unsigned long elapsed_ms = jiffies_to_msecs(
				jiffies - ps->sleeping_start_jiffies);

			if (elapsed_ms > timeout_ms) {
				/* Timeout detected - force WAKE state */
				ps->state = NRC_PS_STATE_WAKE;
				ps->mode = NRC_PS_NONE;
				timeout_occurred = true;
			}
		}
		spin_unlock_irqrestore(&ps->lock, flags);
	}

	return timeout_occurred;
}

#endif /* _NRC_PS_COMMON_H_ */
