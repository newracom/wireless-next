# SPDX-License-Identifier: BSD-3-Clause-Clear

# Unique symbol prefix for this layer's debug state (see common/nrc-debug-common.h)
ccflags-y += -DNRC_DBG_PREFIX=nrc_wlan_

obj-$(CONFIG_NRC7394) += nrc_wlan.o

nrc_wlan-y := nrc-mac80211.o \
	      nrc-mac80211-twt.o \
	      nrc-twt-sched.o \
	      nrc-trx.o \
	      mac80211-ext.o \
	      nrc-netlink.o \
	      nrc-s1g.o \
	      nrc-stats.o \
	      nrc-pm.o \
	      nrc-ps.o \
	      nrc-debug.o \
	      nrc-wlan-callback.o \
	      nrc-wlan-post-init.o \
	      nrc-wlan-params.o \
	      nrc-wlan-hal-init.o \
	      nrc-wlan-init.o \
	      nrc-apf.o \
	      nrc-wim-wlan.o
