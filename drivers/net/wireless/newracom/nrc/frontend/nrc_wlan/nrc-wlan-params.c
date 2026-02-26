/*
 * Copyright (c) 2016-2019 Newracom, Inc.
 *
 * NRC WLAN Module Parameters
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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Common directory headers - Interfaces */
#include "nrc-vendor.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Local module headers */
#include "nrc-twt-sched.h"
#include "nrc-wlan-params.h"

int flash_fw = 0;
module_param(flash_fw, int, 0600);
MODULE_PARM_DESC(
	flash_fw,
	"Fusing Boot loader and Firmware to sflash (1: enable, 0: disable)");

char *dl_name;
module_param(dl_name, charp, 0444);
MODULE_PARM_DESC(dl_name, "Downloader file name");

char *bl_name;
module_param(bl_name, charp, 0444);
MODULE_PARM_DESC(bl_name, "Boot Loader file name");

/* Firmware file name */
char *fw_name = NULL;
module_param(fw_name, charp, 0444);
MODULE_PARM_DESC(fw_name, "Firmware file name");

char *fw_update_name;
module_param(fw_update_name, charp, 0444);
MODULE_PARM_DESC(fw_update, "Firmware file name to update");

bool auto_fw_update = false;
module_param(auto_fw_update, bool, 0600);
MODULE_PARM_DESC(auto_fw_update, "Enable Beacon Bypass");

/* Board Data file name */
#if defined(CONFIG_SUPPORT_BD)
char *bd_name = "bd.dat";
module_param(bd_name, charp, 0600);
MODULE_PARM_DESC(bd_name, "Board Data file name");
#endif /* defined(CONFIG_SUPPORT_BD) */

/* MAC Address */
char *macaddr = NULL;
module_param(macaddr, charp, 0);
MODULE_PARM_DESC(macaddr, "MAC Address");

/* ===========================================================================
 * Power Management Parameters
 * =========================================================================== */

/**
 * enable/disable the power save mode by default
 */
int power_save = 0;
module_param(power_save, int, 0600);
MODULE_PARM_DESC(power_save, "power save");

/**
 * enable/disable the idle mode by default
 */
bool idle_mode = false;
module_param(idle_mode, bool, 0600);
MODULE_PARM_DESC(idle_mode, "idle mode");

/**
 * deepsleep duration of non-TIM mode power save
 */
int sleep_duration[2] = {
	0,
};
module_param_array(sleep_duration, int, NULL, 0600);
MODULE_PARM_DESC(sleep_duration,
		 "deepsleep duration of non-TIM mode power save");

/**
 * Maximum beacon loss count
 */
int beacon_loss_count = 20;
module_param(beacon_loss_count, int, 0600);
MODULE_PARM_DESC(beacon_loss_count,
		 "Number of beacon intervals before we decide beacon was lost");

/**
 * Ignore listen interval value while comparing it with bss max idle on AP
 */
bool ignore_listen_interval = false;
module_param(ignore_listen_interval, bool, 0600);
MODULE_PARM_DESC(
	ignore_listen_interval,
	"Ignore listen interval value while comparing it with bss max idle on AP");

/* ===========================================================================
 * Connection Management Parameters
 * =========================================================================== */

/* Disable CQM (Connection Quality Monitor) */
int disable_cqm = 0;
module_param(disable_cqm, int, 0600);
MODULE_PARM_DESC(disable_cqm, "Disable CQM (0: enable, 1: disable)");

/* Disable CQM on scan */
int disable_cqm_on_scan = 0;
module_param(disable_cqm_on_scan, int, 0600);
MODULE_PARM_DESC(disable_cqm_on_scan,
		 "Disable CQM on scan (0: enable, 1: disable)");

bool enable_sched_scan = false;
module_param(enable_sched_scan, bool, 0600);
MODULE_PARM_DESC(enable_sched_scan,
		 "Enable sched scan (0: disable, 1: enable)");

/* Listen interval */
int listen_interval = 100;
module_param(listen_interval, int, 0600);
MODULE_PARM_DESC(listen_interval, "Listen Interval");

/* BSS Max Idle */
int bss_max_idle = 0;
module_param(bss_max_idle, int, 0600);
MODULE_PARM_DESC(bss_max_idle, "BSS Max Idle");

/* BSS Max Idle Offset */
int bss_max_idle_offset = 0;
module_param(bss_max_idle_offset, int, 0600);
MODULE_PARM_DESC(bss_max_idle_offset, "BSS Max Idle Offset");

/* ===========================================================================
 * WLAN Feature Parameters
 * =========================================================================== */

/* Enable monitor mode */
bool enable_monitor = false;
module_param(enable_monitor, bool, 0600);
MODULE_PARM_DESC(enable_monitor, "Enable Monitor");

/* Enable short beacon interval */
bool enable_short_bi = false;
module_param(enable_short_bi, bool, 0600);
MODULE_PARM_DESC(enable_short_bi, "Enable Short Beacon Interval");

/* Enable legacy ACK mode */
bool enable_legacy_ack = false;
module_param(enable_legacy_ack, bool, 0600);
MODULE_PARM_DESC(enable_legacy_ack, "Enable Legacy ACK mode");

/* Enable beacon bypass */
bool enable_beacon_bypass = false;
module_param(enable_beacon_bypass, bool, 0600);
MODULE_PARM_DESC(enable_beacon_bypass, "Enable Beacon Bypass");

/* Software encryption */
int sw_enc = 0;
module_param(sw_enc, int, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(sw_enc, "Use SW Encryption instead of HW Encryption");

/* AMPDU mode */
int ampdu_mode = 2; /* NRC_AMPDU_AUTO */
module_param(ampdu_mode, int, 0600);
MODULE_PARM_DESC(ampdu_mode, "Set AMPDU mode");

/* Null function enable */
bool nullfunc_enable = false;
module_param(nullfunc_enable, bool, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(nullfunc_enable, "Enable null func on mac80211");

/* ===========================================================================
 * Regional and RF Parameters
 * =========================================================================== */

/* Country code */
char *nrc_country_code = "!";
module_param(nrc_country_code, charp, 0444);
MODULE_PARM_DESC(nrc_country_code, "Two letter fw country code");

/* KR band specification */
int kr_band = -1;
module_param(kr_band, int, 0600);
MODULE_PARM_DESC(kr_band, "Specify KR band (KR USN1(1) or KR USN5(2))");

/* Supported channel width */
int support_ch_width = 1;
module_param(support_ch_width, int, 0600);
MODULE_PARM_DESC(support_ch_width,
		 "Supported CH width (0:1/2MHz Support, 1:1/2/4Mhz Support");

/* CCA threshold */
int set_cca_threshold = -75;
module_param(set_cca_threshold, int, 0600);
MODULE_PARM_DESC(set_cca_threshold, "Set cca threshold (default: -75)");

/* ===========================================================================
 * Advanced WLAN Features
 * =========================================================================== */

/* Signal monitor - initialized from params, synchronized on runtime changes */
bool signal_monitor = false;
module_param(signal_monitor, bool, 0600);
MODULE_PARM_DESC(signal_monitor, "Enable SIGNAL(RSSI/SNR) Monitor");

/* NDP Probe Request - now managed in nrc_params structure */
static bool ndp_preq = false;
module_param_named(ndp_preq, ndp_preq, bool, 0600);
MODULE_PARM_DESC(ndp_preq, "Enable NDP Probe Request");

/* 1M NDP ACK */
bool ndp_ack_1m = false;
module_param(ndp_ack_1m, bool, 0600);
MODULE_PARM_DESC(ndp_ack_1m, "Enable 1M NDP ACK");

/* ===========================================================================
 * TWT (Target Wake Time) Parameters
 * =========================================================================== */

/* TWT service number */
unsigned int twt_num = 0;
module_param(twt_num, uint, 0600);
MODULE_PARM_DESC(twt_num, "Total TWT service number (default: 0, disabled)");

/* TWT service period */
unsigned long long twt_sp = 0;
module_param(twt_sp, ullong, 0600);
MODULE_PARM_DESC(twt_sp, "TWT service period (default: 0, usec)");

/* TWT wake interval */
unsigned long long twt_int = 0;
module_param(twt_int, ullong, 0600);
MODULE_PARM_DESC(twt_int, "TWT Wake Interval (default: 0, usec)");

/* TWT service indicator */
bool twt_service = false;
module_param(twt_service, bool, S_IRUSR);
MODULE_PARM_DESC(twt_service, "Indicate TWT service on (only STA)");

/* TWT force sleep */
bool twt_force_sleep = false;
module_param(twt_force_sleep, bool, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(twt_force_sleep,
		 "Force sleep at the end of service (only STA)");

/* TWT max STA number in group */
unsigned int twt_num_in_group = 1;
module_param(twt_num_in_group, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(twt_num_in_group, "TWT Max STA Number in a Group (only AP)");

/* TWT scheduling algorithm */
unsigned char twt_algo = 0;
module_param(twt_algo, byte, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(twt_algo,
		 "TWT scheduling algorithm (only AP, 0:Balanced, 1:FCFS)");

/* RAW enable (AP only) */
bool raw = 0;
module_param(raw, bool, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(raw, "RAW Enable (AP only)");

/* ===========================================================================
 * Test and Debug Parameters
 * =========================================================================== */

/* WLAN test mode */
bool wlantest = false;
module_param(wlantest, bool, 0600);
MODULE_PARM_DESC(wlantest, "wlantest");

/* Debug level all */
bool debug_level_all = false;
module_param(debug_level_all, bool, 0600);
MODULE_PARM_DESC(debug_level_all, "Driver debug level all");

/* Debug level: 0=ERR, 1=WARN, 2=INFO, 3=DBG */
int debug_level = DEFAULT_NRC_DBG_LEVEL;
module_param(debug_level, int, 0600);
MODULE_PARM_DESC(debug_level, "Debug level (0=ERR, 1=WARN, 2=INFO, 3=DBG)");

/* Debug mask: bitmask for categories */
unsigned long debug_mask = DEFAULT_NRC_DBG_MASK;
module_param(debug_mask, ulong, 0600);
MODULE_PARM_DESC(debug_mask, "Debug category mask (BASIC=0x1, HIF=0x2, WIM=0x4, TX=0x8, RX=0x10, MAC=0x20, CAPI=0x40, PS=0x80, STATS=0x100, STATE=0x200, BD=0x400, FW=0x800, AMPDU=0x1000, CREDIT=0x2000, SLOT=0x4000, BUS=0x8000, ALL=0xFFFFFFFF)");

/* Discard deauth (test only) */
bool discard_deauth = false;
module_param(discard_deauth, bool, 0600);
MODULE_PARM_DESC(discard_deauth,
		 "(Test only) discard TX deauth for Multi-STA test");

/* Debug flow control */
bool dbg_flow_control = false;
module_param(dbg_flow_control, bool, 0600);
MODULE_PARM_DESC(dbg_flow_control, "Print slot and credit status");

/* ===========================================================================
 * NRC7292 Specific Parameters
 * =========================================================================== */

/* Bitmap encoding (NRC7292 only) */
bool bitmap_encoding = true;
module_param(bitmap_encoding, bool, 0600);
MODULE_PARM_DESC(bitmap_encoding,
		 "(NRC7292 only) Use bitmap encoding for block ack");

/* Reverse scrambler (NRC7292 only) */
bool reverse_scrambler = true;
module_param(reverse_scrambler, bool, 0600);
MODULE_PARM_DESC(reverse_scrambler, "(NRC7292 only) Apply scrambler reversely");

/* ===========================================================================
 * Advanced Configuration Arrays
 * =========================================================================== */

/* Authentication control */
int set_auth_control[5] = {0, 0, 0, 0, 0};
module_param_array(set_auth_control, int, NULL, 0600);
MODULE_PARM_DESC(set_auth_control,
		 "Set auth control (0|1(off,on), slot, ti_min, ti_max, scale");

/* Power save pretend */
bool ps_pretend = false;
module_param(ps_pretend, bool, 0600);
MODULE_PARM_DESC(ps_pretend, "Power save pretend for no response STA");

/* Duty cycle control */
int set_duty_cycle[3] = {0, 0, 0};
module_param_array(set_duty_cycle, int, NULL, 0600);
MODULE_PARM_DESC(set_duty_cycle,
		 "Set duty cycle (0|1(off,on), window, duration");

/* ===========================================================================
 * HAL Parameters (moved from HAL module)
 * =========================================================================== */

/* HIF loopback */
bool loopback = false;
module_param(loopback, bool, 0600);
MODULE_PARM_DESC(loopback, "HIF loopback");

/* HIF loopback Buffer count */
int lb_count = 1;
module_param(lb_count, int, 0600);
MODULE_PARM_DESC(lb_count, "HIF loopback Buffer count");

/* ===========================================================================
 * gpio for band selection (default Target_output(GP17))
 * =========================================================================== */

uint8_t band_selection_gpio_num = -1;
module_param(band_selection_gpio_num, byte, 0600);
MODULE_PARM_DESC(band_selection_gpio_num,
		 "target gpio number for band selection");

bool band_selection_gpio_polarity = 0;
module_param(band_selection_gpio_polarity, bool, 0600);
MODULE_PARM_DESC(band_selection_gpio_polarity,
		 "target gpio polarity for band selection");

/* ===========================================================================
 * Parameter Synchronization Functions
 * =========================================================================== */

/**
 * nrc_wlan_sync_params - Synchronize WLAN parameters to nrc structure
 * @nw: NRC device structure
 *
 * This function copies WLAN module parameters to the nrc params structure
 * for organized access by all layers.
 */
void nrc_wlan_sync_params(struct nrc *nw)
{
	struct nrc_params *params = nw->params;

	if (!nw || !nw->params) {
		ERR_WLAN("Invalid nrc structure for parameter synchronization");
		return;
	}

	params->power_save = power_save;
	params->idle_mode = idle_mode;
	params->ampdu_mode = ampdu_mode;
	params->sw_enc = sw_enc;
	params->signal_monitor = signal_monitor;
	params->kr_band = kr_band;
	params->debug_level_all = debug_level_all;
	params->discard_deauth = discard_deauth;
	params->dbg_flow_control = dbg_flow_control;
	params->bitmap_encoding = bitmap_encoding;
	params->reverse_scrambler = reverse_scrambler;
	params->nrc_country_code = nrc_country_code;
	params->twt_num = twt_num;
	params->twt_sp = twt_sp;
	params->twt_int = twt_int;
	params->twt_service = twt_service;
	params->twt_force_sleep = twt_force_sleep;
	params->twt_num_in_group = twt_num_in_group;
	params->twt_algo = twt_algo;
	params->raw = raw;
	params->ps_pretend = ps_pretend;
	params->set_cca_threshold = set_cca_threshold;
	params->nullfunc_enable = nullfunc_enable;
	params->enable_monitor = enable_monitor;
	params->enable_short_bi = enable_short_bi;
	params->enable_legacy_ack = enable_legacy_ack;
	params->enable_beacon_bypass = enable_beacon_bypass;
	params->support_ch_width = support_ch_width;

	/* Set bd_name only if not already set by first frontend */
	if (!params->bd_name) {
		if (bd_name) {
			params->bd_name = kstrdup(bd_name, GFP_KERNEL);
			if (!params->bd_name) {
				ERR_WLAN("Failed to allocate memory for bd_name");
				return;
			}
		}
	} else if (bd_name && strcmp(bd_name, params->bd_name) != 0) {
		WARN_WLAN("WLAN bd_name ('%s') differs from first frontend ('%s') - using first frontend's BD",
			  bd_name, params->bd_name);
	}

	/* Set macaddr only if not already set by first frontend */
	if (!params->macaddr) {
		if (macaddr) {
			params->macaddr = kstrdup(macaddr, GFP_KERNEL);
			if (!params->macaddr) {
				ERR_WLAN("Failed to allocate memory for macaddr");
				/* Clean up bd_name if macaddr allocation fails */
				if (params->bd_name) {
					kfree(params->bd_name);
					params->bd_name = NULL;
				}
				return;
			}
		}
	}

	/* Set fw_name only if not already set by first frontend */
	if (!params->fw_name) {
		if (fw_name) {
			params->fw_name = kstrdup(fw_name, GFP_KERNEL);
			if (!params->fw_name) {
				ERR_WLAN("Failed to allocate memory for fw_name");
				/* Clean up previously allocated strings */
				if (params->macaddr) {
					kfree(params->macaddr);
					params->macaddr = NULL;
				}
				if (params->bd_name) {
					kfree(params->bd_name);
					params->bd_name = NULL;
				}
				return;
			}
		}
	} else if (fw_name && strcmp(fw_name, params->fw_name) != 0) {
		WARN_WLAN("WLAN fw_name ('%s') differs from first frontend ('%s') - using first frontend's FW",
			  fw_name, params->fw_name);
	}

	/* Allocate memory for fw_update_name */
	if (fw_update_name) {
		params->fw_update_name = kstrdup(fw_update_name, GFP_KERNEL);
		if (!params->fw_update_name) {
			ERR_WLAN("Failed to allocate memory for fw_update_name");
			goto cleanup_strings;
		}
	}

	params->auto_fw_update = auto_fw_update;
	params->flash_fw = flash_fw;

	/* Allocate memory for dl_name */
	if (dl_name) {
		params->dl_name = kstrdup(dl_name, GFP_KERNEL);
		if (!params->dl_name) {
			ERR_WLAN("Failed to allocate memory for dl_name");
			goto cleanup_strings;
		}
	}

	/* Allocate memory for bl_name */
	if (bl_name) {
		params->bl_name = kstrdup(bl_name, GFP_KERNEL);
		if (!params->bl_name) {
			ERR_WLAN("Failed to allocate memory for bl_name");
			goto cleanup_strings;
		}
	}
	params->ndp_preq = ndp_preq;
	params->ndp_ack_1m = ndp_ack_1m;
	params->wlantest = wlantest;
	params->loopback = loopback;
	params->lb_count = lb_count;
	params->disable_cqm = disable_cqm;
	params->disable_cqm_on_scan = disable_cqm_on_scan;
	params->listen_interval = listen_interval;
	params->bss_max_idle = bss_max_idle;
	params->bss_max_idle_offset = bss_max_idle_offset;
	params->beacon_loss_count = beacon_loss_count;
	params->ignore_listen_interval = ignore_listen_interval;
	params->enable_sched_scan = enable_sched_scan;

	memcpy(params->sleep_duration, sleep_duration, sizeof(sleep_duration));
	memcpy(params->set_duty_cycle, set_duty_cycle, sizeof(set_duty_cycle));
	memcpy(params->set_auth_control, set_auth_control,
	       sizeof(set_auth_control));
	/* power_save_gpio is synchronized by backend module via nrc_backend_set_hal_core_refs() */

	/* Band Selection GPIO parameters */
	params->band_selection_gpio_num = band_selection_gpio_num;
	params->band_selection_gpio_polarity = band_selection_gpio_polarity;

	/* Initialize TWT scheduler if parameters are set */
	if (params->twt_sp + params->twt_num + params->twt_int != 0) {
		if (params->twt_num_in_group == 0) {
			params->twt_num_in_group = 1;
		}
		nw->twt_sched = nrc_twt_sched_init(
			nw, params->twt_sp, params->twt_num, params->twt_int,
			params->twt_num_in_group, params->twt_algo);
		//nw->twt_requester = false;
		//nw->twt_responder = true;
		// INFO("TWT scheduler initialized (sp=%llu, num=%u, int=%llu)",
		// 	params->twt_sp, params->twt_num, params->twt_int);
	} else {
		nw->twt_sched = NULL;
		//nw->twt_requester = false;
		//nw->twt_responder = false;
		/* TWT was already set to NULL in HAL */
		INFO("TWT is disabled");
	}

	/* Override with insmod parameters */
	if (params->listen_interval) {
		nw->hw->max_listen_interval = params->listen_interval;
	}
	return;

cleanup_strings:
	/* Clean up all allocated strings on error */
	if (params->bl_name) {
		kfree(params->bl_name);
		params->bl_name = NULL;
	}
	if (params->dl_name) {
		kfree(params->dl_name);
		params->dl_name = NULL;
	}
	if (params->fw_update_name) {
		kfree(params->fw_update_name);
		params->fw_update_name = NULL;
	}
	if (params->fw_name) {
		kfree(params->fw_name);
		params->fw_name = NULL;
	}
	if (params->macaddr) {
		kfree(params->macaddr);
		params->macaddr = NULL;
	}
	if (params->bd_name) {
		kfree(params->bd_name);
		params->bd_name = NULL;
	}
}
