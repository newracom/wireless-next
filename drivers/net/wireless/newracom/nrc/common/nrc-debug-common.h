/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC Debug Interface - Unified debug macros for all modules
 */

#ifndef _NRC_DEBUG_COMMON_H_
#define _NRC_DEBUG_COMMON_H_

/* Linux kernel headers */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/kernel.h>

/* Forward declarations */
struct hif;

/* Debug level enum - priority order from highest to lowest */
enum NRC_DEBUG_LEVEL {
	NRC_DBG_LEVEL_ERR = 0, /* Error messages - always shown */
	NRC_DBG_LEVEL_WARN = 1, /* Warning messages */
	NRC_DBG_LEVEL_INFO = 2, /* Information messages */
	NRC_DBG_LEVEL_DBG = 3, /* Debug messages - only in DEBUG builds */
	NRC_DBG_LEVEL_MAX
};

/* Default debug level: INFO in production, DBG when DEBUG is defined */
#if defined(DEBUG)
#define DEFAULT_NRC_DBG_LEVEL NRC_DBG_LEVEL_DBG
#else
#define DEFAULT_NRC_DBG_LEVEL NRC_DBG_LEVEL_INFO
#endif

/* Common debug masks for all modules - category filtering */
enum NRC_DEBUG_MASK {
	NRC_DBG_BASIC = 0,
	NRC_DBG_HIF = 1,
	NRC_DBG_WIM = 2,
	NRC_DBG_TX = 3,
	NRC_DBG_RX = 4,
	NRC_DBG_MAC = 5,
	NRC_DBG_CAPI = 6,
	NRC_DBG_PS = 7,
	NRC_DBG_STATS = 8,
	NRC_DBG_STATE = 9,
	NRC_DBG_BD = 10,
	NRC_DBG_FW = 11,
	NRC_DBG_AMPDU = 12,
	NRC_DBG_CREDIT = 13,
	NRC_DBG_SLOT = 14,
	NRC_DBG_BUS = 15,
};

#define NRC_DBG_MASK_ANY (0xFFFFFFFF)
#define DEFAULT_NRC_DBG_MASK_ALL (NRC_DBG_MASK_ANY)
#define DEFAULT_NRC_DBG_MASK (BIT(NRC_DBG_BASIC) | BIT(NRC_DBG_STATE))

/* Category name mapping - must match enum NRC_DEBUG_MASK order */
static const char *const nrc_debug_category_names[] = {
	[NRC_DBG_BASIC] = "Dbg",   [NRC_DBG_HIF] = "Hif",
	[NRC_DBG_WIM] = "Wim",	   [NRC_DBG_TX] = "Tx",
	[NRC_DBG_RX] = "Rx",	   [NRC_DBG_MAC] = "Mac",
	[NRC_DBG_CAPI] = "Capi",   [NRC_DBG_PS] = "Ps",
	[NRC_DBG_STATS] = "Stats", [NRC_DBG_STATE] = "State",
	[NRC_DBG_BD] = "Bd",	   [NRC_DBG_FW] = "Fw",
	[NRC_DBG_AMPDU] = "Ampdu", [NRC_DBG_CREDIT] = "Credit",
	[NRC_DBG_SLOT] = "Slot",   [NRC_DBG_BUS] = "Bus",
};

/* Debug print flags - can be overridden per module */
#ifndef NRC_DBG_PRINT_FRAME_TX
#define NRC_DBG_PRINT_FRAME_TX 0
#endif

#ifndef NRC_DBG_PRINT_FRAME_RX
#define NRC_DBG_PRINT_FRAME_RX 0
#endif

#ifndef NRC_DBG_PRINT_ARP_FRAME
#define NRC_DBG_PRINT_ARP_FRAME 0
#endif

/* Category token to bitmask converter helper */
#define CAT(c) BIT(NRC_DBG_##c)

/* General debug macro - multi-mask support */
#define DBG(masks, fmt, ...) \
	nrc_dbg_level_multi(NRC_DBG_LEVEL_DBG, masks, fmt, ##__VA_ARGS__)

/* Level-based debug macros with category prefix */
/* DBG level macros - detailed debug information (only in DEBUG builds) */
/* Category prefix is automatically added by nrc_dbg_level_multi */
#define DBG_HIF(fmt, ...) DBG(CAT(HIF), fmt, ##__VA_ARGS__)
#define DBG_WIM(fmt, ...) DBG(CAT(WIM), fmt, ##__VA_ARGS__)
#define DBG_TX(fmt, ...) DBG(CAT(TX), fmt, ##__VA_ARGS__)
#define DBG_RX(fmt, ...) DBG(CAT(RX), fmt, ##__VA_ARGS__)
#define DBG_MAC(fmt, ...) DBG(CAT(MAC), fmt, ##__VA_ARGS__)
#define DBG_CAPI(fmt, ...) DBG(CAT(CAPI), fmt, ##__VA_ARGS__)
#define DBG_PS(fmt, ...) DBG(CAT(PS), fmt, ##__VA_ARGS__)
#define DBG_STATS(fmt, ...) DBG(CAT(STATS), fmt, ##__VA_ARGS__)
#define DBG_STATE(fmt, ...) DBG(CAT(STATE), fmt, ##__VA_ARGS__)
#define DBG_BD(fmt, ...) DBG(CAT(BD), fmt, ##__VA_ARGS__)
#define DBG_FW(fmt, ...) DBG(CAT(FW), fmt, ##__VA_ARGS__)
#define DBG_AMPDU(fmt, ...) DBG(CAT(AMPDU), fmt, ##__VA_ARGS__)
#define DBG_CREDIT(fmt, ...) DBG(CAT(CREDIT), fmt, ##__VA_ARGS__)
#define DBG_SLOT(fmt, ...) DBG(CAT(SLOT), fmt, ##__VA_ARGS__)
#define DBG_BUS(fmt, ...) DBG(CAT(BUS), fmt, ##__VA_ARGS__)

/* INFO level macros - informational messages (shown by default) */
#define INFO(fmt, ...)                                                \
	nrc_dbg_level(NRC_DBG_LEVEL_INFO, NRC_DBG_BASIC, "Info " fmt, \
		      ##__VA_ARGS__)

/* Category-based helper macros using category name array */
#define INFO_CAT(c, fmt, ...)                                                 \
	nrc_dbg_info("Info [%s] " fmt, nrc_debug_category_names[NRC_DBG_##c], \
		     ##__VA_ARGS__)

#define WARN_CAT(c, fmt, ...)                                         \
	nrc_dbg_warn("Warning [%s] %s:%d " fmt,                       \
		     nrc_debug_category_names[NRC_DBG_##c], __func__, \
		     __LINE__, ##__VA_ARGS__)

#define ERR_CAT(c, fmt, ...)                                                   \
	nrc_dbg_err("Error [%s] %s:%d " fmt,                                   \
		    nrc_debug_category_names[NRC_DBG_##c], __func__, __LINE__, \
		    ##__VA_ARGS__)

/* Legacy string-based macros - deprecated, use _CAT versions */
#define INFo(category, fmt, ...) \
	nrc_dbg_info("Info [" category "] " fmt "", ##__VA_ARGS__)

#define WARn(category, fmt, ...)                                       \
	nrc_dbg_warn("Warning [" category "] %s:%d " fmt "", __func__, \
		     __LINE__, ##__VA_ARGS__)

#define ERR(category, fmt, ...)                                               \
	nrc_dbg_err("Error [" category "] %s:%d " fmt "", __func__, __LINE__, \
		    ##__VA_ARGS__)

/* Category-specific info macros - Common categories only */
#define INFO_WIM(fmt, ...) INFO_CAT(WIM, fmt, ##__VA_ARGS__)
#define INFO_HIF(fmt, ...) INFO_CAT(HIF, fmt, ##__VA_ARGS__)
#define INFO_BD(fmt, ...) INFO_CAT(BD, fmt, ##__VA_ARGS__)
#define INFO_FW(fmt, ...) INFO_CAT(FW, fmt, ##__VA_ARGS__)
#define INFO_PS(fmt, ...) INFO_CAT(PS, fmt, ##__VA_ARGS__)
#define INFO_TX(fmt, ...) INFO_CAT(TX, fmt, ##__VA_ARGS__)
#define INFO_RX(fmt, ...) INFO_CAT(RX, fmt, ##__VA_ARGS__)
#define INFO_MAC(fmt, ...) INFO_CAT(MAC, fmt, ##__VA_ARGS__)
#define INFO_CAPI(fmt, ...) INFO_CAT(CAPI, fmt, ##__VA_ARGS__)
#define INFO_STATS(fmt, ...) INFO_CAT(STATS, fmt, ##__VA_ARGS__)
#define INFO_STATE(fmt, ...) INFO_CAT(STATE, fmt, ##__VA_ARGS__)
#define INFO_AMPDU(fmt, ...) INFO_CAT(AMPDU, fmt, ##__VA_ARGS__)
#define INFO_CREDIT(fmt, ...) INFO_CAT(CREDIT, fmt, ##__VA_ARGS__)
#define INFO_SLOT(fmt, ...) INFO_CAT(SLOT, fmt, ##__VA_ARGS__)
#define INFO_BUS(fmt, ...) INFO_CAT(BUS, fmt, ##__VA_ARGS__)

/* Category-specific warning macros - Common categories only */
#define WARN_WIM(fmt, ...) WARN_CAT(WIM, fmt, ##__VA_ARGS__)
#define WARN_HIF(fmt, ...) WARN_CAT(HIF, fmt, ##__VA_ARGS__)
#define WARN_BD(fmt, ...) WARN_CAT(BD, fmt, ##__VA_ARGS__)
#define WARN_FW(fmt, ...) WARN_CAT(FW, fmt, ##__VA_ARGS__)
#define WARN_PS(fmt, ...) WARN_CAT(PS, fmt, ##__VA_ARGS__)
#define WARN_TX(fmt, ...) WARN_CAT(TX, fmt, ##__VA_ARGS__)
#define WARN_RX(fmt, ...) WARN_CAT(RX, fmt, ##__VA_ARGS__)
#define WARN_MAC(fmt, ...) WARN_CAT(MAC, fmt, ##__VA_ARGS__)
#define WARN_CAPI(fmt, ...) WARN_CAT(CAPI, fmt, ##__VA_ARGS__)
#define WARN_STATS(fmt, ...) WARN_CAT(STATS, fmt, ##__VA_ARGS__)
#define WARN_STATE(fmt, ...) WARN_CAT(STATE, fmt, ##__VA_ARGS__)
#define WARN_AMPDU(fmt, ...) WARN_CAT(AMPDU, fmt, ##__VA_ARGS__)
#define WARN_CREDIT(fmt, ...) WARN_CAT(CREDIT, fmt, ##__VA_ARGS__)
#define WARN_SLOT(fmt, ...) WARN_CAT(SLOT, fmt, ##__VA_ARGS__)
#define WARN_BUS(fmt, ...) WARN_CAT(BUS, fmt, ##__VA_ARGS__)

/* Category-specific error macros - Common categories only */
#define ERR_WIM(fmt, ...) ERR_CAT(WIM, fmt, ##__VA_ARGS__)
#define ERR_HIF(fmt, ...) ERR_CAT(HIF, fmt, ##__VA_ARGS__)
#define ERR_BD(fmt, ...) ERR_CAT(BD, fmt, ##__VA_ARGS__)
#define ERR_FW(fmt, ...) ERR_CAT(FW, fmt, ##__VA_ARGS__)
#define ERR_PS(fmt, ...) ERR_CAT(PS, fmt, ##__VA_ARGS__)
#define ERR_TX(fmt, ...) ERR_CAT(TX, fmt, ##__VA_ARGS__)
#define ERR_RX(fmt, ...) ERR_CAT(RX, fmt, ##__VA_ARGS__)
#define ERR_MAC(fmt, ...) ERR_CAT(MAC, fmt, ##__VA_ARGS__)
#define ERR_CAPI(fmt, ...) ERR_CAT(CAPI, fmt, ##__VA_ARGS__)
#define ERR_STATS(fmt, ...) ERR_CAT(STATS, fmt, ##__VA_ARGS__)
#define ERR_STATE(fmt, ...) ERR_CAT(STATE, fmt, ##__VA_ARGS__)
#define ERR_AMPDU(fmt, ...) ERR_CAT(AMPDU, fmt, ##__VA_ARGS__)
#define ERR_CREDIT(fmt, ...) ERR_CAT(CREDIT, fmt, ##__VA_ARGS__)
#define ERR_SLOT(fmt, ...) ERR_CAT(SLOT, fmt, ##__VA_ARGS__)
#define ERR_BUS(fmt, ...) ERR_CAT(BUS, fmt, ##__VA_ARGS__)

/* MAC address formatting macros */
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

/* Global debug variables - each module should define these */
extern unsigned long debug_mask;
extern int debug_level;
extern struct device *g_dev;

/* Core debug functions - inline implementations for common use */
static inline void nrc_dbg_init(struct device *dev)
{
	/* Module parameters (debug_level, debug_mask) are already set during insmod */
	g_dev = dev;
}

static inline void nrc_dbg_enable(enum NRC_DEBUG_MASK mk)
{
	set_bit(mk, &debug_mask);
}

static inline void nrc_dbg_disable(enum NRC_DEBUG_MASK mk)
{
	clear_bit(mk, &debug_mask);
}

static inline void nrc_dbg_set_level(enum NRC_DEBUG_LEVEL level)
{
	if (level < NRC_DBG_LEVEL_MAX)
		debug_level = level;
}

static inline enum NRC_DEBUG_LEVEL nrc_dbg_get_level(void)
{
	return debug_level;
}

static inline void nrc_hal_set_debug_mask(unsigned long mask)
{
	debug_mask = mask;
}

static inline void nrc_set_debug_mask(unsigned long mask)
{
	debug_mask = mask;
}

/* Warning function - shown based on level, no mask check */
static inline void nrc_dbg_warn(const char *fmt, ...)
{
	va_list args;
	int i;
	static char buf[512] = {
		0,
	};

	/* WARN level messages: only check if level allows WARN */
	if (NRC_DBG_LEVEL_WARN > debug_level)
		return;

	/* No category mask check for warnings - they should be shown based on level only */

	va_start(args, fmt);
	if (fmt != NULL) {
		i = vsnprintf(buf, sizeof(buf), fmt, args);
	} else {
		strcpy(buf, "Format string is NULL !!!");
		i = strlen(buf);
	}
	va_end(args);

	if (g_dev == NULL)
		pr_warn("%s\n", buf); /* Use pr_warn for warnings */
	else
		dev_warn(g_dev, "%s\n", buf); /* Use dev_warn for warnings */
}

/* Info function - shown based on level, no mask check */
static inline void nrc_dbg_info(const char *fmt, ...)
{
	va_list args;
	int i;
	static char buf[512] = {
		0,
	};

	/* INFO level messages: only check if level allows INFO */
	if (NRC_DBG_LEVEL_INFO > debug_level)
		return;

	/* No category mask check for info - they should be shown based on level only */

	va_start(args, fmt);
	if (fmt != NULL) {
		i = vsnprintf(buf, sizeof(buf), fmt, args);
	} else {
		strcpy(buf, "Format string is NULL !!!");
		i = strlen(buf);
	}
	va_end(args);

	if (g_dev == NULL)
		pr_info("%s\n", buf); /* Use pr_info for info */
	else
		dev_info(g_dev, "%s\n", buf); /* Use dev_info for info */
}

/* Error function - always shown, no mask check (only level check) */
static inline void nrc_dbg_err(const char *fmt, ...)
{
	va_list args;
	int i;
	static char buf[512] = {
		0,
	};

	/* ERR level messages: only check if level allows ERR (should always pass) */
	if (NRC_DBG_LEVEL_ERR > debug_level)
		return;

	/* No category mask check for errors - they should always be shown */

	va_start(args, fmt);
	if (fmt != NULL) {
		i = vsnprintf(buf, sizeof(buf), fmt, args);
	} else {
		strcpy(buf, "Format string is NULL !!!");
		i = strlen(buf);
	}
	va_end(args);

	if (g_dev == NULL)
		pr_err("%s\n", buf); /* Use pr_err for errors */
	else
		dev_err(g_dev, "%s\n", buf); /* Use dev_err for errors */
}

/* Main nrc_dbg_level function - with level and category filtering */
static inline void nrc_dbg_level(enum NRC_DEBUG_LEVEL level,
				 enum NRC_DEBUG_MASK mk, const char *fmt, ...)
{
	va_list args;
	int i;
	static char buf[512] = {
		0,
	};

	/* Check debug level first - skip if message level is higher than current level */
	if (level > debug_level)
		return;

	/* Then check category mask */
	if (!test_bit(mk, &debug_mask))
		return;

	va_start(args, fmt);
	if (fmt != NULL) {
		i = vsnprintf(buf, sizeof(buf), fmt, args);
	} else {
		strcpy(buf, "Format string is NULL !!!");
		i = strlen(buf);
	}
	va_end(args);

	if (g_dev == NULL)
		pr_info("%s\n", buf);
	else
		dev_info(g_dev, "%s\n", buf);
}

/* Multi-mask debug function - allows multiple category masks */
static inline void nrc_dbg_level_multi(enum NRC_DEBUG_LEVEL level,
				       unsigned long masks, const char *fmt,
				       ...)
{
	va_list args;
	int i, pos = 0;
	static char buf[512] = {
		0,
	};
	char prefix[64] = {0};
	bool matched = false;
	int count = 0;

	/* Check debug level first */
	if (level > debug_level)
		return;

	/* Check if ANY of the provided masks are enabled */
	for (i = 0; i < 32; i++) {
		if ((masks & BIT(i)) && test_bit(i, &debug_mask)) {
			matched = true;
			break;
		}
	}

	if (!matched)
		return;

	/* Build prefix from all masks in the combination - loop-based approach */
	for (i = 0; i < ARRAY_SIZE(nrc_debug_category_names); i++) {
		if (masks & BIT(i)) {
			pos += snprintf(prefix + pos, sizeof(prefix) - pos,
					"%s%s", count ? "/" : "",
					nrc_debug_category_names[i]);
			count++;
		}
	}

	/* Add trailing space */
	if (count > 0)
		snprintf(prefix + pos, sizeof(prefix) - pos, " ");

	/* Format message with prefix */
	va_start(args, fmt);
	if (fmt != NULL) {
		i = snprintf(buf, sizeof(buf), "%s", prefix);
		vsnprintf(buf + i, sizeof(buf) - i, fmt, args);
	} else {
		strcpy(buf, "Format string is NULL !!!");
	}
	va_end(args);

	if (g_dev == NULL)
		pr_info("%s\n", buf);
	else
		dev_info(g_dev, "%s\n", buf);
}

/* Loopback debug */
struct lb_time_info {
	int _i;
	s64 _txt;
	s64 _rxt;
};

/* All debug variables are now in struct nrc_debug - access via nw->debug->variable */

enum LOOPBACK_MODE {
	LOOPBACK_MODE_ROUNDTRIP,
	LOOPBACK_MODE_TX_ONLY,
	LOOPBACK_MODE_RX_ONLY,
	LOOPBACK_MODE_MAX
};

/* VALIDATE_HIF_HEADER macro is defined in nrc-hif.h (requires struct hif, HIF_TYPE_MAX) */

#endif /* _NRC_DEBUG_COMMON_H_ */
