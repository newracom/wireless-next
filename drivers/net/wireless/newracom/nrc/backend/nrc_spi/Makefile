# SPDX-License-Identifier: BSD-3-Clause-Clear

# C-SPI is the only host interface in this tree, and the backend always
# owns the hardware reset line.
ccflags-y := -DCONFIG_NRC_HIF_CSPI -DENABLE_HW_RESET

# Unique symbol prefix for this layer's debug state (see common/nrc-debug-common.h)
ccflags-y += -DNRC_DBG_PREFIX=nrc_spi_

obj-$(CONFIG_NRC7394) += nrc_spi.o

nrc_spi-y := nrc-hif-cspi.o \
	     nrc-spi-hif-ops.o \
	     nrc-spi-gpio.o \
	     nrc-debug.o \
	     nrc-spi-callback.o \
	     nrc-spi-device.o \
	     nrc-spi-params.o \
	     nrc-spi-init.o
