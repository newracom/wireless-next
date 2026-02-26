/*
 * Copyright (c) 2016-2024 Newracom, Inc.
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

#include <linux/kernel.h>
#include <linux/version.h>

#include "nrc.h"
#include "nrc-hif.h"
#include "nrc-debug.h"
#include "nrc-ps.h"
#include "nrc-mac80211.h"
#include "nrc-apf.h"
#include "nrc-hal-core-interface.h"
#include "nrc-wim-wlan.h"

//#define DEBUG_APF

static int nrc_apf_wake_target(struct nrc *nw)
{
	int ret = 0;

	if (NRC_DRV_IS_ASLEEP(nw->hdev)) {
		DBG_STATE("Wake target for APF");
		ret = nrc_ps_set_mode(nw, NRC_PS_NONE, 2000, NULL,
				      NRC_PS_REASON_DRV_APF_CONFIG);
		if (ret == -1) {
			ret = -EBUSY;
		}
	}

	return ret;
}

static int nrc_apf_sleep_target(struct nrc *nw)
{
	if (!NRC_DRV_IS_ASLEEP(nw->hdev)) {
		nrc_ps_set_idle_mode(nw, "APF");
	}

	return 0;
}

static bool nrc_apf_is_supported(struct nrc *nw)
{
	int max_len;

	max_len = nrc_apf_get_maxlen(nw);
	if (max_len <= 0) {
		ERR_WLAN("Operation not supported in current firmware");
		return false;
	}

	return true;
}

int nrc_apf_set_enable(struct nrc *nw, int enable)
{
	int ret;

	if (!nrc_apf_is_supported(nw)) {
		return -EOPNOTSUPP;
	}

	ret = nrc_wim_wlan_apf_set_enable(nw->hdev, enable);

	return ret;
}

int nrc_apf_get_enable(struct nrc *nw, int *enable)
{
	int ret;

	if (!nrc_apf_is_supported(nw)) {
		return -EOPNOTSUPP;
	}

	ret = nrc_wim_wlan_apf_get_enable(nw->hdev, enable);

	return ret;
}

int nrc_apf_get_version(struct nrc *nw)
{
	int ret;

	if (!nrc_apf_is_supported(nw)) {
		return -EOPNOTSUPP;
	}

	ret = nrc_wim_wlan_apf_get_version(nw->hdev);

	return ret;
}

int nrc_apf_get_maxlen(struct nrc *nw)
{
	int ret;

	ret = nrc_wim_wlan_apf_get_maxlen(nw->hdev);

	return ret;
}

int nrc_apf_set_packet_filter(struct nrc *nw, u8 *program, size_t len)
{
	int ret;
	int max_len;

	if (!nrc_apf_is_supported(nw)) {
		return -EOPNOTSUPP;
	}

	max_len = nrc_apf_get_maxlen(nw);

	if (len > max_len) {
		ERR_WLAN("The length value(%zu) exceeds the filter size(%d)",
			 len, max_len);
		return -EMSGSIZE;
	}

	ret = nrc_wim_wlan_apf_set_packet_filter(nw->hdev, program, len);
	if (ret)
		return ret;

	return len;
}

#define MAX_SYSBUF_SIZE 430

int nrc_apf_read_packet_filter(struct nrc *nw, u32 src_offset, u8 *host_dst,
			       u32 len)
{
	int ret = 0;
	u32 offset, remain, read;
	int max_len;

	remain = len;
	offset = 0;

	DBG_STATE("APF read (offset: %u, len: %u)", src_offset, len);

	if (!nrc_apf_is_supported(nw)) {
		return -EOPNOTSUPP;
	}

	max_len = nrc_apf_get_maxlen(nw);

	if (src_offset >= max_len) {
		ERR_WLAN("The offset value(%u) exceeds the filter size(%u)",
			 src_offset, max_len);
		return -EINVAL;
	}

	len = min_t(int, max_len - src_offset, len);
	DBG_STATE("Real read len: %u", len);
	remain = len;

	while (remain) {
		if (remain > MAX_SYSBUF_SIZE) {
			read = MAX_SYSBUF_SIZE;
		} else {
			read = remain;
		}

		ret = nrc_wim_wlan_apf_get_packet_filter(
			nw->hdev, src_offset + offset, host_dst + offset, read);
		if (ret) {
			goto done;
		}
		offset += read;
		remain -= read;
	}

done:
	return (len - remain);
}

static ssize_t nrc_apf_cap_version_read(struct file *file,
					char __user *user_buf, size_t count,
					loff_t *ppos)
{
	struct nrc *nw = file->private_data;

	char *text;
	int m, ret, text_size = 256;

	int version = 0;

	if (*ppos != 0) {
		return 0;
	}
	text = kmalloc(text_size, GFP_KERNEL);
	if (!text) {
		return -ENOMEM;
	}

	ret = nrc_apf_wake_target(nw);
	if (ret)
		return ret;

	version = nrc_apf_get_version(nw);
	if (version <= 0) {
		ret = version;
		goto done;
	}

	m = 0;

	m += scnprintf(text + m, text_size - m, "%d\n", version);

	m = min_t(int, m, text_size);

	ret = simple_read_from_buffer(user_buf, count, ppos, text, m);

done:
	if (text)
		kfree(text);
	nrc_apf_sleep_target(nw);
	return ret;
}

static ssize_t nrc_apf_cap_version_write(struct file *file,
					 const char __user *buf, size_t len,
					 loff_t *ppos)
{
	DBG_STATE("%s:%d", __FUNCTION__, __LINE__);
	return len;
}

static const struct file_operations nrc_apf_cap_version_ops = {
	.read = nrc_apf_cap_version_read,
	.write = nrc_apf_cap_version_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static ssize_t nrc_apf_cap_maxlen_read(struct file *file, char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct nrc *nw = file->private_data;

	char *text = NULL;
	int m, ret, text_size = 256;

	int maxlen = 0;

	if (*ppos != 0) {
		return 0;
	}
	text = kmalloc(text_size, GFP_KERNEL);
	if (!text) {
		ret = -ENOMEM;
		goto done;
	}

	ret = nrc_apf_wake_target(nw);
	if (ret)
		return ret;

	maxlen = nrc_apf_get_maxlen(nw);
	if (maxlen <= 0) {
		ret = -EOPNOTSUPP;
		goto done;
	}

	m = 0;

	m += scnprintf(text + m, text_size - m, "%d\n", maxlen);

	m = min_t(int, m, text_size);

	ret = simple_read_from_buffer(user_buf, count, ppos, text, m);

done:
	if (text)
		kfree(text);
	nrc_apf_sleep_target(nw);
	return ret;
}

static ssize_t nrc_apf_cap_maxlen_write(struct file *file,
					const char __user *buf, size_t len,
					loff_t *ppos)
{
	DBG_STATE("%s:%d", __FUNCTION__, __LINE__);
	return len;
}

static const struct file_operations nrc_apf_cap_maxlen_ops = {
	.read = nrc_apf_cap_maxlen_read,
	.write = nrc_apf_cap_maxlen_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static ssize_t nrc_apf_set_packet_filter_read(struct file *file,
					      char __user *user_buf,
					      size_t count, loff_t *ppos)
{
	DBG_STATE("%s:%d", __FUNCTION__, __LINE__);
	return 0;
}

static ssize_t nrc_apf_set_packet_filter_write(struct file *file,
					       const char __user *buf,
					       size_t len, loff_t *ppos)
{
	struct nrc *nw = file->private_data;

	u8 *text = NULL;
	int ret = 0;

	if (*ppos != 0) {
		return 0;
	}

	ret = nrc_apf_wake_target(nw);
	if (ret)
		return ret;

	text = kmalloc(len, GFP_KERNEL);
	if (!text) {
		ret = -ENOMEM;
		goto done;
	}

	ret = simple_write_to_buffer(text, len, ppos, buf, len);

#ifdef DEBUG_APF
	print_hex_dump(KERN_DEBUG, "prog: ", DUMP_PREFIX_OFFSET, 16, 1, text,
		       len, false);
#endif

	if (len == 1 && text[0] == '\n') { /* echo > /sys/xx */
		ret = nrc_apf_set_packet_filter(nw, text, 0);
		if (ret == 0) {
			ret = len;
		}
	} else {
		ret = nrc_apf_set_packet_filter(nw, text, len);
	}

	if (ret < 0) {
		ERR_WLAN("Failed to write packet filter");
		goto done;
	}

done:
	if (text)
		kfree(text);
	nrc_apf_sleep_target(nw);
	return ret;
}

static const struct file_operations nrc_apf_set_packet_filter_ops = {
	.read = nrc_apf_set_packet_filter_read,
	.write = nrc_apf_set_packet_filter_write,
	.open = simple_open,
	.llseek = default_llseek,
};

u32 apf_read_offset;
u32 apf_read_len;

static ssize_t nrc_apf_read_packet_filter_read(struct file *file,
					       char __user *user_buf,
					       size_t count, loff_t *ppos)
{
	struct nrc *nw = file->private_data;

	int ret;
	char *text = NULL;

	if (*ppos != 0) {
		return 0;
	}

	ret = nrc_apf_wake_target(nw);
	if (ret)
		return ret;

	text = kmalloc(apf_read_len, GFP_KERNEL);
	if (!text) {
		ret = -ENOMEM;
		goto done;
	}

	ret = nrc_apf_read_packet_filter(nw, apf_read_offset, text,
					 apf_read_len);
	if (ret < 0) {
		ERR_WLAN("Failed to read packet filter");
		goto done;
	}

#ifdef DEBUG_APF
	print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_OFFSET, 16, 1, text,
		       ret, false);
#endif

	ret = simple_read_from_buffer(user_buf, count, ppos, text, ret);

done:
	if (text)
		kfree(text);
	nrc_apf_sleep_target(nw);
	return ret;
}

static ssize_t nrc_apf_read_packet_filter_write(struct file *file,
						const char __user *user_buf,
						size_t count, loff_t *ppos)
{
	char buf[16];
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (count && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	else
		buf[count] = '\0';

	ret = sscanf(buf, "%u %u", &apf_read_offset, &apf_read_len);
	if (ret != 2) {
		ERR_WLAN("Invalid format, expected format 'offset len'");
		return -EINVAL;
	}

	DBG_STATE("APF read (offset: %u, len: %u)", apf_read_offset,
		  apf_read_len);

	return count;
}

static const struct file_operations nrc_apf_read_packet_filter_ops = {
	.read = nrc_apf_read_packet_filter_read,
	.write = nrc_apf_read_packet_filter_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static int nrc_apf_enable_read(void *data, u64 *val)
{
	struct nrc *nw = data;

	int ret;
	int enable;

	DBG_STATE("APF enable read");

	ret = nrc_apf_wake_target(nw);
	if (ret)
		return ret;

	ret = nrc_apf_get_enable(nw, &enable);
	if (ret < 0) {
		ERR_WLAN("Failed to read enable");
		goto done;
	}

	*val = enable;

done:
	nrc_apf_sleep_target(nw);
	return ret;
}

static int nrc_apf_enable_write(void *data, u64 val)
{
	struct nrc *nw = data;
	int ret;

	DBG_STATE("APF enable write (%llu)", val);

	ret = nrc_apf_wake_target(nw);
	if (ret)
		return ret;

	ret = nrc_apf_set_enable(nw, val);
	if (ret < 0) {
		ERR_WLAN("Failed to write enable");
		goto done;
	}

done:
	nrc_apf_sleep_target(nw);
	return ret;
}

DEFINE_SIMPLE_ATTRIBUTE(nrc_apf_enable_ops, nrc_apf_enable_read,
			nrc_apf_enable_write, "%llu\n");

void nrc_apf_debugfs_init(struct dentry *root, void *nw) /* (struct nrc *nw) */
{
	debugfs_create_file("version", 0600, root, nw,
			    &nrc_apf_cap_version_ops);
	debugfs_create_file("maxlen", 0600, root, nw, &nrc_apf_cap_maxlen_ops);
	debugfs_create_file("set_packet_filter", 0600, root, nw,
			    &nrc_apf_set_packet_filter_ops);
	debugfs_create_file("read_packet_filter", 0600, root, nw,
			    &nrc_apf_read_packet_filter_ops);
	debugfs_create_file("enable", 0600, root, nw, &nrc_apf_enable_ops);
}
