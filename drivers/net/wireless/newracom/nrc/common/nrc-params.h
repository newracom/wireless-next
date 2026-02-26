/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC Module Parameters - Centralized parameter definitions
 *
 * This header defines the nrc_params structure which holds all module
 * parameters shared between frontend modules (WLAN, MCP) and the HAL core.
 */

#ifndef _NRC_PARAMS_H_
#define _NRC_PARAMS_H_

#include <linux/types.h>

/**
 * struct nrc_params - NRC Module Parameters Structure
 *
 * This structure centralizes all module parameters that are shared
 * between different layers of the driver (frontend, HAL, backend).
 * Parameters are synchronized from individual frontend modules to this
 * structure during initialization.
 *
 * @power_save: Power save mode (0: disabled, 1: enabled)
 * @idle_mode: Idle mode enable flag
 * @sleep_duration: Deep sleep duration for non-TIM power save [start, end]
 * @ampdu_mode: AMPDU mode (0: disable, 1: manual, 2: auto)
 * @sw_enc: Software encryption enable (0: HW, 1: SW)
 * @signal_monitor: RSSI/SNR signal monitoring enable
 * @kr_band: Korea band specification (-1: auto, 1: USN1, 2: USN5)
 * @debug_level_all: Enable all debug levels
 * @discard_deauth: Discard TX deauth for Multi-STA test
 * @dbg_flow_control: Print slot and credit status
 * @bitmap_encoding: Use bitmap encoding for block ack (NRC7292)
 * @reverse_scrambler: Apply scrambler reversely (NRC7292)
 * @nrc_country_code: Two letter country code for firmware
 * @twt_num: Total TWT service number (0: disabled)
 * @twt_sp: TWT service period in usec
 * @twt_int: TWT wake interval in usec
 * @twt_service: TWT service indicator (STA only)
 * @twt_force_sleep: Force sleep at end of TWT service (STA only)
 * @twt_num_in_group: Max STA number in TWT group (AP only)
 * @twt_algo: TWT scheduling algorithm (AP only, 0: Balanced, 1: FCFS)
 * @ps_pretend: Power save pretend for non-responsive STA
 * @set_duty_cycle: Duty cycle control [on/off, window, duration]
 * @set_cca_threshold: CCA threshold value (default: -75)
 * @nullfunc_enable: Enable null function on mac80211
 * @enable_monitor: Enable monitor mode
 * @enable_short_bi: Enable short beacon interval
 * @enable_legacy_ack: Enable legacy ACK mode
 * @enable_beacon_bypass: Enable beacon bypass
 * @set_auth_control: Auth control [on/off, slot, ti_min, ti_max, scale]
 * @support_ch_width: Supported channel width (0: 1/2MHz, 1: 1/2/4MHz)
 * @bd_name: Board data file name
 * @macaddr: MAC address string
 * @fw_name: Firmware file name
 * @fw_update_name: Firmware update file name
 * @auto_fw_update: Auto firmware update enable
 * @flash_fw: Flash firmware enable (1: enable, 0: disable)
 * @dl_name: Downloader file name
 * @bl_name: Bootloader file name
 * @power_save_gpio: Power save GPIO configuration [gpio_num, on_value, off_value]
 * @ndp_preq: NDP probe request enable
 * @ndp_ack_1m: 1M NDP ACK enable
 * @wlantest: WLAN test mode enable
 * @loopback: HIF loopback mode enable
 * @lb_count: HIF loopback buffer count
 * @disable_cqm: Disable Connection Quality Monitor
 * @disable_cqm_on_scan: Disable CQM during scan
 * @listen_interval: Listen interval value
 * @bss_max_idle: BSS max idle period
 * @bss_max_idle_offset: BSS max idle offset
 * @beacon_loss_count: Beacon loss count before declaring loss
 * @ignore_listen_interval: Ignore listen interval on AP
 * @skip_idle_mode: Skip idle mode
 * @extra_ps_timeout: Extra power save timeout
 * @enable_sched_scan: Enable scheduled scan
 * @band_selection_gpio_num: Target GPIO number for band selection
 * @band_selection_gpio_polarity: Target GPIO polarity for band selection
 * @mcp_priority: Enable MCP priority over WLAN TX
 */
struct nrc_params {
	/* Power Management */
	int power_save;
	bool idle_mode;
	int sleep_duration[2];

	/* WLAN Features */
	int ampdu_mode;
	int sw_enc;
	bool signal_monitor;

	/* Regional Configuration */
	int kr_band;
	char *nrc_country_code;
	int support_ch_width;

	/* Debug Settings */
	bool debug_level_all;
	bool discard_deauth;
	bool dbg_flow_control;

	/* NRC7292 Specific */
	bool bitmap_encoding;
	bool reverse_scrambler;

	/* TWT (Target Wake Time) */
	unsigned int twt_num;
	unsigned long long twt_sp;
	unsigned long long twt_int;
	bool twt_service;
	bool twt_force_sleep;
	unsigned int twt_num_in_group;
	unsigned char twt_algo;

	/* RAW (Restricted Access Window) */
	bool raw;

	/* Advanced Configuration */
	bool ps_pretend;
	int set_duty_cycle[3];
	int set_cca_threshold;
	bool nullfunc_enable;
	bool enable_monitor;
	bool enable_short_bi;
	bool enable_legacy_ack;
	bool enable_beacon_bypass;
	int set_auth_control[5];

	/* Firmware and Board Data */
	char *bd_name;
	char *macaddr;
	char *fw_name;
	char *fw_update_name;
	bool auto_fw_update;
	int flash_fw;
	char *dl_name;
	char *bl_name;

	/* GPIO Configuration */
	int power_save_gpio[3];
	uint8_t band_selection_gpio_num;
	bool band_selection_gpio_polarity;

	/* NDP Configuration */
	bool ndp_preq;
	bool ndp_ack_1m;

	/* Test and Debug */
	bool wlantest;
	bool loopback;
	int lb_count;

	/* Connection Management */
	int disable_cqm;
	int disable_cqm_on_scan;
	int listen_interval;
	int bss_max_idle;
	int bss_max_idle_offset;
	int beacon_loss_count;
	bool ignore_listen_interval;
	bool skip_idle_mode;
	int extra_ps_timeout;
	bool enable_sched_scan;

	/* MCP Priority Control */
	bool mcp_priority;
};

/**
 * struct nrc_debug - NRC Debug Structure
 *
 * This structure holds debug-related state and timing information
 * shared across the driver layers.
 *
 * @g_nrc_beacon_updated: Global beacon updated flag
 * @lb_hexdump: Loopback hexdump control
 * @time_info_array: Loopback timing info array
 * @tx_time_first: First TX timestamp
 * @tx_time_last: Last TX timestamp
 * @rcv_time_first: First receive timestamp
 * @rcv_time_last: Last receive timestamp
 * @arv_time_first: First arrival time
 * @arv_time_last: Last arrival time
 */
struct nrc_debug {
	int g_nrc_beacon_updated;
	u32 lb_hexdump;
	struct lb_time_info *time_info_array;

	/* Loopback timing variables */
	s64 tx_time_first;
	s64 tx_time_last;
	s64 rcv_time_first;
	s64 rcv_time_last;
	u32 arv_time_first;
	u32 arv_time_last;
};

#endif /* _NRC_PARAMS_H_ */
