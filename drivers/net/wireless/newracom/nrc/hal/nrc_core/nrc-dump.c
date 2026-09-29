// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 */

/* Linux kernel headers */
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/syscalls.h>

/* Assembly headers */
#include <asm/uaccess.h>

/* Common directory headers - Core */
#include "nrc.h"

/* Local module headers */
#include "nrc-dump.h"
#include "nrc-debug.h"

static void write_file(char *filename, char *data, int len)
{
	struct file *filp;
	filp = filp_open(filename, O_CREAT|O_RDWR, 0606);
	if (IS_ERR(filp)) {
		ERR("error:%d", IS_ERR(filp));
		return;
	}
	kernel_write(filp, data, len, &filp->f_pos);

	filp_close(filp, NULL);
}

static int cnt;
void nrc_dump_store(char *src, int len)
{
	char str[32];

	if (!src) {
		ERR_HAL("Invalid src pointer");
		return;
	}

	scnprintf(str, sizeof(str), "./host_core_dump_%d.bin", cnt);
	write_file(str, src, len);
	cnt++;
}
