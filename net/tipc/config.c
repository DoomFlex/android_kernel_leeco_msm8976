/*
 * net/tipc/config.c: TIPC configuration management code
 *
 * Copyright (c) 2002-2006, Ericsson AB
 * Copyright (c) 2004-2007, 2010-2013, Wind River Systems
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "core.h"
#include "socket.h"
#include "name_table.h"
#include "config.h"
#include "server.h"

#define REPLY_TRUNCATED "<truncated>\n"

static const void *req_tlv_area;	/* request message TLV area */
static int req_tlv_space;		/* request message TLV area size */
static int rep_headroom;		/* reply message headroom to use */

struct sk_buff *tipc_cfg_reply_alloc(int payload_size)
{
	struct sk_buff *buf;

	buf = alloc_skb(rep_headroom + payload_size, GFP_ATOMIC);
	if (buf)
		skb_reserve(buf, rep_headroom);
	return buf;
}

int tipc_cfg_append_tlv(struct sk_buff *buf, int tlv_type,
			void *tlv_data, int tlv_data_size)
{
	struct tlv_desc *tlv = (struct tlv_desc *)skb_tail_pointer(buf);
	int new_tlv_space = TLV_SPACE(tlv_data_size);

	if (skb_tailroom(buf) < new_tlv_space)
		return 0;
	skb_put(buf, new_tlv_space);
	tlv->tlv_type = htons(tlv_type);
	tlv->tlv_len  = htons(TLV_LENGTH(tlv_data_size));
	if (tlv_data_size && tlv_data)
		memcpy(TLV_DATA(tlv), tlv_data, tlv_data_size);
	return 1;
}

static struct sk_buff *tipc_cfg_reply_unsigned_type(u16 tlv_type, u32 value)
{
	struct sk_buff *buf;
	__be32 value_net;

	buf = tipc_cfg_reply_alloc(TLV_SPACE(sizeof(value)));
	if (buf) {
		value_net = htonl(value);
		tipc_cfg_append_tlv(buf, tlv_type, &value_net,
				    sizeof(value_net));
	}
	return buf;
}

static struct sk_buff *tipc_cfg_reply_unsigned(u32 value)
{
	return tipc_cfg_reply_unsigned_type(TIPC_TLV_UNSIGNED, value);
}

struct sk_buff *tipc_cfg_reply_string_type(u16 tlv_type, char *string)
{
	struct sk_buff *buf;
	int string_len = strlen(string) + 1;

	buf = tipc_cfg_reply_alloc(TLV_SPACE(string_len));
	if (buf)
		tipc_cfg_append_tlv(buf, tlv_type, string, string_len);
	return buf;
}

static struct sk_buff *tipc_show_stats(void)
{
	struct sk_buff *buf;
	struct tlv_desc *rep_tlv;
	char *pb;
	int pb_len;
	int str_len;
	u32 value;

	if (!TLV_CHECK(req_tlv_area, req_tlv_space, TIPC_TLV_UNSIGNED))
		return tipc_cfg_reply_error_string(TIPC_CFG_TLV_ERROR);

	value = ntohl(*(u32 *)TLV_DATA(req_tlv_area));
	if (value != 0)
		return tipc_cfg_reply_error_string("unsupported argument");

	buf = tipc_cfg_reply_alloc(TLV_SPACE(ULTRA_STRING_MAX_LEN));
	if (buf == NULL)
		return NULL;

	rep_tlv = (struct tlv_desc *)buf->data;
	pb = TLV_DATA(rep_tlv);
	pb_len = ULTRA_STRING_MAX_LEN;

	str_len = tipc_snprintf(pb, pb_len, "TIPC version " TIPC_MOD_VER "\n");
	str_len += 1;	/* for "\0" */
	skb_put(buf, TLV_SPACE(str_len));
	TLV_SET(rep_tlv, TIPC_TLV_ULTRA_STRING, NULL, str_len);

	return buf;
}

static struct sk_buff *cfg_enable_bearer(void)
{
	struct tipc_bearer_config *args;

	if (!TLV_CHECK(req_tlv_area, req_tlv_space, TIPC_TLV_BEARER_CONFIG))
		return tipc_cfg_reply_error_string(TIPC_CFG_TLV_ERROR);

	args = (struct tipc_bearer_config *)TLV_DATA(req_tlv_area);
	if (tipc_enable_bearer(args->name,
			       ntohl(args->disc_domain),
			       ntohl(args->priority)))
		return tipc_cfg_reply_error_string("unable to enable bearer");

	return tipc_cfg_reply_none();
}

static struct sk_buff *cfg_disable_bearer(void)
{
	if (!TLV_CHECK(req_tlv_area, req_tlv_space, TIPC_TLV_BEARER_NAME))
		return tipc_cfg_reply_error_string(TIPC_CFG_TLV_ERROR);

	if (tipc_disable_bearer((char *)TLV_DATA(req_tlv_area)))
		return tipc_cfg_reply_error_string("unable to disable bearer");

	return tipc_cfg_reply_none();
}

static struct sk_buff *cfg_set_own_addr(void)
{
	u32 addr;

	if (!TLV_CHECK(req_tlv_area, req_tlv_space, TIPC_TLV_NET_ADDR))
		return tipc_cfg_reply_error_string(TIPC_CFG_TLV_ERROR);

	addr = ntohl(*(__be32 *)TLV_DATA(req_tlv_area));
	if (addr == tipc_own_addr)
		return tipc_cfg_reply_none();
	if (!tipc_addr_node_valid(addr))
		return tipc_cfg_reply_error_string(TIPC_CFG_INVALID_VALUE
						   " (node address)");
	if (tipc_own_addr)
		return tipc_cfg_reply_error_string(TIPC_CFG_NOT_SUPPORTED
						   " (cannot change node address once assigned)");
	if (!tipc_net_start(addr))
		return tipc_cfg_reply_none();

	return tipc_cfg_reply_error_string("cannot change to network mode");
}

static struct sk_buff *cfg_set_netid(void)
{
	u32 value;

	if (!TLV_CHECK(req_tlv_area, req_tlv_space, TIPC_TLV_UNSIGNED))
		return tipc_cfg_reply_error_string(TIPC_CFG_TLV_ERROR);
	value = ntohl(*(__be32 *)TLV_DATA(req_tlv_area));
	if (value == tipc_net_id)
		return tipc_cfg_reply_none();
	if (value < 1 || value > 9999)
		return tipc_cfg_reply_error_string(TIPC_CFG_INVALID_VALUE
						   " (network id must be 1-9999)");
	if (tipc_own_addr)
		return tipc_cfg_reply_error_string(TIPC_CFG_NOT_SUPPORTED
			" (cannot change network id once TIPC has joined a network)");
	tipc_net_id = value;
	return tipc_cfg_reply_none();
}

struct sk_buff *tipc_cfg_do_cmd(u32 orig_node, u16 cmd, const void *request_area,
				int request_space, int reply_headroom)
{
	struct sk_buff *rep_tlv_buf;

	rtnl_lock();

	/* Save request and reply details in a well-known location */
	req_tlv_area = request_area;
	req_tlv_space = request_space;
	rep_headroom = reply_headroom;

	/* Check command authorization */
	if (likely(in_own_node(orig_node))) {
		/* command is permitted */
	} else {
		rep_tlv_buf = tipc_cfg_reply_error_string(TIPC_CFG_NOT_SUPPORTED
							  " (cannot be done remotely)");
		goto exit;
	}

	/* Call appropriate processing routine */
	switch (cmd) {
	case TIPC_CMD_NOOP:
		rep_tlv_buf = tipc_cfg_reply_none();
		break;
	case TIPC_CMD_GET_NODES:
		rep_tlv_buf = tipc_node_get_nodes(req_tlv_area, req_tlv_space);
		break;
	case TIPC_CMD_GET_LINKS:
		rep_tlv_buf = tipc_node_get_links(req_tlv_area, req_tlv_space);
		break;
	case TIPC_CMD_SHOW_LINK_STATS:
		rep_tlv_buf = tipc_link_cmd_show_stats(req_tlv_area, req_tlv_space);
		break;
	case TIPC_CMD_RESET_LINK_STATS:
		rep_tlv_buf = tipc_link_cmd_reset_stats(req_tlv_area, req_tlv_space);
		break;
	case TIPC_CMD_SHOW_NAME_TABLE:
		rep_tlv_buf = tipc_nametbl_get(req_tlv_area, req_tlv_space);
		break;
	case TIPC_CMD_GET_BEARER_NAMES:
		rep_tlv_buf = tipc_bearer_get_names();
		break;
	case TIPC_CMD_GET_MEDIA_NAMES:
		rep_tlv_buf = tipc_media_get_names();
		break;
	case TIPC_CMD_SHOW_PORTS:
		rep_tlv_buf = tipc_sk_socks_show();
		break;
	case TIPC_CMD_SHOW_STATS:
		rep_tlv_buf = tipc_show_stats();
		break;
	case TIPC_CMD_SET_LINK_TOL:
	case TIPC_CMD_SET_LINK_PRI:
	case TIPC_CMD_SET_LINK_WINDOW:
		rep_tlv_buf = tipc_link_cmd_config(req_tlv_area, req_tlv_space, cmd);
		break;
	case TIPC_CMD_ENABLE_BEARER:
		rep_tlv_buf = cfg_enable_bearer();
		break;
	case TIPC_CMD_DISABLE_BEARER:
		rep_tlv_buf = cfg_disable_bearer();
		break;
	case TIPC_CMD_SET_NODE_ADDR:
		rep_tlv_buf = cfg_set_own_addr();
		break;
	case TIPC_CMD_SET_NETID:
		rep_tlv_buf = cfg_set_netid();
		break;
	case TIPC_CMD_GET_NETID:
		rep_tlv_buf = tipc_cfg_reply_unsigned(tipc_net_id);
		break;
	case TIPC_CMD_NOT_NET_ADMIN:
		rep_tlv_buf =
			tipc_cfg_reply_error_string(TIPC_CFG_NOT_NET_ADMIN);
		break;
	case TIPC_CMD_SET_MAX_ZONES:
	case TIPC_CMD_GET_MAX_ZONES:
	case TIPC_CMD_SET_MAX_SLAVES:
	case TIPC_CMD_GET_MAX_SLAVES:
	case TIPC_CMD_SET_MAX_CLUSTERS:
	case TIPC_CMD_GET_MAX_CLUSTERS:
	case TIPC_CMD_SET_MAX_NODES:
	case TIPC_CMD_GET_MAX_NODES:
	case TIPC_CMD_SET_MAX_SUBSCR:
	case TIPC_CMD_GET_MAX_SUBSCR:
	case TIPC_CMD_SET_MAX_PUBL:
	case TIPC_CMD_GET_MAX_PUBL:
	case TIPC_CMD_SET_LOG_SIZE:
	case TIPC_CMD_SET_REMOTE_MNG:
	case TIPC_CMD_GET_REMOTE_MNG:
	case TIPC_CMD_DUMP_LOG:
	case TIPC_CMD_SET_MAX_PORTS:
	case TIPC_CMD_GET_MAX_PORTS:
		rep_tlv_buf = tipc_cfg_reply_error_string(TIPC_CFG_NOT_SUPPORTED
							  " (obsolete command)");
		break;
	default:
		rep_tlv_buf = tipc_cfg_reply_error_string(TIPC_CFG_NOT_SUPPORTED
							  " (unknown command)");
		break;
	}

	WARN_ON(rep_tlv_buf->len > TLV_SPACE(ULTRA_STRING_MAX_LEN));

	/* Append an error message if we cannot return all requested data */
	if (rep_tlv_buf->len == TLV_SPACE(ULTRA_STRING_MAX_LEN)) {
		if (*(rep_tlv_buf->data + ULTRA_STRING_MAX_LEN) != '\0')
			sprintf(rep_tlv_buf->data + rep_tlv_buf->len -
				sizeof(REPLY_TRUNCATED) - 1, REPLY_TRUNCATED);
	}

	/* Return reply buffer */
exit:
	rtnl_unlock();
	return rep_tlv_buf;
}
