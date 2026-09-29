# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Newracom NRC7394 802.11ah HaLow driver
#
# The driver is split into a SPI backend, a HAL core and one or more
# frontends. Every layer talks to the others only through the headers in
# common/, so that directory is the one include path shared by all of them.

subdir-ccflags-y := -I $(srctree)/$(src)/common
subdir-ccflags-$(CONFIG_NRC7394_DEBUG) += -DDEBUG

obj-$(CONFIG_NRC7394)		+= backend/nrc_spi/
obj-$(CONFIG_NRC7394)		+= hal/nrc_core/
obj-$(CONFIG_NRC7394)		+= frontend/nrc_wlan/
obj-$(CONFIG_NRC7394_MCP)	+= frontend/nrc_mcp/
