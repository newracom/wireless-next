/* SPDX-License-Identifier: ISC */
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC MCP Log Interface - Unified debug system integration
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

#ifndef NRC_LOG_H
#define NRC_LOG_H

#include "nrc-debug.h"

/* Legacy log level definitions - for backward compatibility only */
#define LOG_LEVEL_NONE (0)
#define LOG_LEVEL_ERR (1)
#define LOG_LEVEL_WARN (2)
#define LOG_LEVEL_INFO (3)
#define LOG_LEVEL_DBG (4)

/**
 * Legacy log macros - mapped to new optimized debug macros
 *
 * These macros maintain backward compatibility while using the unified
 * nrc-debug-common infrastructure underneath.
 */
#define LOG_ERR(...) ERR_MCP(__VA_ARGS__)
#define LOG_WARN(...) WARN_MCP(__VA_ARGS__)
#define LOG_INFO(...) INFO(__VA_ARGS__)
#define LOG_WIM(...) DBG_WIM(__VA_ARGS__)

/**
 * nrc_logger_set - Legacy string-based log level control
 * @str_module: Module name (currently ignored)
 * @str_level: Log level string ("none", "err", "warn", "info", "dbg")
 *
 * Provides backward compatibility for netlink-based log control.
 */
void nrc_logger_set(const char *str_module, const char *str_level);

#endif /* NRC_LOG_H */
