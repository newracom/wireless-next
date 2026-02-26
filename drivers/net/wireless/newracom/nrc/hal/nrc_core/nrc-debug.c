/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC Debug Header - HAL Core debug implementation
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

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-backend-hif-interface.h"

/* Local module headers */
#include "nrc-debug.h"
#include "nrc-init.h"
#include "hif.h"
#include "nrc-ps.h"

/* Global debug variables - defined as module parameters in nrc-hal-init.c */
extern unsigned long debug_mask;
extern int debug_level;
struct device *g_dev;

/* Debug functions now implemented as inline in common/nrc-debug.h */

#ifdef CONFIG_DEBUG_FS
/* HAL-specific debugfs root */
static struct dentry *nrc_core_debugfs_root;
static struct dentry *loopback_debugfs_root;

/* Loopback test global variables
 * Note: g_lb_skb is no longer used as a persistent template.
 * SKBs are created directly during test execution.
 */
static u32 g_lb_skb_len = 1024; /* Default sample size */
static u8 g_lb_subtype;
static u32 g_lb_count = 100; /* Default frame count */
static u32 g_lb_bunch = 0;

/* Forward declarations for debugfs */
static int nrc_debugfs_skb_stats_show(struct seq_file *s, void *unused);
static int nrc_debugfs_skb_stats_open(struct inode *inode, struct file *file);

/* SKB Statistics debugfs file operations */
static const struct file_operations skb_stats_ops = {
	.open = nrc_debugfs_skb_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ========================================================================
 * Common debugfs entries (shared by WLAN and MCP frontends)
 * ======================================================================== */

/* Credit queue information */
static int nrc_debugfs_credit_show(struct seq_file *s, void *unused)
{
	struct nrc_hif_device *hdev = s->private;
	int i;

	if (!hdev) {
		seq_printf(s, "Invalid HIF device\n");
		return -EINVAL;
	}

	for (i = 0; i < CREDIT_QUEUE_MAX; i++) {
		u8 room = 0;
		int available;

		/* Wrap-around handling */
		if (hdev->credit.front[i] >= hdev->credit.rear[i]) {
			room = hdev->credit.front[i] - hdev->credit.rear[i];
		} else {
			room = (255 - hdev->credit.rear[i]) + hdev->credit.front[i];
		}

		room = min_t(u8, hdev->credit.credit_max[i], room);
		available = hdev->credit.credit_max[i] - room;

		seq_printf(s,
			   "CREDIT[%d] front=%d rear=%d credit=%d avail=%d\n",
			   i, hdev->credit.front[i], hdev->credit.rear[i],
			   hdev->credit.credit_max[i], available);
	}

	for (i = 0; i < IEEE80211_NUM_ACS * 3; i++) {
		seq_printf(s, "AC[%d] tx_credit=%d tx_pend=%d\n", i,
			   atomic_read(&hdev->credit.tx_credit[i]),
			   atomic_read(&hdev->credit.tx_pend[i]));
	}

	return 0;
}

static int nrc_debugfs_credit_open(struct inode *inode, struct file *file)
{
	return single_open(file, nrc_debugfs_credit_show, inode->i_private);
}

static const struct file_operations nrc_debugfs_credit_fops = {
	.owner = THIS_MODULE,
	.open = nrc_debugfs_credit_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Slot queue information */
static int nrc_debugfs_slot_show(struct seq_file *s, void *unused)
{
	struct nrc_hif_device *hdev = s->private;

	if (!hdev) {
		seq_printf(s, "Invalid HIF device\n");
		return -EINVAL;
	}

	seq_printf(s, "TX_SLOT: head=%d, tail=%d, size=%d, count=%d\n",
		   hdev->slot[0].head, hdev->slot[0].tail, hdev->slot[0].size,
		   hdev->slot[0].count);

	seq_printf(s, "RX_SLOT: head=%d, tail=%d, size=%d, count=%d\n",
		   hdev->slot[1].head, hdev->slot[1].tail, hdev->slot[1].size,
		   hdev->slot[1].count);

	return 0;
}

static int nrc_debugfs_slot_open(struct inode *inode, struct file *file)
{
	return single_open(file, nrc_debugfs_slot_show, inode->i_private);
}

static const struct file_operations nrc_debugfs_slot_fops = {
	.owner = THIS_MODULE,
	.open = nrc_debugfs_slot_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Common debug message mask (affects all modules) */
static int nrc_debugfs_debug_read(void *data, u64 *val)
{
	*val = debug_mask;
	return 0;
}

static int nrc_debugfs_debug_write(void *data, u64 val)
{
	debug_mask = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_debug_fops, nrc_debugfs_debug_read,
			nrc_debugfs_debug_write, "%llu\n");

/* Common debug level (affects all modules) */
static int nrc_debugfs_debug_level_read(void *data, u64 *val)
{
	*val = debug_level;
	return 0;
}

static int nrc_debugfs_debug_level_write(void *data, u64 val)
{
	if (val >= NRC_DBG_LEVEL_MAX) {
		ERR_HAL("Invalid debug level %llu (max=%d)", val,
			NRC_DBG_LEVEL_MAX - 1);
		return -EINVAL;
	}
	debug_level = (enum NRC_DEBUG_LEVEL)val;
	INFO("Debug level set to %llu", val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_debug_level_fops,
			nrc_debugfs_debug_level_read,
			nrc_debugfs_debug_level_write, "%llu\n");

/* Core module-specific debug mask */
static int nrc_core_debugfs_debug_read(void *data, u64 *val)
{
	*val = debug_mask;
	return 0;
}

static int nrc_core_debugfs_debug_write(void *data, u64 val)
{
	debug_mask = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_core_debugfs_debug_fops,
			nrc_core_debugfs_debug_read,
			nrc_core_debugfs_debug_write, "%llu\n");

/* Restart device */
static int nrc_debugfs_restart_device_read(void *data, u64 *val)
{
	*val = 0;
	return 0;
}

static int nrc_debugfs_restart_device_write(void *data, u64 val)
{
	/* Use HAL restart function */
	nrc_nw_restart();
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_restart_device_fops,
			nrc_debugfs_restart_device_read,
			nrc_debugfs_restart_device_write, "%llu\n");

/* ========================================================================
 * Loopback Test Functions (HIF TX queue testing)
 * ======================================================================== */

/* Loopback hexdump control */
static int nrc_debugfs_lb_hexdump_read(void *data, u64 *val)
{
	struct nrc_hif_device *hdev = (struct nrc_hif_device *)data;
	*val = (hdev && hdev->debug) ? (u64)hdev->debug->lb_hexdump : 0;
	return 0;
}

static int nrc_debugfs_lb_hexdump_write(void *data, u64 val)
{
	struct nrc_hif_device *hdev = (struct nrc_hif_device *)data;
	if (hdev && hdev->debug)
		hdev->debug->lb_hexdump = (u32)val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_lb_hexdump_fops,
			nrc_debugfs_lb_hexdump_read,
			nrc_debugfs_lb_hexdump_write, "%llu\n");

/* Loopback test mode */
static int nrc_debugfs_lb_test_read(void *data, u64 *val)
{
	*val = (u64)g_lb_subtype;
	return 0;
}

static int nrc_debugfs_lb_test_write(void *data, u64 val)
{
	struct nrc_hif_device *hdev = (struct nrc_hif_device *)data;
	struct sk_buff *skb = NULL;
	struct hif_lb_hdr *hif = NULL;
	u32 c = 0;
	u8 *p;
	int i;

	if (!hdev)
		return -EINVAL;

	g_lb_subtype = (u8)val;
	if (g_lb_subtype >= LOOPBACK_MODE_MAX)
		g_lb_subtype = LOOPBACK_MODE_ROUNDTRIP;

	/* Validate sample size */
	if (g_lb_skb_len == 0 || g_lb_skb_len > 1600) {
		ERR_HIF("[Loopback] Invalid sample size %u (must be 1-1600)",
			g_lb_skb_len);
		return -EINVAL;
	}

	/* Allocate time_info_array if debug structure exists */
	if (hdev->debug) {
		if (hdev->debug->time_info_array) {
			kfree(hdev->debug->time_info_array);
			hdev->debug->time_info_array = NULL;
		}

		hdev->debug->time_info_array = kzalloc(
			sizeof(struct lb_time_info) * g_lb_count, GFP_KERNEL);
		if (!hdev->debug->time_info_array) {
			ERR_HIF("Failed to alloc time_info_array");
			return -ENOMEM;
		}
	}

	if (g_lb_subtype < LOOPBACK_MODE_RX_ONLY) {
		/* TX modes: Round-trip or TX-only */
		while (c < g_lb_count) {
			/* Create SKB directly for each frame */
			skb = dev_alloc_skb(g_lb_skb_len +
					    sizeof(struct hif_lb_hdr));
			if (!skb) {
				ERR_HIF("Failed to alloc loopback skb");
				return -ENOMEM;
			}
			NRC_SKB_CB_INIT(skb);
			NRC_SKB_TRACK_ALLOC(hdev, skb, HIF_TYPE_LOOPBACK, false,
					    false);

			/* Fill SKB with test pattern */
			p = (u8 *)skb_put(
				skb, g_lb_skb_len + sizeof(struct hif_lb_hdr));
			for (i = 0; i < g_lb_skb_len; i++)
				*(p + sizeof(struct hif_lb_hdr) + i) = (u8)i;

			/* Setup HIF loopback header */
			hif = (struct hif_lb_hdr *)p;
			hif->type = HIF_TYPE_LOOPBACK;
			hif->len = g_lb_skb_len;
			hif->count = g_lb_count;
			hif->index = c++;
			hif->subtype = g_lb_subtype;

			if (c == 1 && hdev->debug && hdev->debug->lb_hexdump) {
				print_hex_dump(KERN_DEBUG, "HIF ",
					       DUMP_PREFIX_NONE, 16, 1,
					       skb->data,
					       sizeof(struct hif_lb_hdr),
					       false);
				print_hex_dump(
					KERN_DEBUG, "ORIGIN ", DUMP_PREFIX_NONE,
					16, 1,
					skb->data + sizeof(struct hif_lb_hdr),
					skb->len - sizeof(struct hif_lb_hdr),
					false);
			}

			skb_queue_tail(&hdev->queue[(hif->type) % 2], skb);
			if (!hdev->workqueue)
				return -EINVAL;

			if (c < 5)
				queue_work(hdev->workqueue, &hdev->work);
		}
	} else if (g_lb_subtype == LOOPBACK_MODE_RX_ONLY) {
		/* RX-only mode: send single trigger frame */
		skb = dev_alloc_skb(g_lb_skb_len + sizeof(struct hif_lb_hdr));
		if (!skb) {
			ERR_HIF("Failed to alloc loopback skb");
			return -ENOMEM;
		}
		NRC_SKB_TRACK_ALLOC(hdev, skb, HIF_TYPE_LOOPBACK, false, false);

		/* Fill SKB with test pattern */
		p = (u8 *)skb_put(skb,
				  g_lb_skb_len + sizeof(struct hif_lb_hdr));
		for (i = 0; i < g_lb_skb_len; i++)
			*(p + sizeof(struct hif_lb_hdr) + i) = (u8)i;

		/* Setup HIF loopback header */
		hif = (struct hif_lb_hdr *)p;
		hif->type = HIF_TYPE_LOOPBACK;
		hif->len = g_lb_skb_len;
		hif->count = g_lb_count;
		hif->index = 0;
		hif->subtype = g_lb_subtype;

		/* Trim for RX-only trigger */
		skb_trim(skb, 400);

		if (hdev->debug && hdev->debug->lb_hexdump) {
			print_hex_dump(KERN_DEBUG, "HIF ", DUMP_PREFIX_NONE, 16,
				       1, skb->data, sizeof(struct hif_lb_hdr),
				       false);
		}

		skb_queue_tail(&hdev->queue[(hif->type) % 2], skb);
		if (!hdev->workqueue)
			return -EINVAL;

		queue_work(hdev->workqueue, &hdev->work);
	}

	INFO("[Loopback Test] TX Done. (size=%u, count=%u, mode=%u)",
	     g_lb_skb_len, g_lb_count, g_lb_subtype);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_lb_test_fops, nrc_debugfs_lb_test_read,
			nrc_debugfs_lb_test_write, "%llu\n");

/* Loopback count */
static int nrc_debugfs_lb_count_read(void *data, u64 *val)
{
	*val = (u64)g_lb_count;
	return 0;
}

static int nrc_debugfs_lb_count_write(void *data, u64 val)
{
	if ((u32)val > 0xffff)
		g_lb_count = 0xffff;
	else
		g_lb_count = (u32)val;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_debugfs_lb_count_fops, nrc_debugfs_lb_count_read,
			nrc_debugfs_lb_count_write, "%llu\n");

/* Loopback sample size - simplified to just set/show size */
static int nrc_debugfs_lb_sample_show(struct seq_file *s, void *data)
{
	seq_printf(s, "Sample Configuration:\n");
	seq_printf(s, "  Size: %u bytes\n", g_lb_skb_len);
	seq_printf(s, "  Count: %u frames\n", g_lb_count);
	seq_printf(s, "  Mode: %u\n", g_lb_subtype);
	seq_printf(
		s,
		"\nTest Pattern: Sequential bytes (0x00, 0x01, ..., 0xFF, 0x00, ...)\n");
	seq_printf(s, "\nUsage:\n");
	seq_printf(s, "  Set size:  echo <bytes> > sample  (1-1600)\n");
	seq_printf(s, "  Set count: echo <count> > count   (1-65535)\n");
	seq_printf(
		s,
		"  Run test:  echo <mode> > test     (0=roundtrip, 1=TX, 2=RX)\n");
	seq_printf(s, "  View results: cat report\n");

	return 0;
}

static ssize_t nrc_debugfs_lb_sample_write(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	long ret;
	u32 new_len;

	ret = kstrtol_from_user(ubuf, count, 10, (long int *)&new_len);
	if (ret)
		return ret;

	/* Validate and set sample size */
	if (new_len == 0 || new_len > 1600) {
		ERR_HIF("[Loopback] Sample size must be 1-1600 bytes");
		return -EINVAL;
	}

	g_lb_skb_len = new_len;
	INFO("[Loopback] Sample size set to %u bytes", g_lb_skb_len);

	return count;
}

static int nrc_debugfs_lb_sample_open(struct inode *inode, struct file *file)
{
	return single_open(file, nrc_debugfs_lb_sample_show, inode->i_private);
}

static const struct file_operations lb_sample_ops = {
	.open = nrc_debugfs_lb_sample_open,
	.read = seq_read,
	.write = nrc_debugfs_lb_sample_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Loopback report - summary and timing details */
#define LB_RESET_VARS() (c = sum_tx = sum_rx = 0)
#define LB_INFO(x) ((hdev->debug->time_info_array + i)->_##x)
#define LB_INFO_(x) ((hdev->debug->time_info_array + i - 1)->_##x)
#define LB_SUB(x) (LB_INFO(x) - LB_INFO_(x))

static int nrc_debugfs_lb_report_show(struct seq_file *s, void *data)
{
	struct nrc_hif_device *hdev = s->private;
	unsigned int i, slots, tx, rx, c, rl;
	unsigned long diff;
	unsigned long long t;
	unsigned long long sum_tx, sum_rx;
	u8 *str_type[] = {"Round-trip", "TX only", "RX only"};
	u32 frame_len = g_lb_skb_len + sizeof(struct hif_lb_hdr);

	if (!hdev || !hdev->debug)
		return -EINVAL;

	if (g_lb_skb_len == 0) {
		seq_puts(s, "No test data. Run a loopback test first.\n");
		return 0;
	}

	seq_printf(s, "########## SUMMARY (%s) ##########\n",
		   str_type[g_lb_subtype]);
	slots = DIV_ROUND_UP(frame_len, TX_SLOT_SIZE);

	seq_printf(s, "1. Total frame counts: %d\n", g_lb_count);
	if (g_lb_subtype != LOOPBACK_MODE_RX_ONLY)
		seq_printf(s, "2. Frame length: %d bytes (%d slots)\n",
			   frame_len, slots);
	else
		seq_printf(s, "2. Frame length: %d bytes\n", frame_len);

	if (g_lb_subtype == LOOPBACK_MODE_ROUNDTRIP ||
	    g_lb_subtype >= LOOPBACK_MODE_MAX) {
		rl = (frame_len > RX_SLOT_SIZE) ? frame_len : RX_SLOT_SIZE;
		seq_printf(s,
			   "   => Actual tx bytes: %d, Actual rx bytes: %d\n\n",
			   slots * TX_SLOT_SIZE, rl);
		seq_printf(s, "3. Total tx bytes (HOST -> TARGET): %d bytes\n",
			   tx = g_lb_count * slots * TX_SLOT_SIZE);
		seq_printf(s, "4. Total rx bytes (TARGET -> HOST): %d bytes\n",
			   rx = g_lb_count * rl);
		seq_printf(s, "   => Total transferred bytes: %llu bytes\n\n",
			   t = (unsigned long long)(tx + rx));
		seq_printf(s, "5. First frame transmit time: %lld us\n",
			   hdev->debug->tx_time_first);
		seq_printf(s, "6. Last frame transmit time: %lld us\n",
			   hdev->debug->tx_time_last);
		seq_printf(s, "   (diff: %lld us)\n",
			   hdev->debug->tx_time_last -
				   hdev->debug->tx_time_first);
		seq_printf(s, "7. First frame received time: %lld us\n",
			   hdev->debug->rcv_time_first);
		seq_printf(s, "8. Last frame received time: %lld us\n",
			   hdev->debug->rcv_time_last);
		seq_printf(s, "   (diff: %lld us)\n",
			   hdev->debug->rcv_time_last -
				   hdev->debug->rcv_time_first);
		seq_puts(s,
			 "   --------------------------------------------\n");
		seq_printf(s, "   => First frame RTT (No.7 - No.5): %lld us\n",
			   hdev->debug->rcv_time_first -
				   hdev->debug->tx_time_first);
		seq_printf(s, "   => Last frame RTT (No.8 - No.6): %lld us\n",
			   hdev->debug->rcv_time_last -
				   hdev->debug->tx_time_last);
		seq_printf(s, "   => Time diff (No.8 - No.5): %lu us\n",
			   diff = hdev->debug->rcv_time_last -
				  hdev->debug->tx_time_first);
		t *= 7812; /* 8(bit) / 1024(kbit) * 1000000(sec) = 7812.5 */
		seq_printf(s, "   => Throughput: %llu kbps\n",
			   div_u64(t, diff));
	} else if (g_lb_subtype == LOOPBACK_MODE_TX_ONLY) {
		seq_printf(s, "   => Actual tx bytes: %d\n\n",
			   slots * TX_SLOT_SIZE);
		seq_printf(s,
			   "3. Total tx bytes (HOST -> TARGET): %lld bytes\n\n",
			   t = (g_lb_count - 1) * slots * TX_SLOT_SIZE);
		seq_printf(s, "4. First frame transmit time: %lld us\n",
			   hdev->debug->tx_time_first);
		seq_printf(s, "5. Last frame transmit time: %lld us\n",
			   hdev->debug->tx_time_last);
		seq_printf(s, "   (diff: %lld us)\n",
			   hdev->debug->tx_time_last -
				   hdev->debug->tx_time_first);
		seq_printf(s, "6. First frame arrival time (TSF): %u us\n",
			   hdev->debug->arv_time_first);
		seq_printf(s, "7. Last frame arrival time (TSF): %u us\n",
			   hdev->debug->arv_time_last);
		seq_printf(s, "   (diff: %lu us)\n",
			   diff = hdev->debug->arv_time_last -
				  hdev->debug->arv_time_first);
		seq_puts(s,
			 "   --------------------------------------------\n");
		t *= 7812;
		seq_printf(s, "   => Throughput: %llu kbps\n",
			   div_u64(t, diff));
	} else if (g_lb_subtype == LOOPBACK_MODE_RX_ONLY) {
		rl = (frame_len > RX_SLOT_SIZE) ? frame_len : RX_SLOT_SIZE;
		seq_printf(s, "   => Actual rx bytes: %d\n\n", rl);
		seq_printf(s,
			   "3. Total rx bytes (TARGET -> HOST): %lld bytes\n\n",
			   t = (g_lb_count - 1) * rl);
		seq_printf(s, "7. First frame received time: %lld us\n",
			   hdev->debug->rcv_time_first);
		seq_printf(s, "8. Last frame received time: %lld us\n",
			   hdev->debug->rcv_time_last);
		seq_printf(s, "   (diff: %lu us)\n",
			   diff = hdev->debug->rcv_time_last -
				  hdev->debug->rcv_time_first);
		seq_puts(s,
			 "   --------------------------------------------\n");
		t *= 7812;
		seq_printf(s, "   => Throughput: %llu kbps\n",
			   div_u64(t, diff));
	}

	/* Detail section */
	if (g_lb_bunch > 0 && hdev->debug->time_info_array) {
		int x = g_lb_count < 50 ? g_lb_count : 50;
		seq_puts(s, "\n########## DETAIL ##########\n");
		seq_puts(
			s,
			"[frame index] [tx time(us)] [time diff] [rx time(us)] [time diff]\n");
		for (i = 0; i < x; i++) {
			if (LB_INFO(i) == i) {
				seq_printf(
					s,
					"[%5d] \t%lld \t%lld \t%lld \t%lld\n",
					i, LB_INFO(txt),
					(i == 0) ? 0LL : LB_SUB(txt),
					LB_INFO(rxt),
					(i == 0) ? 0LL : LB_SUB(rxt));
			} else {
				seq_puts(s,
					 "[----] this frame might be lost.\n");
			}
		}
		if (g_lb_count > 50) {
			LB_RESET_VARS();
			for (i = 50; i < g_lb_count; i++) {
				if (c < g_lb_bunch) {
					sum_tx += LB_SUB(txt);
					sum_rx += LB_SUB(rxt);
					++c;
				}
				if (c == g_lb_bunch) {
					if (g_lb_bunch == 1) {
						if (LB_INFO(i) == i) {
							seq_printf(
								s,
								"[%5d] \t%lld \t%lld \t%lld \t%lld\n",
								i, LB_INFO(txt),
								LB_SUB(txt),
								LB_INFO(rxt),
								LB_SUB(rxt));
						} else {
							seq_puts(
								s,
								"[----] frame lost.\n");
						}
					} else {
						seq_printf(
							s,
							"[%d - %d] \t%llu \t%llu\n",
							i + 1 - g_lb_bunch, i,
							div_u64(sum_tx,
								g_lb_bunch),
							div_u64(sum_rx,
								g_lb_bunch));
					}
					LB_RESET_VARS();
				}
			}
			if (c != 0) {
				seq_printf(s, "[%d - %d] \t%llu \t%llu\n",
					   i - c, i, div_u64(sum_tx, c),
					   div_u64(sum_rx, c));
			}
		}
	}

	return 0;
}

static ssize_t nrc_debugfs_lb_report_write(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	long ret;

	ret = kstrtol_from_user(ubuf, count, 10, (long int *)&g_lb_bunch);
	if (ret)
		return ret;

	return count;
}

static int nrc_debugfs_lb_report_open(struct inode *inode, struct file *file)
{
	return single_open(file, nrc_debugfs_lb_report_show, inode->i_private);
}

static const struct file_operations lb_report_ops = {
	.open = nrc_debugfs_lb_report_open,
	.read = seq_read,
	.write = nrc_debugfs_lb_report_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ========================================================================
 * End of Loopback Test Functions
 * ======================================================================== */

/* PS Timing debugfs */
static int ps_timing_show(struct seq_file *m, void *v)
{
	struct nrc_hif_device *hdev = m->private;
	struct nrc_ps_timing_event *event;
	unsigned long flags;
	int i, start_idx, count;
	struct timespec64 ts;

	if (!hdev)
		return -EINVAL;

	spin_lock_irqsave(&hdev->ps.lock, flags);

	seq_printf(m, "# PS Timing History (total events: %d)\n",
		   hdev->ps.history_count);
	seq_printf(m,
		   "# Format: timestamp|type|state|mode|reason|timeout_ms\n");
	seq_printf(m, "# Type: S=Sleep, W=Wake\n\n");

	/* Calculate starting index for circular buffer */
	count = min(hdev->ps.history_count, NRC_PS_HISTORY_SIZE);
	if (hdev->ps.history_count >= NRC_PS_HISTORY_SIZE) {
		start_idx = hdev->ps.history_idx;
	} else {
		start_idx = 0;
	}

	/* Print events in chronological order */
	for (i = 0; i < count; i++) {
		int idx = (start_idx + i) % NRC_PS_HISTORY_SIZE;
		event = &hdev->ps.history[idx];

		ts = ktime_to_timespec64(event->timestamp);

		seq_printf(m, "%lld.%09ld|%c|%s|%s|%d|%llu\n",
			   (long long)ts.tv_sec, ts.tv_nsec,
			   event->is_sleep ? 'S' : 'W',
			   nrc_ps_state_str(event->state),
			   nrc_ps_mode_str(event->mode), event->reason,
			   event->timeout_ms);
	}

	spin_unlock_irqrestore(&hdev->ps.lock, flags);

	return 0;
}

static int ps_timing_open(struct inode *inode, struct file *file)
{
	return single_open(file, ps_timing_show, inode->i_private);
}

static const struct file_operations ps_timing_ops = {
	.owner = THIS_MODULE,
	.open = ps_timing_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* PS Control debugfs */
static int ps_control_show(struct seq_file *m, void *v)
{
	struct nrc_hif_device *hdev = m->private;
	unsigned long flags;

	if (!hdev)
		return -EINVAL;

	spin_lock_irqsave(&hdev->ps.lock, flags);

	seq_printf(m, "# NRC Power Save Status\n");
	seq_printf(m, "# =======================\n\n");
	seq_printf(m, "State:                %s\n",
		   nrc_ps_state_str(hdev->ps.state));
	seq_printf(m, "Mode:                 %s\n",
		   nrc_ps_mode_str(hdev->ps.mode));
	seq_printf(m, "PS Enabled:           %s\n",
		   hdev->ps.enabled ? "Yes" : "No");
	seq_printf(m, "Modem Enabled:        %s\n",
		   hdev->ps.modem_enabled ? "Yes" : "No");
	seq_printf(m, "Supports Dynamic PS:  %s\n",
		   hdev->ps.supports_dynamic_ps ? "Yes" : "No");
	seq_printf(m, "Dynamic PS Timeout:   %d ms\n", hdev->ps.timeout);
	seq_printf(m, "Last Sleep Timeout:   %llu ms\n",
		   hdev->ps.last_sleep_timeout_ms);
	seq_printf(m, "Wake Pending:         %s\n",
		   hdev->ps.wake_pending ? "Yes" : "No");
	seq_printf(m, "\n");

	/* Module parameters */
	seq_printf(m, "# Module Parameters\n");
	seq_printf(m, "# -----------------\n");
	seq_printf(m, "power_save:           %d (%s)\n",
		   hdev->params->power_save,
		   hdev->params->power_save == 0 ? "NONE" :
		   hdev->params->power_save == 1 ? "MODEMSLEEP" :
		   hdev->params->power_save == 2 ? "DEEPSLEEP_TIM" :
		   hdev->params->power_save == 3 ? "DEEPSLEEP_NONTIM" : "Unknown");
	seq_printf(m, "sleep_duration:       [%d, %d] ms\n",
		   hdev->params->sleep_duration[0],
		   hdev->params->sleep_duration[1]);
	seq_printf(m, "listen_interval:      %d\n",
		   hdev->params->listen_interval);
	seq_printf(m, "bss_max_idle:         %d\n",
		   hdev->params->bss_max_idle);
	seq_printf(m, "bss_max_idle_offset:  %d\n",
		   hdev->params->bss_max_idle_offset);
	seq_printf(m, "nullfunc_enable:      %s\n",
		   hdev->params->nullfunc_enable ? "Yes" : "No");
	seq_printf(m, "extra_ps_timeout:     %d ms\n",
		   hdev->params->extra_ps_timeout);
	seq_printf(m, "\n");

	seq_printf(m, "# Commands:\n");
	seq_printf(m, "#   echo wake [timeout_ms] > ps_control\n");
	seq_printf(m, "#   echo sleep <mode> <timeout_ms> > ps_control\n");
	seq_printf(
		m,
		"#   echo reset > ps_control  (force reset PS state to WAKE)\n");
	seq_printf(
		m,
		"#   Modes: 0=NONE, 1=MODEMSLEEP, 2=DEEPSLEEP_TIM, 3=DEEPSLEEP_NONTIM\n");

	spin_unlock_irqrestore(&hdev->ps.lock, flags);

	return 0;
}

static int ps_control_open(struct inode *inode, struct file *file)
{
	return single_open(file, ps_control_show, inode->i_private);
}

static ssize_t ps_control_write(struct file *file, const char __user *user_buf,
				size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct nrc_hif_device *hdev = m->private;
	char buf[128];
	char cmd[16];
	int mode, timeout;
	int ret;

	if (!hdev)
		return -EINVAL;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = '\0';

	/* Parse command */
	if (sscanf(buf, "%15s %d %d", cmd, &mode, &timeout) >= 1) {
		if (strcmp(cmd, "wake") == 0) {
			/* Wake command */
			if (sscanf(buf, "%15s %d", cmd, &timeout) < 2)
				timeout = 2000; /* Default timeout */

			INFO("PS Control: Wake request (timeout=%d ms)",
			     timeout);
			ret = nrc_hal_ps_request_wake(
				timeout, NRC_PS_REASON_USER_DEBUG_WAKE);
			if (ret < 0) {
				ERR_HAL("Wake request failed: %d", ret);
				return ret;
			}
		} else if (strcmp(cmd, "sleep") == 0) {
			/* Sleep command */
			if (sscanf(buf, "%15s %d %d", cmd, &mode, &timeout) <
			    3) {
				ERR_HAL("Invalid sleep command format");
				return -EINVAL;
			}

			/* Check if PS is enabled */
			if (!hdev->ps.enabled) {
				ERR_HAL("Power Save is disabled. Cannot enter sleep mode.");
				ERR_HAL("Enable PS first via WLAN configuration (e.g., iw dev wlan0 set power_save on)");
				return -EPERM;
			}

			if (mode < 0 || mode >= NRC_PS_MAX) {
				ERR_HAL("Invalid PS mode: %d", mode);
				return -EINVAL;
			}

			INFO("PS Control: Sleep request (mode=%s, timeout=%d ms)",
			     nrc_ps_mode_str(mode), timeout);
			ret = nrc_hal_ps_request_sleep(
				mode, timeout, NULL,
				NRC_PS_REASON_USER_DEBUG_SLEEP);
			if (ret < 0) {
				ERR_HAL("Sleep request failed: %d", ret);
				return ret;
			}
		} else if (strcmp(cmd, "reset") == 0) {
			/* Force reset PS state to WAKE */
			unsigned long flags;

			INFO("PS Control: Force reset to WAKE state");

			spin_lock_irqsave(&hdev->ps.lock, flags);

			/* Force state to WAKE */
			hdev->ps.state = NRC_PS_STATE_WAKE;
			hdev->ps.mode = NRC_PS_NONE;
			hdev->ps.wake_pending = false;

			/* Complete any pending wake operations */
			complete_all(&hdev->wake_done);

			spin_unlock_irqrestore(&hdev->ps.lock, flags);

			INFO("PS state forcibly reset to WAKE");
		} else {
			ERR_HAL("Unknown command: %s", cmd);
			return -EINVAL;
		}
	} else {
		ERR_HAL("Invalid command format");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations ps_control_ops = {
	.owner = THIS_MODULE,
	.open = ps_control_open,
	.read = seq_read,
	.write = ps_control_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ========================================================================
 * Firmware Control debugfs
 * ======================================================================== */

static int fw_control_show(struct seq_file *s, void *unused)
{
	struct nrc_hif_device *hdev = s->private;
	enum NRC_FW_STATE fw_state;
	enum NRC_FW_STATE hw_state;

	if (!hdev) {
		seq_printf(s, "Invalid HIF device\n");
		return -EINVAL;
	}

	/* Read NRC FW state from atomic variable */
	fw_state = atomic_read(&hdev->fw.state);

	/* Read HW state from backend (SPI) */
	hw_state = NRC_FW_STATE_ROM;
	if (hdev->hif_ops && hdev->hif_ops->fw_state)
		hw_state = hdev->hif_ops->fw_state(hdev);

	seq_printf(s, "========================================\n");
	seq_printf(s, "Firmware Status\n");
	seq_printf(s, "========================================\n");
	seq_printf(s, "NRC FW State: %s (%d)\n", nrc_fw_state_str(fw_state),
		   fw_state);
	seq_printf(s, "FW Started: %s\n",
		   NRC_FW_IS_STARTED(hdev) ? "YES" : "NO");
	seq_printf(s, "HW State: %s (%d)\n", nrc_fw_state_str(hw_state),
		   hw_state);
	seq_printf(s, "\n");
	seq_printf(s, "Available Commands:\n");
	seq_printf(s, "  status  - Show firmware status (read operation)\n");
	seq_printf(s, "  unload  - Unload firmware (future implementation)\n");

	return 0;
}

static int fw_control_open(struct inode *inode, struct file *file)
{
	return single_open(file, fw_control_show, inode->i_private);
}

static ssize_t fw_control_write(struct file *file, const char __user *user_buf,
				size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct nrc_hif_device *hdev = s->private;
	char buf[64];
	char cmd[16];

	if (!hdev)
		return -EINVAL;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = '\0';

	/* Parse command */
	if (sscanf(buf, "%15s", cmd) == 1) {
		if (strcmp(cmd, "status") == 0) {
			/* Status command - just read current status */
			enum NRC_FW_STATE fw_state =
				atomic_read(&hdev->fw.state);
			enum NRC_FW_STATE hw_state = NRC_FW_STATE_ROM;

			if (hdev->hif_ops && hdev->hif_ops->fw_state)
				hw_state = hdev->hif_ops->fw_state(hdev);

			INFO("FW Control: Status check - NRC FW State: %s, HW State: %s",
			     nrc_fw_state_str(fw_state),
			     nrc_fw_state_str(hw_state));
		} else if (strcmp(cmd, "unload") == 0) {
			/* Unload command - future implementation */
			ERR_HAL("FW Control: Unload command not yet implemented");
			return -ENOSYS;
		} else {
			ERR_HAL("FW Control: Unknown command '%s'", cmd);
			return -EINVAL;
		}
	} else {
		ERR_HAL("FW Control: Invalid command format");
		return -EINVAL;
	}

	return count;
}

static const struct file_operations fw_control_ops = {
	.owner = THIS_MODULE,
	.open = fw_control_open,
	.read = seq_read,
	.write = fw_control_write,
	.llseek = seq_lseek,
	.release = single_release,
};

#endif /* CONFIG_DEBUG_FS */

/* HAL-specific debugfs initialization */
void nrc_core_init_debugfs(struct nrc_hif_device *hdev)
{
#ifdef CONFIG_DEBUG_FS
	if (!hdev)
		return;

	/* Initialize SKB debug control (disabled by default) */
	hdev->skb_debug_enabled = false;

	/* Create debugfs root directory for core module */
	nrc_core_debugfs_root = debugfs_create_dir("nrc_core", NULL);
	if (!nrc_core_debugfs_root) {
		ERR_HAL("Failed to create nrc_core debugfs directory");
		return;
	}

	/* Create common debugfs entries (shared by all frontends) */
	debugfs_create_file("credit", 0664, nrc_core_debugfs_root, hdev,
			    &nrc_debugfs_credit_fops);
	debugfs_create_file("slot", 0664, nrc_core_debugfs_root, hdev,
			    &nrc_debugfs_slot_fops);
	debugfs_create_file("debug_mask", 0664, nrc_core_debugfs_root, hdev,
			    &nrc_debugfs_debug_fops);
	debugfs_create_file("debug_level", 0664, nrc_core_debugfs_root, hdev,
			    &nrc_debugfs_debug_level_fops);
	debugfs_create_file("restart", 0664, nrc_core_debugfs_root, hdev,
			    &nrc_debugfs_restart_device_fops);

	/* Create hdev-specific debugfs entries */
	debugfs_create_file("skb_stats", 0444, nrc_core_debugfs_root, hdev,
			    &skb_stats_ops);
	debugfs_create_bool("skb_debug", 0664, nrc_core_debugfs_root,
			    &hdev->skb_debug_enabled);

	/* Create PS timing debugfs entry */
	debugfs_create_file("ps_timing", 0444, nrc_core_debugfs_root, hdev,
			    &ps_timing_ops);

	/* Create PS control debugfs entry */
	debugfs_create_file("ps_control", 0664, nrc_core_debugfs_root, hdev,
			    &ps_control_ops);

	/* Create FW control debugfs entry */
	debugfs_create_file("fw_control", 0664, nrc_core_debugfs_root, hdev,
			    &fw_control_ops);

	/* Create loopback test directory and entries */
	loopback_debugfs_root =
		debugfs_create_dir("loopback", nrc_core_debugfs_root);
	if (loopback_debugfs_root) {
		debugfs_create_file("test", 0664, loopback_debugfs_root, hdev,
				    &nrc_debugfs_lb_test_fops);
		debugfs_create_file("count", 0664, loopback_debugfs_root, hdev,
				    &nrc_debugfs_lb_count_fops);
		debugfs_create_file("sample", 0664, loopback_debugfs_root, hdev,
				    &lb_sample_ops);
		debugfs_create_file("report", 0664, loopback_debugfs_root, hdev,
				    &lb_report_ops);
		debugfs_create_file("hexdump", 0664, loopback_debugfs_root,
				    hdev, &nrc_debugfs_lb_hexdump_fops);
	}

	INFO("Core debugfs initialized at /sys/kernel/debug/nrc_core/");
#endif
}

void nrc_core_exit_debugfs(void)
{
#ifdef CONFIG_DEBUG_FS
	/* No loopback template SKB to free - SKBs are created/freed per test */

	/* Remove debugfs directory and all entries */
	debugfs_remove_recursive(nrc_core_debugfs_root);
	nrc_core_debugfs_root = NULL;
	loopback_debugfs_root = NULL;
#endif
}

// void nrc_core_health_check(struct nrc_hif_device *hdev)
// {
// 	DBG_HIF("HAL health check for device %s",
// 		    hdev->dev ? dev_name(hdev->dev) : "unknown");
// }

static char *wim_cmd_str[] = {
	[WIM_CMD_INIT] = "INIT",
	[WIM_CMD_START] = "START",
	[WIM_CMD_STOP] = "STOP",
	[WIM_CMD_SCAN_START] = "SCAN_START",
	[WIM_CMD_SCAN_STOP] = "SCAN_STOP",
	[WIM_CMD_SET_KEY] = "SET_KEY",
	[WIM_CMD_DISABLE_KEY] = "DISABLE_KEY",
	[WIM_CMD_STA_CMD] = "STA_CMD",
	[WIM_CMD_SET] = "SET",
	[WIM_CMD_REQ_FW] = "REQ_FW",
	[WIM_CMD_FW_RELOAD] = "FW_RELOAD",
	[WIM_CMD_AMPDU_ACTION] = "AMPDU_ACTION",
	[WIM_CMD_SHELL] = "SHELL",
	[WIM_CMD_SLEEP] = "SLEEP",
	[WIM_CMD_MIC_SCAN] = "MIC_SCAN",
	[WIM_CMD_KEEP_ALIVE] = "KEEP_ALIVE",
	[WIM_CMD_SET_IE] = "SET_IE",
	[WIM_CMD_SET_SAE] = "SET_SAE",
	[WIM_CMD_SHELL_RAW] = "SHELL_RAW",
	[WIM_CMD_RESET_HIF_TX] = "RESET_HIF_TX",
	[WIM_CMD_RESET_HIF_RX] = "RESET_HIF_RX",
	[WIM_CMD_GET] = "GET",
	[WIM_CMD_GET_TX_STATS] = "GET_TX_STATS",
	[WIM_CMD_SCHED_SCAN_START] = "SCHED_SCAN_START",
	[WIM_CMD_SCHED_SCAN_STOP] = "SCHED_SCAN_STOP",
	[WIM_CMD_MCP_CHAN_ID_CONTROL_H2F] = "MCP_CHAN_ID_CONTROL_H2F",
	[WIM_CMD_MCP_CHAN_ID_CONTROL_F2H] = "MCP_CHAN_ID_CONTROL_F2H",
};

const char *nrc_wim_cmd_str(int cmd)
{
	if (cmd >= 0 && cmd < WIM_CMD_MAX && wim_cmd_str[cmd])
		return wim_cmd_str[cmd];
	return "UNKNOWN";
}

static char *wim_event_str[] = {
	[WIM_EVENT_SCAN_COMPLETED] = "SCAN_COMPLETED",
	[WIM_EVENT_READY] = "READY",
	[WIM_EVENT_CREDIT_REPORT] = "CREDIT_REPORT",
	[WIM_EVENT_PS_READY] = "PS_READY",
	[WIM_EVENT_PS_WAKEUP] = "PS_WAKEUP",
	[WIM_EVENT_KEEP_ALIVE] = "KEEP_ALIVE",
	[WIM_EVENT_REQ_DEAUTH] = "REQ_DEAUTH",
	[WIM_EVENT_CSA] = "CSA",
	[WIM_EVENT_CH_SWITCH] = "CH_SWITCH",
	[WIM_EVENT_LBT_ENABLED] = "LBT_ENABLED",
	[WIM_EVENT_LBT_DISABLED] = "LBT_DISABLED",
	[WIN_EVENT_CLEAN_TXQ_STA] = "CLEAN_TXQ_STA",
	[WIM_EVENT_SCHED_SCAN_COMPLETED] = "SCHED_SCAN_COMPLETED",
	[WIM_EVENT_MCP_NEXT_GROUP] = "MCP_NEXT_GROUP",
	[WIM_EVENT_MCP_KEEP_ALIVE] = "MCP_KEEP_ALIVE",
	[WIM_EVENT_MCP_SCHEDULE_REPORT] = "MCP_SCHEDULE_REPORT",
	[WIM_EVENT_MCP_IMMEDIATE_REPORT] = "MCP_IMMEDIATE_REPORT",
};

const char *nrc_wim_event_str(int event)
{
	if (event >= 0 && event < WIM_EVENT_MAX && wim_event_str[event])
		return wim_event_str[event];
	return "UNKNOWN";
}

const char *nrc_wim_subtype_str(u8 subtype)
{
	switch (subtype) {
	case HIF_WIM_SUB_REQUEST:
		return "REQUEST";
	case HIF_WIM_SUB_RESPONSE:
		return "RESPONSE";
	case HIF_WIM_SUB_EVENT:
		return "EVENT";
	default:
		return "UNKNOWN";
	}
}

void nrc_dump_wim(struct sk_buff *skb)
{
	struct hif *hif = (void *)skb->data;
	struct wim *wim = (void *)(hif + 1);
	u8 stype = hif->subtype;

	DBG_MAC("wim: %s, vif=%d, seqno=%d, len=%d",
		stype == HIF_WIM_SUB_REQUEST ? nrc_wim_cmd_str(wim->cmd) :
		stype == HIF_WIM_SUB_EVENT   ? nrc_wim_event_str(wim->event) :
					       "resp",
		hif->vifindex, wim->seqno, hif->len);
}

/* SKB Statistics debugfs */
static int nrc_debugfs_skb_stats_show(struct seq_file *s, void *unused)
{
	struct nrc_hif_device *hdev = s->private;
	struct nrc_skb_stats *stats;
	int i, alloc, free, leak, queued;
	int total_alloc, total_free, total_leak, total_queued, total_err;
	int rx_alloc, rx_free, rx_leak, rx_queued, rx_err;
	int tx_alloc, tx_free, tx_leak, tx_queued, tx_err;

	if (!hdev) {
		seq_printf(s, "Error: HIF device not available\n");
		return 0;
	}

	stats = &hdev->skb_stats;

	total_alloc = atomic_read(&stats->total_alloc);
	total_free = atomic_read(&stats->total_free);
	total_leak = total_alloc - total_free;
	total_queued = atomic_read(&stats->total_queued);
	total_err = atomic_read(&stats->total_err);

	rx_alloc = atomic_read(&stats->rx_alloc);
	rx_free = atomic_read(&stats->rx_free);
	rx_leak = rx_alloc - rx_free;
	rx_queued = atomic_read(&stats->rx_queued);
	rx_err = atomic_read(&stats->rx_err);

	tx_alloc = atomic_read(&stats->tx_alloc);
	tx_free = atomic_read(&stats->tx_free);
	tx_leak = tx_alloc - tx_free;
	tx_queued = atomic_read(&stats->tx_queued);
	tx_err = atomic_read(&stats->tx_err);

	seq_printf(s, "SKB Statistics (driver-managed only)\n");
	seq_printf(s, "====================================\n");
	seq_printf(s, "Total:  Alloc=%d Free=%d Leak=%d Queued=%d ErrDrop=%d\n",
		   total_alloc, total_free, total_leak, total_queued,
		   total_err);
	seq_printf(s, "RX:     Alloc=%d Free=%d Leak=%d Queued=%d ErrDrop=%d\n",
		   rx_alloc, rx_free, rx_leak, rx_queued, rx_err);
	seq_printf(s,
		   "TX:     Alloc=%d Free=%d Leak=%d Queued=%d ErrDrop=%d\n\n",
		   tx_alloc, tx_free, tx_leak, tx_queued, tx_err);

	seq_printf(s, "RX By Type:\n");
	seq_printf(s, "%-10s %8s %8s %8s %8s\n", "Type", "Alloc", "Free",
		   "Leak", "Queued");
	seq_printf(s, "----------------------------------------------\n");

	for (i = 0; i < HIF_TYPE_MAX; i++) {
		alloc = atomic_read(&stats->rx_alloc_by_type[i]);
		free = atomic_read(&stats->rx_free_by_type[i]);
		leak = alloc - free;
		queued = atomic_read(&stats->rx_queued_by_type[i]);

		if (alloc > 0 || free > 0) {
			seq_printf(s, "%-10s %8d %8d %8d %8d\n",
				   nrc_hif_type_str(i), alloc, free, leak,
				   queued);
		}
	}

	seq_printf(s, "\nTX By Type:\n");
	seq_printf(s, "%-10s %8s %8s %8s %8s\n", "Type", "Alloc", "Free",
		   "Leak", "Queued");
	seq_printf(s, "----------------------------------------------\n");

	for (i = 0; i < HIF_TYPE_MAX; i++) {
		alloc = atomic_read(&stats->tx_alloc_by_type[i]);
		free = atomic_read(&stats->tx_free_by_type[i]);
		leak = alloc - free;
		queued = atomic_read(&stats->tx_queued_by_type[i]);

		if (alloc > 0 || free > 0) {
			seq_printf(s, "%-10s %8d %8d %8d %8d\n",
				   nrc_hif_type_str(i), alloc, free, leak,
				   queued);
		}
	}

	return 0;
}

static int nrc_debugfs_skb_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, nrc_debugfs_skb_stats_show, inode->i_private);
}
