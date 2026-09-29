/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef _NRC_S1G_H_
#define _NRC_S1G_H_

#include "nrc.h"
#include "nrc-country.h"
#include "nrc-wim-types.h"
#include <linux/types.h>

/* Supported proxy↔S1G channel list (owned by Frontend / S1G layer) */
#define NRC_BD_MAX_CH_LIST 70

struct bd_supp_param {
	uint8_t num_ch;
	uint8_t s1g_ch_index[NRC_BD_MAX_CH_LIST];
	uint16_t nons1g_ch_freq[NRC_BD_MAX_CH_LIST]; /* proxy freq MHz */
	uint16_t s1g_ch_freq[NRC_BD_MAX_CH_LIST];    /* S1G freq ×10 */
};

/*
 * Always-available proxy map and channel list functions.
 * These update the driver-internal S1G country tables and must be called
 * on every regulatory domain change regardless of CONFIG_S1G_CHANNEL.
 */
enum nrc_country_id nrc_get_current_ccid_by_country(const char *country_code);
char *nrc_get_current_s1g_country(void);
void nrc_set_s1g_country(char *country_code);
void nrc_s1g_build_supp_ch_list(void);
const struct bd_supp_param *nrc_s1g_get_supp_ch_list(void);
bool nrc_s1g_is_proxy_freq_supported(u32 proxy_mhz);

#if defined(CONFIG_S1G_CHANNEL)
#define FREQ_TO_100KHZ(mhz, khz) (mhz * 10 + khz / 100)

#define MAX_S1G_CHANNEL_NUM 70 /* Max Num of S1G Channels */
#define BW_1M 0
#define BW_2M 1
#define BW_4M 2

int nrc_get_num_channels_by_current_country(void);
int nrc_get_s1g_freq_by_arr_idx(int arr_index);
int nrc_get_s1g_width_by_freq(int freq);
int nrc_get_s1g_width_by_arr_idx(int arr_index);
uint8_t nrc_get_channel_idx_by_freq(int freq);
uint8_t nrc_get_cca_by_freq(int freq);
uint8_t nrc_get_oper_class_by_freq(int freq);
uint8_t nrc_get_offset_by_freq(int freq);
uint8_t nrc_get_pri_loc_by_freq(int freq);
void nrc_s1g_set_channel_bw(int freq, struct cfg80211_chan_def *chandef);
const struct s1g_channel_table *nrc_get_current_s1g_cc_table(void);
/* BW-aware lookups for Op35/Op36 (same S1G freq, different BW) */
int nrc_nl80211_width_to_s1g_bw(enum nl80211_chan_width width);
uint8_t nrc_get_channel_idx_by_freq_bw(int freq, int bw);
uint8_t nrc_get_cca_by_freq_bw(int freq, int bw);
uint8_t nrc_get_oper_class_by_freq_bw(int freq, int bw);
uint8_t nrc_get_offset_by_freq_bw(int freq, int bw);
uint8_t nrc_get_pri_loc_by_freq_bw(int freq, int bw);
#endif /* CONFIG_S1G_CHANNEL */
#endif
