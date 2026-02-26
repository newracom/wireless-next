/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Debug Implementation
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

/* Linux kernel headers */
#include <linux/debugfs.h>
#include <linux/math64.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

/* Linux networking headers */
#include <net/mac80211.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-wim-types.h"

/* Local module headers */
#include "nrc-mac80211.h"
#include "nrc-mac80211-twt.h"
#if defined(CONFIG_S1G_CHANNEL)
#include "nrc-s1g.h"
#endif
#include "nrc-hal-core-interface.h"
#include "nrc-stats.h"
#include "nrc-debug.h"
#include "nrc-apf.h"
#include "nrc-ps.h"

/* Global debug variables - defined as module parameters in nrc-wlan-params.c */
extern unsigned long debug_mask;
extern int debug_level;
struct device *g_dev;

/* Note: Common debugfs entries moved to nrc_core module:
 *   - nrc_credit, nrc_debug, nrc_cspi, nrc_hif, nrc_reset, nrc_restart
 */

/* Debugfs */

#ifdef CONFIG_DEBUG_FS
/* WLAN module-specific debug mask control */
static int nrc_wlan_debugfs_debug_read(void *data, u64 *val)
{
	*val = debug_mask;
	return 0;
}

static int nrc_wlan_debugfs_debug_write(void *data, u64 val)
{
	debug_mask = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_wlan_debugfs_debug_fops,
			nrc_wlan_debugfs_debug_read,
			nrc_wlan_debugfs_debug_write, "%llu\n");

/* WLAN module-specific debug level control */
static int nrc_wlan_debugfs_level_read(void *data, u64 *val)
{
	*val = debug_level;
	return 0;
}

static int nrc_wlan_debugfs_level_write(void *data, u64 val)
{
	if (val < NRC_DBG_LEVEL_MAX)
		debug_level = (enum NRC_DEBUG_LEVEL)val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_wlan_debugfs_level_fops,
			nrc_wlan_debugfs_level_read,
			nrc_wlan_debugfs_level_write, "%llu\n");

static int nrc_debugfs_wakeup_device_read(void *data, u64 *val)
{
	*val = 0;
	return 0;
}

static int nrc_debugfs_wakeup_device_write(void *data, u64 val)
{
	struct nrc *nw = (struct nrc *)data;

	nrc_ps_set_mode(nw, NRC_PS_NONE, val, NULL,
			NRC_PS_REASON_USER_DEBUG_WAKE);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_wakeup_device,
			nrc_debugfs_wakeup_device_read,
			nrc_debugfs_wakeup_device_write, "%llu\n");

static int nrc_debugfs_sleep_device_read(void *data, u64 *val)
{
	*val = 0;
	return 0;
}

static int nrc_debugfs_sleep_device_write(void *data, u64 val)
{
	struct nrc *nw = (struct nrc *)data;

	nrc_ps_set_mode(nw, NRC_PS_DEEPSLEEP_NONTIM, val, NULL,
			NRC_PS_REASON_USER_DEBUG_SLEEP);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_sleep_device, nrc_debugfs_sleep_device_read,
			nrc_debugfs_sleep_device_write, "%llu\n");

/* Note: nrc_reset and nrc_restart moved to nrc_core module */

static int nrc_debugfs_snr_read(void *data, u64 *val)
{
	*val = nrc_stats_snr();
	return 0;
}

static int nrc_debugfs_snr_write(void *data, u64 val)
{
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_snr, nrc_debugfs_snr_read,
			nrc_debugfs_snr_write, "%llu\n");

static int nrc_debugfs_rssi_read(void *data, u64 *val)
{
	*val = nrc_stats_rssi();
	return 0;
}

static int nrc_debugfs_rssi_write(void *data, u64 val)
{
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_rssi, nrc_debugfs_rssi_read,
			nrc_debugfs_rssi_write, "%lld\n");

static int nrc_debugfs_beacon_updated_read(void *data, u64 *val)
{
	struct nrc *nw = (struct nrc *)data;
	*val = nw->debug->g_nrc_beacon_updated; //nrc_get_beacon_updated();
	return 0;
}

static int nrc_debugfs_beacon_updated_write(void *data, u64 val)
{
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_beacon_updated,
			nrc_debugfs_beacon_updated_read,
			nrc_debugfs_beacon_updated_write, "%lld\n");

/* WLAN Interface Information */
static int nrc_debugfs_wlan_info_show(struct seq_file *s, void *unused)
{
	struct nrc *nw = s->private;
	struct ieee80211_vif *vif;
	int i;
	const char *iftype_str = "UNKNOWN";

	if (!nw) {
		seq_printf(s, "ERROR: Invalid device\n");
		return 0;
	}

	seq_printf(s, "=== WLAN Interface Information ===\n\n");

	/* Iterate through all VIFs */
	for (i = 0; i < NR_NRC_VIF; i++) {
		vif = nw->vif[i];
		if (!vif)
			continue;

		/* Get interface type string */
		switch (vif->type) {
		case NL80211_IFTYPE_STATION:
			iftype_str = "STATION";
			break;
		case NL80211_IFTYPE_AP:
			iftype_str = "AP";
			break;
		case NL80211_IFTYPE_ADHOC:
			iftype_str = "ADHOC";
			break;
		case NL80211_IFTYPE_MESH_POINT:
			iftype_str = "MESH_POINT";
			break;
		default:
			iftype_str = "UNKNOWN";
			break;
		}

		seq_printf(s, "Interface %d:\n", i);
		seq_printf(s, "  Mode: %s\n", iftype_str);
		seq_printf(s, "  MAC Address: %pM\n", vif->addr);

		/* Channel information */
#if KERNEL_VERSION(6, 9, 0) <= NRC_TARGET_KERNEL_VERSION
		if (vif->bss_conf.chanreq.oper.chan) {
			int hw_value =
				vif->bss_conf.chanreq.oper.chan->hw_value;
			int center_freq =
				vif->bss_conf.chanreq.oper.chan->center_freq;

			/* 2.4GHz/5GHz channel information */
			seq_printf(s, "  2.4/5GHz Channel: %d\n", hw_value);
			seq_printf(s, "  2.4/5GHz Center Frequency: %d MHz\n",
				   center_freq);
			seq_printf(s, "  Channel Width: ");
			switch (vif->bss_conf.chanreq.oper.width) {
#else
		if (vif->bss_conf.chandef.chan) {
			int hw_value = vif->bss_conf.chandef.chan->hw_value;
			int center_freq =
				vif->bss_conf.chandef.chan->center_freq;

			/* 2.4GHz/5GHz channel information */
			seq_printf(s, "  2.4/5GHz Channel: %d\n", hw_value);
			seq_printf(s, "  2.4/5GHz Center Frequency: %d MHz\n",
				   center_freq);
			seq_printf(s, "  Channel Width: ");
			switch (vif->bss_conf.chandef.width) {
#endif
			case NL80211_CHAN_WIDTH_20:
				seq_printf(s, "20 MHz\n");
				break;
			case NL80211_CHAN_WIDTH_40:
				seq_printf(s, "40 MHz\n");
				break;
			case NL80211_CHAN_WIDTH_80:
				seq_printf(s, "80 MHz\n");
				break;
			case NL80211_CHAN_WIDTH_160:
				seq_printf(s, "160 MHz\n");
				break;
#if defined(CONFIG_S1G_CHANNEL)
			case NL80211_CHAN_WIDTH_1:
				seq_printf(s, "1 MHz (S1G)\n");
				break;
			case NL80211_CHAN_WIDTH_2:
				seq_printf(s, "2 MHz (S1G)\n");
				break;
			case NL80211_CHAN_WIDTH_4:
				seq_printf(s, "4 MHz (S1G)\n");
				break;
			case NL80211_CHAN_WIDTH_8:
				seq_printf(s, "8 MHz (S1G)\n");
				break;
			case NL80211_CHAN_WIDTH_16:
				seq_printf(s, "16 MHz (S1G)\n");
				break;
#endif
			default:
				seq_printf(s, "Unknown\n");
				break;
			}

#if defined(CONFIG_S1G_CHANNEL)
			/* S1G channel mapping information */
			{
				int s1g_freq =
					nrc_get_s1g_freq_by_arr_idx(hw_value);
				int s1g_width =
					nrc_get_s1g_width_by_arr_idx(hw_value);
				uint8_t s1g_ch_idx =
					nrc_get_channel_idx_by_freq(s1g_freq);
				char *country = nrc_get_current_s1g_country();

				seq_printf(s, "  --- Mapped S1G Info ---\n");
				seq_printf(s, "  S1G Country: %s\n",
					   country ? country : "Unknown");
				seq_printf(s, "  S1G Frequency: %d.%d MHz\n",
					   s1g_freq / 10, s1g_freq % 10);
				seq_printf(s, "  S1G Channel Index: %d\n",
					   s1g_ch_idx);
				seq_printf(s, "  S1G Bandwidth: %d MHz\n",
					   s1g_width);
			}
#endif
		} else {
			seq_printf(s, "  Channel: Not configured\n");
		}

		/* Connection status for STA mode */
		if (vif->type == NL80211_IFTYPE_STATION) {
#ifdef CONFIG_USE_VIF_CFG
			seq_printf(s, "  Associated: %s\n",
				   vif->cfg.assoc ? "Yes" : "No");
			if (vif->cfg.assoc) {
#else
			seq_printf(s, "  Associated: %s\n",
				   vif->bss_conf.assoc ? "Yes" : "No");
			if (vif->bss_conf.assoc) {
#endif
				seq_printf(s, "  BSSID: %pM\n",
					   vif->bss_conf.bssid);
			}
		}

		/* AP mode information */
		if (vif->type == NL80211_IFTYPE_AP) {
			seq_printf(s, "  Beacon Interval: %d\n",
				   vif->bss_conf.beacon_int);
			seq_printf(s, "  DTIM Period: %d\n",
				   vif->bss_conf.dtim_period);
		}

		seq_printf(s, "\n");
	}

	return 0;
}

static int nrc_debugfs_wlan_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, nrc_debugfs_wlan_info_show, inode->i_private);
}

static const struct file_operations nrc_debugfs_wlan_info_fops = {
	.owner = THIS_MODULE,
	.open = nrc_debugfs_wlan_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int nrc_debugfs_expected_tput_read(void *data, u64 *val)
{
	struct nrc *nw = (struct nrc *)data;
	struct sk_buff *skb_resp;

	(void)nw; /* May be unused if NRC_SKB_DEBUG_ENABLED = 0 */
	if (!nrc_hal_ops_wim_request(NULL, WIM_CMD_GET_TX_STATS,
				     (WIM_RESP_TIMEOUT * 30), false,
				     &skb_resp)) {
		struct wim *wim = (struct wim *)skb_resp->data;
		struct wim_tlv *tlv = (struct wim_tlv *)(wim + 1);
		struct nrc_tx_stats *tx_stats = (struct nrc_tx_stats *)tlv->v;
		struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
		INFO("mcs:%d bw:%d gi:%d", tx_stats->mcs, tx_stats->bw,
		     tx_stats->gi);
		nrc_stats_update_tx_stats(tx_stats);
		/* Track WIM response SKB free with parsed cmd/event */
		NRC_SKB_TRACK_WIM_FREE(hdev, skb_resp, wim->cmd, wim->event,
				       true, false);
	} else {
	}
	*val = nrc_stats_calc_metric();
	return 0;
}

static int nrc_debugfs_expected_tput_write(void *data, u64 val)
{
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_expected_tput,
			nrc_debugfs_expected_tput_read,
			nrc_debugfs_expected_tput_write, "%llu\n");

static struct dentry *twt_debugfs_root;
static struct dentry *apf_debugfs_root;

/* Note: Loopback (hspi) test functions moved to nrc_core module
 * Access via /sys/kernel/debug/nrc_core/loopback/
 */

void nrc_init_debugfs(struct nrc *nw)
{
#define nrc_debugfs_create_file(name, fops) \
	debugfs_create_file(name, 0664, nw->debugfs, nw, fops)

	nw->debugfs = nw->hw->wiphy->debugfsdir;

	/* Note: Common entries (nrc_credit, nrc_debug, nrc_cspi, nrc_hif, nrc_reset, nrc_restart)
	 * are now in nrc_core module at /sys/kernel/debug/nrc_core/
	 */

	/* WLAN module-specific debug mask and level */
	nrc_debugfs_create_file("debug_mask", &nrc_wlan_debugfs_debug_fops);
	nrc_debugfs_create_file("debug_level", &nrc_wlan_debugfs_level_fops);

	/* WLAN-specific debugfs entries */
	nrc_debugfs_create_file("wakeup", &nrc_debugfs_wakeup_device);
	nrc_debugfs_create_file("sleep", &nrc_debugfs_sleep_device);
	nrc_debugfs_create_file("snr", &nrc_debugfs_snr);
	nrc_debugfs_create_file("rssi", &nrc_debugfs_rssi);
	nrc_debugfs_create_file("beacon_updated", &nrc_debugfs_beacon_updated);
	nrc_debugfs_create_file("expected_tput", &nrc_debugfs_expected_tput);
	debugfs_create_file("info", 0444, nw->debugfs, nw,
			    &nrc_debugfs_wlan_info_fops);

	/* Note: Loopback (hspi) test moved to nrc_core module
	 * Access via /sys/kernel/debug/nrc_core/loopback/
	 */

	twt_debugfs_root = debugfs_create_dir("twt", nw->debugfs);
	nrc_mac_twt_debugfs_init(twt_debugfs_root, nw);

	apf_debugfs_root = debugfs_create_dir("apf", nw->debugfs);
	nrc_apf_debugfs_init(apf_debugfs_root, nw);
}

void nrc_exit_debugfs(struct nrc *nw)
{
	/* time_info_array is now managed by nrc_core module */
}
#endif /* CONFIG_DEBUG_FS */
