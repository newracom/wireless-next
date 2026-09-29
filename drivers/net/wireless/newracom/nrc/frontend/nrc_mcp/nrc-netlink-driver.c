// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2016-2025 Newracom, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netlink.h>
#include <linux/mutex.h>
#include <net/sock.h>
#include <net/netlink.h>
#include <linux/skbuff.h>
#include <net/genetlink.h>
#include <linux/spi/spi.h>

/* Common directory headers - Debug & Trace */
#include "nrc-debug-common.h"

/* Local module headers - Debug */
#include "nrc-debug.h"

/* Local module headers */
#include "nrc-log.h"
#include "nrc-hif.h"
#include "mcp.h"

#include "nrc-hal-core-interface.h"
#include "nrc-mcp-transmit.h"
#include "nrc-mcp-init.h"
#include "nrc-netlink-driver.h"

/* Using common debug system - LOG_MODULE/LOG_LEVEL no longer needed */

struct user_info {
	uint32_t id;
	uint32_t portid;
};

static struct user_info user_info[CHAN_ID_MAX];

static const struct nla_policy nrc_policy[ATTR_MAX] = {
	[ATTR_REQUEST] = {.type = NLA_BINARY},
	[ATTR_RESPONSE] = {.type = NLA_BINARY},
};

static struct genl_family nrc_family[CHAN_ID_MAX];
static struct genl_ops nrc_ops[CHAN_ID_MAX][OPS_MAX];

#define DRIVER_CHAR_MAX (32)

typedef struct {
	char name[DRIVER_CHAR_MAX];
} __attribute__((packed)) driver_firmware_t;

typedef struct {
	char name[DRIVER_CHAR_MAX];
	char level[DRIVER_CHAR_MAX];
} __attribute__((packed)) driver_log_level_t;

typedef struct {
	uint16_t fw_tx_slot;
	uint16_t fw_rx_slot;
	uint16_t data;
} __attribute__((packed)) driver_credit_t;

int send_to_netlink(int id, struct sk_buff *skb, struct nrc_hif_device *hdev,
		    u8 hif_type, bool is_rx_path)
{
	struct sk_buff *reply_skb = NULL;
	void *reply_head = NULL;

	/* Validate input parameters */
	if (!skb || !skb->data) {
		ERR("send_to_netlink: Invalid SKB: skb=%p, data=%p", skb,
		    skb ? skb->data : NULL);
		return -EINVAL;
	}

	if (id < 0 || id >= CHAN_ID_MAX) {
		ERR("send_to_netlink: Invalid channel ID: %d (max: %d)", id,
		    CHAN_ID_MAX);
		goto cleanup;
	}

	/* Check if netlink family is initialized */
	if (!nrc_family[id].name[0]) {
		ERR("send_to_netlink: Family %d not initialized", id);
		goto cleanup;
	}

	/* Check if user is connected */
	if (!user_info[id].portid) {
		ERR("send_to_netlink: No user connected to channel %d", id);
		goto cleanup;
	}

	reply_skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!reply_skb) {
		ERR("Failed to allocate reply SKB");
		goto cleanup;
	}

	reply_head =
		genlmsg_put(reply_skb, 0, 0, &nrc_family[id], 0, ATTR_RESPONSE);
	if (!reply_head) {
		ERR("Failed to create message header");
		goto cleanup;
	}

	if (nla_put(reply_skb, ATTR_REQUEST, skb->len, skb->data)) {
		ERR("Failed to add TLV attribute");
		goto cleanup;
	}

	genlmsg_end(reply_skb, reply_head);

	LOG_WIM("Send to netlink: %s(%d), input_skb=%d, reply_skb=%d",
		channel_id_to_str(id), id, skb ? skb->len : 0,
		reply_skb ? reply_skb->len : 0);

	genlmsg_unicast(&init_net, reply_skb, user_info[id].portid);

	/* Track SKB free */
	NRC_SKB_TRACK_FREE(hdev, skb, hif_type, is_rx_path, false);

	return 0;

cleanup:
	if (reply_skb) {
		nlmsg_free(reply_skb);
	}
	if (skb) {
		/* Track SKB free */
		NRC_SKB_TRACK_FREE(hdev, skb, hif_type, is_rx_path, false);
	}

	return -ENOMEM;
}

static int process_control_h2f(struct sk_buff *skb, struct genl_info *info)
{
	int ret;
	struct nlattr *attr = info->attrs[ATTR_REQUEST];
	struct wim_tlv *tlv;

	if (!attr) {
		ERR("Missing DRIVER_ATTR_TLV attribute");
		return -EINVAL;
	}

	if (nla_len(attr) < (int)sizeof(struct wim_tlv)) {
		ERR("TLV attribute too short (%d)", nla_len(attr));
		return -EINVAL;
	}

	tlv = nla_data(attr);

	/* Bound the embedded TLV length against the actual attribute payload */
	if (tlv->l > nla_len(attr) - (int)sizeof(struct wim_tlv)) {
		ERR("TLV payload length %u exceeds attribute", tlv->l);
		return -EINVAL;
	}

	/* Process WIM TLVs specific to MCP control channel */
	ret = nrc_mcp_process_wim_request_wait(CHAN_ID_CONTROL_H2F,
					       CHAN_ID_CONTROL_F2H,
					       WIM_CMD_MCP_CHAN_ID_CONTROL_H2F,
					       tlv);
	if (ret) {
		ERR("Failed to WIM tlv->type=0x%x err %d", tlv->t, ret);
	}

	return ret;
}

static int process_control_f2h(struct sk_buff *skb, struct genl_info *info)
{
	/* not allowed */
	return 0;
}

static int process_data(struct sk_buff *skb, struct genl_info *info)
{
	int ret;
	struct nlattr *attr = info->attrs[ATTR_REQUEST];

	if (!attr) {
		ERR("Missing DRIVER_ATTR_TLV attribute");
		return -EINVAL;
	}

	if (nla_len(attr) <= 0) {
		ERR("Empty data payload");
		return -EINVAL;
	}

	LOG_INFO("MCP: Processing data H2F len=%u", nla_len(attr));

	/* Transmit as HIF_TYPE_FRAME with HIF_FRAME_SUB_MCP_DATA */
	ret = nrc_mcp_xmit_hif_frame(HIF_FRAME_SUB_MCP_DATA,
				     (u8 *)nla_data(attr), nla_len(attr),
				     false);
	if (ret) {
		ERR("Failed to transmit protocol frame, err=%d", ret);
		return ret;
	}

	return 0;
}

static int process_event(struct sk_buff *skb, struct genl_info *info)
{
	/* not allowed */
	return 0;
}

static int process_protocol_h2f(struct sk_buff *skb, struct genl_info *info)
{
	int ret;
	struct nlattr *attr = info->attrs[ATTR_REQUEST];

	if (!attr) {
		ERR("Missing DRIVER_ATTR_TLV attribute");
		return -EINVAL;
	}

	if (nla_len(attr) <= 0) {
		ERR("Empty protocol payload");
		return -EINVAL;
	}

	LOG_INFO("MCP: Processing protocol H2F len=%u", nla_len(attr));

	/* Transmit as HIF_TYPE_FRAME with HIF_FRAME_SUB_MCP_PROTOCOL */
	ret = nrc_mcp_xmit_hif_frame(HIF_FRAME_SUB_MCP_PROTOCOL,
				     (u8 *)nla_data(attr), nla_len(attr),
				     false);
	if (ret) {
		ERR("Failed to transmit protocol frame, err=%d", ret);
		return ret;
	}

	return 0;
}

static int process_protocol_f2h(struct sk_buff *skb, struct genl_info *info)
{
	/* not allowed */
	return 0;
}

static int process_protocol_event(struct sk_buff *skb, struct genl_info *info)
{
	/* not allowed */
	return 0;
}

static int process_driver_h2d(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *attr = info->attrs[ATTR_REQUEST];
	struct wim_tlv *tlv;
	int ret;

	if (!attr) {
		ERR("Missing DRIVER_ATTR_TLV attribute");
		return -EINVAL;
	}

	if (nla_len(attr) < (int)sizeof(struct wim_tlv)) {
		ERR("TLV attribute too short (%d)", nla_len(attr));
		return -EINVAL;
	}

	tlv = nla_data(attr);

	/* Bound the embedded TLV length against the actual attribute payload */
	if (tlv->l > nla_len(attr) - (int)sizeof(struct wim_tlv)) {
		ERR("TLV payload length %u exceeds attribute", tlv->l);
		return -EINVAL;
	}

	if (tlv->t == TLV_TYPE_DRIVER_RAW_PACKET) {
		/* Raw packet with pre-built HIF header - send directly without modification */
		driver_raw_packet_t *raw_pkt;
		int avail = nla_len(attr) - (int)sizeof(struct wim_tlv) -
			    (int)sizeof(driver_raw_packet_t);

		if (avail < 0) {
			ERR("raw packet TLV payload too short");
			return -EINVAL;
		}

		raw_pkt = (driver_raw_packet_t *)(tlv + 1);

		/*
		 * Re-bound the embedded length against the original attribute
		 * size before the transmit path copies raw_pkt->length bytes.
		 */
		if (raw_pkt->length < (int)sizeof(struct hif) ||
		    raw_pkt->length > avail) {
			ERR("raw packet length %d exceeds attribute (avail %d)",
			    raw_pkt->length, avail);
			return -EINVAL;
		}

		/* The embedded TLV length must also cover the raw packet */
		if ((size_t)tlv->l < sizeof(driver_raw_packet_t) ||
		    (size_t)raw_pkt->length >
			    tlv->l - sizeof(driver_raw_packet_t)) {
			ERR("raw packet length %d exceeds TLV length %u",
			    raw_pkt->length, tlv->l);
			return -EINVAL;
		}

		LOG_INFO("Raw packet TX: length=%d", raw_pkt->length);

		/* Transmit raw packet with HIF header already included */
		ret = nrc_mcp_xmit_hif_frame(HIF_FRAME_SUB_MCP_DATA,
					     raw_pkt->data, raw_pkt->length,
					     true);
		if (ret) {
			ERR("Failed to transmit protocol frame, err=%d", ret);
			return ret;
		}
	} else if (tlv->t == TLV_TYPE_DRIVER_FIRMWARE) {
		driver_firmware_t *firmware;
		char name[DRIVER_CHAR_MAX + 1];

		if (nla_len(attr) < (int)sizeof(struct wim_tlv) +
					    (int)sizeof(driver_firmware_t)) {
			ERR("firmware TLV payload too short");
			return -EINVAL;
		}

		firmware = (driver_firmware_t *)(tlv + 1);

		/*
		 * The name field is caller-supplied and is not guaranteed to
		 * be NUL-terminated; terminate a local copy before use.
		 */
		memcpy(name, firmware->name, DRIVER_CHAR_MAX);
		name[DRIVER_CHAR_MAX] = '\0';

		LOG_INFO("MCP: Firmware download request skipped: %s", name);

		/* Trigger network restart which will handle firmware download */
		// nrc_hal_ops_nw_restart();
	} else if (tlv->t == TLV_TYPE_DRIVER_SET_LOG) {
		driver_log_level_t *log;
		char name[DRIVER_CHAR_MAX + 1];
		char level[DRIVER_CHAR_MAX + 1];

		if (nla_len(attr) < (int)sizeof(struct wim_tlv) +
					    (int)sizeof(driver_log_level_t)) {
			ERR("set_log TLV payload too short");
			return -EINVAL;
		}

		log = (driver_log_level_t *)(tlv + 1);

		/*
		 * Both fields are caller-supplied and are not guaranteed to be
		 * NUL-terminated; terminate local copies before use.
		 */
		memcpy(name, log->name, DRIVER_CHAR_MAX);
		name[DRIVER_CHAR_MAX] = '\0';
		memcpy(level, log->level, DRIVER_CHAR_MAX);
		level[DRIVER_CHAR_MAX] = '\0';

		LOG_INFO("MCP: SET_LOG name=%s level=%s", name, level);
		nrc_logger_set(name, level);
	} else if (tlv->t == TLV_TYPE_DRIVER_REQ_CREDIT) {
		struct sk_buff *reply_skb = dev_alloc_skb(2048);
		driver_credit_t *credit;
		struct mcp_priv *mcp = nrc_mcp_get_device();
		struct nrc_hif_device *hdev = mcp ? mcp->hdev : NULL;
		struct wim_tlv *tlv;

		if (!reply_skb) {
			ERR("Failed to allocate credit response SKB");
			return -ENOMEM;
		}

		/* Track internal SKB allocation (TX path) */
		NRC_SKB_TRACK_ALLOC(hdev, reply_skb, HIF_TYPE_ND_WIM, false,
				    false);

		tlv = (void *)reply_skb->data;
		tlv->t = TLV_TYPE_DRIVER_RSP_CREDIT;
		tlv->l = sizeof(driver_credit_t);
		credit = (void *)tlv->v;
		if (hdev) {
			credit->fw_tx_slot = hdev->slot[TX_SLOT].count;
			credit->fw_rx_slot = hdev->slot[RX_SLOT].count;
			credit->data = 0;
		} else {
			credit->fw_tx_slot = 0;
			credit->fw_rx_slot = 0;
			credit->data = 0;
		}

		skb_put(reply_skb, tlv->l + sizeof(*tlv));

		LOG_WIM("tx_slot=%d, rx_slot=%d, data=%d", credit->fw_tx_slot,
			credit->fw_rx_slot, credit->data);

		/* send_to_netlink will free the SKB */
		send_to_netlink(CHAN_ID_DRIVER_D2H, reply_skb, hdev,
				HIF_TYPE_ND_WIM, false);
	} else if (tlv->t == TLV_TYPE_REQ_DRIVER_PING) {
		struct sk_buff *reply_skb = dev_alloc_skb(2048);
		struct nrc_hif_device *hdev = nrc_hal_core_get_hdev();
		u8 *ping_data;
		struct wim_tlv *reply_tlv;

		if (!reply_skb) {
			ERR("Failed to allocate driver ping response SKB");
			return -ENOMEM;
		}

		/* Track internal SKB allocation (TX path) */
		NRC_SKB_TRACK_ALLOC(hdev, reply_skb, HIF_TYPE_ND_WIM, false,
				    false);

		reply_tlv = (void *)reply_skb->data;
		reply_tlv->t = TLV_TYPE_RSP_DRIVER_PING;
		reply_tlv->l = sizeof(u8);
		ping_data = (void *)reply_tlv->v;
		*ping_data = 0; // Dummy response

		skb_put(reply_skb, reply_tlv->l + sizeof(*reply_tlv));

		LOG_INFO("Driver ping: Request received and response sent");

		send_to_netlink(CHAN_ID_DRIVER_D2H, reply_skb, hdev,
				HIF_TYPE_ND_WIM, false);
	} else {
		ERR("Unknown: tlv->type=0x%x tlv->length=%d", tlv->t, tlv->l);
	}

	return 0;
}

static int process_driver_d2h(struct sk_buff *skb, struct genl_info *info)
{
	/* not allowed */
	return 0;
}

static int process_control_h2f_init(struct sk_buff *skb, struct genl_info *info)
{
	user_info[CHAN_ID_CONTROL_H2F].portid = info->snd_portid;

	return 0;
}

static int process_control_f2h_init(struct sk_buff *skb, struct genl_info *info)
{
	user_info[CHAN_ID_CONTROL_F2H].portid = info->snd_portid;

	return 0;
}

static int process_data_init(struct sk_buff *skb, struct genl_info *info)
{
	user_info[CHAN_ID_DATA].portid = info->snd_portid;
	return 0;
}

static int process_event_init(struct sk_buff *skb, struct genl_info *info)
{
	user_info[CHAN_ID_EVENT].portid = info->snd_portid;
	return 0;
}

static int process_protocol_h2f_init(struct sk_buff *skb,
				     struct genl_info *info)
{
	user_info[CHAN_ID_PROTOCOL_H2F].portid = info->snd_portid;
	return 0;
}

static int process_protocol_f2h_init(struct sk_buff *skb,
				     struct genl_info *info)
{
	user_info[CHAN_ID_PROTOCOL_F2H].portid = info->snd_portid;
	return 0;
}

static int process_protocol_event_init(struct sk_buff *skb,
				       struct genl_info *info)
{
	user_info[CHAN_ID_PROTOCOL_EVENT].portid = info->snd_portid;
	return 0;
}

static int process_driver_d2h_init(struct sk_buff *skb, struct genl_info *info)
{
	user_info[CHAN_ID_DRIVER_D2H].portid = info->snd_portid;
	return 0;
}

static int process_driver_h2d_init(struct sk_buff *skb, struct genl_info *info)
{
	user_info[CHAN_ID_DRIVER_H2D].portid = info->snd_portid;
	return 0;
}

static int init_family(int id)
{
	struct genl_family *family = &nrc_family[id];
	struct genl_ops *ops = nrc_ops[id];
	struct user_info *info = &user_info[id];
	size_t n_ops = OPS_MAX;

	/* Initialize the family structure to zero */
	memset(family, 0, sizeof(*family));
	memset(ops, 0, sizeof(nrc_ops[id]));

	family->version = ATTR_VERSION;
	family->maxattr = ATTR_MAX;
	family->module = THIS_MODULE;
	family->ops = ops;
	family->n_ops = n_ops;
	//family->priv = info;

	ops[OPS_INIT].cmd = OPS_INIT;
	ops[OPS_INIT].flags = GENL_ADMIN_PERM;
	ops[OPS_INIT].policy = nrc_policy;

	ops[OPS_REQUEST].cmd = OPS_REQUEST;
	ops[OPS_REQUEST].flags = GENL_ADMIN_PERM;
	ops[OPS_REQUEST].policy = nrc_policy;

	if (ops[OPS_INIT].cmd >= family->resv_start_op) {
		ops[OPS_INIT].validate = 0;
		ops[OPS_REQUEST].validate = 0;
	} else {
		ops[OPS_INIT].validate = GENL_DONT_VALIDATE_STRICT |
					 GENL_DONT_VALIDATE_DUMP;
		ops[OPS_REQUEST].validate = GENL_DONT_VALIDATE_STRICT |
					    GENL_DONT_VALIDATE_DUMP;
	}

	info->id = id;

	switch (id) {
	case CHAN_ID_CONTROL_H2F:
		strcpy(family->name, "nrc-ctrl-h2f");
		ops[OPS_INIT].doit = process_control_h2f_init;
		ops[OPS_REQUEST].doit = process_control_h2f;
		break;

	case CHAN_ID_CONTROL_F2H:
		strcpy(family->name, "nrc-ctrl-f2h");
		ops[OPS_INIT].doit = process_control_f2h_init;
		ops[OPS_REQUEST].doit = process_control_f2h;
		break;

	case CHAN_ID_DATA:
		strcpy(family->name, "nrc-data");
		ops[OPS_INIT].doit = process_data_init;
		ops[OPS_REQUEST].doit = process_data;
		break;

	case CHAN_ID_EVENT:
		strcpy(family->name, "nrc-event");
		ops[OPS_INIT].doit = process_event_init;
		ops[OPS_REQUEST].doit = process_event;
		break;

	case CHAN_ID_PROTOCOL_H2F:
		strcpy(family->name, "nrc-proto-h2f");
		ops[OPS_INIT].doit = process_protocol_h2f_init;
		ops[OPS_REQUEST].doit = process_protocol_h2f;
		break;

	case CHAN_ID_PROTOCOL_F2H:
		strcpy(family->name, "nrc-proto-f2h");
		ops[OPS_INIT].doit = process_protocol_f2h_init;
		ops[OPS_REQUEST].doit = process_protocol_f2h;
		break;

	case CHAN_ID_PROTOCOL_EVENT:
		strcpy(family->name, "nrc-proto-event");
		ops[OPS_INIT].doit = process_protocol_event_init;
		ops[OPS_REQUEST].doit = process_protocol_event;
		break;

	case CHAN_ID_DRIVER_H2D:
		strcpy(family->name, "nrc-driver-h2d");
		ops[OPS_INIT].doit = process_driver_h2d_init;
		ops[OPS_REQUEST].doit = process_driver_h2d;
		break;

	case CHAN_ID_DRIVER_D2H:
		strcpy(family->name, "nrc-driver-d2h");
		ops[OPS_INIT].doit = process_driver_d2h_init;
		ops[OPS_REQUEST].doit = process_driver_d2h;
		break;
	}
	return genl_register_family(family);
}

static void deinit_family(struct genl_family *family)
{
	genl_unregister_family(family);
}

int netlink_driver_init(struct nrc_hif_device *hdev)
{
	int ret;
	int id, deinit_id;

	LOG_INFO("MCP: %s", __FUNCTION__);

	for (id = 0; id < CHAN_ID_MAX; id++) {
		ret = init_family(id);
		if (ret) {
			ERR("Failed to register family[%d]: %d", id, ret);
			goto err;
		}
	}

	return 0;

err:
	for (deinit_id = 0; deinit_id < id; deinit_id++) {
		deinit_family(&nrc_family[deinit_id]);
	}
	return -EINVAL;
}

void netlink_driver_exit(void)
{
	int id;

	LOG_INFO("MCP: %s", __FUNCTION__);

	for (id = 0; id < CHAN_ID_MAX; id++) {
		deinit_family(&nrc_family[id]);
	}
}
