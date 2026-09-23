# SPDX-License-Identifier: BSD-3-Clause-Clear

# Unique symbol prefix for this layer's debug state (see common/nrc-debug-common.h)
ccflags-y += -DNRC_DBG_PREFIX=nrc_core_

obj-$(CONFIG_NRC7394) += nrc_core.o

nrc_core-y := hif.o \
	      wim.o \
	      nrc-init.o \
	      nrc-dump.o \
	      nrc-bd.o \
	      nrc-debug.o \
	      nrc-hal-callback.o \
	      nrc-hal-ops-impl.o \
	      nrc-hal-export.o \
	      nrc-fw.o \
	      nrc-hal-init.o \
	      nrc-tx.o \
	      nrc-ps.o \
	      nrc-recovery.o
