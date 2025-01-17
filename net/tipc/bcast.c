/*
 * net/tipc/bcast.c: TIPC broadcast code
 *
 * Copyright (c) 2004-2006, 2014, Ericsson AB
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2005, 2010-2011, Wind River Systems
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
#include "link.h"
#include "socket.h"
#include "msg.h"
#include "bcast.h"
#include "name_distr.h"

#define	MAX_PKT_DEFAULT_MCAST	1500	/* bcast link max packet size (fixed) */
#define	BCLINK_WIN_DEFAULT	20	/* bcast link window size (default) */
#define	BCBEARER		MAX_BEARERS

/**
 * struct tipc_bcbearer_pair - a pair of bearers used by broadcast link
 * @primary: pointer to primary bearer
 * @secondary: pointer to secondary bearer
 *
 * Bearers must have same priority and same set of reachable destinations
 * to be paired.
 */

struct tipc_bcbearer_pair {
	struct tipc_bearer *primary;
	struct tipc_bearer *secondary;
};

/**
 * struct tipc_bcbearer - bearer used by broadcast link
 * @bearer: (non-standard) broadcast bearer structure
 * @media: (non-standard) broadcast media structure
 * @bpairs: array of bearer pairs
 * @bpairs_temp: temporary array of bearer pairs used by tipc_bcbearer_sort()
 * @remains: temporary node map used by tipc_bcbearer_send()
 * @remains_new: temporary node map used tipc_bcbearer_send()
 *
 * Note: The fields labelled "temporary" are incorporated into the bearer
 * to avoid consuming potentially limited stack space through the use of
 * large local variables within multicast routines.  Concurrent access is
 * prevented through use of the spinlock "bclink_lock".
 */
struct tipc_bcbearer {
	struct tipc_bearer bearer;
	struct tipc_media media;
	struct tipc_bcbearer_pair bpairs[MAX_BEARERS];
	struct tipc_bcbearer_pair bpairs_temp[TIPC_MAX_LINK_PRI + 1];
	struct tipc_node_map remains;
	struct tipc_node_map remains_new;
};

/**
 * struct tipc_bclink - link used for broadcast messages
 * @lock: spinlock governing access to structure
 * @link: (non-standard) broadcast link structure
 * @node: (non-standard) node structure representing b'cast link's peer node
 * @flags: represent bclink states
 * @bcast_nodes: map of broadcast-capable nodes
 * @retransmit_to: node that most recently requested a retransmit
 *
 * Handles sequence numbering, fragmentation, bundling, etc.
 */
struct tipc_bclink {
	spinlock_t lock;
	struct tipc_link link;
	struct tipc_node node;
	unsigned int flags;
	struct tipc_node_map bcast_nodes;
	struct tipc_node *retransmit_to;
};

static struct tipc_bcbearer *bcbearer;
static struct tipc_bclink *bclink;
static struct tipc_link *bcl;

const char tipc_bclink_name[] = "broadcast-link";

static void tipc_nmap_diff(struct tipc_node_map *nm_a,
			   struct tipc_node_map *nm_b,
			   struct tipc_node_map *nm_diff);
static void tipc_nmap_add(struct tipc_node_map *nm_ptr, u32 node);
static void tipc_nmap_remove(struct tipc_node_map *nm_ptr, u32 node);

static void tipc_bclink_lock(void)
{
	spin_lock_bh(&bclink->lock);
}

static void tipc_bclink_unlock(void)
{
	struct tipc_node *node = NULL;

	if (likely(!bclink->flags)) {
		spin_unlock_bh(&bclink->lock);
		return;
	}

	if (bclink->flags & TIPC_BCLINK_RESET) {
		bclink->flags &= ~TIPC_BCLINK_RESET;
		node = tipc_bclink_retransmit_to();
	}
	spin_unlock_bh(&bclink->lock);

	if (node)
		tipc_link_reset_all(node);
}

uint  tipc_bclink_get_mtu(void)
{
	return MAX_PKT_DEFAULT_MCAST;
}

void tipc_bclink_set_flags(unsigned int flags)
{
	bclink->flags |= flags;
}

static u32 bcbuf_acks(struct sk_buff *buf)
{
	return (u32)(unsigned long)TIPC_SKB_CB(buf)->handle;
}

static void bcbuf_set_acks(struct sk_buff *buf, u32 acks)
{
	TIPC_SKB_CB(buf)->handle = (void *)(unsigned long)acks;
}

static void bcbuf_decr_acks(struct sk_buff *buf)
{
	bcbuf_set_acks(buf, bcbuf_acks(buf) - 1);
}

void tipc_bclink_add_node(u32 addr)
{
	tipc_bclink_lock();
	tipc_nmap_add(&bclink->bcast_nodes, addr);
	tipc_bclink_unlock();
}

void tipc_bclink_remove_node(u32 addr)
{
	tipc_bclink_lock();
	tipc_nmap_remove(&bclink->bcast_nodes, addr);
	tipc_bclink_unlock();
}

static void bclink_set_last_sent(void)
{
	if (bcl->next_out)
		bcl->fsm_msg_cnt = mod(buf_seqno(bcl->next_out) - 1);
	else
		bcl->fsm_msg_cnt = mod(bcl->next_out_no - 1);
}

u32 tipc_bclink_get_last_sent(void)
{
	return bcl->fsm_msg_cnt;
}

static void bclink_update_last_sent(struct tipc_node *node, u32 seqno)
{
	node->bclink.last_sent = less_eq(node->bclink.last_sent, seqno) ?
						seqno : node->bclink.last_sent;
}


/**
 * tipc_bclink_retransmit_to - get most recent node to request retransmission
 *
 * Called with bclink_lock locked
 */
struct tipc_node *tipc_bclink_retransmit_to(void)
{
	return bclink->retransmit_to;
}

/**
 * bclink_retransmit_pkt - retransmit broadcast packets
 * @after: sequence number of last packet to *not* retransmit
 * @to: sequence number of last packet to retransmit
 *
 * Called with bclink_lock locked
 */
static void bclink_retransmit_pkt(u32 after, u32 to)
{
	struct sk_buff *skb;

	skb_queue_walk(&bcl->outqueue, skb) {
		if (more(buf_seqno(skb), after))
			break;
	}
	tipc_link_retransmit(bcl, skb, mod(to - after));
}

/**
 * tipc_bclink_wakeup_users - wake up pending users
 *
 * Called with no locks taken
 */
void tipc_bclink_wakeup_users(void)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&bclink->link.waiting_sks)))
		tipc_sk_rcv(skb);

}

/**
 * tipc_bclink_acknowledge - handle acknowledgement of broadcast packets
 * @n_ptr: node that sent acknowledgement info
 * @acked: broadcast sequence # that has been acknowledged
 *
 * Node is locked, bclink_lock unlocked.
 */
void tipc_bclink_acknowledge(struct tipc_node *n_ptr, u32 acked)
{
	struct sk_buff *skb, *tmp;
	struct sk_buff *next;
	unsigned int released = 0;

	tipc_bclink_lock();
	/* Bail out if tx queue is empty (no clean up is required) */
	skb = skb_peek(&bcl->outqueue);
	if (!skb)
		goto exit;

	/* Determine which messages need to be acknowledged */
	if (acked == INVALID_LINK_SEQ) {
		/*
		 * Contact with specified node has been lost, so need to
		 * acknowledge sent messages only (if other nodes still exist)
		 * or both sent and unsent messages (otherwise)
		 */
		if (bclink->bcast_nodes.count)
			acked = bcl->fsm_msg_cnt;
		else
			acked = bcl->next_out_no;
	} else {
		/*
		 * Bail out if specified sequence number does not correspond
		 * to a message that has been sent and not yet acknowledged
		 */
		if (less(acked, buf_seqno(skb)) ||
		    less(bcl->fsm_msg_cnt, acked) ||
		    less_eq(acked, n_ptr->bclink.acked))
			goto exit;
	}

	/* Skip over packets that node has previously acknowledged */
	skb_queue_walk(&bcl->outqueue, skb) {
		if (more(buf_seqno(skb), n_ptr->bclink.acked))
			break;
	}

	/* Update packets that node is now acknowledging */
	skb_queue_walk_from_safe(&bcl->outqueue, skb, tmp) {
		if (more(buf_seqno(skb), acked))
			break;

		next = tipc_skb_queue_next(&bcl->outqueue, skb);
		if (skb != bcl->next_out) {
			bcbuf_decr_acks(skb);
		} else {
			bcbuf_set_acks(skb, 0);
			bcl->next_out = next;
			bclink_set_last_sent();
		}

		if (bcbuf_acks(skb) == 0) {
			__skb_unlink(skb, &bcl->outqueue);
			kfree_skb(skb);
			released = 1;
		}
	}
	n_ptr->bclink.acked = acked;

	/* Try resolving broadcast link congestion, if necessary */
	if (unlikely(bcl->next_out)) {
		tipc_link_push_packets(bcl);
		bclink_set_last_sent();
	}
	if (unlikely(released && !skb_queue_empty(&bcl->waiting_sks)))
		n_ptr->action_flags |= TIPC_WAKEUP_BCAST_USERS;

exit:
	tipc_bclink_unlock();
}

/**
 * tipc_bclink_update_link_state - update broadcast link state
 *
 * RCU and node lock set
 */
void tipc_bclink_update_link_state(struct tipc_node *n_ptr, u32 last_sent)
{
	struct sk_buff *buf;

	/* Ignore "stale" link state info */
	if (less_eq(last_sent, n_ptr->bclink.last_in))
		return;

	/* Update link synchronization state; quit if in sync */
	bclink_update_last_sent(n_ptr, last_sent);

	if (n_ptr->bclink.last_sent == n_ptr->bclink.last_in)
		return;

	/* Update out-of-sync state; quit if loss is still unconfirmed */
	if ((++n_ptr->bclink.oos_state) == 1) {
		if (n_ptr->bclink.deferred_size < (TIPC_MIN_LINK_WIN / 2))
			return;
		n_ptr->bclink.oos_state++;
	}

	/* Don't NACK if one has been recently sent (or seen) */
	if (n_ptr->bclink.oos_state & 0x1)
		return;

	/* Send NACK */
	buf = tipc_buf_acquire(INT_H_SIZE);
	if (buf) {
		struct tipc_msg *msg = buf_msg(buf);
		struct sk_buff *skb = skb_peek(&n_ptr->bclink.deferred_queue);
		u32 to = skb ? buf_seqno(skb) - 1 : n_ptr->bclink.last_sent;

		tipc_msg_init(msg, BCAST_PROTOCOL, STATE_MSG,
			      INT_H_SIZE, n_ptr->addr);
		msg_set_non_seq(msg, 1);
		msg_set_mc_netid(msg, tipc_net_id);
		msg_set_bcast_ack(msg, n_ptr->bclink.last_in);
		msg_set_bcgap_after(msg, n_ptr->bclink.last_in);
		msg_set_bcgap_to(msg, to);

		tipc_bclink_lock();
		tipc_bearer_send(MAX_BEARERS, buf, NULL);
		bcl->stats.sent_nacks++;
		tipc_bclink_unlock();
		kfree_skb(buf);

		n_ptr->bclink.oos_state++;
	}
}

/**
 * bclink_peek_nack - monitor retransmission requests sent by other nodes
 *
 * Delay any upcoming NACK by this node if another node has already
 * requested the first message this node is going to ask for.
 */
static void bclink_peek_nack(struct tipc_msg *msg)
{
	struct tipc_node *n_ptr = tipc_node_find(msg_destnode(msg));

	if (unlikely(!n_ptr))
		return;

	tipc_node_lock(n_ptr);

	if (n_ptr->bclink.recv_permitted &&
	    (n_ptr->bclink.last_in != n_ptr->bclink.last_sent) &&
	    (n_ptr->bclink.last_in == msg_bcgap_after(msg)))
		n_ptr->bclink.oos_state = 2;

	tipc_node_unlock(n_ptr);
}

/* tipc_bclink_xmit - broadcast buffer chain to all nodes in cluster
 *                    and to identified node local sockets
 * @list: chain of buffers containing message
 * Consumes the buffer chain, except when returning -ELINKCONG
 * Returns 0 if success, otherwise errno: -ELINKCONG,-EHOSTUNREACH,-EMSGSIZE
 */
int tipc_bclink_xmit(struct sk_buff_head *list)
{
	int rc = 0;
	int bc = 0;
	struct sk_buff *skb;

	/* Prepare clone of message for local node */
	skb = tipc_msg_reassemble(list);
	if (unlikely(!skb)) {
		__skb_queue_purge(list);
		return -EHOSTUNREACH;
	}

	/* Broadcast to all other nodes */
	if (likely(bclink)) {
		tipc_bclink_lock();
		if (likely(bclink->bcast_nodes.count)) {
			rc = __tipc_link_xmit(bcl, list);
			if (likely(!rc)) {
				u32 len = skb_queue_len(&bcl->outqueue);

				bclink_set_last_sent();
				bcl->stats.queue_sz_counts++;
				bcl->stats.accu_queue_sz += len;
			}
			bc = 1;
		}
		tipc_bclink_unlock();
	}

	if (unlikely(!bc))
		__skb_queue_purge(list);

	/* Deliver message clone */
	if (likely(!rc))
		tipc_sk_mcast_rcv(skb);
	else
		kfree_skb(skb);

	return rc;
}

/**
 * bclink_accept_pkt - accept an incoming, in-sequence broadcast packet
 *
 * Called with both sending node's lock and bclink_lock taken.
 */
static void bclink_accept_pkt(struct tipc_node *node, u32 seqno)
{
	bclink_update_last_sent(node, seqno);
	node->bclink.last_in = seqno;
	node->bclink.oos_state = 0;
	bcl->stats.recv_info++;

	/*
	 * Unicast an ACK periodically, ensuring that
	 * all nodes in the cluster don't ACK at the same time
	 */
	if (((seqno - tipc_own_addr) % TIPC_MIN_LINK_WIN) == 0) {
		tipc_link_proto_xmit(node->active_links[node->addr & 1],
				     STATE_MSG, 0, 0, 0, 0, 0);
		bcl->stats.sent_acks++;
	}
}

/**
 * tipc_bclink_rcv - receive a broadcast packet, and deliver upwards
 *
 * RCU is locked, no other locks set
 */
void tipc_bclink_rcv(struct sk_buff *buf)
{
	struct tipc_msg *msg = buf_msg(buf);
	struct tipc_node *node;
	u32 next_in;
	u32 seqno;
	int deferred = 0;

	/* Screen out unwanted broadcast messages */
	if (msg_mc_netid(msg) != tipc_net_id)
		goto exit;

	node = tipc_node_find(msg_prevnode(msg));
	if (unlikely(!node))
		goto exit;

	tipc_node_lock(node);
	if (unlikely(!node->bclink.recv_permitted))
		goto unlock;

	/* Handle broadcast protocol message */
	if (unlikely(msg_user(msg) == BCAST_PROTOCOL)) {
		if (msg_type(msg) != STATE_MSG)
			goto unlock;
		if (msg_destnode(msg) == tipc_own_addr) {
			tipc_bclink_acknowledge(node, msg_bcast_ack(msg));
			tipc_node_unlock(node);
			tipc_bclink_lock();
			bcl->stats.recv_nacks++;
			bclink->retransmit_to = node;
			bclink_retransmit_pkt(msg_bcgap_after(msg),
					      msg_bcgap_to(msg));
			tipc_bclink_unlock();
		} else {
			tipc_node_unlock(node);
			bclink_peek_nack(msg);
		}
		goto exit;
	}

	/* Handle in-sequence broadcast message */
	seqno = msg_seqno(msg);
	next_in = mod(node->bclink.last_in + 1);

	if (likely(seqno == next_in)) {
receive:
		/* Deliver message to destination */
		if (likely(msg_isdata(msg))) {
			tipc_bclink_lock();
			bclink_accept_pkt(node, seqno);
			tipc_bclink_unlock();
			tipc_node_unlock(node);
			if (likely(msg_mcast(msg)))
				tipc_sk_mcast_rcv(buf);
			else
				kfree_skb(buf);
		} else if (msg_user(msg) == MSG_BUNDLER) {
			tipc_bclink_lock();
			bclink_accept_pkt(node, seqno);
			bcl->stats.recv_bundles++;
			bcl->stats.recv_bundled += msg_msgcnt(msg);
			tipc_bclink_unlock();
			tipc_node_unlock(node);
			tipc_link_bundle_rcv(buf);
		} else if (msg_user(msg) == MSG_FRAGMENTER) {
			tipc_buf_append(&node->bclink.reasm_buf, &buf);
			if (unlikely(!buf && !node->bclink.reasm_buf))
				goto unlock;
			tipc_bclink_lock();
			bclink_accept_pkt(node, seqno);
			bcl->stats.recv_fragments++;
			if (buf) {
				bcl->stats.recv_fragmented++;
				msg = buf_msg(buf);
				tipc_bclink_unlock();
				goto receive;
			}
			tipc_bclink_unlock();
			tipc_node_unlock(node);
		} else if (msg_user(msg) == NAME_DISTRIBUTOR) {
			tipc_bclink_lock();
			bclink_accept_pkt(node, seqno);
			tipc_bclink_unlock();
			tipc_node_unlock(node);
			tipc_named_rcv(buf);
		} else {
			tipc_bclink_lock();
			bclink_accept_pkt(node, seqno);
			tipc_bclink_unlock();
			tipc_node_unlock(node);
			kfree_skb(buf);
		}
		buf = NULL;

		/* Determine new synchronization state */
		tipc_node_lock(node);
		if (unlikely(!tipc_node_is_up(node)))
			goto unlock;

		if (node->bclink.last_in == node->bclink.last_sent)
			goto unlock;

		if (skb_queue_empty(&node->bclink.deferred_queue)) {
			node->bclink.oos_state = 1;
			goto unlock;
		}

		msg = buf_msg(skb_peek(&node->bclink.deferred_queue));
		seqno = msg_seqno(msg);
		next_in = mod(next_in + 1);
		if (seqno != next_in)
			goto unlock;

		/* Take in-sequence message from deferred queue & deliver it */
		buf = __skb_dequeue(&node->bclink.deferred_queue);
		goto receive;
	}

	/* Handle out-of-sequence broadcast message */
	if (less(next_in, seqno)) {
		deferred = tipc_link_defer_pkt(&node->bclink.deferred_queue,
					       buf);
		bclink_update_last_sent(node, seqno);
		buf = NULL;
	}

	tipc_bclink_lock();

	if (deferred)
		bcl->stats.deferred_recv++;
	else
		bcl->stats.duplicates++;

	tipc_bclink_unlock();

unlock:
	tipc_node_unlock(node);
exit:
	kfree_skb(buf);
}

u32 tipc_bclink_acks_missing(struct tipc_node *n_ptr)
{
	return (n_ptr->bclink.recv_permitted &&
		(tipc_bclink_get_last_sent() != n_ptr->bclink.acked));
}


/**
 * tipc_bcbearer_send - send a packet through the broadcast pseudo-bearer
 *
 * Send packet over as many bearers as necessary to reach all nodes
 * that have joined the broadcast link.
 *
 * Returns 0 (packet sent successfully) under all circumstances,
 * since the broadcast link's pseudo-bearer never blocks
 */
static int tipc_bcbearer_send(struct sk_buff *buf, struct tipc_bearer *unused1,
			      struct tipc_media_addr *unused2)
{
	int bp_index;
	struct tipc_msg *msg = buf_msg(buf);

	/* Prepare broadcast link message for reliable transmission,
	 * if first time trying to send it;
	 * preparation is skipped for broadcast link protocol messages
	 * since they are sent in an unreliable manner and don't need it
	 */
	if (likely(!msg_non_seq(buf_msg(buf)))) {
		bcbuf_set_acks(buf, bclink->bcast_nodes.count);
		msg_set_non_seq(msg, 1);
		msg_set_mc_netid(msg, tipc_net_id);
		bcl->stats.sent_info++;

		if (WARN_ON(!bclink->bcast_nodes.count)) {
			dump_stack();
			return 0;
		}
	}

	/* Send buffer over bearers until all targets reached */
	bcbearer->remains = bclink->bcast_nodes;

	for (bp_index = 0; bp_index < MAX_BEARERS; bp_index++) {
		struct tipc_bearer *p = bcbearer->bpairs[bp_index].primary;
		struct tipc_bearer *s = bcbearer->bpairs[bp_index].secondary;
		struct tipc_bearer *bp[2] = {p, s};
		struct tipc_bearer *b = bp[msg_link_selector(msg)];
		struct sk_buff *tbuf;

		if (!p)
			break; /* No more bearers to try */
		if (!b)
			b = p;
		tipc_nmap_diff(&bcbearer->remains, &b->nodes,
			       &bcbearer->remains_new);
		if (bcbearer->remains_new.count == bcbearer->remains.count)
			continue; /* Nothing added by bearer pair */

		if (bp_index == 0) {
			/* Use original buffer for first bearer */
			tipc_bearer_send(b->identity, buf, &b->bcast_addr);
		} else {
			/* Avoid concurrent buffer access */
			tbuf = pskb_copy(buf, GFP_ATOMIC);
			if (!tbuf)
				break;
			tipc_bearer_send(b->identity, tbuf, &b->bcast_addr);
			kfree_skb(tbuf); /* Bearer keeps a clone */
		}
		if (bcbearer->remains_new.count == 0)
			break; /* All targets reached */

		bcbearer->remains = bcbearer->remains_new;
	}

	return 0;
}

/**
 * tipc_bcbearer_sort - create sets of bearer pairs used by broadcast bearer
 */
void tipc_bcbearer_sort(struct tipc_node_map *nm_ptr, u32 node, bool action)
{
	struct tipc_bcbearer_pair *bp_temp = bcbearer->bpairs_temp;
	struct tipc_bcbearer_pair *bp_curr;
	struct tipc_bearer *b;
	int b_index;
	int pri;

	tipc_bclink_lock();

	if (action)
		tipc_nmap_add(nm_ptr, node);
	else
		tipc_nmap_remove(nm_ptr, node);

	/* Group bearers by priority (can assume max of two per priority) */
	memset(bp_temp, 0, sizeof(bcbearer->bpairs_temp));

	rcu_read_lock();
	for (b_index = 0; b_index < MAX_BEARERS; b_index++) {
		b = rcu_dereference_rtnl(bearer_list[b_index]);
		if (!b || !b->nodes.count)
			continue;

		if (!bp_temp[b->priority].primary)
			bp_temp[b->priority].primary = b;
		else
			bp_temp[b->priority].secondary = b;
	}
	rcu_read_unlock();

	/* Create array of bearer pairs for broadcasting */
	bp_curr = bcbearer->bpairs;
	memset(bcbearer->bpairs, 0, sizeof(bcbearer->bpairs));

	for (pri = TIPC_MAX_LINK_PRI; pri >= 0; pri--) {

		if (!bp_temp[pri].primary)
			continue;

		bp_curr->primary = bp_temp[pri].primary;

		if (bp_temp[pri].secondary) {
			if (tipc_nmap_equal(&bp_temp[pri].primary->nodes,
					    &bp_temp[pri].secondary->nodes)) {
				bp_curr->secondary = bp_temp[pri].secondary;
			} else {
				bp_curr++;
				bp_curr->primary = bp_temp[pri].secondary;
			}
		}

		bp_curr++;
	}

	tipc_bclink_unlock();
}

static int __tipc_nl_add_bc_link_stat(struct sk_buff *skb,
				      struct tipc_stats *stats)
{
	int i;
	struct nlattr *nest;

	struct nla_map {
		__u32 key;
		__u32 val;
	};

	struct nla_map map[] = {
		{TIPC_NLA_STATS_RX_INFO, stats->recv_info},
		{TIPC_NLA_STATS_RX_FRAGMENTS, stats->recv_fragments},
		{TIPC_NLA_STATS_RX_FRAGMENTED, stats->recv_fragmented},
		{TIPC_NLA_STATS_RX_BUNDLES, stats->recv_bundles},
		{TIPC_NLA_STATS_RX_BUNDLED, stats->recv_bundled},
		{TIPC_NLA_STATS_TX_INFO, stats->sent_info},
		{TIPC_NLA_STATS_TX_FRAGMENTS, stats->sent_fragments},
		{TIPC_NLA_STATS_TX_FRAGMENTED, stats->sent_fragmented},
		{TIPC_NLA_STATS_TX_BUNDLES, stats->sent_bundles},
		{TIPC_NLA_STATS_TX_BUNDLED, stats->sent_bundled},
		{TIPC_NLA_STATS_RX_NACKS, stats->recv_nacks},
		{TIPC_NLA_STATS_RX_DEFERRED, stats->deferred_recv},
		{TIPC_NLA_STATS_TX_NACKS, stats->sent_nacks},
		{TIPC_NLA_STATS_TX_ACKS, stats->sent_acks},
		{TIPC_NLA_STATS_RETRANSMITTED, stats->retransmitted},
		{TIPC_NLA_STATS_DUPLICATES, stats->duplicates},
		{TIPC_NLA_STATS_LINK_CONGS, stats->link_congs},
		{TIPC_NLA_STATS_MAX_QUEUE, stats->max_queue_sz},
		{TIPC_NLA_STATS_AVG_QUEUE, stats->queue_sz_counts ?
			(stats->accu_queue_sz / stats->queue_sz_counts) : 0}
	};

	nest = nla_nest_start(skb, TIPC_NLA_LINK_STATS);
	if (!nest)
		return -EMSGSIZE;

	for (i = 0; i <  ARRAY_SIZE(map); i++)
		if (nla_put_u32(skb, map[i].key, map[i].val))
			goto msg_full;

	nla_nest_end(skb, nest);

	return 0;
msg_full:
	nla_nest_cancel(skb, nest);

	return -EMSGSIZE;
}

int tipc_nl_add_bc_link(struct tipc_nl_msg *msg)
{
	int err;
	void *hdr;
	struct nlattr *attrs;
	struct nlattr *prop;

	if (!bcl)
		return 0;

	tipc_bclink_lock();

	hdr = genlmsg_put(msg->skb, msg->portid, msg->seq, &tipc_genl_v2_family,
			  NLM_F_MULTI, TIPC_NL_LINK_GET);
	if (!hdr)
		return -EMSGSIZE;

	attrs = nla_nest_start(msg->skb, TIPC_NLA_LINK);
	if (!attrs)
		goto msg_full;

	/* The broadcast link is always up */
	if (nla_put_flag(msg->skb, TIPC_NLA_LINK_UP))
		goto attr_msg_full;

	if (nla_put_flag(msg->skb, TIPC_NLA_LINK_BROADCAST))
		goto attr_msg_full;
	if (nla_put_string(msg->skb, TIPC_NLA_LINK_NAME, bcl->name))
		goto attr_msg_full;
	if (nla_put_u32(msg->skb, TIPC_NLA_LINK_RX, bcl->next_in_no))
		goto attr_msg_full;
	if (nla_put_u32(msg->skb, TIPC_NLA_LINK_TX, bcl->next_out_no))
		goto attr_msg_full;

	prop = nla_nest_start(msg->skb, TIPC_NLA_LINK_PROP);
	if (!prop)
		goto attr_msg_full;
	if (nla_put_u32(msg->skb, TIPC_NLA_PROP_WIN, bcl->queue_limit[0]))
		goto prop_msg_full;
	nla_nest_end(msg->skb, prop);

	err = __tipc_nl_add_bc_link_stat(msg->skb, &bcl->stats);
	if (err)
		goto attr_msg_full;

	tipc_bclink_unlock();
	nla_nest_end(msg->skb, attrs);
	genlmsg_end(msg->skb, hdr);

	return 0;

prop_msg_full:
	nla_nest_cancel(msg->skb, prop);
attr_msg_full:
	nla_nest_cancel(msg->skb, attrs);
msg_full:
	tipc_bclink_unlock();
	genlmsg_cancel(msg->skb, hdr);

	return -EMSGSIZE;
}

int tipc_bclink_stats(char *buf, const u32 buf_size)
{
	int ret;
	struct tipc_stats *s;

	if (!bcl)
		return 0;

	tipc_bclink_lock();

	s = &bcl->stats;

	ret = tipc_snprintf(buf, buf_size, "Link <%s>\n"
			    "  Window:%u packets\n",
			    bcl->name, bcl->queue_limit[0]);
	ret += tipc_snprintf(buf + ret, buf_size - ret,
			     "  RX packets:%u fragments:%u/%u bundles:%u/%u\n",
			     s->recv_info, s->recv_fragments,
			     s->recv_fragmented, s->recv_bundles,
			     s->recv_bundled);
	ret += tipc_snprintf(buf + ret, buf_size - ret,
			     "  TX packets:%u fragments:%u/%u bundles:%u/%u\n",
			     s->sent_info, s->sent_fragments,
			     s->sent_fragmented, s->sent_bundles,
			     s->sent_bundled);
	ret += tipc_snprintf(buf + ret, buf_size - ret,
			     "  RX naks:%u defs:%u dups:%u\n",
			     s->recv_nacks, s->deferred_recv, s->duplicates);
	ret += tipc_snprintf(buf + ret, buf_size - ret,
			     "  TX naks:%u acks:%u dups:%u\n",
			     s->sent_nacks, s->sent_acks, s->retransmitted);
	ret += tipc_snprintf(buf + ret, buf_size - ret,
			     "  Congestion link:%u  Send queue max:%u avg:%u\n",
			     s->link_congs, s->max_queue_sz,
			     s->queue_sz_counts ?
			     (s->accu_queue_sz / s->queue_sz_counts) : 0);

	tipc_bclink_unlock();
	return ret;
}

int tipc_bclink_reset_stats(void)
{
	if (!bcl)
		return -ENOPROTOOPT;

	tipc_bclink_lock();
	memset(&bcl->stats, 0, sizeof(bcl->stats));
	tipc_bclink_unlock();
	return 0;
}

int tipc_bclink_set_queue_limits(u32 limit)
{
	if (!bcl)
		return -ENOPROTOOPT;
	if ((limit < TIPC_MIN_LINK_WIN) || (limit > TIPC_MAX_LINK_WIN))
		return -EINVAL;

	tipc_bclink_lock();
	tipc_link_set_queue_limits(bcl, limit);
	tipc_bclink_unlock();
	return 0;
}

int tipc_bclink_init(void)
{
	bcbearer = kzalloc(sizeof(*bcbearer), GFP_ATOMIC);
	if (!bcbearer)
		return -ENOMEM;

	bclink = kzalloc(sizeof(*bclink), GFP_ATOMIC);
	if (!bclink) {
		kfree(bcbearer);
		return -ENOMEM;
	}

	bcl = &bclink->link;
	bcbearer->bearer.media = &bcbearer->media;
	bcbearer->media.send_msg = tipc_bcbearer_send;
	sprintf(bcbearer->media.name, "tipc-broadcast");

	spin_lock_init(&bclink->lock);
	__skb_queue_head_init(&bcl->outqueue);
	__skb_queue_head_init(&bcl->deferred_queue);
	skb_queue_head_init(&bcl->waiting_sks);
	bcl->next_out_no = 1;
	spin_lock_init(&bclink->node.lock);
	__skb_queue_head_init(&bclink->node.waiting_sks);
	bcl->owner = &bclink->node;
	bcl->max_pkt = MAX_PKT_DEFAULT_MCAST;
	tipc_link_set_queue_limits(bcl, BCLINK_WIN_DEFAULT);
	bcl->bearer_id = MAX_BEARERS;
	rcu_assign_pointer(bearer_list[MAX_BEARERS], &bcbearer->bearer);
	bcl->state = WORKING_WORKING;
	strlcpy(bcl->name, tipc_bclink_name, TIPC_MAX_LINK_NAME);
	return 0;
}

void tipc_bclink_stop(void)
{
	tipc_bclink_lock();
	tipc_link_purge_queues(bcl);
	tipc_bclink_unlock();

	RCU_INIT_POINTER(bearer_list[BCBEARER], NULL);
	synchronize_net();
	kfree(bcbearer);
	kfree(bclink);
}

/**
 * tipc_nmap_add - add a node to a node map
 */
static void tipc_nmap_add(struct tipc_node_map *nm_ptr, u32 node)
{
	int n = tipc_node(node);
	int w = n / WSIZE;
	u32 mask = (1 << (n % WSIZE));

	if ((nm_ptr->map[w] & mask) == 0) {
		nm_ptr->count++;
		nm_ptr->map[w] |= mask;
	}
}

/**
 * tipc_nmap_remove - remove a node from a node map
 */
static void tipc_nmap_remove(struct tipc_node_map *nm_ptr, u32 node)
{
	int n = tipc_node(node);
	int w = n / WSIZE;
	u32 mask = (1 << (n % WSIZE));

	if ((nm_ptr->map[w] & mask) != 0) {
		nm_ptr->map[w] &= ~mask;
		nm_ptr->count--;
	}
}

/**
 * tipc_nmap_diff - find differences between node maps
 * @nm_a: input node map A
 * @nm_b: input node map B
 * @nm_diff: output node map A-B (i.e. nodes of A that are not in B)
 */
static void tipc_nmap_diff(struct tipc_node_map *nm_a,
			   struct tipc_node_map *nm_b,
			   struct tipc_node_map *nm_diff)
{
	int stop = ARRAY_SIZE(nm_a->map);
	int w;
	int b;
	u32 map;

	memset(nm_diff, 0, sizeof(*nm_diff));
	for (w = 0; w < stop; w++) {
		map = nm_a->map[w] ^ (nm_a->map[w] & nm_b->map[w]);
		nm_diff->map[w] = map;
		if (map != 0) {
			for (b = 0 ; b < WSIZE; b++) {
				if (map & (1 << b))
					nm_diff->count++;
			}
		}
	}
}

/**
 * tipc_port_list_add - add a port to a port list, ensuring no duplicates
 */
void tipc_port_list_add(struct tipc_port_list *pl_ptr, u32 port)
{
	struct tipc_port_list *item = pl_ptr;
	int i;
	int item_sz = PLSIZE;
	int cnt = pl_ptr->count;

	for (; ; cnt -= item_sz, item = item->next) {
		if (cnt < PLSIZE)
			item_sz = cnt;
		for (i = 0; i < item_sz; i++)
			if (item->ports[i] == port)
				return;
		if (i < PLSIZE) {
			item->ports[i] = port;
			pl_ptr->count++;
			return;
		}
		if (!item->next) {
			item->next = kmalloc(sizeof(*item), GFP_ATOMIC);
			if (!item->next) {
				pr_warn("Incomplete multicast delivery, no memory\n");
				return;
			}
			item->next->next = NULL;
		}
	}
}

/**
 * tipc_port_list_free - free dynamically created entries in port_list chain
 *
 */
void tipc_port_list_free(struct tipc_port_list *pl_ptr)
{
	struct tipc_port_list *item;
	struct tipc_port_list *next;

	for (item = pl_ptr->next; item; item = next) {
		next = item->next;
		kfree(item);
	}
}
