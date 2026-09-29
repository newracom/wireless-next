// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Debug Interface - Using unified common debug system
 */

#include <linux/kernel.h>
#include <linux/string.h>

#include "nrc-log.h"

/**
 * nrc_logger_set - Set debug level using legacy string interface
 * @str_module: Module name (ignored - applies globally in MCP)
 * @str_level: Log level string ("none", "err", "warn", "info", "dbg")
 *
 * Converts legacy string-based log level control to common debug level system.
 * This maintains backward compatibility with existing netlink-based log control.
 */
void nrc_logger_set(const char *str_module, const char *str_level)
{
	unsigned long mask = DEFAULT_NRC_DBG_MASK;
	enum NRC_DEBUG_LEVEL level = DEFAULT_NRC_DBG_LEVEL;

	if (!strcmp(str_level, "none")) {
		mask = 0;
		level = NRC_DBG_LEVEL_ERR;
	} else if (!strcmp(str_level, "err")) {
		level = NRC_DBG_LEVEL_ERR;
	} else if (!strcmp(str_level, "warn")) {
		level = NRC_DBG_LEVEL_WARN;
	} else if (!strcmp(str_level, "info")) {
		level = NRC_DBG_LEVEL_INFO;
	} else if (!strcmp(str_level, "dbg")) {
		level = NRC_DBG_LEVEL_DBG;
	} else {
		return;
	}

	nrc_set_debug_mask(mask);
	nrc_dbg_set_level(level);
}
