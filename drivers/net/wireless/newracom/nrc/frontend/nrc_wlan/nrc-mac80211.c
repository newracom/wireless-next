// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 */

/* Linux kernel headers */
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/gpio.h>
#include <linux/if_arp.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

/* Linux networking headers */
#include <net/dst.h>
#include <net/genetlink.h>
#include <net/ieee80211_radiotap.h>
#include <net/mac80211.h>
#include <net/xfrm.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-country.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Common directory headers - Interfaces */
#include "nrc-hal-core-interface.h"
#include "nrc-wim-types.h"

/* Local module headers */
#include "nrc-mac80211.h"
#include "nrc-netlink.h"
#include "nrc-pm.h"
#include "nrc-ps.h"
#include "nrc-stats.h"
#include "nrc-vendor.h"
#include "nrc-wlan-hal-init.h"
#include "nrc-hal-core-interface.h"
#include "nrc-wim-wlan.h"
#include "nrc-s1g.h"
#include "nrc-mac80211-twt.h"
#include "nrc-twt-sched.h"

#if defined(CONFIG_SUPPORT_BD)
#include <linux/kernel.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

/* Board Data validity flag - moved from HAL module to WLAN module */
static bool g_bd_valid = false;
#else
#define US_BASE_FREQ 2412
#define K1_BASE_FREQ 5180
#define K2_BASE_FREQ 5180
#define JP_BASE_FREQ 5200
#define TW_BASE_FREQ 5180
#define EU_BASE_FREQ 5180
#define NZ_BASE_FREQ 5180
#define AU_BASE_FREQ 5180
#define SG_BASE_FREQ 5180
#endif /* defined(CONFIG_SUPPORT_BD) */

typedef struct _vendor_cmd_entry {
	struct list_head list;
	u8 subcmd;
	u8 data_len;
	u8 data[];
} VCMD_ENTRY;

typedef struct _vendor_cmd_info {
	bool initialized;
	bool reset_by_wdt;
	u8 vif_id;
	struct nrc *nw;
	struct delayed_work vcmd_backup_reinstall;
	struct list_head head;
} VCMD_BACKUP_INFO;

/* not convert bss_max_idle value using usf */
bool no_convert_usf = false;

/*
 * Current alpha-2 country code. Sized for the trailing NUL so that it can be
 * passed to %s: KASAN caught a global-out-of-bounds read when this was two
 * bytes and nrc_set_s1g_country() printed it as a string.
 */
char nrc_cc[3];

#define CHAN2G(freq)                             \
	{                                        \
		.band = NL80211_BAND_2GHZ,       \
		.center_freq = (freq),           \
		.hw_value = ((freq - 2407) / 5), \
		.max_power = 20,                 \
	}

#define CHAN5G(freq)                             \
	{                                        \
		.band = NL80211_BAND_5GHZ,       \
		.center_freq = (freq),           \
		.hw_value = ((freq - 5000) / 5), \
		.max_power = 20,                 \
	}

#define NRC_CONFIGURE_FILTERS \
	(FIF_ALLMULTI | FIF_PSPOLL | FIF_BCN_PRBRESP_PROMISC | FIF_PROBE_REQ)

#define FREQ_TO_100KHZ(mhz, khz) (mhz * 10 + khz / 100)

#if defined(CONFIG_S1G_CHANNEL)
static const struct ieee80211_sta_s1g_cap nrc_s1g_cap = {
	.s1g = true,
	.cap = {S1G_CAP0_SGI_1MHZ | S1G_CAP0_SGI_2MHZ | S1G_CAP0_SGI_4MHZ, 0, 0,
		S1G_CAP3_MAX_MPDU_LEN, 0, S1G_CAP5_AMPDU, 0, S1G_CAP7_DUP_1MHZ,
		S1G_CAP8_TWT_RESPOND | S1G_CAP8_TWT_REQUEST, 0},
	.nss_mcs = {0xfc | 1, /* MCS 7 for 1 SS */
		    /* RX Highest Supported Long GI Data Rate 0:7 */
		    0,
		    /* RX Highest Supported Long GI Data Rate 0:7 */
		    /* TX S1G MCS Map 0:6 */
		    0xfa,
		    /* TX S1G MCS Map :7 */
		    /* TX Highest Supported Long GI Data Rate 0:6 */
		    0x80,
		    /* TX Highest Supported Long GI Data Rate 7:8 */
		    /* Rx Single spatial stream and S1G-MCS Map for 1MHz */
		    /* Tx Single spatial stream and S1G-MCS Map for 1MHz */
		    0},
};
static struct ieee80211_channel nrc_channels_s1ghz[MAX_S1G_CHANNEL_NUM];

#else
static struct ieee80211_channel nrc_channels_2ghz[] = {
	CHAN2G(2412), /* Channel 1 */
	CHAN2G(2417), /* Channel 2 */
	CHAN2G(2422), /* Channel 3 */
	CHAN2G(2427), /* Channel 4 */
	CHAN2G(2432), /* Channel 5 */
	CHAN2G(2437), /* Channel 6 */
	CHAN2G(2442), /* Channel 7 */
	CHAN2G(2447), /* Channel 8 */
	CHAN2G(2452), /* Channel 9 */
	CHAN2G(2457), /* Channel 10 */
	CHAN2G(2462), /* Channel 11 */
	CHAN2G(2467), /* Channel 12 */
	CHAN2G(2472), /* Channel 13 */
};

static struct ieee80211_channel nrc_channels_5ghz[] = {
	CHAN5G(5180), /* Channel 36 */
	CHAN5G(5185), /* Channel 37 */
	CHAN5G(5190), /* Channel 38 */
	CHAN5G(5195), /* Channel 39 */
	CHAN5G(5200), /* Channel 40 */
	CHAN5G(5205), /* Channel 41 */
	CHAN5G(5210), /* Channel 42 */
	CHAN5G(5215), /* Channel 43 */
	CHAN5G(5220), /* Channel 44 */
	CHAN5G(5225), /* Channel 45 */
	CHAN5G(5230), /* Channel 46 */
	CHAN5G(5235), /* Channel 47 */
	CHAN5G(5240), /* Channel 48 */
	/* Op35 (2 MHz, S1G ch 128-172): proxy 5250-5360 MHz, 10 MHz step */
	CHAN5G(5250), /* Channel 50  (Op35 S1G ch128 2M proxy) */
	CHAN5G(5260), /* Channel 52  (Op35 S1G ch132 2M proxy) */
	CHAN5G(5270), /* Channel 54  (Op35 S1G ch136 2M proxy) */
	CHAN5G(5280), /* Channel 56  (Op35 S1G ch140 2M proxy) */
	CHAN5G(5290), /* Channel 58  (Op35 S1G ch144 2M proxy) */
	CHAN5G(5300), /* Channel 60  (Op35 S1G ch148 2M proxy) */
	CHAN5G(5310), /* Channel 62  (Op35 S1G ch152 2M proxy) */
	CHAN5G(5320), /* Channel 64  (Op35 S1G ch156 2M proxy) */
	CHAN5G(5330), /* Channel 66  (Op35 S1G ch160 2M proxy) */
	CHAN5G(5340), /* Channel 68  (Op35 S1G ch164 2M proxy) */
	CHAN5G(5350), /* Channel 70  (Op35 S1G ch168 2M proxy) */
	CHAN5G(5360), /* Channel 72  (Op35 S1G ch172 2M proxy) */
	/* Op36 (4 MHz, S1G ch 130-170): proxy 5380-5480 MHz, 20 MHz step */
	CHAN5G(5380), /* Channel 76  (Op36 S1G ch130 4M proxy) */
	CHAN5G(5400), /* Channel 80  (Op36 S1G ch138 4M proxy) */
	CHAN5G(5420), /* Channel 84  (Op36 S1G ch146 4M proxy) */
	CHAN5G(5440), /* Channel 88  (Op36 S1G ch154 4M proxy) */
	CHAN5G(5460), /* Channel 92  (Op36 S1G ch162 4M proxy) */
	CHAN5G(5480), /* Channel 96  (Op36 S1G ch170 4M proxy) */
	CHAN5G(5500), /* Channel 100 */
	CHAN5G(5520), /* Channel 104 */
	CHAN5G(5540), /* Channel 108 */
	CHAN5G(5560), /* Channel 112 */
	CHAN5G(5580), /* Channel 116 */
	CHAN5G(5745), /* Channel 149 */
	CHAN5G(5750), /* Channel 150 */
	CHAN5G(5755), /* Channel 151 */
	CHAN5G(5760), /* Channel 152 */
	CHAN5G(5765), /* Channel 153 */
	CHAN5G(5770), /* Channel 154 */
	CHAN5G(5775), /* Channel 155 */
	CHAN5G(5780), /* Channel 156 */
	CHAN5G(5785), /* Channel 157 */
	CHAN5G(5790), /* Channel 158 */
	CHAN5G(5795), /* Channel 159 */
	CHAN5G(5800), /* Channel 160 */
	CHAN5G(5805), /* Channel 161 */
	CHAN5G(5810), /* Channel 162 */
	CHAN5G(5815), /* Channel 163 */
	CHAN5G(5820), /* Channel 164 */
	CHAN5G(5825), /* Channel 165 */
};
#endif /* CONFIG_S1G_CHANNEL */

#if !defined(CONFIG_S1G_CHANNEL)
static struct ieee80211_rate nrc_rates[] = {
	/* 11b rates */
	{.bitrate = 10},
	{.bitrate = 20, .flags = IEEE80211_RATE_SHORT_PREAMBLE},
	{.bitrate = 55, .flags = IEEE80211_RATE_SHORT_PREAMBLE},
	{.bitrate = 110, .flags = IEEE80211_RATE_SHORT_PREAMBLE},

	/* 11g rates */
	{.bitrate = 60},
	{.bitrate = 90},
	{.bitrate = 120},
	{.bitrate = 180},
	{.bitrate = 240},

	/* README it is removed for 11N Certification 5.2.34 */
	{.bitrate = 360},
	{.bitrate = 480},
	{.bitrate = 540}};
#else
static struct ieee80211_rate nrc_rates[] = {
	/* 11ah rates */
	{.bitrate = 10},
	{.bitrate = 20, .flags = IEEE80211_RATE_SHORT_PREAMBLE},
	{.bitrate = 40, .flags = IEEE80211_RATE_SHORT_PREAMBLE},
	//	{ .bitrate = 80, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	//	{ .bitrate = 160, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
};
#endif

#if defined(CONFIG_S1G_CHANNEL)
static const struct ieee80211_regdomain mac80211_regdom = {
	.n_reg_rules = 1,
	.alpha2 = "99",
	.reg_rules =
		{
			REG_RULE(400, 1000, 4, 0, 30, 0),
		},
};
#else
static const struct ieee80211_regdomain mac80211_regdom = {
	/*
	 * All 5 GHz entries are proxy frequencies for S1G operation only;
	 * no real 5 GHz RF is used by NRC7394.
	 *
	 * Rule 2 extends the original 5180-5320 range to cover the 18 new
	 * Op35/Op36 proxy channels (5250-5480 MHz).
	 * Rules 3 and 4 are unchanged from the original configuration.
	 */
	.n_reg_rules = 4,
	.alpha2 = "99",
	.reg_rules =
		{
			REG_RULE(2412 - 10, 2484 + 10, 40, 0, 30, 0),
			/* 5180-5480: original 5180-5320 + Op35/Op36 proxy block */
			REG_RULE(5180 - 10, 5480 + 10, 40, 0, 30, 0),
			REG_RULE(5500 - 10, 5580 + 10, 40, 0, 30, 0),
			REG_RULE(5745 - 10, 5825 + 10, 40, 0, 30, 0),
		},
};
#endif /* CONFIG_S1G_CHANNEL */

static const char nrc_gstrings_stats[][ETH_GSTRING_LEN] = {
	"tx_pkts_nic",	"tx_bytes_nic", "rx_pkts_nic",
	"rx_bytes_nic", "d_tx_dropped", "d_tx_failed",
	"d_ps_mode",	"d_group",	"d_tx_power",
};

static const u32 nrc_cipher_supported[] = {
#if !defined(CONFIG_S1G_CHANNEL)
	WLAN_CIPHER_SUITE_WEP40,	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
#endif
	WLAN_CIPHER_SUITE_CCMP,
#ifdef CONFIG_SUPPORT_CCMP_256
	WLAN_CIPHER_SUITE_CCMP_256,
#endif
#ifdef CONFIG_SUPPORT_GCMP
	WLAN_CIPHER_SUITE_GCMP,		WLAN_CIPHER_SUITE_GCMP_256,
#endif
#ifdef CONFIG_SUPPORT_GMAC
	WLAN_CIPHER_SUITE_BIP_GMAC_128, WLAN_CIPHER_SUITE_BIP_GMAC_256,
#endif

	WLAN_CIPHER_SUITE_AES_CMAC,
};

static const struct ieee80211_iface_limit if_limits_multi[] = {
	{.max = 2,
	 .types = BIT(NL80211_IFTYPE_STATION) |
#ifdef CONFIG_MAC80211_MESH
		  BIT(NL80211_IFTYPE_MESH_POINT) |
#endif
		  BIT(NL80211_IFTYPE_AP)},
};

static const struct ieee80211_iface_combination if_comb_multi[] = {
	{
		.limits = if_limits_multi,
		.n_limits = ARRAY_SIZE(if_limits_multi),
		/* The number of supported VIF (2) + P2P_DEVICE */
		.max_interfaces = 2,
		.num_different_channels = 1,

		.beacon_int_infra_match = true,
#if !defined(CONFIG_S1G_CHANNEL)
		.radar_detect_widths = BIT(NL80211_CHAN_WIDTH_20_NOHT) |
				       BIT(NL80211_CHAN_WIDTH_20) |
				       BIT(NL80211_CHAN_WIDTH_40) |
				       BIT(NL80211_CHAN_WIDTH_80) |
				       BIT(NL80211_CHAN_WIDTH_160),
#endif
	},
};

#if defined(CONFIG_SUPPORT_IBSS)
u64 current_bssid_beacon_timestamp;
#endif

static void nrc_vcmd_backup_init_info(u8 vif_id, struct nrc *nw);
static VCMD_BACKUP_INFO *nrc_vcmd_backup_get_info_addr(u8 vif_id);
static void nrc_vcmd_backup_del_all_entry(u8 vif_id);
static int nrc_vcmd_backup_remove_entry(struct ieee80211_vif *vif, u8 subcmd);

static void force_sw_enc_mode_by_sta_type(struct nrc *nw,
					  struct ieee80211_vif *vif)
{
	int is_relay = ((nw->vif[0] && nw->vif[1]) &&
			(nw->params->sw_enc != WIM_ENCDEC_HYBRID));
#ifdef CONFIG_SUPPORT_IBSS
	int is_ibss = (vif->type == NL80211_IFTYPE_ADHOC);
#else
	int is_ibss = 0;
#endif
	int is_mesh = (((nw->vif[0] &&
			 nw->vif[0]->type == NL80211_IFTYPE_MESH_POINT) ||
			(nw->vif[1] &&
			 nw->vif[1]->type == NL80211_IFTYPE_MESH_POINT)) &&
		       (nw->params->sw_enc != WIM_ENCDEC_HYBRID));

	switch (nw->hdev->chip_id) {
	case 0x7292:
		if (is_relay || is_ibss || is_mesh) {
			nw->params->sw_enc = WIM_ENCDEC_SW;
			DBG_MAC("Force sw enc");
		}
		break;
	case 0x7394:
		if (is_mesh) {
			nw->params->sw_enc = WIM_ENCDEC_SW;
			DBG_MAC("Force sw enc");
		} else if (is_ibss) {
			if (nw->hdev->fw.info.chip_rev_num > 0) {
				// if metal revised 7394, uses WIM_ENCDEC_HW.
				nw->params->sw_enc = WIM_ENCDEC_HW;
			} else {
				nw->params->sw_enc = WIM_ENCDEC_SW;
			}
		}
		break;
	default:
		/*
		 * Unknown chipset: keep the configured sw_enc default rather
		 * than taking the kernel down over a chip id we cannot map.
		 */
		ERR("Unknown Newracom IEEE80211 chipset %04x",
		    nw->hdev->chip_id);
		break;
	}
}

static bool get_intf_addr(const char *intf_name, char *addr)
{
	struct socket *sock = NULL;
	struct net_device *dev = NULL;
	struct net *net = NULL;
	int retval = 0;

	if (!addr || !intf_name)
		return false;

	retval = sock_create(AF_INET, SOCK_STREAM, 0, &sock);
	if (retval < 0)
		return false;

	net = sock_net(sock->sk);

	dev = dev_get_by_name_rcu(net, intf_name);
	if (!dev) {
		sock_release(sock);
		return false;
	}

	memcpy(addr, dev->dev_addr, 6);

	sock_release(sock);

	return true;
}

static void set_mac_address(struct mac_address *macaddr, u8 vif)
{
	eth_zero_addr(macaddr->addr);
	get_intf_addr("eth0", macaddr->addr);

	macaddr->addr[0] = 0x2;
	macaddr->addr[1] = vif;
	macaddr->addr[5]++;
}

static inline struct ieee80211_txq *to_txq(struct nrc_txq *p)
{
	return container_of((void *)p, struct ieee80211_txq, drv_priv);
}

/**
 * nrc_txq_init - Initialize txq driver data
 */
static void nrc_init_txq(struct ieee80211_txq *txq, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta)
{
	struct nrc_txq *q;

	if (!txq)
		return;

	q = (void *)txq->drv_priv;
	INIT_LIST_HEAD(&q->list);
	q->hw_queue = vif->hw_queue[txq->ac];
	q->sta = sta;
}

static void nrc_flush_txq(struct nrc *nw)
{
	struct sk_buff *skb;
	struct ieee80211_txq *txq;
	u32 ac;

	spin_lock_bh(&nw->txq_lock);
	rcu_read_lock();
	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		ieee80211_txq_schedule_start(nw->hw, ac);
		while ((txq = ieee80211_next_txq(nw->hw, ac))) {
			while ((skb = ieee80211_tx_dequeue_ni(nw->hw, txq)) !=
			       NULL) {
				ieee80211_free_txskb(nw->hw, skb);
			}
			ieee80211_return_txq(nw->hw, txq, false);
		}
		ieee80211_txq_schedule_end(nw->hw, ac);
	}
	rcu_read_unlock();
	spin_unlock_bh(&nw->txq_lock);
}

static unsigned int nrc_ac_credit(struct nrc *nw, int ac)
{
	int ret;

	ret = atomic_read(&nw->hdev->credit.tx_credit[ac]) -
	      atomic_read(&nw->hdev->credit.tx_pend[ac]);
	if (ret < 0)
		return 0;
	return ret;
}

static __u32 nrc_txq_pending(struct ieee80211_hw *hw)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct nrc_txq *cur, *next;
	int ac;
	int len = 0;
	u64 now = 0, diff = 0;

	if (NRC_DRV_IS_NOT_RUNNING(hdev))
		return 0;

	now = ktime_to_us(ktime_get_real());

	spin_lock_bh(&nw->txq_lock);

	rcu_read_lock();
	list_for_each_entry_safe(cur, next, &nw->txq, list)
	{
		ac = cur->hw_queue;
		len += atomic_read(&nw->hdev->credit.tx_pend[ac]);
		/* Need to consider credit? */
	}
	rcu_read_unlock();

	spin_unlock_bh(&nw->txq_lock);

	diff = ktime_to_us(ktime_get_real()) - now;
	if ((!diff) || (diff > NRC_MAC80211_RCU_LOCK_THRESHOLD))
		DBG_MAC("%s, diff=%lu", __func__, (unsigned long)diff);

	return len;
}

static int nrc_push_txq(struct nrc *nw, struct nrc_txq *ntxq)
{
	struct sk_buff *skb;
	struct ieee80211_txq *txq;
	struct ieee80211_tx_control control;
	int ac, credit;
	int ret = 0;
	int pushed_count = 0;

	txq = to_txq(ntxq);
	ac = ntxq->hw_queue;
	credit = nrc_ac_credit(nw, ac);

	if (credit == 0) {
		DBG(CAT(TX) | CAT(CREDIT),
		    "TX tasklet scheduled but no credit available (ac=%d credit=0)",
		    ac);
		return 1;
	}

	control.sta = ntxq->sta;

	rcu_read_lock();

	while ((skb = ieee80211_tx_dequeue(nw->hw, txq)) != NULL) {
		// DBG_TX("Dequeued frame from TXQ: skb_len=%u ac=%d credit_remain=%d",
		//        skb->len, ac, credit);
		nrc_mac_tx_process(nw->hw, &control, skb, true);
		pushed_count++;
		credit--;
		if (credit <= 0) {
			ret = 1;
			break;
		}
	}

	rcu_read_unlock();

	DBG_CREDIT("push_txq done: ac=%d pushed=%d credit_remain=%d ret=%d", ac,
		   pushed_count, credit, ret);

	/* 0: all skb is pushed, 1: all credit is consumed */
	return ret;
}

static void nrc_mac_tx(struct ieee80211_hw *hw,
		       struct ieee80211_tx_control *control,
		       struct sk_buff *skb)
{
	nrc_mac_tx_process(hw, control, skb, true);
}

/**
 * nrc_wake_tx_queue
 *
 * The function moves skb in @txq to sk_buff_head in its private data
 * structure, and add it to woken up list.
 */
static void nrc_wake_tx_queue(struct ieee80211_hw *hw,
			      struct ieee80211_txq *txq)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct nrc_txq *ntxq = (void *)txq->drv_priv;
	unsigned long frame_cnt, byte_cnt;

	// DBG_MAC("%s called", __FUNCTION__);

	/* Check if txq actually has data before waking device */
	ieee80211_txq_get_depth(txq, &frame_cnt, &byte_cnt);
	if (frame_cnt == 0) {
		/* This is normal - race condition where txq was already processed */
		DBG_TX("wake_tx_queue: txq already empty (processed by another thread)");
		return;
	}

	/* Enable PS wakeup for TX path - use state machine (atomic context safe) */
	if (NRC_DRV_IS_ASLEEP(hdev)) {
		if (!(nw->twt_sched && hdev->params->twt_force_sleep)) {
			/* Request wake using state machine - MUST use timeout=0 for atomic context */
			DBG_PS("TXQ wakeup: Device asleep, waking for TX (TXQ has %u frames)",
			       frame_cnt);
			nrc_hal_ops_ps_request_wake(
				0, NRC_PS_REASON_DRV_TX_WAKEUP);
		}
	}

	spin_lock_bh(&nw->txq_lock);

	if (list_empty(&ntxq->list))
		list_add_tail(&ntxq->list, &nw->txq);

	spin_unlock_bh(&nw->txq_lock);

	nrc_kick_txq(nw);
}

void nrc_tx_tasklet(struct tasklet_struct *t)
{
	struct nrc *nw = from_tasklet(nw, t, tx_tasklet);
	struct nrc_txq *ntxq, *tmp;
	int ret;
	int txq_count = 0;

	spin_lock_bh(&nw->txq_lock);

	list_for_each_entry_safe(ntxq, tmp, &nw->txq, list)
	{
		txq_count++;
		ret = nrc_push_txq(
			nw,
			ntxq); /* 0: all skb is pushed, 1: all credit is consumed */
		if (ret == 0) {
			list_del_init(&ntxq->list);
		} else { /* If the credit is insufficient, give way to the next txq. */
			list_move_tail(&ntxq->list, &nw->txq);
			break;
		}
	}

	spin_unlock_bh(&nw->txq_lock);
}

/**
 * nrc_kick_txq - push tx frames to hif.
 *
 * This function is called from both wake_tx_queue() callback
 * and the ac queue status event handler.
 *
 * TODO:
 * Decide scheduling algorithm: first-come first-serve or
 * fairness between txq's (currently, it's first-come first served).
 */
void nrc_kick_txq(struct nrc *nw)
{
	struct nrc_hif_device *hdev = nw->hdev;

	/* Block only when HAL is not initialized or shutting down.
	 * PS state (NRC_DRV_PS) passes through so frames reach the HAL queue,
	 * which handles PS wakeup internally (see nrc-tx.c). */
	if (NRC_HIF_DRV_STATE(hdev) < NRC_DRV_RUNNING) {
		WARN_MAC("kick_txq skipped: drv=%s ps=%s",
			 NRC_DRV_STATE_STR(hdev), NRC_PS_STATE_STR(hdev));
		return;
	}

	VBS_TX("kick_txq: schedule tasklet (drv=%s ps=%s)",
	       NRC_DRV_STATE_STR(hdev), NRC_PS_STATE_STR(hdev));
	tasklet_schedule(&nw->tx_tasklet);
}

/*
 * nrc_cleanup_txq_all - cleanup txq
 *
 * This function is called from nrc_unregister_hw()
 *
 * This function must be called before ieee80211_unregister_hw()
 */
void nrc_cleanup_txq_all(struct nrc *nw)
{
	struct sk_buff *skb;
	struct ieee80211_txq *txq;
	struct nrc_txq *cur, *next;

	spin_lock_bh(&nw->txq_lock);
	rcu_read_lock();
	list_for_each_entry_safe(cur, next, &nw->txq, list)
	{
		txq = to_txq(cur);
		while ((skb = ieee80211_tx_dequeue(nw->hw, txq)) != NULL) {
			ieee80211_free_txskb(nw->hw, skb);
		}
		list_del_init(&cur->list);
	}
	rcu_read_unlock();
	spin_unlock_bh(&nw->txq_lock);
}

void nrc_cleanup_txq(struct nrc *nw, struct ieee80211_txq *txq)
{
	struct sk_buff *skb;
	struct nrc_txq *ntxq;

	if (!txq)
		return;

	ntxq = (struct nrc_txq *)txq->drv_priv;

	spin_lock_bh(&nw->txq_lock);
	rcu_read_lock();
	while ((skb = ieee80211_tx_dequeue(nw->hw, txq)) != NULL) {
		ieee80211_free_txskb(nw->hw, skb);
	}
	if (!list_empty(&ntxq->list)) {
		list_del_init(&ntxq->list);
	}
	rcu_read_unlock();
	spin_unlock_bh(&nw->txq_lock);
}

void nrc_cleanup_txq_by_macaddr(struct nrc *nw, struct ieee80211_vif *vif,
				uint8_t *macaddr)
{
	struct ieee80211_sta *sta;
	int i;

	rcu_read_lock();
	sta = ieee80211_find_all_sta(vif, macaddr);

	if (!sta) {
		rcu_read_unlock();
		DBG_MAC("[%s] sta is NULL", __func__);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(sta->txq); i++) {
		nrc_cleanup_txq(nw, sta->txq[i]);
	}
	rcu_read_unlock();
}

static void nrc_assoc_h_basic(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_bss_conf *info,
			      struct ieee80211_sta *sta, struct sk_buff *skb)
{
	struct nrc *nw __maybe_unused = hw->priv;
	struct nrc_vif *i_vif = to_i_vif(vif);
	struct ieee80211_vif_cfg *vif_cfg = &vif->cfg;
	struct ieee80211_chanctx_conf *conf;

	enum nl80211_band band;

	DBG_MAC("%s: aid=%u, bssid=%pM", __func__, vif_cfg->aid, info->bssid);
	i_vif->aid = vif_cfg->aid;

#ifdef CONFIG_TRX_BACKOFF
	nw->hdev->ampdu_supported = 0;
#endif
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_AID, sizeof(vif_cfg->aid),
				    &vif_cfg->aid);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BSSID, ETH_ALEN,
				    (void *)info->bssid);

	/* Enable later when rate adaptation is supported in the target */
	conf = rcu_dereference(vif->bss_conf.chanctx_conf);
	if (!conf) {
		WARN_MAC("%s: chanctx_conf is NULL, skipping band TLV",
			 __func__);
		return;
	}
	band = conf->def.chan->band;

	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SUPPORTED_RATES,
				    sizeof(sta->deflink.supp_rates[band]),
				    &sta->deflink.supp_rates[band]);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BASIC_RATE,
				    sizeof(info->basic_rates),
				    &info->basic_rates);
}

static void nrc_assoc_h_ht(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			   struct ieee80211_bss_conf *bss_conf,
			   struct ieee80211_sta *sta, struct sk_buff *skb)
{
	struct ieee80211_sta_ht_cap *ht_cap = &sta->deflink.ht_cap;

	/* Assumption: ht_cap->ht_supported is false if HT Capabilities
	 * element is not included in the Association Response frame
	 */

	DBG_MAC("%s: %s (ht_cap=%04x)", __func__,
		ht_cap->ht_supported ? "HT" : "non-HT",
		ht_cap->ht_supported ? ht_cap->cap : 0x0);

	if (!ht_cap->ht_supported)
		return;

	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_HT_CAP, sizeof(u16),
				    &ht_cap->cap);

	/* TODO: MCS */
}

static void nrc_assoc_h_phymode(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_bss_conf *bss_conf,
				struct ieee80211_sta *sta, struct sk_buff *skb)
{
	enum { PHY_HT_NONE = 0, PHY_11B = 1, PHY_HT_MF = 3 };
	static char *const phymodestr[] = {"HT-none", "11b", "", "HT"};
	u8 phymode;

	/* HT_MF, non-HT, 11b */
	if (sta->deflink.ht_cap.ht_supported)
		phymode = PHY_HT_MF;
	else if (sta->deflink.supp_rates[NL80211_BAND_2GHZ] >> 4)
		phymode = PHY_HT_NONE;
	else
		phymode = PHY_11B;

	DBG_MAC("%s: phy mode=%s", __func__, phymodestr[phymode]);

	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_PHY_MODE, sizeof(u8),
				    &phymode);
}

static void nrc_bss_assoc(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct ieee80211_bss_conf *bss_conf,
			  struct sk_buff *skb)
{
	struct ieee80211_sta *ap_sta;
	u64 now = 0, diff = 0;

	now = ktime_to_us(ktime_get_real());

	rcu_read_lock();
	ap_sta = ieee80211_find_sta(vif, bss_conf->bssid);
	if (!ap_sta) {
		DBG_MAC("failed to find sta for bss %pM", bss_conf->bssid);
		goto out;
	}

	nrc_assoc_h_basic(hw, vif, bss_conf, ap_sta, skb);
	nrc_assoc_h_ht(hw, vif, bss_conf, ap_sta, skb);
	nrc_assoc_h_phymode(hw, vif, bss_conf, ap_sta, skb);

out:
	rcu_read_unlock();
	diff = ktime_to_us(ktime_get_real()) - now;
	if ((!diff) || (diff > NRC_MAC80211_RCU_LOCK_THRESHOLD))
		DBG_MAC("%s, diff=%lu", __func__, (unsigned long)diff);
}

static int nrc_vendor_update_beacon(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct sk_buff *skb, *b;
	u8 *pos;
	u16 need_headroom, need_tailroom;

	/*
	 * Only a beaconing interface has a beacon template, and mac80211
	 * warns (WARN_ON in __ieee80211_beacon_get) when asked for one on
	 * any other type. The beacon-family vendor commands can arrive on a
	 * STA vif, so refuse them here instead of tripping that warning.
	 */
	if (!vif || (vif->type != NL80211_IFTYPE_AP &&
		     vif->type != NL80211_IFTYPE_ADHOC &&
		     vif->type != NL80211_IFTYPE_MESH_POINT))
		return -EOPNOTSUPP;

	b = ieee80211_beacon_get_template(hw, vif, NULL, vif->bss_conf.link_id);
	if (!b)
		return -EINVAL;

	/* Track beacon template SKB from mac80211 (count_only, will be freed with tracking at line 959) */
	NRC_SKB_TRACK_ALLOC(nw->hdev, b, HIF_TYPE_FRAME, false, false);

	if (nw->vendor_skb_beacon) {
		need_headroom = skb_headroom(b);
		need_tailroom = nw->vendor_skb_beacon->len;

		if (skb_tailroom(b) < need_tailroom) {
			if (pskb_expand_head(b, need_headroom, need_tailroom,
					     GFP_ATOMIC)) {
				DBG_MAC("Fail to expand Beacon for vendor elem (need: %d)",
					need_tailroom);
				/* Track error path SKB frees */
				NRC_SKB_TRACK_FREE(nw->hdev, b, HIF_TYPE_FRAME,
						   false, false);
				NRC_SKB_TRACK_FREE(nw->hdev,
						   nw->vendor_skb_beacon,
						   HIF_TYPE_FRAME, false,
						   false);
				nw->vendor_skb_beacon = NULL;
				return 0;
			}
		}
		pos = skb_put(b, nw->vendor_skb_beacon->len);
		memcpy(pos, nw->vendor_skb_beacon->data,
		       nw->vendor_skb_beacon->len);
	}

	if (b->len > WIM_MAX_SIZE) {
		DBG_MAC("Fail to alloc skb for wim(b->len:%d, max: %d)", b->len,
			WIM_MAX_SIZE);
		NRC_SKB_TRACK_FREE(nw->hdev, b, HIF_TYPE_FRAME, false, false);
		return -EMSGSIZE;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	if (!skb) {
		NRC_SKB_TRACK_FREE(nw->hdev, b, HIF_TYPE_FRAME, false, false);
		return -ENOMEM;
	}
	pos = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BEACON, b->len, b->data);

	/* Track beacon buffer free (from mac80211) */
	NRC_SKB_TRACK_FREE(nw->hdev, b, HIF_TYPE_FRAME, false, false);
	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

static int nrc_vendor_update_probe_req(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct sk_buff *skb;

	if (nw->vendor_skb_probe_req->len > WIM_MAX_SIZE) {
		DBG_MAC("Fail to alloc skb for wim(vendor_skb_probe_req->len:%d, max: %d)",
			nw->vendor_skb_probe_req->len, WIM_MAX_SIZE);
		return -EMSGSIZE;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_PROBE_REQ_VENDOR_IE,
				    nw->vendor_skb_probe_req->len,
				    nw->vendor_skb_probe_req->data);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

static int nrc_vendor_update_probe_rsp(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct sk_buff *skb;

	if (nw->vendor_skb_probe_rsp->len > WIM_MAX_SIZE) {
		DBG_MAC("Fail to alloc skb for wim(vendor_skb_probe_rsp->len:%d, max: %d)",
			nw->vendor_skb_probe_rsp->len, WIM_MAX_SIZE);
		return -EMSGSIZE;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_PROBE_RSP_VENDOR_IE,
				    nw->vendor_skb_probe_rsp->len,
				    nw->vendor_skb_probe_rsp->data);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

static int nrc_vendor_update_assoc_req(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct sk_buff *skb;

	if (nw->vendor_skb_assoc_req->len > WIM_MAX_SIZE) {
		DBG_MAC("Fail to alloc skb for wim(vendor_skb_assoc_req->len:%d, max: %d)",
			nw->vendor_skb_assoc_req->len, WIM_MAX_SIZE);
		return -EMSGSIZE;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_ASSOC_REQ_VENDOR_IE,
				    nw->vendor_skb_assoc_req->len,
				    nw->vendor_skb_assoc_req->data);

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

static int nrc_mac_start(struct ieee80211_hw *hw)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct sk_buff *skb;
	int alloc_size;

	DBG_MAC("%s called", __FUNCTION__);
#if defined(CONFIG_SUPPORT_BD)
	/*
	 * nw->alpha2 is initialised to "99" (the driver sentinel for
	 * "no CC set yet").  At boot with auto-loaded modules, cfg80211
	 * calls nrc_reg_notifier("00") which is skipped, so alpha2 stays
	 * "99" and g_bd_valid stays false.  Detect both "99" and an empty
	 * string as "unset" and default to "US" so that BD is pushed to FW
	 * and WLAN can start.
	 * Under start_modular.py flow this branch never fires because
	 * 'iw reg set US' runs before hostapd/wpa_supplicant starts.
	 */
	if (!nw->alpha2[0] || (nw->alpha2[0] == '9' && nw->alpha2[1] == '9')) {
		WARN_MAC(
			"mac_start: no valid CC set (boot auto-load, CC=%c%c); defaulting to US",
			nw->alpha2[0] ? nw->alpha2[0] : '?',
			nw->alpha2[1] ? nw->alpha2[1] : '?');
		nw->alpha2[0] = 'U';
		nw->alpha2[1] = 'S';
		nrc_restore_reg_domain(nw);
	} else {
		INFO_MAC("mac_start: CC already set to %c%c", nw->alpha2[0],
			 nw->alpha2[1]);
	}

	/* BD must be in FW before any WLAN operation */
	if (!g_bd_valid) {
		ERR_MAC("mac_start: BD not loaded in FW (no valid CC?), blocking");
		return -EINVAL;
	}
#endif

	mutex_lock(&nw->state_mtx);

	if (nrc_idle_mode_get_state(nw)) {
		DBG_STATE("Wake target for mac start");
		nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
				NRC_PS_REASON_DRV_BSS_CONFIG);
	}

	{
		u16 init_aid = 0;
		alloc_size = tlv_len(sizeof(u16)) + tlv_len(ETH_ALEN);
		if (nrc_mac_is_s1g(hdev)) {
			alloc_size += tlv_len(sizeof(u8));
		}
		skb = nrc_hal_ops_wim_alloc_skb(WIM_CMD_SET, alloc_size);
		if (!skb) {
			mutex_unlock(&nw->state_mtx);
			return -ENOMEM;
		}
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_AID, sizeof(u16),
					    &init_aid);
		/* Add MAC address TLV */
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_MACADDR, ETH_ALEN,
					    hdev->mac_addr[0].addr);

		if (nrc_mac_is_s1g(hdev)) {
			u8 ndp_preq = nw->params->ndp_preq;
			nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_NDP_PREQ,
						    sizeof(u8), &ndp_preq);
#if defined(CONFIG_SUPPORT_LEGACY_ACK)
			if (nw->params->enable_legacy_ack) {
				u8 legacy_ack = nw->params->enable_legacy_ack;
				nrc_hal_ops_wim_skb_add_tlv(skb,
							    WIM_TLV_LEGACY_ACK,
							    sizeof(u8),
							    &legacy_ack);
			}
#endif /* CONFIG_SUPPORT_LEGACY_ACK */
		}

		nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
	}

	mutex_unlock(&nw->state_mtx);

	return 0;
}

void nrc_mac_stop(struct ieee80211_hw *hw)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	int ret = 0;

	DBG_MAC("%s called", __FUNCTION__);

	mutex_lock(&nw->state_mtx);

	/* Wake device if sleeping to ensure pending TX can be sent */
	ret = nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
			      NRC_PS_REASON_DRV_STA_ADD);

	if (NRC_HIF_DRV_STATE(hdev) == NRC_DRV_STOP)
		goto out;

	/* Note: mac80211 calls nrc_mac_flush() before stop to flush TX queues
	 * HAL TX workqueue flush is handled by HAL stop (hif.c line 986)
	 * WIM_CMD_STOP and NRC_FW_CLEAR_STARTED are handled by core module
	 * when the last frontend exits (in nrc_hal_core_nw_cleanup) */

	cancel_delayed_work_sync(&nw->idle_work);
	cancel_delayed_work_sync(&nw->beacon_loss_work);

	nrc_ps_set_idle_mode(nw, "mac stop");

out:
	mutex_unlock(&nw->state_mtx);
}

/**
 * DOC: VIF handlers
 *
 */
static int nrc_alloc_vif_index(struct nrc *nw, struct ieee80211_vif *vif)
{
	int i;
	const int P2P_INDEX = 1;

	spin_lock_bh(&nw->vif_lock);
	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		/* Assign P2P DEVICE's index to P2P GO or GC */
		if (vif->p2p && i == P2P_INDEX && nw->vif[i])
			nw->vif[i] = NULL;

		if (!nw->vif[i]) {
			struct nrc_vif *i_vif = to_i_vif(vif);

			i_vif->index = i;
			nw->vif[i] = vif;
			nw->enable_vif[i] = true;
			spin_unlock_bh(&nw->vif_lock);
			return 0;
		} else if (!memcmp(vif->addr, nw->vif[i]->addr, ETH_ALEN)) {
			nw->enable_vif[i] = true;
			spin_unlock_bh(&nw->vif_lock);
			return 0;
		}
	}

	spin_unlock_bh(&nw->vif_lock);
	/* All h/w vifs are in use */
	return -EBUSY;
}

void nrc_free_vif_index(struct nrc *nw, struct ieee80211_vif *vif)
{
	struct nrc_vif *i_vif = to_i_vif(vif);

	spin_lock_bh(&nw->vif_lock);
	nw->vif[i_vif->index] = NULL;
	nw->enable_vif[i_vif->index] = false;
	i_vif->index = -1;
	spin_unlock_bh(&nw->vif_lock);
}

bool nrc_access_vif(struct nrc *nw)
{
	int i;

	spin_lock_bh(&nw->vif_lock);
	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		/* FIXME: use vif_lock for fw_state -- sw.ki -- 2019-1022 */
		if (nw->vif[i] /*&& nw->fw_state == NRC_FW_ACTIVE*/) {
			spin_unlock_bh(&nw->vif_lock);
			return true;
		}
	}
	spin_unlock_bh(&nw->vif_lock);
	return false;
}

static const char *iftype_string(enum nl80211_iftype iftype)
{
	switch (iftype) {
	case NL80211_IFTYPE_UNSPECIFIED:
		return "UNSPECIFIED";
	case NL80211_IFTYPE_ADHOC:
		return "ADHOC";
	case NL80211_IFTYPE_STATION:
		return "STATION";
	case NL80211_IFTYPE_AP:
		return "AP";
	case NL80211_IFTYPE_AP_VLAN:
		return "AP_VLAN";
	case NL80211_IFTYPE_WDS:
		return "WDS";
	case NL80211_IFTYPE_MONITOR:
		return "MONITOR";
	case NL80211_IFTYPE_MESH_POINT:
		return "MESH_POINT";
	case NL80211_IFTYPE_P2P_CLIENT:
		return "P2P_CLIENT";
	case NL80211_IFTYPE_P2P_GO:
		return "P2P_GO";
	case NL80211_IFTYPE_P2P_DEVICE:
		return "P2P_DEVICE";
	case NL80211_IFTYPE_OCB:
		return "OCB";
	default:
		return "UNKNOWN Type";
	}
}

void nrc_mac_roc_finish(struct work_struct *work)
{
	struct nrc *nw = container_of(work, struct nrc, roc_finish.work);

	DBG_MAC("%s", __func__);
	ieee80211_remain_on_channel_expired(nw->hw);
}

#ifdef CONFIG_USE_SCAN_TIMEOUT
static void nrc_mac_scan_timeout(struct work_struct *work);
#endif

extern bool signal_monitor;

static int nrc_mac_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct nrc_vif *i_vif = to_i_vif(vif);
	u64 now = 0, diff = 0;

	/* NOTE: BD gate removed from add_interface — mac80211 can call this before
 * nrc_mac_start() runs (e.g. start_concurrent.py creates wlan1 before wlan0
 * is UP).  BD validity is enforced in nrc_mac_start() and nrc_mac_start_ap().
 */

	/* 20190724, jmjang, CB#8781, Gerrit #2200
	 *	This feature is opposite to legacy power save in host mode and
	 *  so this feature is blocked.
	 *	Consideration: There is an WFA test case about WMM-PS but
	 *  it will be related to Wi-Fi P2P
	 */
	/* vif->driver_flags |= IEEE80211_VIF_SUPPORTS_UAPSD; */
	memset(i_vif, 0, sizeof(*i_vif));

	if (WARN_ON(nrc_alloc_vif_index(nw, vif) < 0))
		return -1;

	if (vif->type == NL80211_IFTYPE_MONITOR) {
		/*
		 * Monitor interfaces are the only active interfaces.
		 * Put the target into promiscuous mode
		 */
		nw->promisc = true;
		nrc_ampdu_mon_init();
		DBG_MAC("promiscuous mode on");
		goto out;
	}

	nw->promisc = false;

	if (vif->type == NL80211_IFTYPE_AP || vif->type == NL80211_IFTYPE_WDS ||
	    vif->type == NL80211_IFTYPE_P2P_GO
#if defined CONFIG_SUPPORT_IBSS
	    || vif->type == NL80211_IFTYPE_ADHOC
#endif
	) {
		if (i_vif->index == 0) { /* VIF0 */
			vif->cab_queue = 4;
		} else { /* VIF1 */
			if (nw->hdev->hw_queues == 6) { /* 7292 type, Use GP */
				vif->cab_queue = 4;
			} else if (nw->hdev->hw_queues ==
				   11) { /* 7393 type, Use BK1, BE1,... */
				vif->cab_queue = 10;
			} else {
				ERR("Invalid Chip ID(0x%x), queues:%d",
				    nw->hdev->chip_id, nw->hdev->hw_queues);
				goto err_free_vif;
			}
		}
	} else
		vif->cab_queue = IEEE80211_INVAL_HW_QUEUE;

	/* VIF0 */
	if (i_vif->index == 0) {
		vif->hw_queue[IEEE80211_AC_VO] = 3;
		vif->hw_queue[IEEE80211_AC_VI] = 2;
		vif->hw_queue[IEEE80211_AC_BE] = 1;
		vif->hw_queue[IEEE80211_AC_BK] = 0;
	}
	/* VIF1 */
	if (i_vif->index == 1) { /* VIF1 */
		switch (nw->hdev->hw_queues) {
		case 6: /* 7292 type, Use GP */
			vif->hw_queue[IEEE80211_AC_VO] = 5;
			vif->hw_queue[IEEE80211_AC_VI] = 5;
			vif->hw_queue[IEEE80211_AC_BE] = 5;
			vif->hw_queue[IEEE80211_AC_BK] = 5;
			break;
		case 11: /* 7393 type, Use BK1, BE1,... */
			vif->hw_queue[IEEE80211_AC_VO] = 9;
			vif->hw_queue[IEEE80211_AC_VI] = 8;
			vif->hw_queue[IEEE80211_AC_BE] = 7;
			vif->hw_queue[IEEE80211_AC_BK] = 6;
			break;
		default:
			ERR("Invalid Chip ID(0x%x), queues:%d",
			    nw->hdev->chip_id, nw->hdev->hw_queues);
			goto err_free_vif;
		}
	}
	if (i_vif->index > 1) {
		ERR("Invalid Vif Index(%d)", i_vif->index);
		goto err_free_vif;
	}
	DBG_MAC("%s: VIF%d's hwqueue:%d", __func__, i_vif->index,
		nw->hdev->hw_queues);

	nrc_init_txq(vif->txq, vif, NULL);

	i_vif->nw = nw;
	i_vif->max_idle_period = nw->hdev->cap.bss_max_idle;

	now = ktime_to_us(ktime_get_real());

	rcu_read_lock();
	i_vif->dev = dev_getbyhwaddr_rcu(wiphy_net(hw->wiphy), ARPHRD_ETHER,
					 vif->addr);
	rcu_read_unlock();

	diff = ktime_to_us(ktime_get_real()) - now;
	if ((!diff) || (diff > NRC_MAC80211_RCU_LOCK_THRESHOLD))
		DBG_MAC("%s, diff=%lu", __func__, (unsigned long)diff);

	if (i_vif->dev == NULL)
		return -1;

	spin_lock_init(&i_vif->preassoc_sta_lock);
	INIT_LIST_HEAD(&i_vif->preassoc_sta_list);

#ifdef CONFIG_USE_SCAN_TIMEOUT
	INIT_DELAYED_WORK(&i_vif->scan_timeout, nrc_mac_scan_timeout);
#endif

	if (vif->type == NL80211_IFTYPE_MESH_POINT) {
		signal_monitor = true;
		nw->params->signal_monitor = true;
	}

	DBG_MAC("%s addr:%pM hwindex:%d %s", __func__, vif->addr, i_vif->index,
		iftype_string(vif->type));

	if (vif->type == NL80211_IFTYPE_AP) {
		ap_max_idle_timer_start(nw, i_vif);
		/* TODO: check TWT capability before starting */
		// nrc_twt_sched_start(nw, vif);
		/* TODO: stop TWT if interface changed to STA */
	}

	if (vif->type == NL80211_IFTYPE_STATION) {
		timer_setup(&i_vif->bcn_mon_timer, nrc_bcn_mon_timer, 0);
	}

out:

	if (vif->p2p)
		nrc_wim_wlan_set_p2p_addr(vif);
	else
		nrc_wim_wlan_set_mac_addr(vif);

	force_sw_enc_mode_by_sta_type(nw, vif);
	nrc_wim_wlan_set_sta_type(vif);

	if ((vif->type == NL80211_IFTYPE_AP) ||
	    (vif->type == NL80211_IFTYPE_STATION)) {
		nrc_vcmd_backup_init_info(i_vif->index, nw);
	}

	return 0;

err_free_vif:
	/* Release the index claimed by nrc_alloc_vif_index() above */
	nrc_free_vif_index(nw, vif);
	return -EINVAL;
}

static int nrc_mac_change_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    enum nl80211_iftype newtype, bool newp2p)
{
	struct nrc *nw = hw->priv;
	struct nrc_vif *i_vif = to_i_vif(vif);

	DBG_MAC("%s addr:%pM hwindex:%d %s->%s", __func__, vif->addr,
		i_vif->index, iftype_string(vif->type), iftype_string(newtype));
	/*
	 * interface may change from non-AP to AP in
	 * which case this needs to be set up again
	 */

	if (vif->type == NL80211_IFTYPE_AP) {
		nrc_twt_sched_stop(nw, vif);
	}

	vif->type = newtype;
	vif->p2p = newp2p;

	nrc_cleanup_txq(nw, vif->txq);

	nrc_wim_wlan_set_mac_addr(vif);
	force_sw_enc_mode_by_sta_type(nw, vif);
	nrc_wim_wlan_set_sta_type(vif);

	vif->cab_queue = 0;

	ap_max_idle_timer_stop(nw, i_vif);

	if (newtype == NL80211_IFTYPE_AP) {
		ap_max_idle_timer_start(nw, i_vif);

		/* TODO: check TWT capability before starting */
		//nrc_twt_sched_start(nw, vif);
		/* TODO: stop TWT if interface changed to STA */
	}

	return 0;
}

static void nrc_mac_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct nrc_vif *i_vif = to_i_vif(vif);

	if (vif->type == NL80211_IFTYPE_MONITOR || vif->p2p) {
		if (vif->type == NL80211_IFTYPE_MONITOR) {
			nrc_ampdu_mon_deinit();
		}
		return;
	}

	DBG_MAC("%s addr:%pM hwindex:%d %s", __func__, vif->addr, i_vif->index,
		iftype_string(vif->type));

	flush_workqueue(hdev->event_workqueue);

	if (vif->type == NL80211_IFTYPE_AP) {
		ap_max_idle_timer_stop(nw, i_vif);
	}

	if (vif->type == NL80211_IFTYPE_STATION) {
		try_to_del_timer_sync(&i_vif->bcn_mon_timer);
		i_vif->associated = false;
		i_vif->fw_channel_set = false;
	}

	/**
	 * when ifconfig wlan0 down on idle_mode, nrc_mac_config is not called such as power_save
	 * so, forcely wake-up.
	 */
	nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
			NRC_PS_REASON_DRV_STA_REMOVE);

	if ((vif->type == NL80211_IFTYPE_AP) ||
	    (vif->type == NL80211_IFTYPE_STATION)) {
		nrc_vcmd_backup_del_all_entry(i_vif->index);

		if ((vif->type == NL80211_IFTYPE_STATION) &&
		    (i_vif->index < VIF_MAX))
			cancel_delayed_work_sync(
				&nrc_vcmd_backup_get_info_addr(i_vif->index)
					 ->vcmd_backup_reinstall);
	}

	nrc_cleanup_txq(nw, vif->txq);

#ifdef CONFIG_USE_SCAN_TIMEOUT
	cancel_delayed_work_sync(&i_vif->scan_timeout);
#endif
	/* PS is now synchronous - chip is awake after nrc_ps_set_mode returns */
	nrc_wim_wlan_unset_sta_type(vif);
	nrc_free_vif_index(hw->priv, vif);

	if (vif->type == NL80211_IFTYPE_AP) {
		nrc_twt_sched_stop(nw, vif);
	}
	DBG_MAC("%s:end", __func__);
}

static u16 total_sta; /* total number of STAs connected */

static void prepare_deauth_sta(void *data, struct ieee80211_sta *sta)
{
	struct nrc_sta *i_sta = to_i_sta(sta);
	struct ieee80211_hw *hw = i_sta->nw->hw;
	struct ieee80211_vif *vif = data;
	struct sk_buff *skb = NULL;
	struct ieee80211_tx_info *txi;
	struct ieee80211_tx_control control = {.sta = sta};

	if (!sta || !vif) {
		WRN("Invalid argument");
		return;
	}

	if (!ieee80211_find_sta(vif, sta->addr))
		return;

	DBG_STATE("(AP Recovery) Disconnect STA(%pM) by force", sta->addr);

	/*
	 * TX a deauth TO the STA so it knows the AP has restarted and must
	 * reconnect.  Without this, the STA never receives a deauth (it only
	 * loses beacons briefly), keeps its association state, and never
	 * initiates a new connection after the AP comes back up.
	 *
	 * NOTE: We no longer inject a fake RX deauth via ieee80211_rx_irqsafe()
	 * because in AP mode, mac80211's ieee80211_rx_h_mgmt() drops all
	 * management frames for AP VIF type, so the fake deauth never gets
	 * processed.  Instead, ieee80211_restart_hw() handles the mac80211
	 * internal STA cleanup (sta_state 4→0 transitions) during reconfig.
	 */
	skb = ieee80211_deauth_get(hw, sta->addr, vif->addr, vif->addr,
				   WLAN_REASON_DEAUTH_LEAVING, sta, true);
	if (skb) {
		skb_set_queue_mapping(skb, IEEE80211_AC_VO);
		txi = IEEE80211_SKB_CB(skb);
		txi->control.vif = vif;
		DBG_STATE("(AP Recovery) TX deauth to STA(%pM) len=%u",
			  sta->addr, skb->len);
		nrc_mac_tx_process(hw, &control, skb, false);
	} else {
		ERR("(AP Recovery) Failed to create TX deauth for STA(%pM)",
		    sta->addr);
	}

	++total_sta;
}

static void scan_complete(struct ieee80211_hw *hw, bool aborted);

int nrc_mac_restart(struct nrc *nw)
{
	int is_relay;
	int i;

	DBG_MAC("Restart NRC MAC");
	is_relay = (nw->vif[0] && nw->vif[1]);

	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		if (nw->vif[i]) {
			if (nw->vif[i]->type == NL80211_IFTYPE_STATION) {
				/*
				 * The target was rebooted due to watchdog.
				 * Then, driver always requests deauth for trying to reconnect.
				 * Note: RX thread already resumed in earlier WDT handling
				 */
				DBG_STATE("STA(%d) : Reconnect to AP", i);
				mdelay(300);
				/*
				 * Cancel any in-progress hw scan and notify mac80211
				 * via ieee80211_scan_completed(). Without this,
				 * mac80211 still believes a scan is running when
				 * ieee80211_restart_hw() is called later (in AP/relay
				 * mode), triggering a WARN in ieee80211_restart_work().
				 */
				if (nrc_cancel_hw_scan(nw->hw, nw->vif[i]))
					scan_complete(nw->hw, true);
				ieee80211_connection_loss(nw->vif[i]);

				/*
				 * Reset per-VIF associated state on recovery
				 * restart.  If bss_info_changed fails (e.g.,
				 * wakeup failure during recovery), the flag would
				 * remain stale, blocking sched_scan after restart.
				 */
				to_i_vif(nw->vif[i])->associated = false;
				if (!is_relay) {
					nrc_vcmd_backup_init_info(i, nw);
					return 0;
				} else {
					nrc_free_vif_index(nw, nw->vif[i]);
				}
			} else if (nw->vif[i]->type == NL80211_IFTYPE_AP) {
				struct nrc_vif *i_vif = to_i_vif(nw->vif[i]);

				ap_max_idle_timer_stop(nw, i_vif);

				ieee80211_iterate_stations_atomic(
					nw->hw, prepare_deauth_sta,
					(void *)nw->vif[i]);
				DBG_STATE(
					"AP(%d) : TX deauth sent to %d STA(s)",
					i, total_sta);

				/*
				 * Brief wait for the TX deauth to be sent out
				 * over SPI before we shut down the HAL.
				 * mac80211 STA cleanup is handled later by
				 * ieee80211_restart_hw() during reconfig
				 * (sta_state 4→3→2→1→0 transitions).
				 *
				 * The old retry loop (10 × 2s = 20s) waiting
				 * for ieee80211_rx_irqsafe() to remove STAs
				 * never worked in AP mode because mac80211's
				 * ieee80211_rx_h_mgmt() drops management
				 * frames for AP VIF type.
				 */
				if (total_sta)
					msleep(200);

				total_sta = 0;
				nrc_hal_ops_tx_cleanup_queues();
				nrc_mac_clean_txq(nw);
				ap_max_idle_timer_stop(
					nw,
					to_i_vif(
						nw->vif[i])); /* when restart or WDT , remove_interface is not called */
				nrc_free_vif_index(nw, nw->vif[i]);
			} else if (nw->vif[i]->type ==
				   NL80211_IFTYPE_MESH_POINT) {
				DBG_STATE(
					"mesh(%d) : Restart and do not repeering",
					i);
				nrc_hal_ops_tx_cleanup_queues();
				nrc_mac_clean_txq(nw);
				nrc_mac_flush_txq(nw);
			}
#ifdef CONFIG_S1G_CHANNEL
			init_s1g_channels(nw);
#endif /* #ifdef CONFIG_S1G_CHANNEL */
		}
	}

	return 1;
}

/**
 * nrc_nw_restart_wlan - Perform a full, synchronous network restart
 * @nw: NRC network device structure
 *
 * Sequence:
 * 1. Set WDT flags so vendor command backup is restored after MAC restart.
 * 2. Cleanup MAC layer state (deauth STAs, trigger reconnection).
 * 3. Shutdown HAL and hardware.
 * 4. Re-probe hardware and reload firmware.
 * 5. Re-send regulatory domain / channel table to freshly loaded FW.
 * 6. Release state_mtx, then call ieee80211_restart_hw() to re-configure
 *    MAC address / AID in the freshly loaded FW and restore all VIF state.
 *    This mirrors the WDT recovery path in nrc_wlan_handle_fw_ready_from_wdt().
 *
 * Returns: 0 on success, negative error code on failure.
 */
int nrc_nw_restart_wlan(struct nrc *nw)
{
	int ret;

	if (!nw || !nw->hw)
		return -EINVAL;

	INFO("Network restart starting");

	mutex_lock(&nw->state_mtx);

	/*
	 * 1. Mark all VIFs as WDT-reset so that nrc_mac_restart() triggers
	 *    vendor command backup restoration (same as the WDT recovery path).
	 *    Without this flag, nrc_vcmd_backup_init_info() skips restoration.
	 */
	nrc_vcmd_backup_set_wdt_flag(0);
	nrc_vcmd_backup_set_wdt_flag(1);

	/*
	 * 2. Cleanup MAC layer state.
	 *    For AP mode this sends a TX deauth to each connected STA
	 *    (via prepare_deauth_sta) so they know to reconnect.
	 *    For STA mode this triggers ieee80211_connection_loss().
	 *    This must happen while the HIF/SPI path is still operational.
	 */
	nrc_mac_restart(nw);

	/*
	 * 3. Shut down HAL.
	 *    Do NOT call ieee80211_stop_queues() here: it stops queues
	 *    with DRIVER reason, but ieee80211_restart_hw() only wakes
	 *    queues for SUSPEND reason during reconfig, leaving the
	 *    DRIVER stop permanently active.  This blocks all TX after
	 *    restart (including AUTH responses), preventing STA reconnection.
	 *    The WDT recovery handler does not stop queues either.
	 *    The HAL stop itself is sufficient to prevent TX during transition.
	 */
	nrc_hal_ops_nw_stop();

	/* 4. Start (Reset → Probe → FW Download → FW Start) */
	ret = nrc_hal_ops_nw_start();
	if (ret) {
		ERR("Restart failed at nw_start: %d", ret);
		mutex_unlock(&nw->state_mtx);
		return ret;
	}

	/*
	 * 5. Re-send country code / board data to the freshly loaded FW.
	 * ieee80211_restart_hw() does not repeat the cfg80211 regulatory
	 * notification, so the FW would assert in CheckNUpdateCHTableByVif()
	 * when nrc_mac_config() tries to configure a channel.
	 * nrc_restore_reg_domain() mirrors what nrc_wlan_handle_fw_ready_from_wdt()
	 * already does for WDT recovery.
	 */
	nrc_restore_reg_domain(nw);

	/*
	 * Release the mutex before calling ieee80211_restart_hw().
	 * ieee80211_restart_hw() schedules a restart_work that eventually
	 * calls nrc_mac_start(), which also acquires state_mtx.  Holding
	 * the lock here would cause a deadlock.
	 */
	mutex_unlock(&nw->state_mtx);

	/*
	 * 6. Notify mac80211 that the hardware was restarted.  It calls
	 *    nrc_mac_start() to re-configure MAC address / AID in the
	 *    freshly loaded FW, re-adds virtual interfaces, and restores
	 *    all per-VIF state.  This mirrors nrc_wlan_handle_fw_ready_from_wdt().
	 */
	DBG_STATE("Restart hw after FW reload");
	ieee80211_restart_hw(nw->hw);

	INFO("Network restart completed");

	return 0;
}

static enum WIM_CHANNEL_PARAM_WIDTH
get_wim_channel_width(enum nl80211_chan_width width)
{
	switch (width) {
	case NL80211_CHAN_WIDTH_20_NOHT:
		return CH_WIDTH_20_NOHT;
	case NL80211_CHAN_WIDTH_20:
		return CH_WIDTH_20;
	case NL80211_CHAN_WIDTH_40:
		return CH_WIDTH_40;
	case NL80211_CHAN_WIDTH_80:
		return CH_WIDTH_80;
	case NL80211_CHAN_WIDTH_80P80:
		return CH_WIDTH_80P80;
	case NL80211_CHAN_WIDTH_160:
		return CH_WIDTH_160;
	case NL80211_CHAN_WIDTH_5:
		return CH_WIDTH_5;
	case NL80211_CHAN_WIDTH_10:
		return CH_WIDTH_10;
#if defined(CONFIG_S1G_CHANNEL)
	case NL80211_CHAN_WIDTH_1:
		return CH_WIDTH_1;
	case NL80211_CHAN_WIDTH_2:
		return CH_WIDTH_2;
	case NL80211_CHAN_WIDTH_4:
		return CH_WIDTH_4;
	case NL80211_CHAN_WIDTH_8:
		return CH_WIDTH_8;
	case NL80211_CHAN_WIDTH_16:
		return CH_WIDTH_16;
	default:
		return CH_WIDTH_1;
#else
	default:
		return CH_WIDTH_20;
#endif /* CONFIG_S1G_CHANNEL */
	}
}

#if !defined(CONFIG_SUPPORT_BD)
extern int kr_band;
uint16_t get_base_freq(void)
{
	uint16_t ret_freq = 0;

	if (nrc_cc[0] == 'U' && nrc_cc[1] == 'S') {
		ret_freq = US_BASE_FREQ;
	} else if (nrc_cc[0] == 'J' && nrc_cc[1] == 'P') {
		ret_freq = JP_BASE_FREQ;
	} else if (nrc_cc[0] == 'T' && nrc_cc[1] == 'W') {
		ret_freq = TW_BASE_FREQ;
	} else if (nrc_cc[0] == 'A' && nrc_cc[1] == 'U') {
		ret_freq = AU_BASE_FREQ;
	} else if (nrc_cc[0] == 'N' && nrc_cc[1] == 'Z') {
		ret_freq = NZ_BASE_FREQ;
	} else if (nrc_cc[0] == 'E' && nrc_cc[1] == 'U') {
		ret_freq = EU_BASE_FREQ;
	} else if (nrc_cc[0] == 'K' && nrc_cc[1] == 'R') {
		ret_freq = (kr_band == 1) ? K1_BASE_FREQ : K2_BASE_FREQ;
	} else if (nrc_cc[0] == 'S' && nrc_cc[1] == 'G') {
		ret_freq = SG_BASE_FREQ;
	} else if (country_match(eu_countries_cc, nrc_cc)) {
		ret_freq = EU_BASE_FREQ;
	}
	return ret_freq;
}
#endif /* #if !defined(CONFIG_SUPPORT_BD) */

#ifdef CONFIG_S1G_CHANNEL
void init_s1g_channels(struct nrc *nw)
{
	struct ieee80211_channel *channels;
	struct sk_buff *skb;
	int i, freq, w;
	extern char *nrc_country_code;

	nrc_set_s1g_country(nrc_country_code);

	DBG_MAC("%s:country table size:%d", __func__,
		sizeof(struct s1g_channel_table) *
			nrc_get_num_channels_by_current_country());
	skb = nrc_hal_ops_wim_alloc_skb(WIM_CMD_SET, WIM_MAX_SIZE);
	nrc_hal_ops_wim_skb_add_tlv(
		skb, WIM_TLV_CH_TABLE,
		sizeof(struct s1g_channel_table) *
			nrc_get_num_channels_by_current_country(),
		(struct s1g_channel_table *)nrc_get_current_s1g_cc_table());
	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);

	DBG_MAC("%s: Country: %s", __func__,
		(char *)nrc_get_current_s1g_country());
	skb = nrc_hal_ops_wim_alloc_skb(WIM_CMD_SET, WIM_MAX_SIZE);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_COUNTRY_CODE, sizeof(u16),
				    (char *)nrc_get_current_s1g_country());

	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
	channels = nrc_channels_s1ghz;

	for (i = 0; i < nrc_get_num_channels_by_current_country(); i++) {
		freq = nrc_get_s1g_freq_by_arr_idx(i);
		channels[i].band = NL80211_BAND_S1GHZ;
		channels[i].center_freq = freq / 10;
		channels[i].freq_offset = freq % 10 * 100;

		w = nrc_get_s1g_width_by_arr_idx(i);

		switch (w) {
		default:
		case 1:
			channels[i].flags = IEEE80211_CHAN_1MHZ;
			break;
		case 2:
			channels[i].flags = IEEE80211_CHAN_2MHZ;
			break;
		case 4:
			channels[i].flags = IEEE80211_CHAN_4MHZ;
			break;
		}
		channels[i].hw_value = i + 1;
	}
}
#endif /* CONFIG_S1G_CHANNEL */

void nrc_mac_add_tlv_channel(struct sk_buff *skb,
			     struct cfg80211_chan_def *chandef)
{
#if !defined(CONFIG_S1G_CHANNEL)
	enum nl80211_channel_type ch_type;
	struct wim_channel_param ch_param;

	ch_type = cfg80211_get_chandef_type(chandef);

	/*temporarily resolved host system freezing */
	if (WARN_ON(!chandef->chan))
		return;

	ch_param.channel = chandef->chan->center_freq;
	ch_param.type = ch_type;
	ch_param.width = get_wim_channel_width(chandef->width);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_CHANNEL, sizeof(ch_param),
				    &ch_param);
#else
	struct s1g_channel_table param;
	static char *wim_s1g_alpha2;
	wim_s1g_alpha2 = nrc_get_current_s1g_country();

	param.alpha2[0] = wim_s1g_alpha2[0];
	param.alpha2[1] = wim_s1g_alpha2[1];
	param.alpha2[2] = wim_s1g_alpha2[2];
	param.s1g_freq = FREQ_TO_100KHZ(chandef->chan->center_freq,
					chandef->chan->freq_offset);
	param.s1g_freq_index = nrc_get_channel_idx_by_freq(param.s1g_freq);
	param.cca_level_type = nrc_get_cca_by_freq(param.s1g_freq);
	nrc_s1g_set_channel_bw(param.s1g_freq, chandef);
	param.chan_spacing = get_wim_channel_width(chandef->width);
	param.global_oper_class = nrc_get_oper_class_by_freq(param.s1g_freq);
	param.offset = nrc_get_offset_by_freq(param.s1g_freq);
	param.primary_loc = nrc_get_pri_loc_by_freq(param.s1g_freq);

	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_S1G_CHANNEL, sizeof(param),
				    &param);
#endif /* CONFIG_S1G_CHANNEL */
}

/**
 * nrc_mac_apply_ps - Apply mac80211 PS state to the driver
 * @nw:         NRC driver state
 * @ps_on:      true if mac80211 has PS enabled for this VIF
 * @timeout_ms: PS timeout in ms from hw->conf.dynamic_ps_timeout.
 *              0 = dynamic PS disabled ("stay awake indefinitely")
 *
 * Single entry point for all mac80211-originated PS state changes.
 * Called from the config() path (IEEE80211_CONF_CHANGE_PS) and the
 * bss_info_changed() path (BSS_CHANGED_PS).
 *
 * Decision table:
 *   timeout=0              → stop timer, force wake (PS disabled)
 *   ps_on=true, timeout>0  → sync timeout, start dynamic PS timer
 *   ps_on=false, timeout>0 → stop timer, wake if currently asleep
 *
 * Note: nrc_bss_handle_ps() early-returns for NRC_PS_NONE so this
 * function is only reached for modem-sleep mode from that path.
 */
static void nrc_mac_apply_ps(struct nrc *nw, bool ps_on, int timeout_ms)
{
	struct nrc_hif_device *hdev = nw->hdev;

	DBG(CAT(MAC) | CAT(PS), "apply_ps: ps_on=%d timeout=%d drv=%s scan=%d",
	    ps_on, timeout_ms, NRC_DRV_STATE_STR(hdev),
	    atomic_read(&nw->scan_mode));

	/*
	 * timeout=0 semantics depend on ps_on:
	 *   ps_on=false, timeout=0 → explicit PS disable; stop timer and wake.
	 *   ps_on=true,  timeout=0 → "immediate sleep" (e.g. user ran
	 *       iwconfig power timeout 0 then iw set power_save on).
	 *       mac80211 sets timeout=0 meaning "no inactivity delay", not
	 *       "disable PS".  Fall back to the driver default so PS is
	 *       actually enabled.
	 */
	if (timeout_ms == 0) {
		if (!ps_on) {
			nw->hdev->ps.timeout = 0;
			nrc_ps_dyn_stop(nw,
					NRC_PS_REASON_MAC_CONFIG_PS_DISABLED);
			return;
		}
		DBG(CAT(MAC) | CAT(PS),
		    "apply_ps: timeout=0 + ps_on=true → using default %dms",
		    NRC_PS_DEFAULT_TIMEOUT_MS);
		timeout_ms = NRC_PS_DEFAULT_TIMEOUT_MS;
	}

	/* Sync timeout from mac80211 */
	nw->hdev->ps.timeout = timeout_ms;

	/* Don't enter PS during scan */
	if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_ACTIVE_SCANNING ||
	    atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_PASSIVE_SCANNING) {
		VBS_PS("Skip PS during scan");
		return;
	}

	if (ps_on) {
		/* Already asleep - nothing to do */
		if (NRC_DRV_IS_ASLEEP(hdev) || hdev->ps.modem_enabled) {
			VBS_PS("Already in PS...");
			return;
		}
		/*
		 * PS enabled: defer sleep briefly so any in-flight frames
		 * complete before the device goes idle.
		 */
		nrc_ps_dyn_start(nw, 2000, NRC_PS_REASON_MAC_CONFIG_PS_ENABLED);
	} else {
		/* PS disabled: stop timer, wake device if asleep */
		nrc_ps_dyn_stop(nw, NRC_PS_REASON_MAC_CONFIG_PS_DISABLED);
	}
}

static void nrc_mac_config_handle_ps(struct nrc *nw, struct ieee80211_hw *hw)
{
	bool ps_on = !!(hw->conf.flags & IEEE80211_CONF_PS);

	/*
	 * Deep sleep is driver-managed: the timer is set from
	 * nrc_bss_handle_assoc() and must not be cancelled by mac80211
	 * reporting dynamic_ps_timeout=0 (which it always does for NRC
	 * deep sleep because SUPPORTS_PS is not set).
	 */
	if (nw->params->power_save >= NRC_PS_DEEPSLEEP_TIM) {
		DBG_MAC("%s deep-sleep mode — skipping", __func__);
		return;
	}

	nrc_mac_apply_ps(nw, ps_on, hw->conf.dynamic_ps_timeout);
}

static void nrc_mac_config_handle_idle(struct nrc *nw, struct ieee80211_hw *hw)
{
	DBG_MAC("IEEE80211_CONF_CHANGE_IDLE");

	cancel_delayed_work_sync(&nw->idle_work);

	if (hw->conf.flags & IEEE80211_CONF_IDLE) {
		nrc_idle_mode_set_state(nw, true);
		nrc_ps_set_idle_mode(nw, "mac config");
	} else {
		DBG_STATE("Changing to Active");
		nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
				NRC_PS_REASON_MAC_IDLE_EXIT);
		nrc_idle_mode_set_state(nw, false);
	}
}

static int nrc_mac_config(struct ieee80211_hw *hw, u32 changed)
{
	struct nrc *nw = hw->priv;
	struct sk_buff *skb;
	int ret = 0;
	struct ieee80211_channel ch = {
		0,
	};
#if defined(CONFIG_SUPPORT_BD)
	int i;
	bool supp_ch_flag = false;
	const struct bd_supp_param *supp_ch_list = NULL;
#endif /* defined(CONFIG_SUPPORT_BD) */
	struct cfg80211_chan_def chandef = {
		0,
	};

	/* hw->conf.chandef.chan may be NULL when using channel context */
	if (!hw->conf.chandef.chan) {
		if (changed & IEEE80211_CONF_CHANGE_CHANNEL)
			WARN_MAC(
				"config: channel change requested but chandef.chan is NULL (channel context path?)");
		else
			DBG_MAC("%s: chandef.chan is NULL, skipping channel configuration",
				__func__);
		goto skip_channel_config;
	}

	memcpy(&chandef, &hw->conf.chandef, sizeof(struct cfg80211_chan_def));
	memcpy(&ch, hw->conf.chandef.chan, sizeof(struct ieee80211_channel));
	chandef.chan = &ch;

	DBG_MAC("%s: changed: 0x%x", __FUNCTION__, changed);
#if defined(CONFIG_SUPPORT_BD)
	supp_ch_list = nrc_s1g_get_supp_ch_list();
	if (supp_ch_list && supp_ch_list->num_ch) {
		if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
			for (i = 0; i < supp_ch_list->num_ch; i++) {
				if (supp_ch_list->nons1g_ch_freq[i] ==
				    hw->conf.chandef.chan->center_freq) {
					supp_ch_flag = true;
					break;
				}
			}
			if (!supp_ch_flag && nw->alpha2[0] != 'U' &&
			    nw->alpha2[1] != 'S' &&
			    hw->conf.chandef.chan->center_freq == 2412) {
				supp_ch_flag = true;
				chandef.chan->center_freq =
					supp_ch_list->nons1g_ch_freq[0];
			}
			if (!supp_ch_flag) {
				if (g_bd_valid) {
					DBG_MAC("%s: Not supported channel %u",
						__func__,
						hw->conf.chandef.chan
							->center_freq);
					return -EINVAL;
				}
				WARN_MAC(
					"config: channel %u MHz not in BD supp list (BD not valid) — continuing",
					hw->conf.chandef.chan->center_freq);
			}
		}
	}
#else
	if (nw->alpha2[0] != 'U' && nw->alpha2[1] != 'S' &&
	    hw->conf.chandef.chan->center_freq == 2412) {
		chandef.chan->center_freq = get_base_freq();
	}
#endif /* CONFIG_SUPPORT_BD */

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		DBG_MAC("%s: changed: IEEE80211_CONF_CHANGE_CHANNEL ",
			__FUNCTION__);
		skb = nrc_hal_ops_wim_alloc_skb(
			WIM_CMD_SET, tlv_len(sizeof(struct wim_channel_param)));
		if (!skb)
			return -ENOMEM;
		nw->band = hw->conf.chandef.chan->band;
		nw->center_freq = hw->conf.chandef.chan->center_freq;

#ifdef CONFIG_S1G_CHANNEL
		init_s1g_channels(nw);
#endif /* #ifdef CONFIG_S1G_CHANNEL */
		nrc_mac_add_tlv_channel(skb, &chandef);
		ret = nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
		/* TODO: band (2G, 5G, etc) and bandwidth (20MHz, 40MHz, etc) */
	}

skip_channel_config:
	mutex_lock(&nw->state_mtx);

	if (changed & IEEE80211_CONF_CHANGE_PS)
		nrc_mac_config_handle_ps(nw, hw);

	if (nw->params->idle_mode && (changed & IEEE80211_CONF_CHANGE_IDLE) &&
	    atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_IDLE)
		nrc_mac_config_handle_idle(nw, hw);

	mutex_unlock(&nw->state_mtx);

	return ret;
}

static void nrc_mac_configure_filter(struct ieee80211_hw *hw,
				     unsigned int changed_flags,
				     unsigned int *total_flags, u64 multicast)
{
	*total_flags &= NRC_CONFIGURE_FILTERS;

	/* TODO: talk to target */
}
static void nrc_mac_update_p2p_ps(struct sk_buff *skb,
				  struct ieee80211_vif *vif)
{
	int i = 0;
	u8 ctwin;
	struct ieee80211_p2p_noa_attr *noa = &vif->bss_conf.p2p_noa_attr;
	struct wim_noa_param *p;

	if (!vif->p2p)
		return;

	if (noa->oppps_ctwindow & IEEE80211_P2P_OPPPS_ENABLE_BIT) {
		ctwin = cpu_to_le32(noa->oppps_ctwindow);
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_P2P_OPPPS, sizeof(u8),
					    &ctwin);
		DBG_MAC("%s: OppPS, ctwindow(%d)", __func__, ctwin);
	}

	for (i = 0; i < IEEE80211_P2P_NOA_DESC_MAX; i++) {
		const struct ieee80211_p2p_noa_desc *desc = &noa->desc[i];

		if (!desc->count || !desc->duration)
			continue;

		p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_P2P_NOA,
						sizeof(*p), NULL);

		p->index = i;
		p->count = desc->count;
		p->start_time = le32_to_cpu(desc->start_time);
		p->interval = le32_to_cpu(desc->interval);
		p->duration = le32_to_cpu(desc->duration);

		DBG_MAC("%s(%d/%d): NoA cnt: %d,st: %d, dur: %d, intv: %d",
			__func__, noa->index, i, p->count, p->start_time,
			p->duration, p->interval);
	}
}

#define BSS_CHANGED_ERP                                        \
	(BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_ERP_PREAMBLE | \
	 BSS_CHANGED_ERP_SLOT)

/* S1G Short Beacon Interval (in TU) */
#define DEF_CFG_S1G_SHORT_BEACON_COUNT 10

/* ---------- bss_info_changed helpers ------------------------------------ */

/**
 * nrc_bss_handle_assoc - Handle BSS_CHANGED_ASSOC
 *
 * Calls nrc_bss_assoc() on association, manages the beacon-monitor timer,
 * and auto-starts the dynamic PS timer for deep-sleep modes.
 */
static void nrc_bss_handle_assoc(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif,
				 struct ieee80211_bss_conf *info,
				 struct sk_buff *skb)
{
	struct nrc *nw = hw->priv;
	struct nrc_vif *i_vif = to_i_vif(vif);
	bool assoc;

	assoc = vif->cfg.assoc;

	if (assoc) {
		nrc_bss_assoc(hw, vif, info, skb);

		spin_lock_bh(&nw->vif_lock);
		i_vif->associated = true;
		if (!nw->params->disable_cqm) {
			DBG_MAC("mod_timer in %s:%d", __FUNCTION__, __LINE__);
			mod_timer(&i_vif->bcn_mon_timer,
				  jiffies + msecs_to_jiffies(
						    i_vif->beacon_timeout));
		}
		spin_unlock_bh(&nw->vif_lock);

		/*
		 * Auto-start PS timer for deep sleep modes.  NRC deep sleep
		 * is driver-managed and mac80211 may never set
		 * dynamic_ps_timeout (leaving it 0).  Fall back to
		 * NRC_PS_DEFAULT_TIMEOUT_MS so deep sleep starts after
		 * association regardless.
		 * Do NOT write hw->conf.dynamic_ps_timeout — that belongs to
		 * mac80211.
		 */
		if (nw->params->power_save >= NRC_PS_DEEPSLEEP_TIM) {
			nw->hdev->ps.timeout =
				hw->conf.dynamic_ps_timeout > 0 ?
					hw->conf.dynamic_ps_timeout :
					NRC_PS_DEFAULT_TIMEOUT_MS;
			DBG_MAC("[BSS_CHANGED_ASSOC] Auto PS start (mode=%d), timeout=%d ms",
				nw->params->power_save, nw->hdev->ps.timeout);
			nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_BSS_CONFIG);
		}
	} else {
		spin_lock_bh(&nw->vif_lock);
		if (!nw->params->disable_cqm) {
			i_vif->beacon_timeout = 0;
			DBG_MAC("del_timer in %s:%d", __FUNCTION__, __LINE__);
			try_to_del_timer_sync(&i_vif->bcn_mon_timer);
			i_vif->is_bcn_timeout = false;
		}
		i_vif->associated = false;
		spin_unlock_bh(&nw->vif_lock);
	}

	DBG_MAC("[BSS_CHANGED_ASSOC] VIF%d associated:%d beacon_timeout:%lu",
		i_vif->index, i_vif->associated, i_vif->beacon_timeout);
}

/**
 * nrc_bss_handle_beacon_int - Handle BSS_CHANGED_BEACON_INT / BEACON_ENABLED
 * @beacon_enabled_changed: true when BSS_CHANGED_BEACON_ENABLED is also set
 *
 * Computes the S1G Short Beacon Interval for AP/MESH and pushes beacon
 * timing TLVs.  BSS_CHANGED_BEACON_ENABLED is AP/MESH only.
 */
static void nrc_bss_handle_beacon_int(struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif,
				      struct ieee80211_bss_conf *info,
				      struct sk_buff *skb,
				      bool beacon_enabled_changed)
{
	struct nrc *nw = hw->priv;
	struct nrc_vif *i_vif = to_i_vif(vif);
	u16 bi, short_bi = 0;
	u8 dtim_period = info->dtim_period;

	DBG_MAC("beacon: %s, interval=%u, dtim_period:%u",
		info->enable_beacon ? "enabled" : "disabled", info->beacon_int,
		info->dtim_period);

	nw->beacon_int = bi = info->beacon_int;
	if (!nw->params->disable_cqm && vif->type == NL80211_IFTYPE_STATION) {
		i_vif->beacon_timeout = nw->params->beacon_loss_count * bi;
		DBG_MAC("[BSS_CHANGED_BEACON_INT] assoc:%d beacon_timeout:%lu",
			i_vif->associated ? i_vif->index : -1,
			i_vif->beacon_timeout);
	}

	/*
	 * S1G Short Beacon Interval: when enable_short_bi is set, the value
	 * from hostapd/wpa_supplicant is treated as the Short BI and the full
	 * BI is Short BI * DEF_CFG_S1G_SHORT_BEACON_COUNT (default 10).
	 * For STA the target extracts the Short BI from received beacons.
	 */
	if ((vif->type == NL80211_IFTYPE_AP ||
	     vif->type == NL80211_IFTYPE_MESH_POINT
#if defined(CONFIG_SUPPORT_IBSS)
	     || vif->type == NL80211_IFTYPE_ADHOC
#endif
	     ) &&
	    nrc_mac_is_s1g(nw->hdev) && nw->params->enable_short_bi) {
		short_bi = info->beacon_int;
		/* beacon interval is 16-bit; clamp the multiplied value */
		if (DEF_CFG_S1G_SHORT_BEACON_COUNT * short_bi <= 65535)
			bi = DEF_CFG_S1G_SHORT_BEACON_COUNT * short_bi;
		else
			bi = (65535 / short_bi) * short_bi;
	}

	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BCN_INTV, sizeof(bi), &bi);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SHORT_BCN_INTV,
				    sizeof(short_bi), &short_bi);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_DTIM_PERIOD,
				    sizeof(dtim_period), &dtim_period);

#if defined(CONFIG_SUPPORT_BEACON_BYPASS)
	if (nw->params->enable_beacon_bypass) {
		uint8_t enable = (uint8_t)nw->params->enable_beacon_bypass;

		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BEACON_BYPASS,
					    sizeof(u8), &enable);
	}
#endif /* CONFIG_SUPPORT_BEACON_BYPASS */

	if (beacon_enabled_changed)
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BEACON_ENABLE,
					    sizeof(info->enable_beacon),
					    &info->enable_beacon);

	/* TWT scheduling must start after beacon interval is configured */
	if (vif->type == NL80211_IFTYPE_AP)
		nrc_twt_sched_start(nw, vif);
}

/**
 * nrc_bss_handle_txpower - Handle BSS_CHANGED_TXPOWER
 */
static void nrc_bss_handle_txpower(struct ieee80211_hw *hw,
				   struct ieee80211_bss_conf *info,
				   struct sk_buff *skb)
{
	int txpower = info->txpower;
	uint16_t txpower_type = info->txpower_type;

	INFO("%s(changed:%s[PW=%d TYPE=%s])", __func__, "BSS_CHANGED_TXPOWER",
	     txpower,
	     txpower_type == TXPWR_LIMIT ? "limit" :
	     txpower_type		 ? "fixed" :
					   "auto");

#ifdef CONFIG_SUPPORT_IW_IWCONFIG_TXPWR
	if (txpower < 1 || txpower > 30) {
		INFO("%s invalid txpower (%d)", __func__, txpower);
	} else {
		u32 p = (txpower_type << 16) | txpower;

		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SET_TXPOWER,
					    sizeof(u32), &p);
	}
#endif /* CONFIG_SUPPORT_IW_IWCONFIG_TXPWR */
}

/**
 * nrc_bss_handle_ps - Handle BSS_CHANGED_PS
 *
 * Handles mac80211 PS notifications only for modem-sleep mode (power_save=1).
 * Skipped for:
 *   - NRC_PS_NONE: nrc_ps_set_mode() is a no-op when power_save=0, so
 *     mac80211 PS notifications have no effect on the driver or firmware.
 *   - NRC_PS_DEEPSLEEP_TIM/NONTIM: driver-managed; mac80211 always reports
 *     dynamic_ps_timeout=0 for NRC deep sleep which must not override the
 *     timeout installed in nrc_bss_handle_assoc().
 */
static void nrc_bss_handle_ps(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_bss_conf *info)
{
	struct nrc *nw = hw->priv;
	bool ps_on;

	if (nw->params->power_save == NRC_PS_NONE) {
		DBG_MAC("%s(changed:%s) PS disabled — skipping", __func__,
			"BSS_CHANGED_PS");
		return;
	}

	if (nw->params->power_save >= NRC_PS_DEEPSLEEP_TIM) {
		DBG_MAC("%s(changed:%s) deep-sleep mode — skipping", __func__,
			"BSS_CHANGED_PS");
		return;
	}

	ps_on = vif->cfg.ps;

	DBG_MAC("%s(changed:%s) ps=%d timeout=%d", __func__, "BSS_CHANGED_PS",
		ps_on, hw->conf.dynamic_ps_timeout);
	nrc_mac_apply_ps(nw, ps_on, hw->conf.dynamic_ps_timeout);
}

/* ---------- bss_info_changed dispatcher --------------------------------- */

void nrc_mac_bss_info_changed(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_bss_conf *info,
			      u64 changed)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct sk_buff *skb;
	int ret;

	DBG_MAC("%s: changed=0x%llx", __func__, (u64)changed);

	if (NRC_DRV_IS_ASLEEP(hdev))
		return;

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	if (!skb)
		return;

	if (changed & BSS_CHANGED_ASSOC)
		nrc_bss_handle_assoc(hw, vif, info, skb);

#ifndef CONFIG_S1G_CHANNEL
	if (changed & BSS_CHANGED_BASIC_RATES) {
		DBG_MAC("basic_rate: %08x", info->basic_rates);
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BASIC_RATE,
					    sizeof(info->basic_rates),
					    &info->basic_rates);
	}
	if (changed & BSS_CHANGED_HT) {
		DBG_MAC("ht: %08x", info->ht_operation_mode);
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_HT_MODE,
					    sizeof(info->ht_operation_mode),
					    &info->ht_operation_mode);
	}
#endif

	if (changed & BSS_CHANGED_BSSID) {
		DBG_MAC("bssid=%pM", info->bssid);
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BSSID, ETH_ALEN,
					    (void *)info->bssid);
	}

	if (changed & (BSS_CHANGED_BEACON_INT | BSS_CHANGED_BEACON_ENABLED)) {
		/*
		 * BEACON_ENABLE is owned exclusively by start_ap / stop_ap.
		 * bss_info_changed is the live-reconfiguration path and only
		 * updates timing parameters (BCN_INTV, SHORT_BCN_INTV, DTIM).
		 */
		nrc_bss_handle_beacon_int(hw, vif, info, skb, false);
	}

	if (changed & BSS_CHANGED_BEACON)
		nrc_vendor_update_beacon(hw, vif);

	if (changed & BSS_CHANGED_SSID) {
		DBG_MAC("ssid=%s", vif->cfg.ssid);
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_SSID,
					    vif->cfg.ssid_len, vif->cfg.ssid);
	}

	if (changed & BSS_CHANGED_ERP) {
		struct wim_erp_param *p;

		p = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_ERP_PARAM,
						sizeof(*p), NULL);
		p->use_11b_protection = info->use_cts_prot;
		p->use_short_preamble = info->use_short_preamble;
		p->use_short_slot = info->use_short_slot;
	}

	/* TODO: implement */
	if (changed & BSS_CHANGED_CQM)
		DBG_MAC("%s(changed:%s)", __func__, "BSS_CHANGED_CQM");
	if (changed & BSS_CHANGED_IBSS)
		DBG_MAC("%s(changed:%s)", __func__, "BSS_CHANGED_IBSS");
	if (changed & BSS_CHANGED_ARP_FILTER)
		DBG_MAC("%s(changed:%s)", __func__, "BSS_CHANGED_ARP_FILTER");
	if (changed & BSS_CHANGED_IDLE)
		DBG_MAC("%s(changed:%s)", __func__, "BSS_CHANGED_IDLE");

	if (changed & BSS_CHANGED_TXPOWER)
		nrc_bss_handle_txpower(hw, info, skb);

	if (changed & BSS_CHANGED_P2P_PS) {
		DBG_MAC("%s(changed:%s)", __func__, "BSS_CHANGED_P2P_PS");
		nrc_mac_update_p2p_ps(skb, vif);
	}

	if (changed & BSS_CHANGED_BEACON_INFO)
		/* dtim_period for STA; target reads it from TIM IE directly */
		DBG_MAC("%s(changed:%s) dtim_period:%u", __func__,
			"BSS_CHANGED_BEACON_INFO", info->dtim_period);

	if (changed & BSS_CHANGED_BANDWIDTH)
		DBG_MAC("%s(changed:%s)", __func__, "BSS_CHANGED_BANDWIDTH");
	if (changed & BSS_CHANGED_OCB)
		DBG_MAC("%s(changed:%s)", __func__, "BSS_CHANGED_OCB");

	if (changed & BSS_CHANGED_PS)
		nrc_bss_handle_ps(hw, vif, info);

	if (skb->len > sizeof(struct wim)) {
		ret = nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
		if (ret < 0) {
			ERR("failed to transmit a wim request (ret=%d)", ret);
			NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false,
					   false);
		}
	} else {
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false, false);
	}
}

/**
 * nrc_mac_start_ap - mac80211 start_ap callback
 *
 * Called by mac80211 after all AP configuration (beacon interval, DTIM,
 * beacon template) is set in bss_conf and the beacon can be retrieved.
 * This is the proper place to initialise AP beaconing — mac80211 no longer
 * delivers BSS_CHANGED_BEACON / BSS_CHANGED_BEACON_ENABLED via
 * bss_info_changed() for the initial AP start.
 */
static int nrc_mac_start_ap(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    struct ieee80211_bss_conf *link_conf)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct ieee80211_bss_conf *info = &vif->bss_conf;
	struct sk_buff *skb;
	int ret;

	INFO_STATE("start_ap: vif=%pM beacon_int=%u dtim_period=%u", vif->addr,
		   info->beacon_int, info->dtim_period);

#if defined(CONFIG_SUPPORT_BD)
	if (!g_bd_valid) {
		ERR_STATE("start_ap: BD not loaded in FW, blocking");
		return -EINVAL;
	}
#endif

	if (!vif->bss_conf.chandef.chan) {
		ERR_STATE(
			"start_ap: channel not configured (chandef.chan is NULL)");
		return -EINVAL;
	}

	/* Send beacon template to firmware */
	ret = nrc_vendor_update_beacon(hw, vif);
	if (ret) {
		ERR_STATE("start_ap: failed to update beacon template (ret=%d)",
			  ret);
		return ret;
	}

	/* Send beacon interval, DTIM, and enable beacon */
	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	if (!skb)
		return -ENOMEM;

	nrc_bss_handle_beacon_int(hw, vif, info, skb, true);

	if (skb->len > sizeof(struct wim)) {
		ret = nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
		if (ret < 0) {
			ERR_STATE("start_ap: failed WIM request (ret=%d)", ret);
			NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false,
					   false);
			return ret;
		}
	} else {
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false, false);
	}

	return 0;
}

/**
 * nrc_mac_stop_ap - mac80211 stop_ap callback
 *
 * Called when the AP interface is stopped.  Disables beaconing in firmware.
 */
static void nrc_mac_stop_ap(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    struct ieee80211_bss_conf *link_conf)
{
	struct nrc_hif_device *hdev = ((struct nrc *)hw->priv)->hdev;
	struct sk_buff *skb;
	u8 beacon_enable = 0;

	INFO_STATE("stop_ap: vif=%pM", vif->addr);

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	if (!skb)
		return;

	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BEACON_ENABLE,
				    sizeof(beacon_enable), &beacon_enable);

	if (skb->len > sizeof(struct wim)) {
		int ret = nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);

		if (ret < 0) {
			ERR_STATE("stop_ap: failed WIM request (ret=%d)", ret);
			NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false,
					   false);
		}
	} else {
		NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false, false);
	}
}

static int nrc_mac_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta)
{
	struct nrc *nw = hw->priv;
	int i;

	DBG_MAC("%s", __func__);
	nrc_stats_add(sta->addr, 16);
	//nrc_stats_print();
	nrc_wim_wlan_change_sta(vif, sta, WIM_STA_CMD_ADD, 0);

	/* Initialize txq driver data.
	 * Assumptions are:
	 * (1) per-sta per-tid txq is not in use up to this point (associated).
	 * (2) EAPOL frames do not resort to txq.
	 */
	for (i = 0; i < ARRAY_SIZE(sta->txq); i++)
		nrc_init_txq(sta->txq[i], vif, sta);

	/* Set higher timeout of mac80211 TX Queue */
	if (nrc_mac_is_s1g(nw->hdev)) {
		rate_control_set_rates(hw, sta, NULL);
	}
	return 0;
}

int nrc_mac_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta)
{
	struct nrc *nw = (struct nrc *)hw->priv;
	struct nrc_sta *i_sta = to_i_sta(sta);

	nrc_stats_del(sta->addr);
	nrc_twt_sched_entry_del_all(nw, i_sta);
	//nrc_stats_print();
	nrc_wim_wlan_change_sta(vif, sta, WIM_STA_CMD_REMOVE, 0);

	return 0;
}

static void nrc_tx_ba_session_work(struct work_struct *work)
{
	struct tx_ba_session *ba_session =
		container_of(work, struct tx_ba_session, ba_session_work);
	struct nrc_sta *i_sta = ba_session->sta;
	struct ieee80211_sta *peer_sta = to_ieee80211_sta(i_sta);
	int ret = 0;

	if (!peer_sta) {
		DBG_MAC("Fail to set up BA. peer_sta is NULL");
		return;
	}

	switch (ba_session->state) {
	case IEEE80211_BA_NONE:
	case IEEE80211_BA_CLOSE:
		DBG_AMPDU("Setup TX BA TID:%d %pM", ba_session->tid,
			  peer_sta->addr);
		i_sta->nw->hdev->ampdu_supported = true;
		i_sta->nw->ampdu_reject = false;
		if ((ret = ieee80211_start_tx_ba_session(
			     peer_sta, ba_session->tid, 0)) != 0) {
			if (ret == -EBUSY) {
				DBG_AMPDU("RX rejected BA TID:%d",
					  ba_session->tid);
				ba_session->state = IEEE80211_BA_DISABLE;
			} else if (ret == -EAGAIN) {
				DBG_AMPDU("BA busy TID:%d", ba_session->tid);
				ieee80211_stop_tx_ba_session(peer_sta,
							     ba_session->tid);
				ba_session->state = IEEE80211_BA_NONE;
				ba_session->ba_req_last_jiffies = 0;
			} else if (ret == -EINVAL) {
				DBG_AMPDU("Invalid BA TID:%d", ba_session->tid);
				ieee80211_stop_tx_ba_session(peer_sta,
							     ba_session->tid);
				ba_session->state = IEEE80211_BA_NONE;
				ba_session->ba_req_last_jiffies = 0;
			} else if (ret == -ENOMEM) {
				DBG_AMPDU("BA alloc fail TID:%d",
					  ba_session->tid);
				ieee80211_stop_tx_ba_session(peer_sta,
							     ba_session->tid);
				ba_session->state = IEEE80211_BA_NONE;
				ba_session->ba_req_last_jiffies = 0;
			}
		}
		break;
	case IEEE80211_BA_REJECT:
		if (jiffies_to_msecs(jiffies -
				     ba_session->ba_req_last_jiffies) > 5000) {
			ba_session->state = IEEE80211_BA_NONE;
			ba_session->ba_req_last_jiffies = 0;
			DBG_AMPDU("Reset BA TID:%d", ba_session->tid);
		}
		break;
	case IEEE80211_BA_REQUEST:
		if (jiffies_to_msecs(jiffies -
				     ba_session->ba_req_last_jiffies) > 5000) {
			ba_session->state = IEEE80211_BA_NONE;
			ba_session->ba_req_last_jiffies = 0;
			DBG_AMPDU("Timeout BA TID:%d", ba_session->tid);
		}
		break;
	default:
		break;
	}
}

static void nrc_init_sta_ba_session(struct ieee80211_sta *sta)
{
	struct nrc_sta *i_sta = NULL;
	int i;

	if (sta) {
		i_sta = to_i_sta(sta);
		if (i_sta) {
			for (i = 0; i < NRC_MAX_TID; i++) {
				i_sta->tx_ba_session[i].sta = i_sta;
				i_sta->tx_ba_session[i].tid = i;
				i_sta->tx_ba_session[i].state =
					IEEE80211_BA_NONE;
				i_sta->tx_ba_session[i].ba_req_last_jiffies = 0;
				INIT_WORK(
					&i_sta->tx_ba_session[i].ba_session_work,
					nrc_tx_ba_session_work);
				i_sta->rx_ba_session[i].started = false;
				i_sta->rx_ba_session[i].sn = 0;
				i_sta->rx_ba_session[i].buf_size =
					IEEE80211_MAX_AMPDU_BUF_HT;
			}
		}
	}
}

static void nrc_deinit_sta_ba_session(struct nrc_sta *i_sta)
{
	int i;

	if (!i_sta)
		return;

	for (i = 0; i < NRC_MAX_TID; i++) {
		if (i_sta->tx_ba_session[i].ba_session_work.func) {
			cancel_work_sync(
				&i_sta->tx_ba_session[i].ba_session_work);
		}
	}
}

static int nrc_wim_change_sta_state(struct nrc *nw, struct ieee80211_vif *vif,
				    struct ieee80211_sta *sta, int new_state)
{
	int state = WIM_STA_CMD_STATE_NOTEXIST;

	switch (new_state) {
	case IEEE80211_STA_NOTEXIST:
		state = WIM_STA_CMD_STATE_NOTEXIST;
		break;
	case IEEE80211_STA_NONE:
		state = WIM_STA_CMD_STATE_NONE;
		break;
	case IEEE80211_STA_AUTH:
		state = WIM_STA_CMD_STATE_AUTH;
		break;
	case IEEE80211_STA_ASSOC:
		state = WIM_STA_CMD_STATE_ASSOC;
		break;
	case IEEE80211_STA_AUTHORIZED:
		state = WIM_STA_CMD_STATE_AUTHORIZED;

		if (nw->params->ampdu_mode == NRC_AMPDU_DISABLE) {
			DBG_AMPDU("disabled");
			nw->hdev->ampdu_supported = false;
			nw->ampdu_reject = true;
			nw->ampdu_started = false;
		} else {
			DBG_AMPDU("ready %pM", sta->addr);
			nrc_init_sta_ba_session(sta);
#ifdef CONFIG_S1G_CHANNEL
			sta->ht_cap.ht_supported = true;
#endif /* #ifdef CONFIG_S1G_CHANNEL */
			nw->hdev->ampdu_supported = true;
			nw->ampdu_reject = false;
			nw->ampdu_started = true;
		}
		break;
	}

	return nrc_wim_wlan_change_sta(vif, sta, state, 0);
}

static u16 convert_usf(u32 interval)
{
	u16 ui, usf, interval_usf;

	if (interval <= S1G_UNSCALED_INTERVAL_MAX) {
		ui = interval;
		usf = 0;
	} else if (interval / 10 <= S1G_UNSCALED_INTERVAL_MAX) {
		ui = interval / 10;
		usf = 1;
	} else if (interval / 1000 <= S1G_UNSCALED_INTERVAL_MAX) {
		ui = interval / 1000;
		usf = 2;
	} else if (interval / 10000 <= S1G_UNSCALED_INTERVAL_MAX) {
		ui = interval / 10000;
		usf = 3;
	} else {
		ui = 0;
		usf = 0;
	}

	interval_usf = (usf << 14) + ui;

	return interval_usf;
}

static int nrc_mac_sta_state(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			     struct ieee80211_sta *sta,
			     enum ieee80211_sta_state old_state,
			     enum ieee80211_sta_state new_state)
{
	struct nrc_vif *i_vif = to_i_vif(vif);
	struct nrc_sta *i_sta = to_i_sta(sta);
	unsigned long flags;
	const struct nrc_sta_handler *h;
	struct nrc *nw = (struct nrc *)hw->priv;
	int i;

	DBG_STATE("%s: sta:%pM, %d->%d", __func__, sta->addr, old_state,
		  new_state);

	i_sta->state = new_state;

#define state_changed(old, new) \
	(old_state == IEEE80211_STA_##old && new_state == IEEE80211_STA_##new)

	if (state_changed(NOTEXIST, NONE)) {
		memset(i_sta, 0, sizeof(*i_sta));
		i_sta->nw = nw;
		i_sta->vif = vif;

		INIT_LIST_HEAD(&i_sta->list);

		spin_lock_irqsave(&i_vif->preassoc_sta_lock, flags);
		list_add_tail(&i_sta->list, &i_vif->preassoc_sta_list);
		spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);

		/* This value is set in rx_h_bss_max_idle_period normally when assoc frames are received
		   but, While test of ifconfig up /down, some weired transition occured without any frame exchanges.
		   so, values is set to zero. this causes invalid timer setting on STA.
		*/

		/* set default max_idle_period from AP */
		DBG_MAC("%s: set default max_idle_period with AP's period: %lu",
			__FUNCTION__, i_vif->max_idle_period);
		if (nrc_mac_is_s1g(nw->hdev) && !no_convert_usf) {
			/* Convert in USF Format (Value (14bit) * USF(2bit)) and save it */
			i_sta->max_idle.period =
				convert_usf(i_vif->max_idle_period);
		} else {
			i_sta->max_idle.period = i_vif->max_idle_period;
		}
		i_sta->max_idle.options = 0;
		if (vif->type == NL80211_IFTYPE_AP) {
			i_sta->max_idle.idle_period =
				i_sta->max_idle.sta_idle_timer =
					i_vif->max_idle_period;
		}

	} else if (state_changed(NONE, NOTEXIST)) {
		nrc_twt_sched_entry_del_all(nw, i_sta);
		nrc_deinit_sta_ba_session(i_sta);

		spin_lock_irqsave(&i_vif->preassoc_sta_lock, flags);
		list_del_init(&i_sta->list);
		spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);
	} else if (state_changed(NONE, AUTH)) {
	} else if (state_changed(AUTH, ASSOC)) {
		i_sta->vif = vif;
		nrc_mac_sta_add(hw, vif, sta);

	} else if (state_changed(ASSOC, AUTH)) {
		int i;
		for (i = 0; i < ARRAY_SIZE(sta->txq); i++) {
			nrc_cleanup_txq(nw, sta->txq[i]);
		}
		nrc_mac_sta_remove(hw, vif, sta);
	}

	nrc_wim_change_sta_state(nw, vif, sta, new_state);

	for (i = 0; i < nrc_sta_handlers_count; i++) {
		h = &nrc_sta_handlers[i];
		h->sta_state(hw, vif, sta, old_state, new_state);
	}

	return 0;
}

static void nrc_mac_sta_pre_rcu_remove(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif,
				       struct ieee80211_sta *sta)
{
	struct nrc_vif *i_vif = to_i_vif(vif);
	struct nrc_sta *i_sta = to_i_sta(sta);
	unsigned long flags;

	spin_lock_irqsave(&i_vif->preassoc_sta_lock, flags);
	list_del_init(&i_sta->list);
	spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);
}

static void nrc_mac_sta_notify(struct ieee80211_hw *hw,
			       struct ieee80211_vif *vif,
			       enum sta_notify_cmd cmd,
			       struct ieee80211_sta *sta)
{
	nrc_wim_wlan_change_sta(vif, sta, WIM_STA_CMD_NOTIFY,
				cmd == STA_NOTIFY_SLEEP);
}

static int nrc_mac_set_tim(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
			   bool set)
{
	struct nrc *nw = hw->priv;
	struct nrc_sta *i_sta = to_i_sta(sta);
	struct sk_buff *skb;
	struct wim_tim_param *tim;

	if (WARN_ON(i_sta->vif == NULL))
		return -EINVAL;

	skb = nrc_hal_ops_wim_alloc_skb_vif(i_sta->vif, WIM_CMD_SET,
					    WIM_MAX_SIZE);

	tim = nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_TIM_PARAM, sizeof(*tim),
					  NULL);
	tim->aid = sta->aid;
	tim->set = set;

	if (!nrc_mac_is_s1g(nw->hdev)) {
		struct sk_buff *b = ieee80211_beacon_get_template(
			hw, i_sta->vif, NULL, i_sta->vif->bss_conf.link_id);
		if (b) {
			/* Track beacon template SKB from mac80211 (count_only, will be freed below) */
			NRC_SKB_TRACK_ALLOC(nw->hdev, b, HIF_TYPE_FRAME, false,
					    false);
			nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BEACON, b->len,
						    b->data);
			/* Track beacon buffer free (from mac80211) */
			NRC_SKB_TRACK_FREE(nw->hdev, b, HIF_TYPE_FRAME, false,
					   false);
		}
	}

	return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
}

static u16 mac80211_to_nrc_aci_map[4] = {3, 2, 1, 0};

int nrc_mac_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    unsigned int link_id,
		    u16 ac, const struct ieee80211_tx_queue_params *params)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct sk_buff *skb;
	static struct wim_tx_queue_param tqp[NRC_QUEUE_MAX];

	/* Bounds the mac80211_to_nrc_aci_map[] lookup below */
	if (ac < IEEE80211_AC_VO || ac > IEEE80211_AC_BK) {
		ERR("Invalid access category %u", ac);
		return -EINVAL;
	}
	ac = mac80211_to_nrc_aci_map[ac];

	if (NRC_HIF_DRV_STATE(hdev) >= NRC_DRV_RUNNING) {
		if (tqp[ac].txop != params->txop ||
		    tqp[ac].cw_min != params->cw_min ||
		    tqp[ac].cw_max != params->cw_max ||
		    tqp[ac].aifsn != params->aifs ||
		    tqp[ac].uapsd != params->uapsd || tqp[ac].ac != ac) {
			skb = nrc_hal_ops_wim_alloc_skb_vif(
				vif, WIM_CMD_SET,
				tlv_len(sizeof(struct wim_tx_queue_param)));
			tqp[ac].ac = ac;
			tqp[ac].txop = params->txop;
			tqp[ac].cw_min = params->cw_min;
			tqp[ac].cw_max = params->cw_max;
			tqp[ac].aifsn = params->aifs;
			tqp[ac].uapsd = params->uapsd;
			nrc_hal_ops_wim_skb_add_tlv(
				skb, WIM_TLV_TXQ_PARAM,
				sizeof(struct wim_tx_queue_param), &tqp[ac]);

			/*
			   don't need to compare values and block log
			   It's because the incorrect transformation of the short beacon no longer sets the counter value correctly.
			 */
			DBG_MAC("%s: ac=%d txop=%d cw_min=%d cw_max=%d aifs=%d uapsd=%d",
				__func__, ac, params->txop, params->cw_min,
				params->cw_max, params->aifs, params->uapsd);

			return nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
		}
	}
	return 0;
}

static int nrc_mac_get_survey(struct ieee80211_hw *hw, int idx,
			      struct survey_info *survey)
{
	struct stats_channel_noise *channel_noise;

	DBG_MAC("%s (idx=%d)", __func__, idx);

	channel_noise = nrc_stats_channel_noise_report(idx, 0);
	if (!channel_noise)
		return -ENOENT;

	survey->channel = channel_noise->chan;

	/*
	 * Magically conjured noise level
	 * --- this is only ok for simulated hardware.
	 *
	 * A real driver which cannot determine the real channel noise MUST NOT
	 * report any noise, especially not a magically conjured one :-)
	 */
	survey->filled = SURVEY_INFO_NOISE_DBM;
	survey->noise = channel_noise->noise;

	return 0;
}

static int nrc_mac_ampdu_action(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_ampdu_params *params)
{
	struct nrc *nw = hw->priv;
	struct nrc_sta *i_sta = NULL;

	enum ieee80211_ampdu_mlme_action action = params->action;
	struct ieee80211_sta *sta = params->sta;
	u16 tid = params->tid;
	u16 *ssn = &params->ssn;
	u16 buf_size = params->buf_size;
	int ret = 0;

	mutex_lock(&nw->state_mtx);

	// DBG_MAC("%s called", __FUNCTION__);

	if (!sta || tid >= NRC_MAX_TID) {
		ERR("sta is NULL");
		ret = -EOPNOTSUPP;
		goto out;
	}

	i_sta = to_i_sta(sta);
	DBG_AMPDU("action: %pM TID(%d)", sta->addr, tid);

	switch (action) {
	case IEEE80211_AMPDU_TX_START:
		DBG_AMPDU("action: TX_START");
		if (!nw->hdev->ampdu_supported ||
		    !sta->deflink.ht_cap.ht_supported) {
			ret = -EOPNOTSUPP;
			goto out;
		}

		if (nrc_wim_wlan_ampdu_action(vif, WIM_AMPDU_TX_START, sta,
					      tid)) {
			ret = -EOPNOTSUPP;
			goto out;
		}

		/*
		 * Freeze dynamic PS during BA session setup: the device must
		 * stay awake until mac80211 calls back with TX_OPERATIONAL
		 * (success) or TX_STOP_* (failure/timeout).  No timer-based
		 * approximation is needed because mac80211 guarantees one of
		 * those two completion callbacks will follow.
		 */
		nrc_ps_dyn_stop(nw, NRC_PS_REASON_DRV_BSS_CONFIG);

		i_sta->tx_ba_session[tid].state = IEEE80211_BA_REQUEST;
		i_sta->tx_ba_session[tid].ba_req_last_jiffies = jiffies;
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		/*
		 * Returning 0 from TX_START makes mac80211
		 * send ADDBA immediately in the same call stack, which races
		 * with concurrent ieee80211_stop_tx_ba_session() setting
		 * WANT_STOP via sta->lock (no wiphy lock needed). Use
		 * DELAY_ADDBA to defer ADDBA to the callback work path
		 * which checks STOPPING/WANT_STOP gracefully.
		 */
		ret = IEEE80211_AMPDU_TX_START_DELAY_ADDBA;
		break;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
		DBG_AMPDU("action: TX_STOP_FLUSH");
		i_sta->tx_ba_session[tid].state = IEEE80211_BA_CLOSE;
		/* BA setup failed/flushed: resume normal PS idle timer */
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_BSS_CONFIG);
		break;
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		DBG_AMPDU("action: TX_STOP_FLUSH_CONT");
		i_sta->tx_ba_session[tid].state = IEEE80211_BA_CLOSE;
		/* BA setup failed/flushed: resume normal PS idle timer */
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_BSS_CONFIG);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		DBG_AMPDU("action: TX_STOP_CONT");
		if (nrc_wim_wlan_ampdu_action(vif, WIM_AMPDU_TX_STOP, sta,
					      tid)) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		i_sta->tx_ba_session[tid].state = IEEE80211_BA_REJECT;
		i_sta->tx_ba_session[tid].ba_req_last_jiffies = jiffies;
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		/* BA session ended: resume normal PS idle timer */
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_BSS_CONFIG);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		DBG_AMPDU("action: TX_OPERATIONAL");
		i_sta->tx_ba_session[tid].state = IEEE80211_BA_ACCEPT;
		if (nrc_wim_wlan_ampdu_action(vif, WIM_AMPDU_TX_OPERATIONAL,
					      sta, tid)) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		/*
		 * BA session is now fully operational.  Resume the normal
		 * dynamic PS idle timer so the device can sleep once TX
		 * traffic settles.
		 */
		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_BSS_CONFIG);
		ret = 0;
		goto out;
	case IEEE80211_AMPDU_RX_START:
		DBG_AMPDU("action: RX_START");
		i_sta->rx_ba_session[tid].sn = *ssn;
		i_sta->rx_ba_session[tid].buf_size = buf_size;
		i_sta->rx_ba_session[tid].started = true;
		if (nw->ampdu_reject) {
			ERR("Reject AMPDU");
			ret = -EOPNOTSUPP;
			goto out;
		}
		ret = 0;
		goto out;
	case IEEE80211_AMPDU_RX_STOP:
		DBG_AMPDU("action: RX_STOP");
		i_sta->rx_ba_session[tid].started = false;
		if (nw->ampdu_reject) {
			ERR("Reject AMPDU");
			ret = -EOPNOTSUPP;
			goto out;
		}
		ret = 0;
		goto out;
	default:
		ret = -EOPNOTSUPP;
		goto out;
	}

	params->amsdu = nw->amsdu_supported;
out:
	mutex_unlock(&nw->state_mtx);
	return ret;
}

/* This must be matched with NRC_SCAN_MODE */
static char *scan_status_str[] = {
	"IDLE", "ACTIVE", "PASSIVE", "SCHED", "ABORTING",
};

static char *nrc_mac_scan_status_str(enum NRC_SCAN_MODE status)
{
	/*
	 * Bounds scan_status_str[]. This only feeds debug logs, so an
	 * unexpected value must not be fatal. The enum is unsigned, so the
	 * upper bound is the only check needed.
	 */
	if (status >= NRC_SCAN_MODE_MAX)
		return "UNKNOWN";

	return scan_status_str[status];
}

static void change_scan_mode(struct nrc *nw, enum NRC_SCAN_MODE new_mode)
{
	DBG_MAC("scan mode changed %s->%s ",
		nrc_mac_scan_status_str(atomic_read(&nw->scan_mode)),
		nrc_mac_scan_status_str(new_mode));

	atomic_set(&nw->scan_mode, new_mode);
}

static void scan_complete(struct ieee80211_hw *hw, bool aborted)
{
	struct nrc *nw = hw->priv;

	struct cfg80211_scan_info info = {
		.aborted = aborted,
	};

	ieee80211_scan_completed(hw, &info);

	DBG_MAC("scan_complete: aborted=%d", aborted);

	nrc_ps_set_idle_mode_delay(nw, "scan complete", 2000);
}

void beacon_loss_check_work_handler(struct work_struct *work)
{
	struct nrc *nw = container_of(to_delayed_work(work), struct nrc,
				      beacon_loss_work);
	int i;

	DBG_STATE("check delayed beacon loss");

	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE)
		return;

	for (i = 0; i < NR_NRC_VIF; i++) {
		struct nrc_vif *i_vif;

		if (!nw->vif[i] || nw->vif[i]->type != NL80211_IFTYPE_STATION)
			continue;
		i_vif = to_i_vif(nw->vif[i]);
		if (i_vif->is_bcn_timeout) {
			DBG_STATE("VIF%d: no beacons received since last scan",
				  i_vif->index);
			ieee80211_beacon_loss(nw->vif[i]);
			i_vif->is_bcn_timeout = false;
		}
	}
}

void nrc_mac_scan_completed_work_handler(struct work_struct *work)
{
	struct wim_event_work *w =
		container_of(work, struct wim_event_work, work);
	struct nrc *nw = w->nw;
#ifdef CONFIG_USE_SCAN_TIMEOUT
	struct ieee80211_vif *vif = w->vif;
#endif

	//int delay_ms = 220;
	int delay_ms = nw->beacon_int * 3;

	/* timer must be updated first before changing scan_mode to IDLE */
	if (!nw->params->disable_cqm) {
		int i;

		for (i = 0; i < NR_NRC_VIF; i++) {
			struct nrc_vif *iv;

			if (!nw->vif[i] ||
			    nw->vif[i]->type != NL80211_IFTYPE_STATION)
				continue;
			iv = to_i_vif(nw->vif[i]);
			if (iv->associated)
				mod_timer(&iv->bcn_mon_timer,
					  jiffies +
						  msecs_to_jiffies(
							  iv->beacon_timeout));
		}
	}

	mutex_lock(&nw->state_mtx);
	DBG_MAC("scan results notify.");

	/* Check if scan is still active */
	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_ACTIVE_SCANNING &&
	    atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_PASSIVE_SCANNING) {
		DBG_MAC("Scan already cancelled");
		mutex_unlock(&nw->state_mtx);
		kfree(w);
		return;
	}

#ifdef CONFIG_USE_SCAN_TIMEOUT
	{
		struct nrc_vif *i_vif = to_i_vif(vif);
		cancel_delayed_work_sync(&i_vif->scan_timeout);
	}
#endif

	mutex_unlock(&nw->state_mtx);

	/* CRITICAL: Keep scan_mode ACTIVE during RX queue processing.
	 * ieee80211_rx_irqsafe() is asynchronous - frames queued for later.
	 * If scan_mode changed to IDLE before frames processed, mac80211
	 * may reject PROBE_RESP as "not in scan mode" and BSS list will be empty.
	 * Delay BEFORE scan_complete() and mode change.
	 */
	DBG_MAC("Waiting 100ms for RX queue processing (scan_mode still ACTIVE)");
	msleep(100);

	scan_complete(nw->hw, false);

	/* Now it's safe to change scan mode to IDLE */
	mutex_lock(&nw->state_mtx);
	change_scan_mode(nw, NRC_SCAN_MODE_IDLE);
	mutex_unlock(&nw->state_mtx);

	if (nrc_has_associated_sta_vif(nw)) {
		if (!nw->params->disable_cqm) {
			if (!nw->params->disable_cqm_on_scan) {
				int i;

				for (i = 0; i < NR_NRC_VIF; i++) {
					struct nrc_vif *iv;

					if (!nw->vif[i] ||
					    nw->vif[i]->type !=
						    NL80211_IFTYPE_STATION)
						continue;
					iv = to_i_vif(nw->vif[i]);
					if (!iv->associated)
						continue;
					if (iv->is_bcn_timeout) {
						INFO("VIF%d: delayed beacon loss due to scan",
						     iv->index);
						queue_delayed_work(
							nw->hdev->workqueue,
							&nw->beacon_loss_work,
							msecs_to_jiffies(
								delay_ms));
						break;
					}
				}
			}
		}

		nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_BSS_CONFIG);
	}

	kfree(w);
}

bool nrc_cancel_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct sk_buff *skb;
#ifdef CONFIG_USE_SCAN_TIMEOUT
	struct nrc_vif *i_vif = to_i_vif(vif);

	cancel_delayed_work_sync(&i_vif->scan_timeout);
#endif

	DBG_MAC("%s called", __FUNCTION__);
	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_ACTIVE_SCANNING &&
	    atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_PASSIVE_SCANNING) {
		/* after disconnected by wpa_cli disconnect, received WIM_EVENT_SCAN_COMPLETED from TFW */
		DBG_STATE("Already cancelled, return");
		return false;
	}

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SCAN_STOP, 0);
	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);

	change_scan_mode(nw, NRC_SCAN_MODE_IDLE);

	return true;
}

void nrc_mac_cancel_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	int ret;
	bool do_complete = false;

	DBG_MAC("SCAN CANCEL called");

	mutex_lock(&nw->state_mtx);

	if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_IDLE)
		goto out;

	if (NRC_HIF_DRV_STATE(hdev) == NRC_DRV_STOP)
		goto skip_wake;

	if (NRC_DRV_IS_ASLEEP(hdev)) {
		DBG_STATE("Wake target for cancelling scan");
		ret = nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
				      NRC_PS_REASON_DRV_SCAN_ABORT);
		if (ret == -1) {
			ERR("Failed to wake to cancel scan");
		}
	}

skip_wake:
	do_complete = nrc_cancel_hw_scan(hw, vif);

out:
	mutex_unlock(&nw->state_mtx);

	if (do_complete)
		scan_complete(hw, false);
}

#ifdef NRC_BUILD_USE_HWSCAN
static int __nrc_mac_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			     struct cfg80211_scan_request *req,
			     struct ieee80211_scan_ies *ies)
{
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
#ifdef CONFIG_USE_SCAN_TIMEOUT
	int scan_to = 30000; /* msec */
	struct nrc_vif *i_vif = to_i_vif(vif);
#endif
	int ret;

	mutex_lock(&nw->state_mtx);
	DBG_MAC("%s: called", __FUNCTION__);

	if (NRC_HIF_DRV_STATE(nw->hdev) == NRC_DRV_REBOOT) {
		ERR(":%s Scan Cancelled, (reason:reboot)", __func__);
		ret = -EBUSY;
		goto out;
	}

	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE) {
		ERR("The scan is in progress...(%s)",
		    nrc_mac_scan_status_str(atomic_read(&nw->scan_mode)));
		ret = -EBUSY;
		goto out;
	}

	/* Wake up device if sleeping */
	if (NRC_PS_IS_ASLEEP(hdev)) {
		ret = nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
				      NRC_PS_REASON_DRV_SCAN_START);
		if (ret == -1) {
			ERR("Failed to wake to scan");
			ret = -EBUSY;
			goto out_idle;
		}
	}

	if (NRC_DRV_IS_NOT_RUNNING(hdev)) {
		DBG_MAC("%s Not running state", __func__);
		ret = -EBUSY;
		goto out;
	}

	if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_ACTIVE_SCANNING ||
	    atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_PASSIVE_SCANNING)
		nrc_mac_cancel_hw_scan(hw, vif);

	if (nrc_stats_channel_noise_reset() < 0)
		DBG_MAC("%s Channel noise reset fail", __func__);

	if (nrc_has_associated_sta_vif(nw)) {
		int i;

		if (!nw->params->disable_cqm &&
		    nw->params->disable_cqm_on_scan) {
			for (i = 0; i < NR_NRC_VIF; i++) {
				struct nrc_vif *iv;

				if (!nw->vif[i] ||
				    nw->vif[i]->type != NL80211_IFTYPE_STATION)
					continue;
				iv = to_i_vif(nw->vif[i]);
				if (iv->associated) {
					DBG_MAC("%s CQM timer off VIF%d %lu",
						__func__, iv->index,
						iv->beacon_timeout);
					try_to_del_timer_sync(
						&iv->bcn_mon_timer);
				}
			}
		}

		nrc_ps_dyn_stop(nw, NRC_PS_REASON_DRV_SCAN_START);
	}

	/*
	 * Multi-STA same-channel constraint: if another STA VIF is already
	 * associated, restrict this scan to the home channel only.  Scanning
	 * off-channel while a STA VIF is connected would disrupt ongoing
	 * traffic and is not supported by the single-radio firmware.
	 */
	if (vif->type == NL80211_IFTYPE_STATION) {
		int i;

		for (i = 0; i < NR_NRC_VIF; i++) {
			struct nrc_vif *iv;

			if (!nw->vif[i] || nw->vif[i] == vif ||
			    nw->vif[i]->type != NL80211_IFTYPE_STATION)
				continue;
			iv = to_i_vif(nw->vif[i]);
			if (iv->associated && nw->center_freq) {
				DBG_MAC("%s VIF%d restricting scan to home channel %u (multi-STA)",
					__func__, to_i_vif(vif)->index,
					nw->center_freq);
				req->n_channels = 1;
				req->channels[0] = ieee80211_get_channel(
					hw->wiphy, nw->center_freq);
				break;
			}
		}
	}

	/*
	 * AP+STA same-channel constraint: if a concurrent AP VIF is active,
	 * restrict the scan to the AP's operating channel only.  The single-
	 * radio FW cannot TX AP beacons while the STA VIF is scanning
	 * off-channel, which causes connected clients to beacon-timeout and
	 * disconnect.  Both APs in a mutual ap+sta setup share the same
	 * channel, so a single-channel directed probe is sufficient.
	 */
	if (vif->type == NL80211_IFTYPE_STATION) {
		int vi;

		rcu_read_lock();
		for (vi = 0; vi < NR_NRC_VIF; vi++) {
			struct ieee80211_vif *ap_vif = nw->vif[vi];
			struct ieee80211_chanctx_conf *ctx;

			if (!ap_vif || ap_vif == vif ||
			    ap_vif->type != NL80211_IFTYPE_AP)
				continue;
			ctx = rcu_dereference(ap_vif->bss_conf.chanctx_conf);
			if (ctx && ctx->def.chan) {
				INFO_MAC(
					"%s: VIF%d restricting scan to AP ch %d MHz (ap+sta concurrent)",
					__func__, to_i_vif(vif)->index,
					ctx->def.chan->center_freq);
				req->n_channels = 1;
				req->channels[0] = ctx->def.chan;
				break;
			}
		}
		rcu_read_unlock();
	}

	/*
	 * In concurrent AP+STA mode, mac80211 may never call
	 * assign_vif_chanctx() for the STA VIF before it starts scanning
	 * (the channel is not known until association).  If FW has no channel
	 * context for this VIF it will ASSERT in lmac_get_phy_txgain() when
	 * it tries to TX a probe request.
	 *
	 * Use fw_channel_set (not chanctx_conf) because mac80211 may assign a
	 * shared chanctx pointer without calling assign_vif_chanctx() for this
	 * VIF, leaving FW without the per-VIF channel WIM.
	 *
	 * Bootstrap using the concurrent AP VIF's channel — not req->channels[0]
	 * which may be outside the BD-covered S1G frequency range.
	 */
	if (vif->type == NL80211_IFTYPE_STATION &&
	    !to_i_vif(vif)->fw_channel_set) {
		struct cfg80211_chan_def ap_def;
		bool found = false;
		int vi;

		rcu_read_lock();
		for (vi = 0; vi < NR_NRC_VIF; vi++) {
			struct ieee80211_vif *ap_vif = nw->vif[vi];
			struct ieee80211_chanctx_conf *ctx;

			if (!ap_vif || ap_vif == vif ||
			    ap_vif->type != NL80211_IFTYPE_AP)
				continue;
			ctx = rcu_dereference(ap_vif->bss_conf.chanctx_conf);
			if (ctx && ctx->def.chan) {
				ap_def = ctx->def;
				found = true;
				break;
			}
		}
		rcu_read_unlock();

		if (found) {
			struct sk_buff *ch_skb;

			ch_skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET,
							       WIM_MAX_SIZE);
			if (ch_skb) {
				INFO_MAC(
					"%s: VIF%d no FW channel, bootstrapping from AP ch %d MHz",
					__func__, to_i_vif(vif)->index,
					ap_def.chan->center_freq);
				nrc_mac_add_tlv_channel(ch_skb, &ap_def);
				nrc_hal_ops_wim_request(ch_skb, 0, 0, false,
							NULL);
				to_i_vif(vif)->fw_channel_set = true;
			}
		}
	}

	nrc_wim_wlan_hw_scan(vif, req, ies);

	if (req->n_ssids)
		change_scan_mode(nw, NRC_SCAN_MODE_ACTIVE_SCANNING);
	else
		change_scan_mode(nw, NRC_SCAN_MODE_PASSIVE_SCANNING);

#ifdef CONFIG_USE_SCAN_TIMEOUT
	queue_delayed_work(nw->hdev->workqueue, &i_vif->scan_timeout,
			   msecs_to_jiffies(scan_to));
#endif

	DBG_MAC("%s exit", __FUNCTION__);
	mutex_unlock(&nw->state_mtx);
	return 0;

out_idle:
	nrc_ps_set_idle_mode(nw, "scan fail");
out:
	DBG_MAC("%s exit2", __FUNCTION__);
	mutex_unlock(&nw->state_mtx);
	return ret;
}

static int nrc_mac_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			   struct ieee80211_scan_request *req)
{
	return __nrc_mac_hw_scan(hw, vif, &req->req, &req->ies);
}

#endif /* NRC_BUILD_USE_HWSCAN */
#ifdef CONFIG_USE_SCAN_TIMEOUT
static void nrc_mac_scan_timeout(struct work_struct *work)
{
	struct nrc_vif *i_vif;
	struct sk_buff *skb;

	i_vif = container_of(work, struct nrc_vif, scan_timeout.work);

	skb = nrc_hal_ops_wim_alloc_skb_vif(i_vif->nw, to_ieee80211_vif(i_vif),
					    WIM_CMD_SCAN_STOP, 0);
	if (!skb)
		return;

	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);

	change_scan_mode(i_vif->nw, NRC_SCAN_MODE_IDLE);

	scan_complete(i_vif->nw->hw, true);
}
#endif

static int nrc_mac_set_bitrate_mask(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    const struct cfg80211_bitrate_mask *mask)
{
	int i, ret;
	uint8_t mcs_mask, mcs_level;
	// struct nrc *nw = hw->priv;
	struct sk_buff *skb;
	enum nl80211_band band = NL80211_BAND_2GHZ;

	DBG_MAC("%s legacy 0x%8X 0x%8X 0x%2X 0x%2X", __func__,
		mask->control[0].legacy, mask->control[1].legacy,
		mask->control[0].ht_mcs[0], mask->control[1].ht_mcs[0]);
#ifndef CONFIG_S1G_CHANNEL
	if (mask->control[band].ht_mcs[0] == 0xFF)
		band = NL80211_BAND_5GHZ;
	mcs_mask = mask->control[band].ht_mcs[0] & 0xFF;

	if (!mcs_mask) {
		mcs_level = 8;
	} else {
		for (i = 0; i < 8; i++) {
			if (mcs_mask & 0x1) {
				mcs_level = (uint8_t)i;
				DBG_MAC("%s mcs level is %d", __func__,
					mcs_level);
				break;
			}
			mcs_mask = mcs_mask >> 1;
		}
	}
#else
	band = NL80211_BAND_S1GHZ;
	mcs_level = 10;
	mcs_mask = 0xFF;
	i = 0; //for removing mask compiler warning
#endif
	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_MCS, sizeof(mcs_level),
				    &mcs_level);
	ret = nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);

	return ret;
}

#ifndef NRC_BUILD_USE_HWSCAN
static void nrc_mac_sw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    const u8 *mac_addr)
{
	struct nrc *nw = hw->priv;

	DBG_MAC("%s", __func__);

	mutex_lock(&nw->state_mtx);

	if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_IDLE) {
		DBG_MAC("two sw_scans detected!");
		goto out;
	}

	atomic_set(&nw->scan_mode, NRC_SCAN_MODE_SCANNING);
out:
	mutex_unlock(&nw->state_mtx);
}

static void nrc_mac_sw_scan_complete(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;

	DBG_MAC("%s", __func__);

	mutex_lock(&nw->state_mtx);

	if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_SCANNING) {
		DBG_MAC("Scan has not started!");
		goto out;
	}

	atomic_set(&nw->scan_mode, NRC_SCAN_MODE_IDLE);
out:
	mutex_unlock(&nw->state_mtx);
}
#endif

static void nrc_mac_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  u32 queues, bool drop)
{
	DBG_MAC("%s", __func__);
}

static bool nrc_mac_tx_frames_pending(struct ieee80211_hw *hw)
{
	return (nrc_txq_pending(hw) > 0);
}

static inline u64 nrc_mac_get_tsf_raw(void)
{
	return ktime_to_us(ktime_get_real());
}

static __le64 _nrc_mac_get_tsf(struct nrc *nw)
{
	u64 now = nrc_mac_get_tsf_raw();

	return cpu_to_le64(now + nw->tsf_offset);
}

static u64 nrc_mac_get_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;

#ifdef CONFIG_SUPPORT_IBSS
	if (vif->type == NL80211_IFTYPE_ADHOC) {
		return current_bssid_beacon_timestamp;
	} else {
		return le64_to_cpu(_nrc_mac_get_tsf(nw));
	}
#else
	return le64_to_cpu(_nrc_mac_get_tsf(nw));
#endif /* CONFIG_SUPPORT_IBSS */
}

static void nrc_mac_set_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    u64 tsf)
{
}

#ifdef CONFIG_SUPPORT_IBSS
/*
* @tx_last_beacon: Determine whether the last IBSS beacon was sent by us.
*  This is needed only for IBSS mode and the result of this function is
*  used to determine whether to reply to Probe Requests.
*  Returns non-zero if this device sent the last beacon.
*  The callback can sleep.
*/
static int nrc_tx_last_beacon(struct ieee80211_hw *hw)
{
	return 1; // To send probe respose
}
#endif

static void nrc_mac_get_et_strings(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif, u32 sset,
				   u8 *data)
{
	if (sset == ETH_SS_STATS)
		memcpy(data, *nrc_gstrings_stats, sizeof(nrc_gstrings_stats));
}

static int nrc_mac_get_et_sset_count(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif, int sset)
{
	if (sset == ETH_SS_STATS)
		return ARRAY_SIZE(nrc_gstrings_stats);
	return 0;
}

static void nrc_mac_get_et_stats(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif,
				 struct ethtool_stats *stats, u64 *data)
{
}

static int nrc_mac_set_rts_threshold(struct ieee80211_hw *hw, u32 value)
{
	struct sk_buff *skb;
	struct nrc *nw = hw->priv;
	struct ieee80211_vif *vif = nw->vif[0];

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);
	if (!skb)
		return -ENOMEM;

	DBG_MAC("RTS Threshold: %u", value);
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_RTS_THRESHOLD, sizeof(u32),
				    &value);
	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
	return 0;
}

static const char *nrc_cipher_str(u32 cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return "WEP40";
	case WLAN_CIPHER_SUITE_WEP104:
		return "WEP104";
	case WLAN_CIPHER_SUITE_TKIP:
		return "TKIP";
	case WLAN_CIPHER_SUITE_CCMP:
		return "CCMP";
	case WLAN_CIPHER_SUITE_CCMP_256:
		return "CCMP-256";
	case WLAN_CIPHER_SUITE_GCMP:
		return "GCMP";
	case WLAN_CIPHER_SUITE_GCMP_256:
		return "GCMP-256";
	case WLAN_CIPHER_SUITE_AES_CMAC:
		return "BIP-CMAC";
	case WLAN_CIPHER_SUITE_BIP_GMAC_128:
		return "BIP-GMAC-128";
	case WLAN_CIPHER_SUITE_BIP_GMAC_256:
		return "BIP-GMAC-256";
	default:
		return "unknown";
	}
}

static int nrc_mac_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			   struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			   struct ieee80211_key_conf *key)
{
	struct nrc *nw = hw->priv;
	struct nrc_vif *i_vif = to_i_vif(vif);
	int vif_id = i_vif->index;
	int ret;

	DBG_MAC("set_key VIF%d %s %s/%s idx=%d sw_enc=%d cap=0x%llx", vif_id,
		(cmd == SET_KEY) ? "SET" : "DEL",
		(key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ? "PTK" : "GTK",
		nrc_cipher_str(key->cipher), key->keyidx, nw->params->sw_enc,
		nw->hdev->cap.vif_caps[vif_id].cap_mask);

	/* if use SW SECURITY, return 1 */
	if (nw->params->sw_enc == WIM_ENCDEC_SW) {
		if ((cmd == SET_KEY) &&
		    (key->cipher == WLAN_CIPHER_SUITE_AES_CMAC ||
		     key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_128 ||
		     key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_256))
			i_vif->cipher_pairwise =
				key->cipher; //for PMF deauth for keep alive on AP

		DBG_MAC("set_key VIF%d SW-only mode, skip HW key install",
			vif_id);
		return 1;
	}

	/* if not use HW SECURITY of VIF , return 1 */
	if (!(nw->hdev->cap.vif_caps[vif_id].cap_mask & WIM_SYSTEM_CAP_HWSEC)) {
		ERR("set_key VIF%d HWSEC not set in cap=0x%llx — skip", vif_id,
		    nw->hdev->cap.vif_caps[vif_id].cap_mask);
		return 1;
	}
	//nrc_wim_install_key need to wait to receive fw result
	//rcu_read_lock();

	/*
	 * Stop dynamic PS and wake the device for the duration of key
	 * installation.  The WIM exchange is synchronous; nrc_ps_dyn_start()
	 * is called at return_with_rcu_unlock to resume the idle timer once
	 * the operation (success or failure) is complete.
	 */
	nrc_ps_dyn_stop(nw, NRC_PS_REASON_DRV_BSS_CONFIG);

	mutex_lock(&nw->state_mtx);

	if (sta == NULL && vif->type == NL80211_IFTYPE_STATION &&
	    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE)) {
		sta = ieee80211_find_sta(vif, vif->bss_conf.bssid);
	}

	if (cmd == DISABLE_KEY) {
		if ((key->flags & IEEE80211_KEY_FLAG_PAIRWISE) && sta == NULL) {
			DBG_MAC("set_key VIF%d DEL PTK but sta=NULL — skip",
				vif_id);
			ret = 1;
			goto return_with_rcu_unlock;
		}

		if (key->cipher == WLAN_CIPHER_SUITE_AES_CMAC ||
		    key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_128 ||
		    key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_256) {
			DBG_MAC("set_key VIF%d DEL BIP cipher=%s — SW only, skip HW",
				vif_id, nrc_cipher_str(key->cipher));
			ret = 1;
			goto return_with_rcu_unlock;
		}

		/* If HYBRID SECURITY of VIF,
			PTK : HW Security remove key, GTK: SW Security (return 1) */
		if (nw->hdev->cap.vif_caps[vif_id].cap_mask &
		    WIM_SYSTEM_CAP_HYBRIDSEC) {
			if (!(key->flags & IEEE80211_KEY_FLAG_PAIRWISE)) {
				DBG_MAC("[DEL] VIF(%d) KEY_FLAG(%u) GTK for HYBRID SECURITY. return 1",
					vif_id, key->flags);
				ret = 1;
				goto return_with_rcu_unlock;
			}
		}
	}

	/* Record key information to per-STA driver data structure for RX */
	/* TODO: DISABLE_KEY -> later */
	if (cmd == SET_KEY) {
		/* HW does NOT Support BIP until now => need to SW-based Crypto => return 1 for this */
		if (key->cipher == WLAN_CIPHER_SUITE_AES_CMAC ||
		    key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_128 ||
		    key->cipher == WLAN_CIPHER_SUITE_BIP_GMAC_256) {
			i_vif->cipher_pairwise =
				key->cipher; //for PMF deauth for keep alive on AP
			key->flags |= IEEE80211_KEY_FLAG_SW_MGMT_TX;
			DBG_MAC("set_key VIF%d SET BIP cipher=%s — SW mgmt TX only",
				vif_id, nrc_cipher_str(key->cipher));
			ret = 1;
			goto return_with_rcu_unlock;
		}

		/* If HYBRID SECURITY of VIF,
			PTK : HW Security(install key), GTK: SW Security (return 1) */
		if (nw->hdev->cap.vif_caps[vif_id].cap_mask &
		    WIM_SYSTEM_CAP_HYBRIDSEC) {
			if (!(key->flags & IEEE80211_KEY_FLAG_PAIRWISE)) {
				DBG_MAC("[ADD] VIF(%d) KEY_FLAG(%u) GTK for HYBRID SECURITY. return 1",
					vif_id, key->flags);
				ret = 1;
				goto return_with_rcu_unlock;
			}
		}

		if (sta) {
			struct nrc_sta *i_sta = to_i_sta(sta);

			if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE)
				i_sta->ptk = key;
			else {
				i_sta->gtk = key;
				if (key->cipher == WLAN_CIPHER_SUITE_WEP40 ||
				    key->cipher == WLAN_CIPHER_SUITE_WEP104)
					i_sta->ptk = key;
			}
		} else
			WARN_ON(!(vif->type == NL80211_IFTYPE_AP ||
#if defined(CONFIG_SUPPORT_IBSS)
				  vif->type == NL80211_IFTYPE_ADHOC ||
#endif
				  vif->type == NL80211_IFTYPE_P2P_GO ||
				  vif->type == NL80211_IFTYPE_MESH_POINT));
	}

	ret = nrc_wim_wlan_install_key(cmd, vif, sta, key);
	if (ret < 0) {
		ERR("set_key VIF%d %s %s/%s idx=%d — install failed ret=%d",
		    vif_id, (cmd == SET_KEY) ? "SET" : "DEL",
		    (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ? "PTK" : "GTK",
		    nrc_cipher_str(key->cipher), key->keyidx, ret);
		ret = -EINVAL;
		goto return_with_rcu_unlock;
	}

	if (0xDEAD == ret) {
		ERR("set_key VIF%d %s PTK/%s idx=%d — EAPOL M4 TX failed",
		    vif_id, (cmd == SET_KEY) ? "SET" : "DEL",
		    nrc_cipher_str(key->cipher), key->keyidx);
		ieee80211_hw_set(
			hw, SW_CRYPTO_CONTROL); /* Disable fallback to SW */
		ret = -EINVAL;
		goto return_with_rcu_unlock;
	}

	key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV; /* IV by the stack */
	/* Check whether MMIC will be generated by HW */
	if (nw->hdev->cap.cap_mask & WIM_SYSTEM_CAP_HWSEC_OFFL)
		key->flags |= IEEE80211_KEY_FLAG_RESERVE_TAILROOM;

return_with_rcu_unlock:
	/* Key install complete (or aborted): resume normal PS idle timer */
	nrc_ps_dyn_start(nw, 0, NRC_PS_REASON_DRV_BSS_CONFIG);
	mutex_unlock(&nw->state_mtx);
	//rcu_read_unlock();
	return ret;
}

static void nrc_mac_set_default_unicast_key(struct ieee80211_hw *hw,
					    struct ieee80211_vif *vif,
					    int keyidx)
{
}

static void nrc_mac_channel_policy(void *data, u8 *mac,
				   struct ieee80211_vif *vif)
{
	struct sk_buff *skb;
	struct nrc_vif *i_vif;
	struct nrc *nw;
	struct cfg80211_chan_def *chan_to_follow =
		(struct cfg80211_chan_def *)data;
	struct wireless_dev *wdev;
	struct cfg80211_chan_def *chandef;

	if (!vif)
		return;

	i_vif = to_i_vif(vif);
	nw = i_vif->nw;

	wdev = ieee80211_vif_to_wdev(vif);

	if (!wdev)
		return;

	if (!chan_to_follow || !chan_to_follow->chan)
		return;
	chandef = wdev_chandef(wdev, vif->bss_conf.link_id);
	if (chandef && chandef->chan &&
	    chandef->chan->center_freq == chan_to_follow->chan->center_freq)
		return;

	if (!(vif->type == NL80211_IFTYPE_STATION ||
	      vif->type == NL80211_IFTYPE_MESH_POINT))
		return;
	if (vif->cfg.assoc)
		return;

	skb = nrc_hal_ops_wim_alloc_skb_vif(vif, WIM_CMD_SET, WIM_MAX_SIZE);

	if (!skb)
		return;

	nrc_mac_add_tlv_channel(skb, chan_to_follow);
	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
	to_i_vif(vif)->fw_channel_set = true;
}

static int nrc_mac_roc(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       struct ieee80211_channel *chan, int duration,
		       enum ieee80211_roc_type type)
{
	struct nrc *nw = hw->priv;
	struct sk_buff *skb;
	struct cfg80211_chan_def chandef = {0};
	struct cfg80211_chan_def *cdef;

	DBG_MAC("%s, ch=%d, dur=%d", __func__, chan->center_freq, duration);

	skb = nrc_hal_ops_wim_alloc_skb_vif(
		vif, WIM_CMD_SET, tlv_len(sizeof(u16)) + tlv_len(ETH_ALEN));

	if (!skb)
		return -EINVAL;

	if (!hw->conf.chandef.chan) {
		chandef.chan = chan;
#ifndef CONFIG_S1G_CHANNEL
		chandef.width = NL80211_CHAN_WIDTH_20;
#else
		chandef.width = NL80211_CHAN_WIDTH_1;
#endif
		cdef = &chandef;
	} else
		cdef = &hw->conf.chandef;

	nrc_mac_add_tlv_channel(skb, cdef);
	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);

	ieee80211_ready_on_channel(hw);

	nw->band = chan->band;
	nw->center_freq = chan->center_freq;

	ieee80211_queue_delayed_work(hw, &nw->roc_finish,
				     msecs_to_jiffies(duration));

	ieee80211_iterate_interfaces(nw->hw, IEEE80211_IFACE_ITER_ACTIVE,
				     nrc_mac_channel_policy, cdef);

	return 0;
}

static int nrc_mac_cancel_roc(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;

	DBG_MAC("%s", __func__);
	cancel_delayed_work_sync(&nw->roc_finish);

	return 0;
}

static void nrc_mac_channel_switch_beacon(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif,
					  struct cfg80211_chan_def *chandef)
{
	struct sk_buff *b;

	b = ieee80211_beacon_get_template(hw, vif, NULL, vif->bss_conf.link_id);

	print_hex_dump(KERN_DEBUG, "new vendor elem: ", DUMP_PREFIX_NONE, 16, 1,
		       b->data, b->len, false);

	nrc_vendor_update_beacon(hw, vif);
	DBG_STATE("[nrc_mac_channel_switch_beacon] Update Beacon for CSA");
}

static int nrc_pre_channel_switch(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct ieee80211_channel_switch *ch_switch)
{
	DBG_STATE("[%s, %d] Channel switch start", __func__, __LINE__);
	return 0;
}

static int nrc_post_channel_switch(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_bss_conf *bss_conf)
{
	DBG_STATE("[%s, %d] Channel switch complete", __func__, __LINE__);
	return 0;
}

static void nrc_channel_switch(struct ieee80211_hw *hw,
			       struct ieee80211_vif *vif,
			       struct ieee80211_channel_switch *ch_switch)
{
	DBG_STATE("[%s, %d] waiting for count less than 1 ... (CH to %d)",
		  __func__, __LINE__, ch_switch->chandef.chan->center_freq);
	// ieee80211_chswitch_done(vif, true);
}

#ifdef CONFIG_PM
static struct wiphy_wowlan_support nrc_wowlan_support = {
	.flags = WIPHY_WOWLAN_ANY,
	/*
	 * We only supports two patterns.
	 */
	.n_patterns = 2,
	.pattern_max_len = WOWLAN_PATTER_SIZE,
	.pattern_min_len = 16,
	.max_pkt_offset = 16,
};

static void nrc_mac_set_wakeup(struct ieee80211_hw *hw, bool enabled)
{
	struct nrc *nw = hw->priv;
	nw->hdev->wowlan_enabled = enabled;
	DBG_PS("[%s, L%d] wowlan enabled:%d", __func__, __LINE__,
	       nw->hdev->wowlan_enabled);
	return;
}

static int nrc_mac_resume(struct ieee80211_hw *hw)
{
	struct nrc *nw = hw->priv;

	DBG_STATE("[%s, L%d]", __func__, __LINE__);

	/* Restore country code / board data after idle-mode wakeup */
	nrc_restore_reg_domain(nw);

	DBG_STATE("[%s, L%d] Resume complete", __func__, __LINE__);

	return 0;
}

static int nrc_mac_suspend(struct ieee80211_hw *hw,
			   struct cfg80211_wowlan *wowlan)
{
	struct nrc *nw = hw->priv;
	int ret = 0;

	DBG_STATE("[%s,L%d] any:%d patterns(%p) n_patterns(%d)\n", __func__,
		  __LINE__, wowlan->any, wowlan->patterns, wowlan->n_patterns);

	mutex_lock(&nw->state_mtx);

	if (!NRC_PS_IS_ASLEEP(nw->hdev)) {
		DBG_STATE("No sleep state");
		ret = -EAGAIN;
		goto out;
	}

out:
	mutex_unlock(&nw->state_mtx);
	DBG_STATE("[%s, L%d] Suspend complete (ret:%d)\n", __func__, __LINE__,
		  ret);
	return ret;
}
#endif

static u32 nrc_get_expected_throughput(struct ieee80211_hw *hw,
				       struct ieee80211_sta *sta)
{
	uint32_t tput = 0;
	struct sk_buff *skb_resp;
	// struct nrc *nw = hw->priv;

	if (!sta)
		return 0;

	if (!nrc_hal_ops_wim_request(NULL, WIM_CMD_GET_TX_STATS,
				     (WIM_RESP_TIMEOUT * 30), false,
				     &skb_resp)) {
		struct wim *wim = (struct wim *)skb_resp->data;
		struct wim_tlv *tlv = (struct wim_tlv *)(wim + 1);
		struct nrc_tx_stats *tx_stats = (struct nrc_tx_stats *)tlv->v;
		struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
		INFO("mcs:%d bw:%d gi:%d", tx_stats->mcs, tx_stats->bw,
		     tx_stats->gi);
		if (tx_stats->bw < 3 && tx_stats->mcs < 11) {
			nrc_stats_update_tx_stats(tx_stats);
		}
		/* Track WIM response SKB free with parsed cmd/event */
		NRC_SKB_TRACK_WIM_FREE(hdev, skb_resp, wim->cmd, wim->event,
				       true, false);
	}

	tput = nrc_stats_metric(sta->addr);
	/* prevent overflow in mac80211 */
	if (tput < 200)
		tput = 200;

	return tput;
}

#define MPDU_LEN_THRESHOLD 511 /* See lmac_11ah.h  */
static int nrc_set_frag_threshold(struct ieee80211_hw *hw, u32 value)
{
	struct nrc *nw = hw->priv;

	nw->frag_threshold = value;
	DBG_MAC("Fragmentation Threshold: %d", nw->frag_threshold);

	if (nw->frag_threshold >= MPDU_LEN_THRESHOLD) {
		ERR("Frag threshold value must be smaller than %d",
		    MPDU_LEN_THRESHOLD);
		return -EINVAL;
	}

	spin_lock_bh(&nw->txq_lock);
	rcu_read_lock();

	if (nw->frag_threshold == -1) {
		DBG_MAC("Fragmentation Disabled.");
	} else {
		DBG_MAC("Fragmentation Enabled.");
		nrc_cleanup_ba_session_all(nw);
	}
	rcu_read_unlock();
	spin_unlock_bh(&nw->txq_lock);

	return 0;
}

static int nrc_mac_sched_scan_start(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct cfg80211_sched_scan_request *req,
				    struct ieee80211_scan_ies *ies)
{
	struct nrc *nw = hw->priv;
	struct cfg80211_ssid *ssid;
	struct cfg80211_match_set *match;
	struct cfg80211_sched_scan_plan *sp;
	int i;
	int ret = 0;

	mutex_lock(&nw->state_mtx);

	DBG_STATE("Sched scan start");

	DBG_MAC("delay: %u", cpu_to_le32(req->delay));
	DBG_MAC("min_rssi_thod: %d", cpu_to_le32(req->min_rssi_thold));
	DBG_MAC("flags: %u", cpu_to_le32(req->flags));
	DBG_MAC("rel rssi: %u:%d", req->relative_rssi_set,
		req->relative_rssi_set);

	for (i = 0; i < req->n_ssids; i++) {
		ssid = &req->ssids[i];
		DBG_MAC("active ssid: %s, len: %d", ssid->ssid,
			cpu_to_le32(ssid->ssid_len));
	}

	for (i = 0; i < req->n_match_sets; i++) {
		match = &req->match_sets[i];
		DBG_MAC("match ssid: %s, len: %d, rssi: %d", match->ssid.ssid,
			cpu_to_le32(match->ssid.ssid_len),
			cpu_to_le32(match->rssi_thold));
	}

	for (i = 0; i < req->n_channels; i++) {
		struct ieee80211_channel *chan;
		chan = req->channels[i];
		DBG_MAC("freq: %d, hw_value: %d", chan->center_freq,
			chan->hw_value);
	}

	for (i = 0; i < req->n_scan_plans; i++) {
		sp = &req->scan_plans[i];
		DBG_MAC("interval: %d, iterations: %d", sp->interval,
			sp->iterations);
	}

	if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_ACTIVE_SCANNING ||
	    atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_PASSIVE_SCANNING) {
		nrc_cancel_hw_scan(hw, vif);
	}

	if (!nrc_idle_mode_get_state(nw)) {
		ERR("Not idle state");
		ret = -EBUSY;
		goto out;
	}

	if (req->n_match_sets <= 0) {
		ERR("invalid number of matchsets specified: %d",
		    req->n_match_sets);
		ret = -EINVAL;
		goto out;
	}

	if (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_IDLE) {
		ERR("The scan is in progress...(%s)",
		    nrc_mac_scan_status_str(atomic_read(&nw->scan_mode)));
		ret = -EBUSY;
		goto out;
	}

	if (NRC_DRV_IS_ASLEEP(nw->hdev)) {
		//  already idle after scan complete, transitioning to deep sleep, but the drv_state has not yet changed to NRC_DRV_PS by spi_suspend.
		DBG_STATE("Wake target for sched scan");
		ret = nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
				      NRC_PS_REASON_DRV_SCAN_START);
		if (ret == -1) {
			ERR("Failed to wake to sched scan");
			ret = -EBUSY;
			goto out_idle;
		}
	}

	if (NRC_DRV_IS_NOT_RUNNING(nw->hdev)) {
		ERR("Not running state");
		ret = -EBUSY;
		goto out;
	}

	ret = nrc_wim_wlan_sched_scan_start(vif, req, ies);
	if (ret != 0) {
		ERR("nrc_wim_sched_scan_start failed");
		ret = -EBUSY;
		goto out_idle;
	}

	change_scan_mode(nw, NRC_SCAN_MODE_SCHED_SCANNING);

out_idle:
	nrc_ps_set_idle_mode(nw, "sched scan start");

out:
	mutex_unlock(&nw->state_mtx);
	return ret;
}

static int nrc_mac_sched_scan_stop(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;
	int ret = 0;

	mutex_lock(&nw->state_mtx);

	DBG_STATE("Sched scan stop");

	ret = nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
			      NRC_PS_REASON_DRV_SCAN_ABORT);
	if (ret == -1) {
		ERR("Failed to wake to stop sched scan");
		ret = -EBUSY;
		goto out;
	}

	ret = nrc_wim_wlan_sched_scan_stop(vif);
	if (ret != 0) {
		ERR("nrc_wim_sched_scan_stop failed");
		ret = -EBUSY;
		goto out;
	}

	nrc_mac_sched_scan_completed(hw, vif);

	nrc_ps_set_idle_mode_delay(nw, "sched scan stop", 1000);
out:
	mutex_unlock(&nw->state_mtx);
	return ret;
}

void nrc_mac_sched_scan_completed(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;

	DBG_MAC("Sched scan completed");

	// target itself disabled
	//nrc_mac_sched_scan_stop(nw->hw, vif);
	//nrc_wim_sched_scan_stop(nw, vif);

	change_scan_mode(nw, NRC_SCAN_MODE_IDLE);
}

void nrc_mac_sched_scan_results(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	struct nrc *nw = hw->priv;

	DBG_MAC("Sched scan results notify.");

	ieee80211_sched_scan_results(nw->hw);

	nrc_ps_set_idle_mode_delay(nw, "sched scan results", 2000);
}

void nrc_mac_sched_scan_results_work_handler(struct work_struct *work)
{
	struct wim_event_work *w =
		container_of(work, struct wim_event_work, work);

	nrc_mac_sched_scan_results(w->nw->hw, w->vif);

	kfree(w);
}

const char *nrc_mac_get_scan_status_str(struct nrc *nw)
{
	return nrc_mac_scan_status_str(atomic_read(&nw->scan_mode));
}

#define HAS_CHANCTX_EMULATORS 0

static const struct ieee80211_ops nrc_mac80211_ops = {
	.tx = nrc_mac_tx,
	.start = nrc_mac_start,
	.stop = nrc_mac_stop,
#ifdef CONFIG_PM
	.suspend = nrc_mac_suspend,
	.resume = nrc_mac_resume,
	.set_wakeup = nrc_mac_set_wakeup,
#endif
	.add_interface = nrc_mac_add_interface,
	.change_interface = nrc_mac_change_interface,
	.remove_interface = nrc_mac_remove_interface,
	.config = nrc_mac_config,
	.configure_filter = nrc_mac_configure_filter,
	.bss_info_changed = nrc_mac_bss_info_changed,
	.start_ap = nrc_mac_start_ap,
	.stop_ap = nrc_mac_stop_ap,
	.wake_tx_queue = nrc_wake_tx_queue,
#ifdef NRC_BUILD_USE_HWSCAN
	.hw_scan = nrc_mac_hw_scan,
	.cancel_hw_scan = nrc_mac_cancel_hw_scan,
#endif
	.set_key = nrc_mac_set_key,
	.set_default_unicast_key = nrc_mac_set_default_unicast_key,
	.sta_state = nrc_mac_sta_state,
	.sta_pre_rcu_remove = nrc_mac_sta_pre_rcu_remove,
	.sta_notify = nrc_mac_sta_notify,
	.set_tim = nrc_mac_set_tim,
	.set_rts_threshold = nrc_mac_set_rts_threshold,
	.set_frag_threshold = nrc_set_frag_threshold,
	.conf_tx = nrc_mac_conf_tx,
	.get_survey = nrc_mac_get_survey,
	.ampdu_action = nrc_mac_ampdu_action,
	.set_bitrate_mask = nrc_mac_set_bitrate_mask,
#ifndef NRC_BUILD_USE_HWSCAN
	.sw_scan_start = nrc_mac_sw_scan,
	.sw_scan_complete = nrc_mac_sw_scan_complete,
#endif
	.flush = nrc_mac_flush,
	.tx_frames_pending = nrc_mac_tx_frames_pending,
	.get_tsf = nrc_mac_get_tsf,
	.set_tsf = nrc_mac_set_tsf,
	.remain_on_channel = nrc_mac_roc,
	.cancel_remain_on_channel = nrc_mac_cancel_roc,
#ifdef CONFIG_SUPPORT_IBSS
	.tx_last_beacon = nrc_tx_last_beacon,
#endif
	.get_et_sset_count = nrc_mac_get_et_sset_count,
	.get_et_stats = nrc_mac_get_et_stats,
	.get_et_strings = nrc_mac_get_et_strings,
#if HAS_CHANCTX_EMULATORS
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx = ieee80211_emulate_switch_vif_chanctx,
#endif
	.channel_switch_beacon = nrc_mac_channel_switch_beacon,
	.pre_channel_switch = nrc_pre_channel_switch,
	.post_channel_switch = nrc_post_channel_switch,
	.channel_switch = nrc_channel_switch,
	.get_expected_throughput = nrc_get_expected_throughput,
	.sched_scan_start = nrc_mac_sched_scan_start,
	.sched_scan_stop = nrc_mac_sched_scan_stop,
};

/*
 * HaLow proxy channel flag suppression table.
 *
 * NRC7394 reuses 5 GHz channel numbers as proxy identifiers for mac80211;
 * actual RF operates in the 900 MHz S1G band.  After a country regulatory
 * update (CRDA/kernel), certain 5 GHz proxy ranges receive flags that
 * block AP operation.  Each entry defines a frequency range (inclusive,
 * MHz) and the set of IEEE80211_CHAN_* flags to unconditionally clear.
 *
 * To add a new proxy range, append a row here — no other code changes
 * are needed.
 */
struct nrc_proxy_rule {
	u32 freq_lo;
	u32 freq_hi;
	u32 clr_flags;
};

static const struct nrc_proxy_rule nrc_halow_proxy_rules[] = {
	/*
	 * Op35 proxy block (5250-5360 MHz, S1G ch128-172, 2 MHz BW).
	 * Falls in UNII-2 / UNII-2e; US/EU regulatory domains require DFS
	 * (IEEE80211_CHAN_RADAR) on this range.  The 5350-5360 MHz tail is
	 * outside any US regdb rule (5250-5350 / 5470-5730), so cfg80211
	 * also marks it IEEE80211_CHAN_DISABLED.
	 */
	{5250, 5360,
	 IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IR},

	/*
	 * Op36 proxy block (5380-5480 MHz, S1G ch130-170, 4 MHz BW).
	 * Outside the standard 802.11a channel plan; CRDA marks them
	 * IEEE80211_CHAN_DISABLED and IEEE80211_CHAN_NO_IR.  The 5480 MHz
	 * edge falls inside the US 5470-5730 DFS rule, which adds
	 * IEEE80211_CHAN_RADAR (NRC7394 cannot run radar CAC on proxy
	 * channels).
	 */
	{5380, 5480,
	 IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IR},

	/*
	 * S1G ch40-48 proxy block (5500-5580 MHz).
	 * UNII-2e; US/EU regulatory domains require DFS (IEEE80211_CHAN_RADAR),
	 * causing mac80211 to enter a 60-second CAC before AP operation.
	 */
	{5500, 5580, IEEE80211_CHAN_RADAR | IEEE80211_CHAN_NO_IR},
};

/**
 * nrc_halow_suppress_proxy_chan_flags() - Clear restrictive regulatory flags
 *                                         on HaLow 5 GHz proxy channels.
 * @wiphy: target wiphy
 *
 * Walks the 5 GHz band and clears any flags listed in
 * @nrc_halow_proxy_rules for the corresponding frequency ranges.
 *
 * Must be called from nrc_reg_notifier() after each country regulatory
 * update, because CRDA re-applies the country domain and restores the
 * original (restrictive) flags every time.
 *
 * NOTE: wiphy_apply_custom_regulatory() cannot be used here;
 * nrc_reg_notifier() is called while cfg80211 holds rtnl_mutex
 * (via wiphy_update_regulatory), so calling it would deadlock.
 * Direct flag manipulation under the existing lock context is safe.
 */
static void nrc_halow_suppress_proxy_chan_flags(struct wiphy *wiphy)
{
	struct ieee80211_supported_band *band = wiphy->bands[NL80211_BAND_5GHZ];
	int i, r;

	if (!band)
		return;

	for (i = 0; i < band->n_channels; i++) {
		struct ieee80211_channel *chan = &band->channels[i];

		for (r = 0; r < ARRAY_SIZE(nrc_halow_proxy_rules); r++) {
			const struct nrc_proxy_rule *rule =
				&nrc_halow_proxy_rules[r];

			if (chan->center_freq >= rule->freq_lo &&
			    chan->center_freq <= rule->freq_hi)
				chan->flags &= ~rule->clr_flags;
		}
	}
}

static void nrc_reg_notifier(struct wiphy *wiphy,
			     struct regulatory_request *request)
{
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct nrc *nw = hw->priv;
	struct nrc_hif_device *hdev = nw->hdev;
	struct sk_buff *skb;
#if defined(CONFIG_SUPPORT_BD)
	struct wim_bd_param *bd_param = NULL;
#endif /* CONFIG_SUPPORT_BD */
#ifdef CONFIG_S1G_CHANNEL
	const struct s1g_channel_table *cc_table;
#endif /* CONFIG_S1G_CHANNEL */

	INFO_MAC("reg_notifier: CC=%c%c initiator=%d", request->alpha2[0],
		 request->alpha2[1], request->initiator);
	nrc_cc[0] = request->alpha2[0];
	nrc_cc[1] = request->alpha2[1];
	nrc_cc[2] = '\0';
	if ((request->alpha2[0] == '0' && request->alpha2[1] == '0') ||
	    (request->alpha2[0] == '9' && request->alpha2[1] == '9')) {
		INFO_MAC(
			"reg_notifier: CC=%c%c skipped (no valid country set yet - boot auto-load?)",
			request->alpha2[0], request->alpha2[1]);
		return;
	}

	if (NRC_DRV_IS_ASLEEP(hdev)) {
		/* HSPI is not ready */
		return;
	}

#if defined(CONFIG_SUPPORT_BD)
	/**
	 * Read board data and save buffer.
	 * nrc_cc can be changed to EU, K1 or K2 in nrc_bd_get_tx_pwr().
	 */
	bd_param = nrc_hal_ops_bd_get_tx_pwr(nrc_cc);
	if (bd_param) {
		int num_entries = (bd_param->length - 4) / 12;
		DBG_FW("type %04X length %04X checksum %04X target_ver %04X",
		       bd_param->type, bd_param->length, bd_param->checksum,
		       bd_param->hw_version);
		/* Print first entry as sample and total count */
		if (num_entries > 0) {
			DBG_FW("Fw %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d (Total %d entries)",
			       (bd_param->value[0]), (bd_param->value[1]),
			       (bd_param->value[2]), (bd_param->value[3]),
			       (bd_param->value[4]), (bd_param->value[5]),
			       (bd_param->value[6]), (bd_param->value[7]),
			       (bd_param->value[8]), (bd_param->value[9]),
			       (bd_param->value[10]), (bd_param->value[11]),
			       num_entries);
		}
	} else {
		/* Default policy is that if board data is invalid, block loading of FW */
		DBG_MAC("BD file is invalid! Stop loading FW");
		g_bd_valid = false;
		return;
	}
#endif /* defined(CONFIG_SUPPORT_BD) */

	nw->alpha2[0] = request->alpha2[0];
	nw->alpha2[1] = request->alpha2[1];

	INFO_MAC("reg_notifier: CC=%c%c applied to FW (initiator=%d)",
		 nw->alpha2[0], nw->alpha2[1], request->initiator);

	/* Always update internal S1G proxy map and supported channel list. */
	nrc_set_s1g_country(nrc_cc);

	skb = nrc_hal_ops_wim_alloc_skb(WIM_CMD_SET, WIM_MAX_SIZE);
#ifndef CONFIG_S1G_CHANNEL
	nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_COUNTRY_CODE, sizeof(u16),
				    nrc_cc);
#endif

#if defined(CONFIG_SUPPORT_BD)
	if (bd_param) {
		nrc_hal_ops_wim_skb_add_tlv(skb, WIM_TLV_BD, sizeof(*bd_param),
					    bd_param);
	} else {
		ERR("fail to load board data on target");
	}
#endif /* defined(CONFIG_SUPPORT_BD) */

	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);

#if defined(CONFIG_SUPPORT_BD)
	/* BD is now in FW — unlock WLAN operations */
	g_bd_valid = true;

	/* Free bd_param after skb transmission to ensure safe memory access */
	if (bd_param) {
		kfree(bd_param);
	}
#endif /* defined(CONFIG_SUPPORT_BD) */

#ifdef CONFIG_S1G_CHANNEL
	DBG_MAC("%s: Country: %s", __func__,
		(char *)nrc_get_current_s1g_country());
	cc_table = (const struct s1g_channel_table *)
		nrc_get_current_s1g_cc_table();
	skb = nrc_hal_ops_wim_alloc_skb(WIM_CMD_SET, WIM_MAX_SIZE);
	nrc_hal_ops_wim_skb_add_tlv(
		skb, WIM_TLV_CH_TABLE,
		sizeof(struct s1g_channel_table) *
			nrc_get_num_channels_by_current_country(),
		(struct s1g_channel_table *)nrc_get_current_s1g_cc_table());
	nrc_hal_ops_wim_request(skb, 0, 0, false, NULL);
#endif /* CONFIG_S1G_CHANNEL */

	nrc_halow_suppress_proxy_chan_flags(wiphy);
}

/**
 * nrc_mac_bd_invalidate - Mark BD as not loaded in FW.
 *
 * Called before any FW restart (WDT, module reload) to reset the BD gate.
 * All WLAN operations (start, add_interface, start_ap) are blocked until
 * nrc_restore_reg_domain() successfully re-sends the BD to FW.
 */
void nrc_mac_bd_invalidate(void)
{
#if defined(CONFIG_SUPPORT_BD)
	g_bd_valid = false;
	INFO_MAC("BD invalidated — WLAN ops blocked until BD re-sent to FW");
#endif
}

/**
 * nrc_restore_reg_domain - Re-send country code (and board data) to FW.
 *
 * After a cold FW reboot (restart_wlan, WDT recovery, idle-mode wakeup) the
 * freshly loaded firmware has no country code or channel table.  This helper
 * replicates the regulatory notification that cfg80211 sends automatically at
 * ieee80211_register_hw() time but does NOT repeat on subsequent restarts.
 *
 * Call this whenever the FW has been restarted and needs its regulatory state
 * re-initialized before any channel or VIF configuration WIM commands arrive.
 */
void nrc_restore_reg_domain(struct nrc *nw)
{
	struct regulatory_request request;

	if (!nw || !nw->hw)
		return;

	request.alpha2[0] = nw->alpha2[0];
	request.alpha2[1] = nw->alpha2[1];
	request.initiator = NL80211_REGDOM_SET_BY_DRIVER;
	nrc_reg_notifier(nw->hw->wiphy, &request);
}

static u8 *nrc_vendor_remove(struct nrc *nw, u8 subcmd)
{
	u8 *pos = NULL;
	struct sk_buff **vendor_skb_ptr = NULL;

	if (((subcmd >= NRC_SUBCMD_ANNOUNCE1) &&
	     (subcmd <= NRC_SUBCMD_BCAST_FOTA_4)) ||
	    (subcmd == NRC_SUBCMD_UTC)) {
		vendor_skb_ptr = &nw->vendor_skb_beacon;
	} else if ((subcmd >= NRC_SUBCMD_ANNOUNCE6) &&
		   (subcmd <= NRC_SUBCMD_ANNOUNCE10)) {
		vendor_skb_ptr = &nw->vendor_skb_probe_req;
	} else if ((subcmd >= NRC_SUBCMD_ANNOUNCE11) &&
		   (subcmd <= NRC_SUBCMD_ANNOUNCE15)) {
		vendor_skb_ptr = &nw->vendor_skb_probe_rsp;
	} else if ((subcmd >= NRC_SUBCMD_ANNOUNCE16) &&
		   (subcmd <= NRC_SUBCMD_ANNOUNCE20)) {
		vendor_skb_ptr = &nw->vendor_skb_assoc_req;
	}

	/*
	 * The sub-command reaches this function straight from user data, so a
	 * value outside the four ranges above leaves vendor_skb_ptr unset.
	 * Reject it instead of dereferencing a null pointer.
	 */
	if (!vendor_skb_ptr) {
		ERR("%s: unknown vendor sub-command %u", __func__, subcmd);
		return NULL;
	}

	if (!(*vendor_skb_ptr))
		return NULL;

	pos = (u8 *)cfg80211_find_vendor_ie(VENDOR_OUI, subcmd,
					    (*vendor_skb_ptr)->data,
					    (*vendor_skb_ptr)->len);

	if (pos) {
		u8 len = *(pos + 1);
		memmove(pos, pos + len + 2,
			(*vendor_skb_ptr)->len -
				(pos - (*vendor_skb_ptr)->data) - (len + 2));
		skb_trim(*vendor_skb_ptr, (*vendor_skb_ptr)->len - (len + 2));
	}

	DBG_MAC("%s: removed vendor IE", __func__);
	return pos;
}

static int nrc_vendor_update(struct nrc *nw, u8 subcmd, const u8 *data,
			     int data_len)
{
	const int OUI_LEN = 3;
	const int MAX_DATALEN = 255;
	int new_elem_len = data_len + 2 /* EID + LEN */ + OUI_LEN + 1;
	u8 *pos;
	struct sk_buff **vendor_skb_ptr = NULL;

	if (!data || data_len < 1 || (data_len + OUI_LEN + 1) > MAX_DATALEN)
		return -EINVAL;

	if ((subcmd >= NRC_SUBCMD_ANNOUNCE1 &&
	     subcmd <= NRC_SUBCMD_BCAST_FOTA_4) ||
	    (subcmd == NRC_SUBCMD_UTC))
		vendor_skb_ptr = &nw->vendor_skb_beacon;
	else if (subcmd >= NRC_SUBCMD_ANNOUNCE6 &&
		 subcmd <= NRC_SUBCMD_ANNOUNCE10)
		vendor_skb_ptr = &nw->vendor_skb_probe_req;
	else if (subcmd >= NRC_SUBCMD_ANNOUNCE11 &&
		 subcmd <= NRC_SUBCMD_ANNOUNCE15)
		vendor_skb_ptr = &nw->vendor_skb_probe_rsp;
	else if (subcmd >= NRC_SUBCMD_ANNOUNCE16 &&
		 subcmd <= NRC_SUBCMD_ANNOUNCE20)
		vendor_skb_ptr = &nw->vendor_skb_assoc_req;
	else
		WARN_ON(true);

	if (!vendor_skb_ptr)
		return -EINVAL;

	if (!(*vendor_skb_ptr)) {
		*vendor_skb_ptr = dev_alloc_skb(IEEE80211_MAX_FRAME_LEN);
		if (!(*vendor_skb_ptr))
			return -ENOMEM;
		/* Track FRAME SKB allocation (TX path - vendor IE) */
		NRC_SKB_TRACK_ALLOC(nw->hdev, *vendor_skb_ptr, HIF_TYPE_FRAME,
				    false, false);
	}

	// Remove old data first
	pos = nrc_vendor_remove(nw, subcmd);

	/*
	 * One buffer holds the elements of every sub-command that shares it, so
	 * the total can outgrow it even though each element is bounded. skb_put()
	 * would panic the kernel on overflow, so refuse the request instead.
	 */
	if (skb_tailroom(*vendor_skb_ptr) < new_elem_len) {
		ERR("%s: vendor IE buffer full (need %d, free %d)", __func__,
		    new_elem_len, skb_tailroom(*vendor_skb_ptr));
		return -ENOSPC;
	}

	/* Append new data */
	pos = skb_put(*vendor_skb_ptr, new_elem_len);
	*pos++ = WLAN_EID_VENDOR_SPECIFIC;
	*pos++ = data_len + OUI_LEN + 1;
	*pos++ = (VENDOR_OUI & 0xFF0000) >> 16;
	*pos++ = (VENDOR_OUI & 0xFF00) >> 8;
	*pos++ = (VENDOR_OUI & 0xFF);
	*pos++ = subcmd;
	memcpy(pos, data, data_len);

	if (nw->params->debug_level_all) {
		print_hex_dump(KERN_DEBUG,
			       "new vendor elem: ", DUMP_PREFIX_NONE, 16, 1,
			       (*vendor_skb_ptr)->data, (*vendor_skb_ptr)->len,
			       false);
	}

	return 0;
}

static int nrc_vendor_cmd_remove(struct wiphy *wiphy, struct wireless_dev *wdev,
				 u8 subcmd)
{
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct ieee80211_vif *vif = wdev_to_ieee80211_vif(wdev);
	struct nrc *nw = hw->priv;

	/* Remove the vendor ie */
	if (nrc_vendor_remove(nw, subcmd) == NULL)
		return -EINVAL;

	nrc_vcmd_backup_remove_entry(vif, subcmd);

	/* Update beacon */
	if ((subcmd >= NRC_SUBCMD_ANNOUNCE1 &&
	     subcmd <= NRC_SUBCMD_BCAST_FOTA_4) ||
	    (subcmd == NRC_SUBCMD_UTC))
		return nrc_vendor_update_beacon(hw, vif);
	else if (subcmd >= NRC_SUBCMD_ANNOUNCE6 &&
		 subcmd <= NRC_SUBCMD_ANNOUNCE10)
		return nrc_vendor_update_probe_req(hw, vif);
	else if (subcmd >= NRC_SUBCMD_ANNOUNCE11 &&
		 subcmd <= NRC_SUBCMD_ANNOUNCE15)
		return nrc_vendor_update_probe_rsp(hw, vif);
	else if (subcmd >= NRC_SUBCMD_ANNOUNCE16 &&
		 subcmd <= NRC_SUBCMD_ANNOUNCE20)
		return nrc_vendor_update_assoc_req(hw, vif);
	else
		WARN_ON(true);

	return -EINVAL;
}

struct timer_list remotecmd_timer;
struct remotecmd_params remotecmd_params;

static void remotecmd_callback(struct timer_list *t)
{
	struct wiphy *wiphy = remotecmd_params.wiphy;
	struct wireless_dev *wdev = remotecmd_params.wdev;
	u8 subcmd = remotecmd_params.subcmd;
	nrc_vendor_cmd_remove(wiphy, wdev, subcmd);
}

static void remotecmd_schedule_off(struct wiphy *wiphy,
				   struct wireless_dev *wdev, u8 subcmd,
				   const u8 cntdwn, u16 beacon_int)
{
	remotecmd_params.wiphy = wiphy;
	remotecmd_params.wdev = wdev;
	remotecmd_params.subcmd = subcmd;

	timer_setup(&remotecmd_timer, remotecmd_callback, 0);
	mod_timer(&remotecmd_timer,
		  jiffies + usecs_to_jiffies(beacon_int * cntdwn * 1024));
}

VCMD_BACKUP_INFO vcmd_backup_info[VIF_MAX];
static int nrc_vcmd_backup_reinstall(u8 vif_id, struct nrc *nw)
{
	VCMD_ENTRY *cur, *next;
	struct ieee80211_hw *hw = nw->hw;
	struct ieee80211_vif *vif = nw->vif[vif_id];

	if ((hw == NULL) || (vif == NULL)) {
		DBG_MAC("%s, No hw(0x%x) and vif(0x%x) info", __func__, hw,
			vif);
		return -EINVAL;
	}

	if (list_empty(&vcmd_backup_info[vif_id].head) != 0) {
		DBG_MAC("%s, No vcmd entry", __func__);
		return 0;
	}

	list_for_each_entry_safe(cur, next, &vcmd_backup_info[vif_id].head,
				 list)
	{
		/* Update local vendor data */
		if (nrc_vendor_update(nw, cur->subcmd, cur->data,
				      cur->data_len) != 0) {
			DBG_MAC("%s, sub_cmd:%d, error: invalid arg", __func__,
				cur->subcmd);
			continue;
		}

		if (cur->subcmd <= NRC_SUBCMD_ANNOUNCE5)
			nrc_vendor_update_beacon(hw, vif);
		else if ((cur->subcmd >= NRC_SUBCMD_ANNOUNCE6) &&
			 (cur->subcmd < NRC_SUBCMD_ANNOUNCE11))
			nrc_vendor_update_probe_req(hw, vif);
		else if ((cur->subcmd >= NRC_SUBCMD_ANNOUNCE11) &&
			 (cur->subcmd < NRC_SUBCMD_ANNOUNCE16))
			nrc_vendor_update_probe_rsp(hw, vif);
		else if ((cur->subcmd >= NRC_SUBCMD_ANNOUNCE16) &&
			 (cur->subcmd <= NRC_SUBCMD_ANNOUNCE20))
			nrc_vendor_update_assoc_req(hw, vif);
		else {
			DBG_MAC("%s, sub_cmd:%d, error: invalid subcmd",
				__func__, cur->subcmd);
			continue;
		}
		DBG_MAC("%s, reinstalled sub_cmd:%d", __func__, cur->subcmd);
	}
	return 0;
}

static void nrc_vcmd_backup_delayed_reinstall(struct work_struct *work)
{
	VCMD_BACKUP_INFO *vcmd_info;
	vcmd_info = container_of(work, VCMD_BACKUP_INFO,
				 vcmd_backup_reinstall.work);
	nrc_vcmd_backup_reinstall(vcmd_info->vif_id, vcmd_info->nw);
}

static void nrc_vcmd_backup_init_info(u8 vif_id, struct nrc *nw)
{
	if (!vcmd_backup_info[vif_id].initialized) {
		INIT_LIST_HEAD(&vcmd_backup_info[vif_id].head);
		vcmd_backup_info[vif_id].initialized = true;
		vcmd_backup_info[vif_id].reset_by_wdt = false;
		vcmd_backup_info[vif_id].vif_id = vif_id;
		vcmd_backup_info[vif_id].nw = nw;
		INIT_DELAYED_WORK(
			&vcmd_backup_info[vif_id].vcmd_backup_reinstall,
			nrc_vcmd_backup_delayed_reinstall);
		DBG_MAC("%s, Init vif(%d) vcmd backup info", __func__, vif_id);
	} else {
		if (vcmd_backup_info[vif_id].reset_by_wdt) {
			if (nw->vif[vif_id]->type == NL80211_IFTYPE_AP) {
				DBG_MAC("%s,AP:vif(%d) is reset by wdt. Restore vcmds",
					__func__, vif_id);
				nrc_vcmd_backup_reinstall(vif_id, nw);

			} else if (nw->vif[vif_id]->type ==
				   NL80211_IFTYPE_STATION) {
				DBG_MAC("%s,STA:vif(%d) is reset by wdt. Restore vcmds",
					__func__, vif_id);
				queue_delayed_work(
					nw->hdev->workqueue,
					&vcmd_backup_info[vif_id]
						 .vcmd_backup_reinstall,
					msecs_to_jiffies(
						VCMD_BACKUP_RESOTRE_WORK_DELAY_MS));
			}
			vcmd_backup_info[vif_id].reset_by_wdt = false;
		}
	}
}

static VCMD_BACKUP_INFO *nrc_vcmd_backup_get_info_addr(u8 vif_id)
{
	if (vif_id < VIF_MAX) {
		return &vcmd_backup_info[vif_id];
	} else {
		return NULL;
	}
}

void nrc_vcmd_backup_set_wdt_flag(u8 vif_id)
{
	if (vif_id < VIF_MAX) {
		vcmd_backup_info[vif_id].reset_by_wdt = true;
		DBG_MAC("%s, vif(%d) is reset by wdt", __func__, vif_id);
	}
}

static void _nrc_vcmd_backup_del_entry(u8 vif_id, u8 subcmd)
{
	VCMD_ENTRY *cur, *next;

	if (list_empty(&vcmd_backup_info[vif_id].head) != 0) {
		DBG_MAC("%s, No vcmd entry", __func__);
		return;
	}

	list_for_each_entry_safe(cur, next, &vcmd_backup_info[vif_id].head,
				 list)
	{
		if (cur->subcmd == subcmd) {
			DBG_MAC("%s, Del vcmd_entry: sub_cmd:%d", __func__,
				subcmd);
			list_del(&cur->list);
			kfree(cur);
			return;
		}
	}
}

static int _nrc_vcmd_backup_add_entry(u8 vif_id, u8 subcmd, const u8 *data,
				      int data_len)
{
	VCMD_ENTRY *vcmd_entry;

	/* Delete same subcmd */
	_nrc_vcmd_backup_del_entry(vif_id, subcmd);

	vcmd_entry = kzalloc((sizeof(VCMD_ENTRY) + data_len - 1), GFP_KERNEL);
	if (!vcmd_entry) {
		DBG_MAC("%s, Failure: kzalloc ", __func__);
		return -ENOMEM;
	}

	vcmd_entry->subcmd = subcmd;
	vcmd_entry->data_len = data_len;
	memcpy(vcmd_entry->data, data, data_len);
	list_add_tail(&vcmd_entry->list, &vcmd_backup_info[vif_id].head);
	return 0;
}

static void nrc_vcmd_backup_del_all_entry(u8 vif_id)
{
	VCMD_ENTRY *cur, *next;

	if (list_empty(&vcmd_backup_info[vif_id].head) != 0) {
		DBG_MAC("%s, No vcmd entry", __func__);
		return;
	}

	list_for_each_entry_safe(cur, next, &vcmd_backup_info[vif_id].head,
				 list)
	{
		DBG_MAC("%s, Del vcmd_entry: sub_cmd:%d", __func__,
			cur->subcmd);
		list_del(&cur->list);
		kfree(cur);
	}
	return;
}

static int nrc_vcmd_backup_remove_entry(struct ieee80211_vif *vif, u8 subcmd)
{
	if (((subcmd >= NRC_SUBCMD_ANNOUNCE1) &&
	     (subcmd <= NRC_SUBCMD_ANNOUNCE5)) ||
	    ((subcmd >= NRC_SUBCMD_ANNOUNCE6) &&
	     (subcmd <= NRC_SUBCMD_ANNOUNCE20))) {
		u8 vif_id = hw_vifindex(vif);
		_nrc_vcmd_backup_del_entry(vif_id, subcmd);
	}
	return 0;
}

static int nrc_vcmd_backup_add_entry(struct ieee80211_vif *vif, u8 subcmd,
				     const u8 *data, int data_len)
{
	if (((subcmd >= NRC_SUBCMD_ANNOUNCE1) &&
	     (subcmd <= NRC_SUBCMD_ANNOUNCE5)) ||
	    ((subcmd >= NRC_SUBCMD_ANNOUNCE6) &&
	     (subcmd <= NRC_SUBCMD_ANNOUNCE20))) {
		u8 vif_id = hw_vifindex(vif);
		_nrc_vcmd_backup_add_entry(vif_id, subcmd, data, data_len);
	}
	return 0;
}

// extern int g_nrc_beacon_updated;
static int nrc_vendor_cmd_append(struct wiphy *wiphy, struct wireless_dev *wdev,
				 u8 subcmd, const void *data, int data_len)
{
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct ieee80211_vif *vif = wdev_to_ieee80211_vif(wdev);
	struct nrc *nw = hw->priv;
	int ret;

	/* Update local vendor data */
	ret = nrc_vendor_update(nw, subcmd, data, data_len);
	if (ret)
		return ret;
	// Schedule async vendor IE removal if REMOTECMD
	if (subcmd == NRC_SUBCMD_REMOTECMD) {
		remotecmd_schedule_off(wiphy, wdev, subcmd, *(const u8 *)data,
				       vif->bss_conf.beacon_int);
	}

	nrc_vcmd_backup_add_entry(vif, subcmd, data, data_len);

	if ((subcmd < NRC_SUBCMD_BCAST_FOTA_1) || (subcmd == NRC_SUBCMD_UTC)) {
		nw->debug->g_nrc_beacon_updated = 0;
		return nrc_vendor_update_beacon(hw, vif);
	} else if ((subcmd >= NRC_SUBCMD_BCAST_FOTA_1) &&
		   (subcmd < NRC_SUBCMD_ANNOUNCE6))
		return 0;
	else if ((subcmd >= NRC_SUBCMD_ANNOUNCE6) &&
		 (subcmd < NRC_SUBCMD_ANNOUNCE11))
		return nrc_vendor_update_probe_req(hw, vif);
	else if ((subcmd >= NRC_SUBCMD_ANNOUNCE11) &&
		 (subcmd < NRC_SUBCMD_ANNOUNCE16))
		return nrc_vendor_update_probe_rsp(hw, vif);
	else if ((subcmd >= NRC_SUBCMD_ANNOUNCE16) &&
		 (subcmd <= NRC_SUBCMD_ANNOUNCE20))
		return nrc_vendor_update_assoc_req(hw, vif);
	else {
		return -EINVAL;
	}
}

static int nrc_vendor_cmd_wowlan_pattern(struct wiphy *wiphy,
					 struct wireless_dev *wdev,
					 const void *data, int data_len)
{
	struct sk_buff *skb;
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct nrc *nw = hw->priv;
	const u8 *new_data = data;
	u8 count;

	DBG_MAC("%s: called", __func__);

	/* One count byte followed by at least one pattern byte. */
	if (!data || data_len < 2)
		return -EINVAL;

	count = *new_data;
	nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_WOWLAN_PATTERN,
			      new_data + 1, data_len - 1);

	queue_delayed_work(nw->hdev->workqueue,
			   &nw->rm_vendor_ie_wowlan_pattern,
			   msecs_to_jiffies(count * nw->beacon_int));

	/* Send a response to the command */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, 10);
	if (!skb)
		return -ENOMEM;

	nla_put_u32(skb, NRC_SUBCMD_WOWLAN_PATTERN, 0x11223344);

	return cfg80211_vendor_cmd_reply(skb);
}

static int nrc_vendor_cmd_announce1(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE1, data,
				     data_len);
}

static int nrc_vendor_cmd_announce2(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE2, data,
				     data_len);
}

static int nrc_vendor_cmd_announce3(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE3, data,
				     data_len);
}

static int nrc_vendor_cmd_announce4(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE4, data,
				     data_len);
}

static int nrc_vendor_cmd_announce5(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE5, data,
				     data_len);
}

static int nrc_vendor_cmd_remotecmd(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_REMOTECMD, data,
				     data_len);
}

static int nrc_vendor_cmd_remove_vendor_ie(struct wiphy *wiphy,
					   struct wireless_dev *wdev,
					   const void *data, int data_len)
{
	DBG_MAC("%s: called", __func__);

	/* The sub-command byte is the whole payload; it has to be there. */
	if (!data || data_len < 1)
		return -EINVAL;

	return nrc_vendor_cmd_remove(wiphy, wdev, *((const u8 *)data));
}

static int nrc_vendor_cmd_bcast_fota_info(struct wiphy *wiphy,
					  struct wireless_dev *wdev,
					  const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_BCAST_FOTA_INFO,
				     data, data_len);
}

static int nrc_vendor_cmd_bcast_fota_1(struct wiphy *wiphy,
				       struct wireless_dev *wdev,
				       const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_BCAST_FOTA_1, data,
				     data_len);
}

static int nrc_vendor_cmd_bcast_fota_2(struct wiphy *wiphy,
				       struct wireless_dev *wdev,
				       const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_BCAST_FOTA_2, data,
				     data_len);
}

static int nrc_vendor_cmd_bcast_fota_3(struct wiphy *wiphy,
				       struct wireless_dev *wdev,
				       const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_BCAST_FOTA_3, data,
				     data_len);
}

static int nrc_vendor_cmd_bcast_fota_4(struct wiphy *wiphy,
				       struct wireless_dev *wdev,
				       const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_BCAST_FOTA_4, data,
				     data_len);
}

static int nrc_vendor_cmd_announce6(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE6, data,
				     data_len);
}

static int nrc_vendor_cmd_announce7(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE7, data,
				     data_len);
}

static int nrc_vendor_cmd_announce8(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE8, data,
				     data_len);
}

static int nrc_vendor_cmd_announce9(struct wiphy *wiphy,
				    struct wireless_dev *wdev, const void *data,
				    int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE9, data,
				     data_len);
}

static int nrc_vendor_cmd_announce10(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE10, data,
				     data_len);
}

static int nrc_vendor_cmd_announce11(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE11, data,
				     data_len);
}

static int nrc_vendor_cmd_announce12(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE12, data,
				     data_len);
}

static int nrc_vendor_cmd_announce13(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE13, data,
				     data_len);
}

static int nrc_vendor_cmd_announce14(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE14, data,
				     data_len);
}

static int nrc_vendor_cmd_announce15(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE15, data,
				     data_len);
}

static int nrc_vendor_cmd_announce16(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE16, data,
				     data_len);
}

static int nrc_vendor_cmd_announce17(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE17, data,
				     data_len);
}

static int nrc_vendor_cmd_announce18(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE18, data,
				     data_len);
}

static int nrc_vendor_cmd_announce19(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE19, data,
				     data_len);
}

static int nrc_vendor_cmd_announce20(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_ANNOUNCE20, data,
				     data_len);
}

static int nrc_vendor_cmd_utc(struct wiphy *wiphy, struct wireless_dev *wdev,
			      const void *data, int data_len)
{
	return nrc_vendor_cmd_append(wiphy, wdev, NRC_SUBCMD_UTC, data,
				     data_len);
}

static struct wiphy_vendor_command nrc_vendor_cmds[] = {
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_WOWLAN_PATTERN},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_wowlan_pattern,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE1},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce1,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE2},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce2,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE3},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce3,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE4},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce4,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE5},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce5,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_REMOTECMD},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_remotecmd,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_RM_VENDOR_IE},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_remove_vendor_ie,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_BCAST_FOTA_INFO},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_bcast_fota_info,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_BCAST_FOTA_1},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_bcast_fota_1,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_BCAST_FOTA_2},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_bcast_fota_2,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_BCAST_FOTA_3},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_bcast_fota_3,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_BCAST_FOTA_4},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_bcast_fota_4,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE6},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce6,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE7},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce7,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE8},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce8,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE9},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce9,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE10},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce10,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE11},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce11,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE12},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce12,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE13},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce13,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE14},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce14,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE15},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce15,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE16},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce16,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE17},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce17,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE18},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce18,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE19},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce19,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_ANNOUNCE20},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_announce20,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
	{
		.info = {.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
			 .subcmd = NRC_SUBCMD_UTC},
		.flags = WIPHY_VENDOR_CMD_NEED_NETDEV,
		.doit = nrc_vendor_cmd_utc,
		.policy = VENDOR_CMD_RAW_DATA,
		.maxattr = MAX_VENDOR_ATTR,
	},
};

static const struct nl80211_vendor_cmd_info nrc_vendor_events[] = {
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE1},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE2},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE3},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE4},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE5},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_REMOTECMD},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_WOWLAN_PATTERN},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_BCAST_FOTA_INFO},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_BCAST_FOTA_1},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_BCAST_FOTA_2},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_BCAST_FOTA_3},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_BCAST_FOTA_4},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE6},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE7},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE8},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE9},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE10},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE11},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE12},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE13},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE14},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE15},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE16},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE17},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE18},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE19},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_ANNOUNCE20},
	{.vendor_id = OUI_IEEE_REGISTRATION_AUTHORITY,
	 .subcmd = NRC_SUBCMD_UTC},
};

void nrc_rm_vendor_ie_wowlan_pattern(struct work_struct *work)
{
	struct nrc *nw = container_of(work, struct nrc,
				      rm_vendor_ie_wowlan_pattern.work);

	/* Remove the vendor ie */
	nrc_vendor_remove(nw, NRC_SUBCMD_WOWLAN_PATTERN);
	/* Update beacon */
	nrc_vendor_update_beacon(nw->hw, nw->vif[0]);
}

void nrc_bcn_mon_timer(struct timer_list *t)
{
	struct nrc_vif *i_vif = from_timer(i_vif, t, bcn_mon_timer);
	struct nrc *nw = i_vif->nw;
	struct nrc_hif_device *hdev = nw->hdev;

	if (NRC_DRV_IS_ASLEEP(hdev)) {
		DBG_MAC("VIF%d in PS state, ignore bcn_mon_timeout",
			i_vif->index);
		return;
	}
	if (nw->params->disable_cqm_on_scan) {
		ieee80211_beacon_loss(to_ieee80211_vif(i_vif));
	} else {
		if (atomic_read(&nw->scan_mode) == NRC_SCAN_MODE_IDLE) {
			ieee80211_beacon_loss(to_ieee80211_vif(i_vif));
		} else {
			DBG_MAC("VIF%d in scan state, delay beacon loss event",
				i_vif->index);
			i_vif->is_bcn_timeout = true;
			mod_timer(&i_vif->bcn_mon_timer,
				  jiffies + msecs_to_jiffies(
						    i_vif->beacon_timeout));
		}
	}
}

/**
 * nrc_hw_register - initialize struct ieee80211_hw instance
 */
int nrc_register_hw(struct nrc *nw, struct nrc_hif_device *hdev)
{
	struct ieee80211_hw *hw = nw->hw;
	struct ieee80211_supported_band *sband = NULL;

	enum nl80211_band band;
	int ret;
	int i;
	/*	char tmp[2];*/

	/* Initialize debug system with wiphy device */
	nrc_dbg_init(&hw->wiphy->dev);

	for (i = 0; i < NR_NRC_VIF; i++) {
		if (!hdev->has_macaddr[i])
			set_mac_address(&hdev->mac_addr[i], i);
	}

	SET_IEEE80211_PERM_ADDR(hw, hdev->mac_addr[0].addr);
	hw->wiphy->n_addresses = NR_NRC_VIF;
	hw->wiphy->addresses = hdev->mac_addr;

	/* Debug: Print all MAC addresses configured for wiphy */
	DBG_MAC("IEEE80211 registered with %d addresses:",
		hw->wiphy->n_addresses);
	for (i = 0; i < hw->wiphy->n_addresses; i++) {
		const char *source = hdev->has_macaddr[i] ? "from FW/chip" :
							    "host-generated";
		DBG_MAC("  Address[%d]: %pM (%s)", i,
			hw->wiphy->addresses[i].addr, source);
	}
	DBG_MAC("Permanent address: %pM", hw->wiphy->perm_addr);

	SET_IEEE80211_DEV(hw, nw->dev);

	hw->wiphy->max_scan_ssids = WIM_MAX_SCAN_SSID;
	hw->wiphy->max_scan_ie_len = WIM_MAX_TLV_SCAN_IE;
	hw->wiphy->max_remain_on_channel_duration = NRC_MAC80211_ROC_DURATION;
	hw->wiphy->interface_modes =
		BIT(NL80211_IFTYPE_STATION) | BIT(NL80211_IFTYPE_AP) |
#ifdef CONFIG_MAC80211_MESH
		BIT(NL80211_IFTYPE_MESH_POINT) |
#endif
#if !defined(CONFIG_S1G_CHANNEL)
#ifdef CONFIG_SUPPORT_P2P
		BIT(NL80211_IFTYPE_P2P_CLIENT) | BIT(NL80211_IFTYPE_P2P_GO) |
		BIT(NL80211_IFTYPE_P2P_DEVICE) |
#endif
#endif /* CONFIG_S1G_CHANNEL */
#if defined(CONFIG_SUPPORT_IBSS)
		BIT(NL80211_IFTYPE_ADHOC) |
#endif
		BIT(NL80211_IFTYPE_MONITOR);
	/* Ensure at least 1 queue is set for basic operation */
	hw->queues = nw->hdev->hw_queues;
	DBG_MAC("Setting hw->queues to %d (nw->hdev->hw_queues=%d)", hw->queues,
		nw->hdev->hw_queues);
	hw->offchannel_tx_hw_queue = (IEEE80211_MAX_QUEUES - 1);

	for (i = 0; i < ARRAY_SIZE(nw->ntxq); i++) {
		struct nrc_txq *ntxq = &nw->ntxq[i];

		INIT_LIST_HEAD(&ntxq->list);
		ntxq->hw_queue = i;
	}

	if (nw->hdev->cap.cap_mask & WIM_SYSTEM_CAP_MULTI_VIF) {
		hw->wiphy->iface_combinations = if_comb_multi;
		hw->wiphy->n_iface_combinations = ARRAY_SIZE(if_comb_multi);
	}

	ieee80211_hw_set(hw, HAS_RATE_CONTROL);
	ieee80211_hw_set(hw, AMPDU_AGGREGATION);
	ieee80211_hw_set(hw, MFP_CAPABLE);
	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, SUPPORTS_PER_STA_GTK);
	ieee80211_hw_set(hw, SINGLE_SCAN_ON_ALL_BANDS);
#ifdef CONFIG_USE_MONITOR_VIF
	ieee80211_hw_set(hw, WANT_MONITOR_VIF);
#endif
	ieee80211_hw_set(hw, CONNECTION_MONITOR);

	if (nw->params->power_save >= NRC_PS_MODEMSLEEP) {
		ieee80211_hw_set(hw, SUPPORTS_PS);

		/*
		 * Dynamic PS timer: controls idle-to-sleep transition.
		 * Currently all PS modes use driver-managed timer unconditionally.
		 * If per-mode control is needed later, enable NRC_PS_PER_MODE_DYN
		 * to restore the original mode-based logic.
		 */
#if defined(NRC_PS_PER_MODE_DYN)
		/* Per-mode dynamic PS: only enable for specific conditions */
		if (nw->params->power_save >= NRC_PS_DEEPSLEEP_TIM) {
			nw->hdev->ps.supports_dynamic_ps = true;
			ieee80211_hw_set(hw, SUPPORTS_DYNAMIC_PS);
		}
#else
		/* All PS modes: always use driver-managed dynamic PS timer */
		nw->hdev->ps.supports_dynamic_ps = true;
		ieee80211_hw_set(hw, SUPPORTS_DYNAMIC_PS);
#endif

		/* Initialize dynamic PS timer (checks supports_dynamic_ps internally) */
		nrc_ps_dyn_init(nw);

		/*
		 * PS_NULLFUNC_STACK: mac80211 handles nullfunc frames
		 * Only set when nullfunc_enable=1 AND not NonTIM mode
		 */
		if (nw->params->nullfunc_enable &&
		    nw->params->power_save < NRC_PS_DEEPSLEEP_NONTIM) {
			ieee80211_hw_set(hw, PS_NULLFUNC_STACK);
		}
	}

	hw->wiphy->flags |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;
	hw->wiphy->flags |= WIPHY_FLAG_HAS_CHANNEL_SWITCH;
	hw->wiphy->features |= NL80211_FEATURE_INACTIVITY_TIMER;

	/* hostapd ver > 2.6 need for NL80211_FEATURE_FULL_AP_CLIENT_STATE */
	hw->wiphy->features |= NL80211_FEATURE_FULL_AP_CLIENT_STATE;
	hw->vif_data_size = sizeof(struct nrc_vif);
	hw->sta_data_size = sizeof(struct nrc_sta);
	hw->txq_data_size = sizeof(struct nrc_txq);
	hw->chanctx_data_size = 0;

	/* FW handles probe-requests in AP-mode */
	hw->wiphy->flags |= WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD;
	hw->wiphy->flags |= WIPHY_FLAG_AP_UAPSD;
	hw->wiphy->flags |= WIPHY_FLAG_IBSS_RSN;

	hw->wiphy->probe_resp_offload =
		NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS2 |
		NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS2 |
		NL80211_PROBE_RESP_OFFLOAD_SUPPORT_P2P;
#ifdef CONFIG_S1G_CHANNEL
	wiphy_ext_feature_set(hw->wiphy, NL80211_EXT_FEATURE_SCAN_FREQ_KHZ);
#endif
	for (band = NL80211_BAND_2GHZ; band < NUM_NL80211_BANDS; band++) {
		sband = &nw->bands[band];

		switch (band) {
#if defined(CONFIG_S1G_CHANNEL)
		case NL80211_BAND_S1GHZ:
			memcpy(&sband->s1g_cap, &nrc_s1g_cap,
			       sizeof(sband->s1g_cap));
			init_s1g_channels(nw);
			sband->channels = nrc_channels_s1ghz;
			sband->n_channels =
				nrc_get_num_channels_by_current_country();
			DBG_MAC("sband->n_channels:%d", sband->n_channels);
			sband->bitrates = nrc_rates;
			sband->n_bitrates = ARRAY_SIZE(nrc_rates);
			sband->band = NL80211_BAND_S1GHZ;
			sband->ht_cap.ht_supported = true;
			hw->wiphy->bands[band] = sband;
			//continue;
			break;
#else
		case NL80211_BAND_5GHZ:
			if (!(nw->hdev->cap.cap_mask &
			      WIM_SYSTEM_CAP_CHANNEL_5G))
				continue;
			sband->channels = nrc_channels_5ghz;
			sband->n_channels = ARRAY_SIZE(nrc_channels_5ghz);
			sband->bitrates = nrc_rates + 4;
			sband->n_bitrates = ARRAY_SIZE(nrc_rates) - 4;
			sband->band = NL80211_BAND_5GHZ;
			sband->ht_cap.ht_supported = true;
			sband->ht_cap.cap = IEEE80211_HT_CAP_SGI_20;
			sband->ht_cap.cap |= IEEE80211_HT_CAP_SGI_40;
			/* wpa_supplicant-2.10 checks HT40 allowed channel pair.
			 * possible HT40+ channels: 36, 44, 52, 60, 100, 108, 116, 124,
			 * 132, 149, 157.
			 * possible HT40- channels: 40, 48, 56, 64, 104, 112, 120, 128,
			 * 136, 153, 161.
			 * but we use other 5Ghz channels to match S1G channels.
			 *
			 * sband->ht_cap.cap |= IEEE80211_HT_CAP_SUP_WIDTH_20_40;
			 */
			sband->ht_cap.cap |=
				(1 << IEEE80211_HT_CAP_RX_STBC_SHIFT);
			sband->ht_cap.ampdu_factor = IEEE80211_HT_MAX_AMPDU_16K;
			sband->ht_cap.ampdu_density =
				IEEE80211_HT_MPDU_DENSITY_8;
			break;

		case NL80211_BAND_2GHZ:
			if (!(nw->hdev->cap.cap_mask &
			      WIM_SYSTEM_CAP_CHANNEL_2G))
				continue;
			sband->channels = nrc_channels_2ghz;
			sband->n_channels = ARRAY_SIZE(nrc_channels_2ghz);
			sband->bitrates = nrc_rates;
			sband->n_bitrates = ARRAY_SIZE(nrc_rates);
			sband->band = NL80211_BAND_2GHZ;
			sband->ht_cap.ht_supported = true;
			sband->ht_cap.cap = IEEE80211_HT_CAP_SGI_20;
			sband->ht_cap.cap |= IEEE80211_HT_CAP_SGI_40;
			sband->ht_cap.cap |= IEEE80211_HT_CAP_SUP_WIDTH_20_40;
			sband->ht_cap.cap |= IEEE80211_HT_CAP_DSSSCCK40;
			sband->ht_cap.cap |=
				(1 << IEEE80211_HT_CAP_RX_STBC_SHIFT);
			sband->ht_cap.ampdu_factor = IEEE80211_HT_MAX_AMPDU_16K;
			sband->ht_cap.ampdu_density =
				IEEE80211_HT_MPDU_DENSITY_8;
			break;
#endif /* CONFIG_S1G_CHANNEL */
		default:
			continue;
		}

		memset(&sband->ht_cap.mcs, 0, sizeof(sband->ht_cap.mcs));

		sband->ht_cap.mcs.rx_mask[0] = 0xff;
		sband->ht_cap.mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED;

		hw->wiphy->bands[band] = sband;
	}

	hw->wiphy->cipher_suites = nrc_cipher_supported;
	hw->wiphy->n_cipher_suites = ARRAY_SIZE(nrc_cipher_supported);

	hw->extra_tx_headroom =
		(sizeof(struct hif) + sizeof(struct frame_hdr) + 32);
	hw->max_mtu = IEEE80211_MAX_DATA_LEN; //Maximum MSDU size (2304)
	hw->max_rates = 4;
	hw->max_rate_tries = 11;

	hw->wiphy->vendor_commands = nrc_vendor_cmds;
	hw->wiphy->n_vendor_commands = ARRAY_SIZE(nrc_vendor_cmds);

	hw->wiphy->vendor_events = nrc_vendor_events;
	hw->wiphy->n_vendor_events = ARRAY_SIZE(nrc_vendor_events);

	hw->wiphy->regulatory_flags = REGULATORY_CUSTOM_REG |
				      WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;

#ifdef CONFIG_PM
	if (nw->hdev->wowlan_pattern_num) { /* if configured */
		nrc_wowlan_support.n_patterns = nw->hdev->wowlan_pattern_num;
	}
	hw->wiphy->wowlan = &nrc_wowlan_support;
#endif

	wiphy_apply_custom_regulatory(hw->wiphy, &mac80211_regdom);
	nw->alpha2[0] = '9';
	nw->alpha2[1] = '9';

#if defined(CONFIG_SUPPORT_BD)
	/*
	 * BD is not in FW at init — it is sent by nrc_reg_notifier() when
	 * a valid country code is received.  Keep g_bd_valid = false until
	 * the first successful nrc_reg_notifier() call sets it to true.
	 */
	g_bd_valid = false;
#endif

	if (nrc_mac_is_s1g(nw->hdev)) {
		/*this is only for 802.11ah*/
		hw->wiphy->reg_notifier = nrc_reg_notifier;
	}

	hw->uapsd_queues = IEEE80211_WMM_IE_STA_QOSINFO_AC_BK |
			   IEEE80211_WMM_IE_STA_QOSINFO_AC_BE |
			   IEEE80211_WMM_IE_STA_QOSINFO_AC_VI |
			   IEEE80211_WMM_IE_STA_QOSINFO_AC_VO;
	nw->hdev->ampdu_supported = false;
	nw->amsdu_supported = true;
	nw->block_frame = false;
	nw->ampdu_reject = false;
	nw->frag_threshold = -1;

	if (nw->params->enable_sched_scan) {
		DBG_STATE("Sched scan is enabled");

		hw->wiphy->max_sched_scan_ie_len = WIM_MAX_TLV_SCAN_IE;
		hw->wiphy->max_sched_scan_ssids = WIM_MAX_SCAN_SSID;
		hw->wiphy->max_match_sets = WIM_MAX_MATCH_SET;
		hw->wiphy->max_sched_scan_reqs = 1;

		hw->wiphy->max_sched_scan_plans = WIM_MAX_SCAN_PLAN;
		hw->wiphy->max_sched_scan_plan_interval = U16_MAX;
		hw->wiphy->max_sched_scan_plan_iterations = 254;
	}

	/* trx */
	nrc_mac_trx_init(nw);

	/* Initialize WLAN module state */
	atomic_set(&nw->hw_unregistering, 0);

	ret = ieee80211_register_hw(hw);
	if (ret < 0) {
		ERR("ieee80211_register_hw failed (%d)", ret);
		/* Don't free hw here - let cleanup functions handle it */
		return ret;
	}

	DBG_STATE("registered network device %s", wiphy_name(hw->wiphy));

	return 0;
}

void nrc_unregister_hw(struct nrc *nw)
{
	DBG_STATE("unregistered network device %s", wiphy_name(nw->hw->wiphy));

	/* Stop WLAN RX processing - prevent new frames from being queued */
	atomic_set(&nw->hw_unregistering, 1);

	/* Wait for in-flight RX processing to complete */
	synchronize_net();

	nrc_cleanup_txq_all(nw);

	/* Cleanup CQM timers before unregistering hardware */
	if (!nw->params->disable_cqm) {
		int i;

		for (i = 0; i < NR_NRC_VIF; i++) {
			if (nw->vif[i] &&
			    nw->vif[i]->type == NL80211_IFTYPE_STATION)
				del_timer(&to_i_vif(nw->vif[i])->bcn_mon_timer);
		}
	}

	ieee80211_unregister_hw(nw->hw);
	SET_IEEE80211_DEV(nw->hw, NULL);
	nrc_hal_ops_tx_cleanup_queues();

	nrc_ps_dyn_deinit(nw);
}

/* nrc_mac_is_s1g function moved to common/nrc.h as static inline */

void nrc_mac_clean_txq(struct nrc *nw)
{
	nrc_cleanup_txq_all(nw);
}

void nrc_mac_flush_txq(struct nrc *nw)
{
	nrc_flush_txq(nw);
}

struct ieee80211_hw *nrc_mac_alloc_hw(size_t priv_data_len,
				      const char *req_name)
{
	struct ieee80211_hw *hw;

	hw = ieee80211_alloc_hw_nm(priv_data_len, &nrc_mac80211_ops, req_name);

	return hw;
}

void nrc_mac_free_hw(struct ieee80211_hw *hw)
{
	/* Ensure all RCU callbacks and pending ACK frames are completed
	 * before freeing hardware to avoid "Have pending ack frames!" warning */
	synchronize_net();
	ieee80211_free_hw(hw);
}

/* Send loss event to all associated STA VIFs */
void nrc_send_beacon_loss(struct nrc *nw)
{
	int i;

	spin_lock_bh(&nw->vif_lock);
	for (i = 0; i < NR_NRC_VIF; i++) {
		struct nrc_vif *iv;

		if (!nw->vif[i] || nw->vif[i]->type != NL80211_IFTYPE_STATION)
			continue;
		iv = to_i_vif(nw->vif[i]);
		if (!iv->associated)
			continue;
		DBG_STATE("beacon loss event to VIF%d", iv->index);
		ieee80211_beacon_loss(nw->vif[i]);
	}
	spin_unlock_bh(&nw->vif_lock);
}

void nrc_cleanup_ba_session_sta(void *data, struct ieee80211_sta *sta)
{
	int i;

	for (i = 0; i < IEEE80211_NUM_TIDS; i++) {
		ieee80211_stop_tx_ba_session(sta, i);
	}

	//nrc_init_sta_ba_session(sta);
}

void nrc_cleanup_ba_session_vif(struct nrc *nw, struct ieee80211_vif *vif)
{
	ieee80211_iterate_stations_atomic(nw->hw, nrc_cleanup_ba_session_sta,
					  (void *)vif);
}

void nrc_cleanup_ba_session_all(struct nrc *nw)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nw->vif); i++) {
		nrc_cleanup_ba_session_vif(nw, nw->vif[i]);
	}
}

char *nrc_idle_mode_get_state_str(struct nrc *nw)
{
	return nrc_idle_mode_get_state(nw) ? "IDLE" : "NO IDLE";
}

bool nrc_idle_mode_get_state(struct nrc *nw)
{
	return nw->hdev->params->idle_mode && nw->idle_state &&
	       !nrc_has_associated_sta_vif(nw) &&
	       (atomic_read(&nw->scan_mode) != NRC_SCAN_MODE_ACTIVE_SCANNING);
}

void nrc_idle_mode_set_state(struct nrc *nw, bool enable)
{
	if (nw->idle_state == enable)
		return;

	DBG_STATE("%s: enable = %d", __FUNCTION__, enable);
	nw->idle_state = enable;
}

/*
 * Explicit STA handler array
 */
const struct nrc_sta_handler nrc_sta_handlers[] = {
	{.sta_state = sta_h_bss_max_idle_period},
};
const int nrc_sta_handlers_count = ARRAY_SIZE(nrc_sta_handlers);
