/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2016-2024 Newracom, Inc.
 *
 * NRC Build Configuration - build-time feature switches
 *
 * The driver targets Linux 6.8 only; there is no kernel-version
 * compatibility layer.
 */

#ifndef _NRC_BUILD_CONFIG_H_
#define _NRC_BUILD_CONFIG_H_

#define NRC_SW_ID 1

#define NRC_BUILD_USE_HWSCAN
/* #define CONFIG_NRC_HIF_PRINT_BEACON */
/* #define CONFIG_NRC_HIF_PRINT_RX_AUTH */
/* #define CONFIG_NRC_HIF_PRINT_TX_MGMT */
/* #define CONFIG_NRC_HIF_DUMP_S1G_RXINFO */

/*#define NRC_TEST_SUPPRESS_STA_KEEEP_ALIVE*/

/*#define CONFIG_SPI_HALF_DUPLEX*/

/*
 * README This is a temporary feature.
 * Use only 11n certification
 */
/* #define CONFIG_TRX_BACKOFF */

/*
 * To change the transmission order of txq in the nrc driver.
 */
#define CONFIG_TXQ_ORDER_CHANGE_NRC_DRV

/*
 * To change the non-QoS data frame to QoS data frame on the nrc driver.
 */
#define CONFIG_CONVERT_NON_QOSDATA

/*
 * README This is a temporary feature.
 * Use S1G channel on host
 */
/* #define CONFIG_S1G_CHANNEL */

#ifdef CONFIG_S1G_CHANNEL
/* #define S1G_INCLUDE_4M_OP_2M_TX */
#endif

/*
 * To improve mesh routing at low RSSI.
 */
#define CONFIG_SUPPORT_MESH_ROUTING

/* Cipher suites handled by software encryption */
#define CONFIG_SUPPORT_CCMP_256
#define CONFIG_SUPPORT_GCMP /* GCMP and GCMP-256 */
#define CONFIG_SUPPORT_GMAC /* GMAC and GMAC-256 */

/* Driver features */
#define CONFIG_USE_MONITOR_VIF
#define CONFIG_SUPPORT_PS
#define CONFIG_SUPPORT_P2P
#define CONFIG_SUPPORT_BD
#define CONFIG_SUPPORT_LEGACY_ACK
#define CONFIG_SUPPORT_BEACON_BYPASS
#define CONFIG_SUPPORT_AUTH_CONTROL

//#define CONFIG_SUPPORT_IBSS

/* uncomment define below to set tx power via iw or iwconfig */
/* #define CONFIG_SUPPORT_IW_IWCONFIG_TXPWR */

/* Describe the SPI device in the device tree (nrc-cspi@0). */
#define CONFIG_SPI_USE_DT

/* Without DT the driver creates the SPI device itself from the spi_bus_num
 * and spi_cs_num module parameters, using a driver-local replacement for the
 * removed spi_busnum_to_master().
 */
#if !defined(CONFIG_SPI_USE_DT)
#define CONFIG_SPI_USE_FUNC
#endif

/*
this feature is disabled
#define CONFIG_USE_KERNEL_S1G_TWT
// To use this feature, some S1G capabilities patch is needed,
*/

/* Software-level recovery: error counting, time window, auto-restart */
#define CONFIG_SUPPORT_RECOVERY

#define CONFIG_QOS_NULL_OFFLOAD

/* If this configuration is enabled,
   the function to wake up the target will be delayed
   from the start of sleep up to TARGET_MAX_TIME_TO_FALL_ASLEEP. */
#define CONFIG_DELAY_WAKE_TARGET

/* When CONFIG_FW_LOAD_ONCE is defined,
 * the firmware update function will only execute once during the driver's runtime.
 * Subsequent calls to the function will be ignored.
 */
//#define CONFIG_FW_LOAD_ONCE

/* When CONFIG_BD_LOAD_ONCE is defined,
 * the BD update function will only execute once during the driver's runtime.
 * Subsequent calls to the function will be ignored.
 */
//#define CONFIG_BD_LOAD_ONCE

#endif
