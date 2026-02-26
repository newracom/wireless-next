/*
 * Copyright (c) 2016-2019 Newracom, Inc.
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
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* Common directory headers - Core */
#include "nrc.h"
#include "nrc-hif.h"

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"
#include "nrc-hal-core-interface.h"
#include "nrc-backend-hif-interface.h"

/* Local module headers */
#include "nrc-fw.h"
#include "nrc-vendor.h"
#include "wim.h"
#include "hif.h"

#define BOOT_START_ADDR (0x10480000)
#define DL_START_ADDR (0x10400000)
#define BL_START_ADDR (0x11000000)
#define FW_FLASH_ADDR (0x11010000)
#define FW_START_ADDR (0x10400000)
#define PACKET_START "NRC["
#define PACKET_END "]MSG"

static const unsigned int crctab32[] = {
	0x00000000U, 0x77073096U, 0xee0e612cU, 0x990951baU, 0x076dc419U,
	0x706af48fU, 0xe963a535U, 0x9e6495a3U, 0x0edb8832U, 0x79dcb8a4U,
	0xe0d5e91eU, 0x97d2d988U, 0x09b64c2bU, 0x7eb17cbdU, 0xe7b82d07U,
	0x90bf1d91U, 0x1db71064U, 0x6ab020f2U, 0xf3b97148U, 0x84be41deU,
	0x1adad47dU, 0x6ddde4ebU, 0xf4d4b551U, 0x83d385c7U, 0x136c9856U,
	0x646ba8c0U, 0xfd62f97aU, 0x8a65c9ecU, 0x14015c4fU, 0x63066cd9U,
	0xfa0f3d63U, 0x8d080df5U, 0x3b6e20c8U, 0x4c69105eU, 0xd56041e4U,
	0xa2677172U, 0x3c03e4d1U, 0x4b04d447U, 0xd20d85fdU, 0xa50ab56bU,
	0x35b5a8faU, 0x42b2986cU, 0xdbbbc9d6U, 0xacbcf940U, 0x32d86ce3U,
	0x45df5c75U, 0xdcd60dcfU, 0xabd13d59U, 0x26d930acU, 0x51de003aU,
	0xc8d75180U, 0xbfd06116U, 0x21b4f4b5U, 0x56b3c423U, 0xcfba9599U,
	0xb8bda50fU, 0x2802b89eU, 0x5f058808U, 0xc60cd9b2U, 0xb10be924U,
	0x2f6f7c87U, 0x58684c11U, 0xc1611dabU, 0xb6662d3dU, 0x76dc4190U,
	0x01db7106U, 0x98d220bcU, 0xefd5102aU, 0x71b18589U, 0x06b6b51fU,
	0x9fbfe4a5U, 0xe8b8d433U, 0x7807c9a2U, 0x0f00f934U, 0x9609a88eU,
	0xe10e9818U, 0x7f6a0dbbU, 0x086d3d2dU, 0x91646c97U, 0xe6635c01U,
	0x6b6b51f4U, 0x1c6c6162U, 0x856530d8U, 0xf262004eU, 0x6c0695edU,
	0x1b01a57bU, 0x8208f4c1U, 0xf50fc457U, 0x65b0d9c6U, 0x12b7e950U,
	0x8bbeb8eaU, 0xfcb9887cU, 0x62dd1ddfU, 0x15da2d49U, 0x8cd37cf3U,
	0xfbd44c65U, 0x4db26158U, 0x3ab551ceU, 0xa3bc0074U, 0xd4bb30e2U,
	0x4adfa541U, 0x3dd895d7U, 0xa4d1c46dU, 0xd3d6f4fbU, 0x4369e96aU,
	0x346ed9fcU, 0xad678846U, 0xda60b8d0U, 0x44042d73U, 0x33031de5U,
	0xaa0a4c5fU, 0xdd0d7cc9U, 0x5005713cU, 0x270241aaU, 0xbe0b1010U,
	0xc90c2086U, 0x5768b525U, 0x206f85b3U, 0xb966d409U, 0xce61e49fU,
	0x5edef90eU, 0x29d9c998U, 0xb0d09822U, 0xc7d7a8b4U, 0x59b33d17U,
	0x2eb40d81U, 0xb7bd5c3bU, 0xc0ba6cadU, 0xedb88320U, 0x9abfb3b6U,
	0x03b6e20cU, 0x74b1d29aU, 0xead54739U, 0x9dd277afU, 0x04db2615U,
	0x73dc1683U, 0xe3630b12U, 0x94643b84U, 0x0d6d6a3eU, 0x7a6a5aa8U,
	0xe40ecf0bU, 0x9309ff9dU, 0x0a00ae27U, 0x7d079eb1U, 0xf00f9344U,
	0x8708a3d2U, 0x1e01f268U, 0x6906c2feU, 0xf762575dU, 0x806567cbU,
	0x196c3671U, 0x6e6b06e7U, 0xfed41b76U, 0x89d32be0U, 0x10da7a5aU,
	0x67dd4accU, 0xf9b9df6fU, 0x8ebeeff9U, 0x17b7be43U, 0x60b08ed5U,
	0xd6d6a3e8U, 0xa1d1937eU, 0x38d8c2c4U, 0x4fdff252U, 0xd1bb67f1U,
	0xa6bc5767U, 0x3fb506ddU, 0x48b2364bU, 0xd80d2bdaU, 0xaf0a1b4cU,
	0x36034af6U, 0x41047a60U, 0xdf60efc3U, 0xa867df55U, 0x316e8eefU,
	0x4669be79U, 0xcb61b38cU, 0xbc66831aU, 0x256fd2a0U, 0x5268e236U,
	0xcc0c7795U, 0xbb0b4703U, 0x220216b9U, 0x5505262fU, 0xc5ba3bbeU,
	0xb2bd0b28U, 0x2bb45a92U, 0x5cb36a04U, 0xc2d7ffa7U, 0xb5d0cf31U,
	0x2cd99e8bU, 0x5bdeae1dU, 0x9b64c2b0U, 0xec63f226U, 0x756aa39cU,
	0x026d930aU, 0x9c0906a9U, 0xeb0e363fU, 0x72076785U, 0x05005713U,
	0x95bf4a82U, 0xe2b87a14U, 0x7bb12baeU, 0x0cb61b38U, 0x92d28e9bU,
	0xe5d5be0dU, 0x7cdcefb7U, 0x0bdbdf21U, 0x86d3d2d4U, 0xf1d4e242U,
	0x68ddb3f8U, 0x1fda836eU, 0x81be16cdU, 0xf6b9265bU, 0x6fb077e1U,
	0x18b74777U, 0x88085ae6U, 0xff0f6a70U, 0x66063bcaU, 0x11010b5cU,
	0x8f659effU, 0xf862ae69U, 0x616bffd3U, 0x166ccf45U, 0xa00ae278U,
	0xd70dd2eeU, 0x4e048354U, 0x3903b3c2U, 0xa7672661U, 0xd06016f7U,
	0x4969474dU, 0x3e6e77dbU, 0xaed16a4aU, 0xd9d65adcU, 0x40df0b66U,
	0x37d83bf0U, 0xa9bcae53U, 0xdebb9ec5U, 0x47b2cf7fU, 0x30b5ffe9U,
	0xbdbdf21cU, 0xcabac28aU, 0x53b39330U, 0x24b4a3a6U, 0xbad03605U,
	0xcdd70693U, 0x54de5729U, 0x23d967bfU, 0xb3667a2eU, 0xc4614ab8U,
	0x5d681b02U, 0x2a6f2b94U, 0xb40bbe37U, 0xc30c8ea1U, 0x5a05df1bU,
	0x2d02ef8dU};

static uint32_t nrc_fw_crc32(uint8_t *data, uint32_t length)
{
	size_t i;
	uint32_t crc;

	crc = 0xFFFFFFFF;
	for (i = 0; i < length; i++) {
		crc = crctab32[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	}

	return (~crc);
}

static unsigned int checksum(unsigned char *buf, int size)
{
	int sum = 0;
	int i;

	for (i = 0; i < size; i++)
		sum += buf[i];
	return sum;
}

/* Forward declarations */
static int fw_wait_ready(struct nrc_hif_device *hdev, int wait_ms,
			 int retry_cnt);

static void fw_send_info(struct nrc_hif_device *hdev, const u8 *data,
			 size_t size)
{
	struct sk_buff *skb;
	struct wim_fw_info_param *p;
	int skb_len;
	struct hif *hif;

	int sum;

	sum = nrc_fw_crc32((unsigned char *)data, size);

	skb = nrc_wim_alloc_skb(WIM_CMD_REQ_FW,
				tlv_len(sizeof(struct wim_fw_info_param)));
	if (!skb) {
		ERR_FW("Failed to allocate WIM SKB for fw info");
		return;
	}

	p = nrc_wim_skb_add_tlv(skb, WIM_TLV_FOTA_INFO,
				sizeof(struct wim_fw_info_param), NULL);
	memset(p, 0, sizeof(struct wim_fw_info_param));

	p->length = size;
	p->crc = sum;

	skb_len = skb->len;

	/* Prepend HIF header - headroom already reserved by nrc_wim_alloc_skb */
	hif = (struct hif *)skb_push(skb, sizeof(struct hif));
	memset(hif, 0, sizeof(*hif));
	hif->type = HIF_TYPE_ND_WIM;
	hif->subtype = HIF_WIM_SUB_REQUEST;
	hif->len = skb_len;
	hif->vifindex = 0;

	INFO("Send FW Info (size:%zu(0x%zX), crc:0x%08X)", size, size, sum);

	nrc_hif_ops_write(skb->data, TX_SLOT_SIZE);

	/* Track and free SKB after successful transmission */
	NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_ND_WIM, false, false);
}

#define FW_GET_RETRY 1
#define FW_GET_DELAY 10

static int fw_get_info(struct nrc_hif_device *hdev, uint32_t *version,
		       uint32_t *length, uint32_t *crc)
{
	struct sk_buff *skb;
	struct wim_fw_info_param *p;
	int skb_len;
	struct hif *hif;

	/* rx */
	u8 buf[RX_SLOT_SIZE];
	struct wim *wim;
	struct wim_tlv *tlv;
	int ret = -1;

	int i = 0;

	skb = nrc_wim_alloc_skb(WIM_CMD_REQ_FW,
				tlv_len(sizeof(struct wim_fw_info_param)));
	if (!skb) {
		ERR_FW("Failed to allocate WIM SKB for fw get info");
		return -ENOMEM;
	}

	p = nrc_wim_skb_add_tlv(skb, WIM_TLV_FW_GET_INFO,
				sizeof(struct wim_fw_info_param), NULL);
	memset(p, 0, sizeof(struct wim_fw_info_param));

	skb_len = skb->len;

	/* Prepend HIF header - headroom already reserved by nrc_wim_alloc_skb */
	hif = (struct hif *)skb_push(skb, sizeof(struct hif));
	memset(hif, 0, sizeof(*hif));
	hif->type = HIF_TYPE_ND_WIM;
	hif->subtype = HIF_WIM_SUB_REQUEST;
	hif->len = skb_len;
	hif->vifindex = 0;

	nrc_hif_ops_write(skb->data, TX_SLOT_SIZE);
	nrc_hif_set_slot_index(hdev, TX_SLOT, SLOT_TAIL, 1);

	while (i < FW_GET_RETRY) {
		msleep(FW_GET_DELAY);
		nrc_hif_inc_slot_index(hdev, RX_SLOT, SLOT_TAIL);
		nrc_hif_ops_read(buf, RX_SLOT_SIZE);

		hif = (struct hif *)buf;
		if (hif->subtype == HIF_WIM_SUB_RESPONSE) {
			wim = (void *)(hif + 1);
			if (wim->cmd == WIM_CMD_REQ_FW) {
				tlv = (struct wim_tlv *)(wim + 1);
				if (tlv->t == WIM_TLV_FW_GET_INFO) {
					memcpy(p, &tlv->v, tlv->l);
					*version = p->version;
					*length = p->length;
					*crc = p->crc;
					ret = 0;
					break;
				}
			}
		}
		ERR_FW("retry to get fw info(%d)", ++i);
	}

	/* Track and free SKB after transmission */
	NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_ND_WIM, false, false);

	return ret;
}

static int fw_check_done(struct nrc_hif_device *hdev)
{
	struct hif *hif;
	struct wim *wim;

	u8 buf[RX_SLOT_SIZE];
	int retry = 20;

	while (retry--) {
		msleep(100);
		nrc_hif_ops_read(buf, RX_SLOT_SIZE);
		hif = (struct hif *)buf;
		if (hif->subtype == HIF_WIM_SUB_RESPONSE) {
			wim = (void *)(hif + 1);
			if (wim->cmd == WIM_CMD_REQ_FW) {
				INFO("FW update is done");
				return 0;
			}
		}
	}

	ERR_FW("Update is timeout");
	return -1;
}

/*
 * Firmware download packet format
 *
 * | eof (4B) | address (4B) | len (4B) | payload (1KB - 16) | checksum (4B) |
 *
 * Last packet with "eof" being 1 may have padding bytes in payload
 * but checksum will not include padding bytes
 */

static void fw_update_frag(struct nrc_fw_priv *priv, struct fw_frag *frag,
			   bool to_xip)
{
	struct fw_frag_hdr *frag_hdr = &priv->frag_hdr;

	if (priv->cur_chunk ==
	    0) { /* not yet called nrc_fw_check_next_frag, update first frag here */
		frag_hdr->address = priv->start_addr;
		frag_hdr->eof = 0;

		if (to_xip) {
			frag_hdr->len = XIP_FRAG_BYTES;
		} else {
			frag_hdr->len = ROM_FRAG_BYTES;
		}
	}

	frag->hdr.eof = frag_hdr->eof;
	frag->hdr.address = frag_hdr->address;
	frag->hdr.len = frag_hdr->len;

	if (to_xip) {
		memcpy(frag->xip.payload, priv->fw_data_pos, frag_hdr->len);
	} else {
		memcpy(frag->rom.payload, priv->fw_data_pos, frag_hdr->len);
	}

	if (to_xip) {
		frag->xip.checksum = 0;
	} else {
		frag->rom.checksum = 0;
	}

	if (priv->csum) {
		if (to_xip) {
			frag->xip.checksum =
				checksum(frag->xip.payload, frag_hdr->len);
		} else {
			frag->rom.checksum =
				checksum(frag->rom.payload, frag_hdr->len);
		}
	}
}

static void fw_send_frag(struct nrc_hif_device *hdev, struct nrc_fw_priv *priv,
			 bool to_xip)
{
	struct sk_buff *skb;
	int skb_len;
	struct hif *hif;
	int frag_tlv_len;
	int sof_size;

	if (to_xip) {
		sof_size = 0;
		frag_tlv_len = XIP_CHUNK_SIZE;
	} else {
		sof_size = 4;
		frag_tlv_len = ROM_CHUNK_SIZE;
	}

	skb = nrc_wim_alloc_skb(WIM_CMD_REQ_FW,
				tlv_len(sizeof(struct fw_frag)));
	if (!skb) {
		ERR_FW("Failed to allocate WIM SKB for firmware fragment");
		return;
	}

	fw_update_frag(priv,
		       nrc_wim_skb_add_tlv(skb, WIM_TLV_FIRMWARE, frag_tlv_len,
					   NULL),
		       to_xip);
	skb_len = skb->len;

	/* Prepend HIF header - headroom already reserved by nrc_wim_alloc_skb */
	hif = (struct hif *)skb_push(skb, sizeof(struct hif));
	memset(hif, 0, sizeof(*hif));
	if (to_xip) {
		hif->type = HIF_TYPE_ND_WIM;
	} else {
		hif->type = HIF_TYPE_WIM;
	}
	hif->subtype = HIF_WIM_SUB_REQUEST;
	hif->len = skb_len;
	hif->vifindex = 0;

	/* Prepend prefix and append postfix for ROM mode */
	if (!to_xip) {
		/* Check if we have enough tailroom for PACKET_END */
		if (skb_tailroom(skb) < sof_size) {
			ERR_FW("Insufficient tailroom for firmware fragment");
			/* Free SKB on error */
			NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_WIM, false,
					   false);
			return;
		}
		memcpy(skb_push(skb, sof_size), PACKET_START, sof_size);
		memcpy(skb_put(skb, sof_size), PACKET_END, sof_size);
	}

	if (to_xip) {
		nrc_hif_ops_write(skb->data, TX_SLOT_SIZE);
	} else {
		nrc_hif_ops_write(skb->data, skb->len);
	}

	/* Track and free SKB after successful transmission */
	NRC_SKB_TRACK_FREE(hdev, skb, HIF_TYPE_ND_WIM, false, false);
}

static bool fw_check_next_frag(struct nrc_hif_device *hdev,
			       struct nrc_fw_priv *priv, bool to_xip)
{
	struct fw_frag_hdr *frag_hdr = &priv->frag_hdr;
	u8 index;
	int ret;

	if (priv->cur_chunk == (priv->num_chunks - 1)) {
		return false;
	}

	if (priv->ack) {
		ret = nrc_hif_ops_wait_rxq_slot(&index, 1);
		if (ret != 0) {
			ERR_FW("Failed to wait ack");
			return true; /* dont' update next info */
		}
	}

	/* success, update info for next */

	/* update source fw info */
	priv->cur_chunk++;
	priv->fw_data_pos += priv->frag_hdr.len;
	priv->remain_bytes -= priv->frag_hdr.len;

	index = priv->index;

	/* update target frag info here, not in nrc_fw_update_frag */
	frag_hdr->address += frag_hdr->len;
	frag_hdr->eof = (priv->cur_chunk == (priv->num_chunks - 1));

	if (to_xip) {
		frag_hdr->len = min_t(u32, XIP_FRAG_BYTES, priv->remain_bytes);
	} else {
		frag_hdr->len = min_t(u32, ROM_FRAG_BYTES, priv->remain_bytes);
	}

	BUG_ON(index != priv->index);
	priv->index++;

	return true;
}

/**
 * nrc_fw_download_with_protection - Download firmware with infinite loop protection
 * @hdev: HIF device
 * @priv: Firmware private data
 * @to_xip: XIP mode flag
 *
 * Returns: true if download completed successfully, false otherwise
 */
static bool fw_download_with_protection(struct nrc_hif_device *hdev,
					struct nrc_fw_priv *priv, bool to_xip)
{
	/* Infinite loop protection */
	u32 max_iterations =
		priv->num_chunks * 100; /* Allow 100x retries per chunk */
	u32 iteration_count = 0;
	u32 prev_chunk = priv->cur_chunk;
	u32 stuck_count = 0;
	const u32 max_stuck_retries = 50; /* Max retries for same chunk */

	do {
		fw_send_frag(hdev, priv, to_xip);
		iteration_count++;

		/* Check for infinite loop: same chunk stuck */
		if (priv->cur_chunk == prev_chunk) {
			stuck_count++;
			if (stuck_count >= max_stuck_retries) {
				ERR_FW("[FW] ERROR: Stuck at chunk %d/%d after %d retries (ACK timeout?)",
				       priv->cur_chunk, priv->num_chunks,
				       stuck_count);
				return false;
			}
		} else {
			/* Progress made, reset stuck counter */
			prev_chunk = priv->cur_chunk;
			stuck_count = 0;
		}

		/* Check for infinite loop: too many total iterations */
		if (iteration_count >= max_iterations) {
			ERR_FW("[FW] ERROR: Infinite loop detected! iterations=%d, chunk=%d/%d",
			       iteration_count, priv->cur_chunk,
			       priv->num_chunks);
			return false;
		}
	} while (fw_check_next_frag(hdev, priv, to_xip));

	/* Check if download completed successfully */
	if (priv->cur_chunk == (priv->num_chunks - 1)) {
		DBG_FW("download complete: %d chunks", priv->num_chunks);
		return true;
	} else {
		ERR_FW("download FAILED at chunk %d/%d", priv->cur_chunk,
		       priv->num_chunks);
		return false;
	}
}

/**
 * nrc_fw_alloc - allocate firmware private structure
 *
 * Returns: pointer to allocated structure, or NULL on failure
 */
struct nrc_fw_priv *nrc_fw_alloc(void)
{
	struct nrc_fw_priv *priv;

	priv = kzalloc(sizeof(struct nrc_fw_priv), GFP_KERNEL);
	if (!priv) {
		ERR_FW("Failed to allocate nrc_fw_priv");
		return NULL;
	}

	return priv;
}

/**
 * nrc_fw_cleanup - free firmware private structure
 * @priv: firmware private structure to free
 */
void nrc_fw_cleanup(struct nrc_fw_priv *priv)
{
	kfree(priv);
}

/**
 * fw_download - download firmware binary to target device (internal)
 * @hdev: HIF device structure
 * @to_xip: true for XIP update, false for normal RAM/flash download
 * @start_address: target memory start address
 * @auto_verify: true to enable built-in verification (credit reset + FW ready check)
 *
 * Downloads firmware binary and optionally verifies FW is running.
 * When auto_verify=true, automatically sets FW state to LOADING/ACTIVE/FAILED.
 * This is an internal function.
 */
static void fw_download(struct nrc_hif_device *hdev, bool to_xip,
			uint32_t start_address, bool auto_verify)
{
	struct firmware *fw;
	struct nrc_fw_priv *priv;
	size_t frag_size;
	int ret;

	/* Safety checks for module unload race condition */
	if (!hdev || !hdev->fw.fw || !hdev->fw.priv) {
		ERR_FW("Cannot download FW: hdev=%p fw=%p priv=%p (module unloading?)",
		       hdev, hdev ? hdev->fw.fw : NULL,
		       hdev ? hdev->fw.priv : NULL);
		return;
	}

	/* Set FW state to LOADING when auto_verify is enabled */
	if (auto_verify && !to_xip) {
		atomic_set(&hdev->fw.state, NRC_FW_LOADING);
	}

	fw = hdev->fw.fw;
	priv = hdev->fw.priv;

	if (to_xip) {
		frag_size = XIP_FRAG_BYTES;
	} else {
		frag_size = ROM_FRAG_BYTES;
	}

	priv->num_chunks = DIV_ROUND_UP(fw->size, frag_size);
	priv->csum = true;
	priv->fw = fw;
	priv->fw_data_pos = fw->data;
	priv->remain_bytes = fw->size;
	priv->cur_chunk = 0;
	priv->index = 0;
	priv->index_fb = 0;
	priv->start_addr = start_address;
	priv->ack = true;

	DBG_FW("download: %s, chunks=%d, size=%zd, auto_verify=%d",
	       to_xip ? hdev->params->fw_update_name : hdev->params->fw_name,
	       priv->num_chunks, fw->size, auto_verify);

	if (to_xip) {
		fw_send_info(hdev, fw->data, fw->size);
	}

	//if (!to_xip) // test for skip
	fw_download_with_protection(hdev, priv, to_xip);

	/* Built-in verification: reset credit and check FW ready */
	if (auto_verify && !to_xip) {
		nrc_hif_reset_slot_credit();

		ret = fw_wait_ready(hdev, 10, 300);
		if (ret != 0) {
			ERR_FW("FW download completed but verification failed");
			atomic_set(&hdev->fw.state, NRC_FW_FAILED);
		} else {
			atomic_set(&hdev->fw.state, NRC_FW_ACTIVE);
			DBG_FW("FW download and verification completed successfully");
		}
	}
}

static bool fw_check_file(struct nrc_hif_device *hdev, char *name)
{
	int status;

	if (name == NULL)
		goto err_fw;

	if (hdev->fw.fw)
		return true;

	status = request_firmware((const struct firmware **)&hdev->fw.fw, name,
				  hdev->dev);
	if (status != 0) {
		ERR_FW("request_firmware(%s) failed: %d", name, status);
		goto err_fw;
	}

	DBG_FW("loaded: %s (%zu bytes)", name, hdev->fw.fw->size);
	return true;

err_fw:
	hdev->fw.fw = NULL;
	return false;
}

static bool fw_xip_need_update(struct nrc_hif_device *hdev)
{
	int ret;
	uint32_t version, length, crc;
	uint32_t update_length, update_crc;

	ret = fw_get_info(hdev, &version, &length, &crc);
	if (ret != 0) {
		return false;
	}

	INFO("Current FW Info (version:%u.%u.%u, length:%d, crc:0x%x)",
	     (version >> 24) & 0xFF, (version >> 16) & 0xFF,
	     (version >> 8) & 0xFF, length, crc);

	update_length = hdev->fw.fw->size;
	update_crc =
		nrc_fw_crc32((unsigned char *)hdev->fw.fw->data, update_length);

	INFO("Update FW Info (length:%d, crc:0x%x)", update_length, update_crc);

	if (hdev->params->auto_fw_update == false) {
		INFO("auto_fw_update disabled, ignore update");
		return false;
	}

	if (length != update_length || crc != update_crc) {
		INFO("The FWs are different");
		return true;
	}

	return false;
}

#define MAX_FW_UPDATE_TRY 3

static bool fw_xip_fusing(struct nrc_hif_device *hdev)
{
	bool ret = false;
	int try_num = 0;
	int wait_ret;

	if (hdev->params->fw_name != NULL) {
		/* No need to fuse when using RAM download mode */
		goto done;
	}

	ret = fw_check_file(hdev, hdev->params->fw_update_name);
	if (ret == false) {
		goto done;
	}

	ret = fw_xip_need_update(hdev);
	if (ret == false) {
		goto release;
	}

try:
	INFO("Start XIP FW fusing...(%d)", ++try_num);

	fw_download(hdev, true, 0, false); /* XIP fusing - no auto_verify */

	ret = fw_check_done(hdev);
	if (ret == 0) {
		/* Reset device to boot from XIP flash */
		nrc_hif_ops_reset_device();

		/* Wait for FW to boot from XIP flash */
		wait_ret = fw_wait_ready(hdev, 1000, 20); /* 20sec */
		if (wait_ret != 0) {
			ERR_FW("FW not ready after XIP fusing reset");
			ret = false;
			goto release;
		}
		ret = true;
		goto release;
	}

	if (try_num < MAX_FW_UPDATE_TRY) {
		goto try;
	}

release:
	if (hdev->fw.fw)
		release_firmware(hdev->fw.fw);
	hdev->fw.fw = NULL;
done:
	return ret;
}

static int fw_wait_ready(struct nrc_hif_device *hdev, int wait_ms,
			 int retry_cnt)
{
	int i;

	BUG_ON(!hdev);

	for (i = 0; i < retry_cnt; i++) {
		if (nrc_hif_ops_fw_is_loaded()) {
			return 0;
		}
		msleep(wait_ms);
	}
	ERR_FW("FW ready check failed after %d retries", retry_cnt);
	return -ETIMEDOUT;
}

/**
 * fw_download_to_ram - Download firmware to RAM
 * @hdev: HIF device structure
 *
 * Internal function for RAM firmware download.
 * Used by both initial boot and PS wake scenarios.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int fw_download_to_ram(struct nrc_hif_device *hdev)
{
	int ret = -1;
	unsigned long start_jiffies = jiffies;
	int current_state;

	/* Safety check for module unload */
	if (!hdev || !hdev->params) {
		ERR_FW("Invalid hdev or params during FW load (module unloading?)");
		return -EINVAL;
	}

	/* Check if FW is already being loaded to prevent duplicate download */
	current_state = atomic_read(&hdev->fw.state);
	if (current_state == NRC_FW_LOADING) {
		WARN_FW("FW download already in progress, rejecting duplicate request");
		return -EBUSY;
	}

	if (fw_check_file(hdev, hdev->params->fw_name)) {
		/* auto_verify=true: built-in FW state management, credit reset, and ready check */
		fw_download(hdev, false, FW_START_ADDR, true);
#if !defined(CONFIG_FW_LOAD_ONCE)
		if (hdev->fw.fw)
			release_firmware(hdev->fw.fw);
		hdev->fw.fw = NULL;
#endif
		DBG_FW("FW download done (wait=%lums)",
		       jiffies_to_msecs(jiffies - start_jiffies));
		ret = 0;
	} else {
		ERR_FW("FW download failed, setting FAILED state");
		atomic_set(&hdev->fw.state, NRC_FW_FAILED);
		ret = -EINVAL;
	}

	return ret;
}

/**
 * nrc_fw_load - Load firmware (initial boot)
 * @hdev: HIF device structure
 *
 * Main entry point for firmware loading during driver initialization.
 * Performs RAM download and optional XIP fusing.
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_fw_load(struct nrc_hif_device *hdev)
{
	int ret;

	INFO("Loading FW...");

	/* Load firmware to RAM */
	ret = fw_download_to_ram(hdev);
	if (ret != 0) {
		return ret;
	}

	/* Check if XIP firmware fusing is needed (includes wait after reset) */
	fw_xip_fusing(hdev);

	return 0;
}

/**
 * nrc_fw_reload - Reload firmware (PS wake)
 * @hdev: HIF device structure
 *
 * Lightweight firmware reload for power save wake scenarios.
 * Only performs RAM download, no XIP fusing.
 *
 * Returns: 0 on success, negative error code on failure
 */
int nrc_fw_reload(struct nrc_hif_device *hdev)
{
	return fw_download_to_ram(hdev);
}

int nrc_fw_fusing(struct nrc_hif_device *hdev)
{
	int ret;

	/* Download DL (Download Loader) */
	ret = fw_check_file(hdev, hdev->params->dl_name);
	if (ret == true) {
		/* No auto_verify - use custom timeout for DL */
		fw_download(hdev, false, DL_START_ADDR, false);
		if (hdev->fw.fw)
			release_firmware(hdev->fw.fw);
		hdev->fw.fw = NULL;

		/* Reset credit before checking FW ready */
		nrc_hif_reset_slot_credit();
	}

	/* DL requires longer timeout (30 sec vs 3 sec for normal FW) */
	ret = fw_wait_ready(hdev, 1000, 30);
	if (ret != 0) {
		return ret;
	}

	/* Download FW to flash */
	ret = fw_check_file(hdev, hdev->params->fw_name);
	if (ret == true) {
		/* No auto_verify - flash write doesn't need FW ready check */
		fw_download(hdev, false, FW_FLASH_ADDR, false);
		if (hdev->fw.fw)
			release_firmware(hdev->fw.fw);
		hdev->fw.fw = NULL;
		mdelay(1000);
	}

	/* Download bootloader */
	ret = fw_check_file(hdev, hdev->params->bl_name);
	if (ret == true) {
		/* No auto_verify - bootloader write doesn't need FW ready check */
		fw_download(hdev, false, BL_START_ADDR, false);
		if (hdev->fw.fw)
			release_firmware(hdev->fw.fw);
		hdev->fw.fw = NULL;
	}

	return ret;
}

static bool fw_has_macaddr_param(struct nrc_hif_device *hdev, uint8_t *dev_mac)
{
	int res;

	// if (macaddr[0] == ':')
	if (!hdev->params->macaddr ||
	    (unsigned long)hdev->params->macaddr < PAGE_SIZE ||
	    hdev->params->macaddr[0] == ':')
		return false;

	// res = sscanf(macaddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
	res = sscanf(hdev->params->macaddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		     &dev_mac[0], &dev_mac[1], &dev_mac[2], &dev_mac[3],
		     &dev_mac[4], &dev_mac[5]);

	return (res == 6);
}

/****************************************************************************
 * FunctionName : nrc_set_macaddr_from_fw
 * Description : This function set MAC Addresses from Serial Flash.
 *	Case1: macaddr0, macaddr1 are both written in Serial Flash
 *	VIF0 = macaddr0 / VIF1 = macaddr1
 *
 *	Case2: only macaddr0 is written in Serial Flash
 *	VIF0 = macaddr0 / VIF1 = macaddr0 with private bit
 *
 *	Case3: only macaddr1 is written in Serial Flash
 *	VIF0 = macaddr1 with private bit / VIF1 = macaddr1
 *
 *	Case4: macaddr0,macaddr1 are both not written in Serial Flash
 *	VIF0/VIF1 = generated macaddr by Host(RP)
 *
 * Returns : NONE
 *****************************************************************************/
static void fw_set_macaddr(struct nrc_hif_device *hdev, struct wim_ready *ready)
{
	int i;

	for (i = 0; i < NR_NRC_VIF; i++)
		hdev->has_macaddr[i] = true;

	if (ready->v.has_macaddr[0] && ready->v.has_macaddr[1]) {
		for (i = 0; i < NR_NRC_VIF; i++) {
			memcpy(&hdev->mac_addr[i].addr[0],
			       &ready->v.macaddr[i][0], ETH_ALEN);
		}
	} else if (ready->v.has_macaddr[0] && !ready->v.has_macaddr[1]) {
		memcpy(&hdev->mac_addr[0].addr[0], &ready->v.macaddr[0][0],
		       ETH_ALEN);
		if (!(hdev->mac_addr[0].addr[0] & 0x2)) {
			memcpy(&hdev->mac_addr[1].addr[0],
			       &ready->v.macaddr[0][0], ETH_ALEN);
			hdev->mac_addr[1].addr[0] |= 0x2;
		} else {
			hdev->has_macaddr[1] = false;
		}
	} else if (!ready->v.has_macaddr[0] && ready->v.has_macaddr[1]) {
		memcpy(&hdev->mac_addr[1].addr[0], &ready->v.macaddr[1][0],
		       ETH_ALEN);
		if (!(hdev->mac_addr[1].addr[0] & 0x2)) {
			memcpy(&hdev->mac_addr[0].addr[0],
			       &ready->v.macaddr[1][0], ETH_ALEN);
			hdev->mac_addr[0].addr[0] |= 0x2;
		} else {
			hdev->has_macaddr[0] = false;
		}
	} else {
		for (i = 0; i < NR_NRC_VIF; i++)
			hdev->has_macaddr[i] = false;
	}
}

static void fw_on_ready(struct sk_buff *skb)
{
	struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
	struct wim_ready *ready;
	struct wim *wim = (struct wim *)skb->data;
	int i;

	ready = (struct wim_ready *)(wim + 1);

	/* Store FW info */
	hdev->fw.info.ready = NRC_FW_ACTIVE;
	hdev->fw.info.version = ready->v.version;
	hdev->fw.info.rx_head_size = ready->v.rx_head_size;
	hdev->fw.info.tx_head_size = ready->v.tx_head_size;
	hdev->fw.info.payload_align = ready->v.payload_align;
	hdev->fw.info.buffer_size = ready->v.buffer_size;
	hdev->fw.info.hw_version = ready->v.hw_version;

	INFO("FW hw_version=%d, version=0x%08X, buffer_size=%d",
	     hdev->fw.info.hw_version, hdev->fw.info.version,
	     hdev->fw.info.buffer_size);

	if (hdev->chip_id == 0x7394) {
		hdev->fw.info.chip_rev_num =
			(ready->v.chip_rev_num > 0) ? ready->v.chip_rev_num : 0;
	}

	/* Store capabilities */
	hdev->cap.cap_mask = ready->v.cap.cap;
	hdev->cap.listen_interval = ready->v.cap.listen_interval;
	hdev->cap.bss_max_idle = ready->v.cap.bss_max_idle;
	hdev->cap.max_vif = ready->v.cap.max_vif;

	/* Log FW ready with key info */
	DBG_FW("ready: ver=0x%08X, hw_ver=%d, cap=0x%x", ready->v.version,
	       ready->v.hw_version, ready->v.cap.cap);

	/* Setup MAC addresses */
	if (fw_has_macaddr_param(hdev, hdev->mac_addr[0].addr)) {
		hdev->has_macaddr[0] = true;
		hdev->has_macaddr[1] = true;
		memcpy(hdev->mac_addr[1].addr, hdev->mac_addr[0].addr,
		       ETH_ALEN);
		hdev->mac_addr[1].addr[1]++;
		hdev->mac_addr[1].addr[5]++;
	} else {
		fw_set_macaddr(hdev, ready);
	}

	/* Setup VIF capabilities */
	for (i = 0; i < hdev->cap.max_vif; i++) {
		hdev->cap.vif_caps[i].cap_mask = ready->v.cap.vif_caps[i].cap;
		if (hdev->params->sw_enc == WIM_ENCDEC_HYBRID) {
			hdev->cap.vif_caps[i].cap_mask |=
				WIM_SYSTEM_CAP_HYBRIDSEC;
		} else if (hdev->params->sw_enc == WIM_ENCDEC_SW) {
			hdev->cap.vif_caps[i].cap_mask &= ~WIM_SYSTEM_CAP_HWSEC;
		}
	}

	if (hdev->chip_id == 0x7394) {
		hdev->fw.use_ext_lna = (ready->v.lna_offset < -10) ? 1 : 0;
	}

	/* Override with insmod parameters */
	if (hdev->params->listen_interval) {
		hdev->cap.listen_interval = hdev->params->listen_interval;
	}

	if (nrc_mac_is_s1g(hdev)) {
		/* bss_max_idle: in unit of 1000 TUs (1024ms = 1.024 seconds) */
		if (hdev->params->bss_max_idle >
			    S1G_UNSCALED_INTERVAL_MAX * 10000 ||
		    hdev->params->bss_max_idle <= 0) {
			hdev->cap.bss_max_idle = 0;
		} else {
			hdev->cap.bss_max_idle = hdev->params->bss_max_idle;
		}
	} else {
		if (hdev->params->bss_max_idle > __UINT16_MAX__ ||
		    hdev->params->bss_max_idle <= 0) {
			hdev->cap.bss_max_idle = 0;
		} else {
			hdev->cap.bss_max_idle = hdev->params->bss_max_idle;
		}
	}

	/* Track WIM ready response SKB free with parsed cmd/event */
	NRC_SKB_TRACK_WIM_FREE(hdev, skb, wim->cmd, wim->event, true, false);
}

int nrc_fw_start(struct nrc_hif_device *hdev)
{
	struct sk_buff *skb_req, *skb_resp;
	struct wim_drv_info_param *p;
	int ret;

	skb_req = nrc_wim_alloc_skb(WIM_CMD_START,
				    tlv_len(sizeof(struct wim_drv_info_param)));
	if (!skb_req)
		return -ENOMEM;

	p = nrc_wim_skb_add_tlv(skb_req, WIM_TLV_DRV_INFO,
				sizeof(struct wim_drv_info_param), NULL);
	p->boot_mode = !!hdev->params->fw_name;
	p->cqm_off = hdev->params->disable_cqm;
	p->bitmap_encoding = hdev->params->bitmap_encoding;
	p->reverse_scrambler = hdev->params->reverse_scrambler;
	p->kern_ver =
		(NRC_TARGET_KERNEL_VERSION >> 8) &
		0x0fff; // 12 bits for kernel version (4 for major, 8 for minor)
	p->ps_pretend_flag = hdev->params->ps_pretend;
	p->vendor_oui = VENDOR_OUI;
	if (hdev->chip_id == 0x7292) {
		p->deepsleep_gpio_dir = TARGET_DEEP_SLEEP_GPIO_DIR_7292;
	} else {
		p->deepsleep_gpio_dir = TARGET_DEEP_SLEEP_GPIO_DIR_739X;
	}
	p->deepsleep_gpio_out = TARGET_DEEP_SLEEP_GPIO_OUT;
	p->deepsleep_gpio_pullup = TARGET_DEEP_SLEEP_GPIO_PULLUP;
	/* 0: HW PTK/GTK, 1: SW PTK/GTK, 2: HW PTK, SW GTK */
	if (hdev->params->sw_enc < 0)
		hdev->params->sw_enc = 0;
	p->sw_enc = hdev->params->sw_enc;
	p->supported_ch_width = hdev->params->support_ch_width;
	p->duty_cycle_enable = hdev->params->set_duty_cycle[0] ? true : false;
	p->duty_cycle_window = hdev->params->set_duty_cycle[1];
	p->duty_cycle_duration = hdev->params->set_duty_cycle[2];
	p->cca_threshold = hdev->params->set_cca_threshold;
	if (!!hdev->nw && hdev->nw->twt_sched) {
		p->twt_wake_interval =
			hdev->nw->twt_sched_interval; //nw->twt_sched->interval;
	} else {
		p->twt_wake_interval = 0;
	}
	p->raw = hdev->params->raw;

	p->auth_control_enable = hdev->params->set_auth_control[0] ? true :
								     false;
	p->auth_control_slot = hdev->params->set_auth_control[1];
	// 1-bit scale (0:1, 1:10)
	p->auth_control_ti_min =
		hdev->params->set_auth_control[2] |
		(hdev->params->set_auth_control[4] == 10 ? 1 << 7 : 0);
	p->auth_control_ti_max = hdev->params->set_auth_control[3];

	p->band_sel_gpio_num = hdev->params->band_selection_gpio_num;
	p->band_sel_gpio_polarity = hdev->params->band_selection_gpio_polarity;

	ret = nrc_wim_request(skb_req, 0, (WIM_RESP_TIMEOUT * 70), false,
			      &skb_resp);
	if (ret) {
		ERR_FW("Failed to start firmware: %d", ret);
		return ret;
	}

	fw_on_ready(skb_resp);
	NRC_FW_SET_STARTED(hdev);
	return 0;
}
