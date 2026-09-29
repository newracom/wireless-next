# SPDX-License-Identifier: BSD-3-Clause-Clear

# Unique symbol prefix for this layer's debug state (see common/nrc-debug-common.h)
ccflags-y += -DNRC_DBG_PREFIX=nrc_mcp_

obj-$(CONFIG_NRC7394_MCP) += nrc-mcp.o

nrc-mcp-y := nrc-netlink-driver.o \
	     nrc-log.o \
	     nrc-debug.o \
	     nrc-mcp-callback.o \
	     nrc-mcp-params.o \
	     nrc-mcp-transmit.o \
	     nrc-mcp-init.o
