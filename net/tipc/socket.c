/*
 * net/tipc/socket.c: TIPC socket API
 *
 * Copyright (c) 2001-2007, 2012-2014, Ericsson AB
 * Copyright (c) 2004-2008, 2010-2013, Wind River Systems
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

#include <linux/rhashtable.h>
#include <linux/jhash.h>
#include "core.h"
#include "name_table.h"
#include "node.h"
#include "link.h"
#include "config.h"
#include "socket.h"

#define SS_LISTENING		-1	/* socket is listening */
#define SS_READY		-2	/* socket is connectionless */

#define CONN_TIMEOUT_DEFAULT	8000	/* default connect timeout = 8s */
#define CONN_PROBING_INTERVAL	3600000	/* [ms] => 1 h */
#define TIPC_FWD_MSG		1
#define TIPC_CONN_OK		0
#define TIPC_CONN_PROBING	1
#define TIPC_MAX_PORT		0xffffffff
#define TIPC_MIN_PORT		1

/**
 * struct tipc_sock - TIPC socket structure
 * @sk: socket - interacts with 'port' and with user via the socket API
 * @connected: non-zero if port is currently connected to a peer port
 * @conn_type: TIPC type used when connection was established
 * @conn_instance: TIPC instance used when connection was established
 * @published: non-zero if port has one or more associated names
 * @max_pkt: maximum packet size "hint" used when building messages sent by port
 * @portid: unique port identity in TIPC socket hash table
 * @phdr: preformatted message header used when sending messages
 * @port_list: adjacent ports in TIPC's global list of ports
 * @publications: list of publications for port
 * @pub_count: total # of publications port has made during its lifetime
 * @probing_state:
 * @probing_interval:
 * @timer:
 * @port: port - interacts with 'sk' and with the rest of the TIPC stack
 * @peer_name: the peer of the connection, if any
 * @conn_timeout: the time we can wait for an unresponded setup request
 * @dupl_rcvcnt: number of bytes counted twice, in both backlog and rcv queue
 * @link_cong: non-zero if owner must sleep because of link congestion
 * @sent_unacked: # messages sent by socket, and not yet acked by peer
 * @rcv_unacked: # messages read by user, but not yet acked back to peer
 * @node: hash table node
 * @rcu: rcu struct for tipc_sock
 */
struct tipc_sock {
	struct sock sk;
	int connected;
	u32 conn_type;
	u32 conn_instance;
	int published;
	u32 max_pkt;
	u32 portid;
	struct tipc_msg phdr;
	struct list_head sock_list;
	struct list_head publications;
	u32 pub_count;
	u32 probing_state;
	u32 probing_interval;
	struct timer_list timer;
	uint conn_timeout;
	atomic_t dupl_rcvcnt;
	bool link_cong;
	uint sent_unacked;
	uint rcv_unacked;
	struct rhash_head node;
	struct rcu_head rcu;
};

static int tipc_backlog_rcv(struct sock *sk, struct sk_buff *skb);
static void tipc_data_ready(struct sock *sk);
static void tipc_write_space(struct sock *sk);
static int tipc_release(struct socket *sock);
static int tipc_accept(struct socket *sock, struct socket *new_sock, int flags);
static int tipc_wait_for_sndmsg(struct socket *sock, long *timeo_p);
static void tipc_sk_timeout(unsigned long portid);
static int tipc_sk_publish(struct tipc_sock *tsk, uint scope,
			   struct tipc_name_seq const *seq);
static int tipc_sk_withdraw(struct tipc_sock *tsk, uint scope,
			    struct tipc_name_seq const *seq);
static struct tipc_sock *tipc_sk_lookup(u32 portid);
static int tipc_sk_insert(struct tipc_sock *tsk);
static void tipc_sk_remove(struct tipc_sock *tsk);

static const struct proto_ops packet_ops;
static const struct proto_ops stream_ops;
static const struct proto_ops msg_ops;

static struct proto tipc_proto;
static struct proto tipc_proto_kern;

static const struct nla_policy tipc_nl_sock_policy[TIPC_NLA_SOCK_MAX + 1] = {
	[TIPC_NLA_SOCK_UNSPEC]		= { .type = NLA_UNSPEC },
	[TIPC_NLA_SOCK_ADDR]		= { .type = NLA_U32 },
	[TIPC_NLA_SOCK_REF]		= { .type = NLA_U32 },
	[TIPC_NLA_SOCK_CON]		= { .type = NLA_NESTED },
	[TIPC_NLA_SOCK_HAS_PUBL]	= { .type = NLA_FLAG }
};

/*
 * Revised TIPC socket locking policy:
 *
 * Most socket operations take the standard socket lock when they start
 * and hold it until they finish (or until they need to sleep).  Acquiring
 * this lock grants the owner exclusive access to the fields of the socket
 * data structures, with the exception of the backlog queue.  A few socket
 * operations can be done without taking the socket lock because they only
 * read socket information that never changes during the life of the socket.
 *
 * Socket operations may acquire the lock for the associated TIPC port if they
 * need to perform an operation on the port.  If any routine needs to acquire
 * both the socket lock and the port lock it must take the socket lock first
 * to avoid the risk of deadlock.
 *
 * The dispatcher handling incoming messages cannot grab the socket lock in
 * the standard fashion, since invoked it runs at the BH level and cannot block.
 * Instead, it checks to see if the socket lock is currently owned by someone,
 * and either handles the message itself or adds it to the socket's backlog
 * queue; in the latter case the queued message is processed once the process
 * owning the socket lock releases it.
 *
 * NOTE: Releasing the socket lock while an operation is sleeping overcomes
 * the problem of a blocked socket operation preventing any other operations
 * from occurring.  However, applications must be careful if they have
 * multiple threads trying to send (or receive) on the same socket, as these
 * operations might interfere with each other.  For example, doing a connect
 * and a receive at the same time might allow the receive to consume the
 * ACK message meant for the connect.  While additional work could be done
 * to try and overcome this, it doesn't seem to be worthwhile at the present.
 *
 * NOTE: Releasing the socket lock while an operation is sleeping also ensures
 * that another operation that must be performed in a non-blocking manner is
 * not delayed for very long because the lock has already been taken.
 *
 * NOTE: This code assumes that certain fields of a port/socket pair are
 * constant over its lifetime; such fields can be examined without taking
 * the socket lock and/or port lock, and do not need to be re-read even
 * after resuming processing after waiting.  These fields include:
 *   - socket type
 *   - pointer to socket sk structure (aka tipc_sock structure)
 *   - pointer to port structure
 *   - port reference
 */

/* Protects tipc socket hash table mutations */
static struct rhashtable tipc_sk_rht;

static u32 tsk_peer_node(struct tipc_sock *tsk)
{
	return msg_destnode(&tsk->phdr);
}

static u32 tsk_peer_port(struct tipc_sock *tsk)
{
	return msg_destport(&tsk->phdr);
}

static  bool tsk_unreliable(struct tipc_sock *tsk)
{
	return msg_src_droppable(&tsk->phdr) != 0;
}

static void tsk_set_unreliable(struct tipc_sock *tsk, bool unreliable)
{
	msg_set_src_droppable(&tsk->phdr, unreliable ? 1 : 0);
}

static bool tsk_unreturnable(struct tipc_sock *tsk)
{
	return msg_dest_droppable(&tsk->phdr) != 0;
}

static void tsk_set_unreturnable(struct tipc_sock *tsk, bool unreturnable)
{
	msg_set_dest_droppable(&tsk->phdr, unreturnable ? 1 : 0);
}

static int tsk_importance(struct tipc_sock *tsk)
{
	return msg_importance(&tsk->phdr);
}

static int tsk_set_importance(struct tipc_sock *tsk, int imp)
{
	if (imp > TIPC_CRITICAL_IMPORTANCE)
		return -EINVAL;
	msg_set_importance(&tsk->phdr, (u32)imp);
	return 0;
}

static struct tipc_sock *tipc_sk(const struct sock *sk)
{
	return container_of(sk, struct tipc_sock, sk);
}

static int tsk_conn_cong(struct tipc_sock *tsk)
{
	return tsk->sent_unacked >= TIPC_FLOWCTRL_WIN;
}

/**
 * tsk_advance_rx_queue - discard first buffer in socket receive queue
 *
 * Caller must hold socket lock
 */
static void tsk_advance_rx_queue(struct sock *sk)
{
	kfree_skb(__skb_dequeue(&sk->sk_receive_queue));
}

/**
 * tsk_rej_rx_queue - reject all buffers in socket receive queue
 *
 * Caller must hold socket lock
 */
static void tsk_rej_rx_queue(struct sock *sk)
{
	struct sk_buff *skb;
	u32 dnode;

	while ((skb = __skb_dequeue(&sk->sk_receive_queue))) {
		if (tipc_msg_reverse(skb, &dnode, TIPC_ERR_NO_PORT))
			tipc_link_xmit_skb(skb, dnode, 0);
	}
}

/* tsk_peer_msg - verify if message was sent by connected port's peer
 *
 * Handles cases where the node's network address has changed from
 * the default of <0.0.0> to its configured setting.
 */
static bool tsk_peer_msg(struct tipc_sock *tsk, struct tipc_msg *msg)
{
	u32 peer_port = tsk_peer_port(tsk);
	u32 orig_node;
	u32 peer_node;

	if (unlikely(!tsk->connected))
		return false;

	if (unlikely(msg_origport(msg) != peer_port))
		return false;

	orig_node = msg_orignode(msg);
	peer_node = tsk_peer_node(tsk);

	if (likely(orig_node == peer_node))
		return true;

	if (!orig_node && (peer_node == tipc_own_addr))
		return true;

	if (!peer_node && (orig_node == tipc_own_addr))
		return true;

	return false;
}

/**
 * tipc_sk_create - create a TIPC socket
 * @net: network namespace (must be default network)
 * @sock: pre-allocated socket structure
 * @protocol: protocol indicator (must be 0)
 * @kern: caused by kernel or by userspace?
 *
 * This routine creates additional data structures used by the TIPC socket,
 * initializes them, and links them together.
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_sk_create(struct net *net, struct socket *sock,
			  int protocol, int kern)
{
	const struct proto_ops *ops;
	socket_state state;
	struct sock *sk;
	struct tipc_sock *tsk;
	struct tipc_msg *msg;

	/* Validate arguments */
	if (unlikely(protocol != 0))
		return -EPROTONOSUPPORT;

	switch (sock->type) {
	case SOCK_STREAM:
		ops = &stream_ops;
		state = SS_UNCONNECTED;
		break;
	case SOCK_SEQPACKET:
		ops = &packet_ops;
		state = SS_UNCONNECTED;
		break;
	case SOCK_DGRAM:
	case SOCK_RDM:
		ops = &msg_ops;
		state = SS_READY;
		break;
	default:
		return -EPROTOTYPE;
	}

	/* Allocate socket's protocol area */
	if (!kern)
		sk = sk_alloc(net, AF_TIPC, GFP_KERNEL, &tipc_proto);
	else
		sk = sk_alloc(net, AF_TIPC, GFP_KERNEL, &tipc_proto_kern);

	if (sk == NULL)
		return -ENOMEM;

	tsk = tipc_sk(sk);
	tsk->max_pkt = MAX_PKT_DEFAULT;
	INIT_LIST_HEAD(&tsk->publications);
	msg = &tsk->phdr;
	tipc_msg_init(msg, TIPC_LOW_IMPORTANCE, TIPC_NAMED_MSG,
		      NAMED_H_SIZE, 0);

	/* Finish initializing socket data structures */
	sock->ops = ops;
	sock->state = state;
	sock_init_data(sock, sk);
	if (tipc_sk_insert(tsk)) {
		pr_warn("Socket create failed; port numbrer exhausted\n");
		return -EINVAL;
	}
	msg_set_origport(msg, tsk->portid);
	k_init_timer(&tsk->timer, (Handler)tipc_sk_timeout, tsk->portid);
	sk->sk_backlog_rcv = tipc_backlog_rcv;
	sk->sk_rcvbuf = sysctl_tipc_rmem[1];
	sk->sk_data_ready = tipc_data_ready;
	sk->sk_write_space = tipc_write_space;
	tsk->conn_timeout = CONN_TIMEOUT_DEFAULT;
	tsk->sent_unacked = 0;
	atomic_set(&tsk->dupl_rcvcnt, 0);

	if (sock->state == SS_READY) {
		tsk_set_unreturnable(tsk, true);
		if (sock->type == SOCK_DGRAM)
			tsk_set_unreliable(tsk, true);
	}
	return 0;
}

/**
 * tipc_sock_create_local - create TIPC socket from inside TIPC module
 * @type: socket type - SOCK_RDM or SOCK_SEQPACKET
 *
 * We cannot use sock_creat_kern here because it bumps module user count.
 * Since socket owner and creator is the same module we must make sure
 * that module count remains zero for module local sockets, otherwise
 * we cannot do rmmod.
 *
 * Returns 0 on success, errno otherwise
 */
int tipc_sock_create_local(int type, struct socket **res)
{
	int rc;

	rc = sock_create_lite(AF_TIPC, type, 0, res);
	if (rc < 0) {
		pr_err("Failed to create kernel socket\n");
		return rc;
	}
	tipc_sk_create(&init_net, *res, 0, 1);

	return 0;
}

/**
 * tipc_sock_release_local - release socket created by tipc_sock_create_local
 * @sock: the socket to be released.
 *
 * Module reference count is not incremented when such sockets are created,
 * so we must keep it from being decremented when they are released.
 */
void tipc_sock_release_local(struct socket *sock)
{
	tipc_release(sock);
	sock->ops = NULL;
	sock_release(sock);
}

/**
 * tipc_sock_accept_local - accept a connection on a socket created
 * with tipc_sock_create_local. Use this function to avoid that
 * module reference count is inadvertently incremented.
 *
 * @sock:    the accepting socket
 * @newsock: reference to the new socket to be created
 * @flags:   socket flags
 */

int tipc_sock_accept_local(struct socket *sock, struct socket **newsock,
			   int flags)
{
	struct sock *sk = sock->sk;
	int ret;

	ret = sock_create_lite(sk->sk_family, sk->sk_type,
			       sk->sk_protocol, newsock);
	if (ret < 0)
		return ret;

	ret = tipc_accept(sock, *newsock, flags);
	if (ret < 0) {
		sock_release(*newsock);
		return ret;
	}
	(*newsock)->ops = sock->ops;
	return ret;
}

static void tipc_sk_callback(struct rcu_head *head)
{
	struct tipc_sock *tsk = container_of(head, struct tipc_sock, rcu);

	sock_put(&tsk->sk);
}

/**
 * tipc_release - destroy a TIPC socket
 * @sock: socket to destroy
 *
 * This routine cleans up any messages that are still queued on the socket.
 * For DGRAM and RDM socket types, all queued messages are rejected.
 * For SEQPACKET and STREAM socket types, the first message is rejected
 * and any others are discarded.  (If the first message on a STREAM socket
 * is partially-read, it is discarded and the next one is rejected instead.)
 *
 * NOTE: Rejected messages are not necessarily returned to the sender!  They
 * are returned or discarded according to the "destination droppable" setting
 * specified for the message by the sender.
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk;
	struct sk_buff *skb;
	u32 dnode;

	/*
	 * Exit if socket isn't fully initialized (occurs when a failed accept()
	 * releases a pre-allocated child socket that was never used)
	 */
	if (sk == NULL)
		return 0;

	tsk = tipc_sk(sk);
	lock_sock(sk);

	/*
	 * Reject all unreceived messages, except on an active connection
	 * (which disconnects locally & sends a 'FIN+' to peer)
	 */
	dnode = tsk_peer_node(tsk);
	while (sock->state != SS_DISCONNECTING) {
		skb = __skb_dequeue(&sk->sk_receive_queue);
		if (skb == NULL)
			break;
		if (TIPC_SKB_CB(skb)->handle != NULL)
			kfree_skb(skb);
		else {
			if ((sock->state == SS_CONNECTING) ||
			    (sock->state == SS_CONNECTED)) {
				sock->state = SS_DISCONNECTING;
				tsk->connected = 0;
				tipc_node_remove_conn(dnode, tsk->portid);
			}
			if (tipc_msg_reverse(skb, &dnode, TIPC_ERR_NO_PORT))
				tipc_link_xmit_skb(skb, dnode, 0);
		}
	}

	tipc_sk_withdraw(tsk, 0, NULL);
	k_cancel_timer(&tsk->timer);
	tipc_sk_remove(tsk);
	if (tsk->connected) {
		skb = tipc_msg_create(TIPC_CRITICAL_IMPORTANCE, TIPC_CONN_MSG,
				      SHORT_H_SIZE, 0, dnode, tipc_own_addr,
				      tsk_peer_port(tsk),
				      tsk->portid, TIPC_ERR_NO_PORT);
		if (skb)
			tipc_link_xmit_skb(skb, dnode, tsk->portid);
		tipc_node_remove_conn(dnode, tsk->portid);
	}
	k_term_timer(&tsk->timer);

	/* Discard any remaining (connection-based) messages in receive queue */
	__skb_queue_purge(&sk->sk_receive_queue);

	/* Reject any messages that accumulated in backlog queue */
	sock->state = SS_DISCONNECTING;
	release_sock(sk);

	call_rcu(&tsk->rcu, tipc_sk_callback);
	sock->sk = NULL;

	return 0;
}

/**
 * tipc_bind - associate or disassocate TIPC name(s) with a socket
 * @sock: socket structure
 * @uaddr: socket address describing name(s) and desired operation
 * @uaddr_len: size of socket address data structure
 *
 * Name and name sequence binding is indicated using a positive scope value;
 * a negative scope value unbinds the specified name.  Specifying no name
 * (i.e. a socket address length of 0) unbinds all names from the socket.
 *
 * Returns 0 on success, errno otherwise
 *
 * NOTE: This routine doesn't need to take the socket lock since it doesn't
 *       access any non-constant socket information.
 */
static int tipc_bind(struct socket *sock, struct sockaddr *uaddr,
		     int uaddr_len)
{
	struct sock *sk = sock->sk;
	struct sockaddr_tipc *addr = (struct sockaddr_tipc *)uaddr;
	struct tipc_sock *tsk = tipc_sk(sk);
	int res = -EINVAL;

	lock_sock(sk);
	if (unlikely(!uaddr_len)) {
		res = tipc_sk_withdraw(tsk, 0, NULL);
		goto exit;
	}

	if (uaddr_len < sizeof(struct sockaddr_tipc)) {
		res = -EINVAL;
		goto exit;
	}
	if (addr->family != AF_TIPC) {
		res = -EAFNOSUPPORT;
		goto exit;
	}

	if (addr->addrtype == TIPC_ADDR_NAME)
		addr->addr.nameseq.upper = addr->addr.nameseq.lower;
	else if (addr->addrtype != TIPC_ADDR_NAMESEQ) {
		res = -EAFNOSUPPORT;
		goto exit;
	}

	if ((addr->addr.nameseq.type < TIPC_RESERVED_TYPES) &&
	    (addr->addr.nameseq.type != TIPC_TOP_SRV) &&
	    (addr->addr.nameseq.type != TIPC_CFG_SRV)) {
		res = -EACCES;
		goto exit;
	}

	res = (addr->scope > 0) ?
		tipc_sk_publish(tsk, addr->scope, &addr->addr.nameseq) :
		tipc_sk_withdraw(tsk, -addr->scope, &addr->addr.nameseq);
exit:
	release_sock(sk);
	return res;
}

/**
 * tipc_getname - get port ID of socket or peer socket
 * @sock: socket structure
 * @uaddr: area for returned socket address
 * @uaddr_len: area for returned length of socket address
 * @peer: 0 = own ID, 1 = current peer ID, 2 = current/former peer ID
 *
 * Returns 0 on success, errno otherwise
 *
 * NOTE: This routine doesn't need to take the socket lock since it only
 *       accesses socket information that is unchanging (or which changes in
 *       a completely predictable manner).
 */
static int tipc_getname(struct socket *sock, struct sockaddr *uaddr,
			int *uaddr_len, int peer)
{
	struct sockaddr_tipc *addr = (struct sockaddr_tipc *)uaddr;
	struct tipc_sock *tsk = tipc_sk(sock->sk);

	memset(addr, 0, sizeof(*addr));
	if (peer) {
		if ((sock->state != SS_CONNECTED) &&
			((peer != 2) || (sock->state != SS_DISCONNECTING)))
			return -ENOTCONN;
		addr->addr.id.ref = tsk_peer_port(tsk);
		addr->addr.id.node = tsk_peer_node(tsk);
	} else {
		addr->addr.id.ref = tsk->portid;
		addr->addr.id.node = tipc_own_addr;
	}

	*uaddr_len = sizeof(*addr);
	addr->addrtype = TIPC_ADDR_ID;
	addr->family = AF_TIPC;
	addr->scope = 0;
	addr->addr.name.domain = 0;

	return 0;
}

/**
 * tipc_poll - read and possibly block on pollmask
 * @file: file structure associated with the socket
 * @sock: socket for which to calculate the poll bits
 * @wait: ???
 *
 * Returns pollmask value
 *
 * COMMENTARY:
 * It appears that the usual socket locking mechanisms are not useful here
 * since the pollmask info is potentially out-of-date the moment this routine
 * exits.  TCP and other protocols seem to rely on higher level poll routines
 * to handle any preventable race conditions, so TIPC will do the same ...
 *
 * TIPC sets the returned events as follows:
 *
 * socket state		flags set
 * ------------		---------
 * unconnected		no read flags
 *			POLLOUT if port is not congested
 *
 * connecting		POLLIN/POLLRDNORM if ACK/NACK in rx queue
 *			no write flags
 *
 * connected		POLLIN/POLLRDNORM if data in rx queue
 *			POLLOUT if port is not congested
 *
 * disconnecting	POLLIN/POLLRDNORM/POLLHUP
 *			no write flags
 *
 * listening		POLLIN if SYN in rx queue
 *			no write flags
 *
 * ready		POLLIN/POLLRDNORM if data in rx queue
 * [connectionless]	POLLOUT (since port cannot be congested)
 *
 * IMPORTANT: The fact that a read or write operation is indicated does NOT
 * imply that the operation will succeed, merely that it should be performed
 * and will not block.
 */
static unsigned int tipc_poll(struct file *file, struct socket *sock,
			      poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	u32 mask = 0;

	sock_poll_wait(file, sk_sleep(sk), wait);

	switch ((int)sock->state) {
	case SS_UNCONNECTED:
		if (!tsk->link_cong)
			mask |= POLLOUT;
		break;
	case SS_READY:
	case SS_CONNECTED:
		if (!tsk->link_cong && !tsk_conn_cong(tsk))
			mask |= POLLOUT;
		/* fall thru' */
	case SS_CONNECTING:
	case SS_LISTENING:
		if (!skb_queue_empty(&sk->sk_receive_queue))
			mask |= (POLLIN | POLLRDNORM);
		break;
	case SS_DISCONNECTING:
		mask = (POLLIN | POLLRDNORM | POLLHUP);
		break;
	}

	return mask;
}

/**
 * tipc_sendmcast - send multicast message
 * @sock: socket structure
 * @seq: destination address
 * @msg: message to send
 * @dsz: total length of message data
 * @timeo: timeout to wait for wakeup
 *
 * Called from function tipc_sendmsg(), which has done all sanity checks
 * Returns the number of bytes sent on success, or errno
 */
static int tipc_sendmcast(struct  socket *sock, struct tipc_name_seq *seq,
			  struct msghdr *msg, size_t dsz, long timeo)
{
	struct sock *sk = sock->sk;
	struct tipc_msg *mhdr = &tipc_sk(sk)->phdr;
	struct sk_buff_head head;
	uint mtu;
	int rc;

	msg_set_type(mhdr, TIPC_MCAST_MSG);
	msg_set_lookup_scope(mhdr, TIPC_CLUSTER_SCOPE);
	msg_set_destport(mhdr, 0);
	msg_set_destnode(mhdr, 0);
	msg_set_nametype(mhdr, seq->type);
	msg_set_namelower(mhdr, seq->lower);
	msg_set_nameupper(mhdr, seq->upper);
	msg_set_hdr_sz(mhdr, MCAST_H_SIZE);

new_mtu:
	mtu = tipc_bclink_get_mtu();
	__skb_queue_head_init(&head);
	rc = tipc_msg_build(mhdr, msg, 0, dsz, mtu, &head);
	if (unlikely(rc < 0))
		return rc;

	do {
		rc = tipc_bclink_xmit(&head);
		if (likely(rc >= 0)) {
			rc = dsz;
			break;
		}
		if (rc == -EMSGSIZE)
			goto new_mtu;
		if (rc != -ELINKCONG)
			break;
		tipc_sk(sk)->link_cong = 1;
		rc = tipc_wait_for_sndmsg(sock, &timeo);
		if (rc)
			__skb_queue_purge(&head);
	} while (!rc);
	return rc;
}

/* tipc_sk_mcast_rcv - Deliver multicast message to all destination sockets
 */
void tipc_sk_mcast_rcv(struct sk_buff *buf)
{
	struct tipc_msg *msg = buf_msg(buf);
	struct tipc_port_list dports = {0, NULL, };
	struct tipc_port_list *item;
	struct sk_buff *b;
	uint i, last, dst = 0;
	u32 scope = TIPC_CLUSTER_SCOPE;

	if (in_own_node(msg_orignode(msg)))
		scope = TIPC_NODE_SCOPE;

	/* Create destination port list: */
	tipc_nametbl_mc_translate(msg_nametype(msg),
				  msg_namelower(msg),
				  msg_nameupper(msg),
				  scope,
				  &dports);
	last = dports.count;
	if (!last) {
		kfree_skb(buf);
		return;
	}

	for (item = &dports; item; item = item->next) {
		for (i = 0; i < PLSIZE && ++dst <= last; i++) {
			b = (dst != last) ? skb_clone(buf, GFP_ATOMIC) : buf;
			if (!b) {
				pr_warn("Failed do clone mcast rcv buffer\n");
				continue;
			}
			msg_set_destport(msg, item->ports[i]);
			tipc_sk_rcv(b);
		}
	}
	tipc_port_list_free(&dports);
}

/**
 * tipc_sk_proto_rcv - receive a connection mng protocol message
 * @tsk: receiving socket
 * @dnode: node to send response message to, if any
 * @buf: buffer containing protocol message
 * Returns 0 (TIPC_OK) if message was consumed, 1 (TIPC_FWD_MSG) if
 * (CONN_PROBE_REPLY) message should be forwarded.
 */
static int tipc_sk_proto_rcv(struct tipc_sock *tsk, u32 *dnode,
			     struct sk_buff *buf)
{
	struct tipc_msg *msg = buf_msg(buf);
	int conn_cong;

	/* Ignore if connection cannot be validated: */
	if (!tsk_peer_msg(tsk, msg))
		goto exit;

	tsk->probing_state = TIPC_CONN_OK;

	if (msg_type(msg) == CONN_ACK) {
		conn_cong = tsk_conn_cong(tsk);
		tsk->sent_unacked -= msg_msgcnt(msg);
		if (conn_cong)
			tsk->sk.sk_write_space(&tsk->sk);
	} else if (msg_type(msg) == CONN_PROBE) {
		if (!tipc_msg_reverse(buf, dnode, TIPC_OK))
			return TIPC_OK;
		msg_set_type(msg, CONN_PROBE_REPLY);
		return TIPC_FWD_MSG;
	}
	/* Do nothing if msg_type() == CONN_PROBE_REPLY */
exit:
	kfree_skb(buf);
	return TIPC_OK;
}

static int tipc_wait_for_sndmsg(struct socket *sock, long *timeo_p)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	DEFINE_WAIT(wait);
	int done;

	do {
		int err = sock_error(sk);
		if (err)
			return err;
		if (sock->state == SS_DISCONNECTING)
			return -EPIPE;
		if (!*timeo_p)
			return -EAGAIN;
		if (signal_pending(current))
			return sock_intr_errno(*timeo_p);

		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		done = sk_wait_event(sk, timeo_p, !tsk->link_cong);
		finish_wait(sk_sleep(sk), &wait);
	} while (!done);
	return 0;
}

/**
 * tipc_sendmsg - send message in connectionless manner
 * @iocb: if NULL, indicates that socket lock is already held
 * @sock: socket structure
 * @m: message to send
 * @dsz: amount of user data to be sent
 *
 * Message must have an destination specified explicitly.
 * Used for SOCK_RDM and SOCK_DGRAM messages,
 * and for 'SYN' messages on SOCK_SEQPACKET and SOCK_STREAM connections.
 * (Note: 'SYN+' is prohibited on SOCK_STREAM.)
 *
 * Returns the number of bytes sent on success, or errno otherwise
 */
static int tipc_sendmsg(struct kiocb *iocb, struct socket *sock,
			struct msghdr *m, size_t dsz)
{
	DECLARE_SOCKADDR(struct sockaddr_tipc *, dest, m->msg_name);
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_msg *mhdr = &tsk->phdr;
	u32 dnode, dport;
	struct sk_buff_head head;
	struct sk_buff *skb;
	struct tipc_name_seq *seq = &dest->addr.nameseq;
	u32 mtu;
	long timeo;
	int rc;

	if (unlikely(!dest))
		return -EDESTADDRREQ;

	if (unlikely((m->msg_namelen < sizeof(*dest)) ||
		     (dest->family != AF_TIPC)))
		return -EINVAL;

	if (dsz > TIPC_MAX_USER_MSG_SIZE)
		return -EMSGSIZE;

	if (iocb)
		lock_sock(sk);

	if (unlikely(sock->state != SS_READY)) {
		if (sock->state == SS_LISTENING) {
			rc = -EPIPE;
			goto exit;
		}
		if (sock->state != SS_UNCONNECTED) {
			rc = -EISCONN;
			goto exit;
		}
		if (tsk->published) {
			rc = -EOPNOTSUPP;
			goto exit;
		}
		if (dest->addrtype == TIPC_ADDR_NAME) {
			tsk->conn_type = dest->addr.name.name.type;
			tsk->conn_instance = dest->addr.name.name.instance;
		}
	}

	timeo = sock_sndtimeo(sk, m->msg_flags & MSG_DONTWAIT);

	if (dest->addrtype == TIPC_ADDR_MCAST) {
		rc = tipc_sendmcast(sock, seq, m, dsz, timeo);
		goto exit;
	} else if (dest->addrtype == TIPC_ADDR_NAME) {
		u32 type = dest->addr.name.name.type;
		u32 inst = dest->addr.name.name.instance;
		u32 domain = dest->addr.name.domain;

		dnode = domain;
		msg_set_type(mhdr, TIPC_NAMED_MSG);
		msg_set_hdr_sz(mhdr, NAMED_H_SIZE);
		msg_set_nametype(mhdr, type);
		msg_set_nameinst(mhdr, inst);
		msg_set_lookup_scope(mhdr, tipc_addr_scope(domain));
		dport = tipc_nametbl_translate(type, inst, &dnode);
		msg_set_destnode(mhdr, dnode);
		msg_set_destport(mhdr, dport);
		if (unlikely(!dport && !dnode)) {
			rc = -EHOSTUNREACH;
			goto exit;
		}
	} else if (dest->addrtype == TIPC_ADDR_ID) {
		dnode = dest->addr.id.node;
		msg_set_type(mhdr, TIPC_DIRECT_MSG);
		msg_set_lookup_scope(mhdr, 0);
		msg_set_destnode(mhdr, dnode);
		msg_set_destport(mhdr, dest->addr.id.ref);
		msg_set_hdr_sz(mhdr, BASIC_H_SIZE);
	}

new_mtu:
	mtu = tipc_node_get_mtu(dnode, tsk->portid);
	__skb_queue_head_init(&head);
	rc = tipc_msg_build(mhdr, m, 0, dsz, mtu, &head);
	if (rc < 0)
		goto exit;

	do {
		skb = skb_peek(&head);
		TIPC_SKB_CB(skb)->wakeup_pending = tsk->link_cong;
		rc = tipc_link_xmit(&head, dnode, tsk->portid);
		if (likely(rc >= 0)) {
			if (sock->state != SS_READY)
				sock->state = SS_CONNECTING;
			rc = dsz;
			break;
		}
		if (rc == -EMSGSIZE)
			goto new_mtu;
		if (rc != -ELINKCONG)
			break;
		tsk->link_cong = 1;
		rc = tipc_wait_for_sndmsg(sock, &timeo);
		if (rc)
			__skb_queue_purge(&head);
	} while (!rc);
exit:
	if (iocb)
		release_sock(sk);

	return rc;
}

static int tipc_wait_for_sndpkt(struct socket *sock, long *timeo_p)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	DEFINE_WAIT(wait);
	int done;

	do {
		int err = sock_error(sk);
		if (err)
			return err;
		if (sock->state == SS_DISCONNECTING)
			return -EPIPE;
		else if (sock->state != SS_CONNECTED)
			return -ENOTCONN;
		if (!*timeo_p)
			return -EAGAIN;
		if (signal_pending(current))
			return sock_intr_errno(*timeo_p);

		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		done = sk_wait_event(sk, timeo_p,
				     (!tsk->link_cong &&
				      !tsk_conn_cong(tsk)) ||
				     !tsk->connected);
		finish_wait(sk_sleep(sk), &wait);
	} while (!done);
	return 0;
}

/**
 * tipc_send_stream - send stream-oriented data
 * @iocb: (unused)
 * @sock: socket structure
 * @m: data to send
 * @dsz: total length of data to be transmitted
 *
 * Used for SOCK_STREAM data.
 *
 * Returns the number of bytes sent on success (or partial success),
 * or errno if no data sent
 */
static int tipc_send_stream(struct kiocb *iocb, struct socket *sock,
			    struct msghdr *m, size_t dsz)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_msg *mhdr = &tsk->phdr;
	struct sk_buff_head head;
	DECLARE_SOCKADDR(struct sockaddr_tipc *, dest, m->msg_name);
	u32 portid = tsk->portid;
	int rc = -EINVAL;
	long timeo;
	u32 dnode;
	uint mtu, send, sent = 0;

	/* Handle implied connection establishment */
	if (unlikely(dest)) {
		rc = tipc_sendmsg(iocb, sock, m, dsz);
		if (dsz && (dsz == rc))
			tsk->sent_unacked = 1;
		return rc;
	}
	if (dsz > (uint)INT_MAX)
		return -EMSGSIZE;

	if (iocb)
		lock_sock(sk);

	if (unlikely(sock->state != SS_CONNECTED)) {
		if (sock->state == SS_DISCONNECTING)
			rc = -EPIPE;
		else
			rc = -ENOTCONN;
		goto exit;
	}

	timeo = sock_sndtimeo(sk, m->msg_flags & MSG_DONTWAIT);
	dnode = tsk_peer_node(tsk);

next:
	mtu = tsk->max_pkt;
	send = min_t(uint, dsz - sent, TIPC_MAX_USER_MSG_SIZE);
	__skb_queue_head_init(&head);
	rc = tipc_msg_build(mhdr, m, sent, send, mtu, &head);
	if (unlikely(rc < 0))
		goto exit;
	do {
		if (likely(!tsk_conn_cong(tsk))) {
			rc = tipc_link_xmit(&head, dnode, portid);
			if (likely(!rc)) {
				tsk->sent_unacked++;
				sent += send;
				if (sent == dsz)
					break;
				goto next;
			}
			if (rc == -EMSGSIZE) {
				tsk->max_pkt = tipc_node_get_mtu(dnode, portid);
				goto next;
			}
			if (rc != -ELINKCONG)
				break;
			tsk->link_cong = 1;
		}
		rc = tipc_wait_for_sndpkt(sock, &timeo);
		if (rc)
			__skb_queue_purge(&head);
	} while (!rc);
exit:
	if (iocb)
		release_sock(sk);
	return sent ? sent : rc;
}

/**
 * tipc_send_packet - send a connection-oriented message
 * @iocb: if NULL, indicates that socket lock is already held
 * @sock: socket structure
 * @m: message to send
 * @dsz: length of data to be transmitted
 *
 * Used for SOCK_SEQPACKET messages.
 *
 * Returns the number of bytes sent on success, or errno otherwise
 */
static int tipc_send_packet(struct kiocb *iocb, struct socket *sock,
			    struct msghdr *m, size_t dsz)
{
	if (dsz > TIPC_MAX_USER_MSG_SIZE)
		return -EMSGSIZE;

	return tipc_send_stream(iocb, sock, m, dsz);
}

/* tipc_sk_finish_conn - complete the setup of a connection
 */
static void tipc_sk_finish_conn(struct tipc_sock *tsk, u32 peer_port,
				u32 peer_node)
{
	struct tipc_msg *msg = &tsk->phdr;

	msg_set_destnode(msg, peer_node);
	msg_set_destport(msg, peer_port);
	msg_set_type(msg, TIPC_CONN_MSG);
	msg_set_lookup_scope(msg, 0);
	msg_set_hdr_sz(msg, SHORT_H_SIZE);

	tsk->probing_interval = CONN_PROBING_INTERVAL;
	tsk->probing_state = TIPC_CONN_OK;
	tsk->connected = 1;
	k_start_timer(&tsk->timer, tsk->probing_interval);
	tipc_node_add_conn(peer_node, tsk->portid, peer_port);
	tsk->max_pkt = tipc_node_get_mtu(peer_node, tsk->portid);
}

/**
 * set_orig_addr - capture sender's address for received message
 * @m: descriptor for message info
 * @msg: received message header
 *
 * Note: Address is not captured if not requested by receiver.
 */
static void set_orig_addr(struct msghdr *m, struct tipc_msg *msg)
{
	DECLARE_SOCKADDR(struct sockaddr_tipc *, addr, m->msg_name);

	if (addr) {
		addr->family = AF_TIPC;
		addr->addrtype = TIPC_ADDR_ID;
		memset(&addr->addr, 0, sizeof(addr->addr));
		addr->addr.id.ref = msg_origport(msg);
		addr->addr.id.node = msg_orignode(msg);
		addr->addr.name.domain = 0;	/* could leave uninitialized */
		addr->scope = 0;		/* could leave uninitialized */
		m->msg_namelen = sizeof(struct sockaddr_tipc);
	}
}

/**
 * tipc_sk_anc_data_recv - optionally capture ancillary data for received message
 * @m: descriptor for message info
 * @msg: received message header
 * @tsk: TIPC port associated with message
 *
 * Note: Ancillary data is not captured if not requested by receiver.
 *
 * Returns 0 if successful, otherwise errno
 */
static int tipc_sk_anc_data_recv(struct msghdr *m, struct tipc_msg *msg,
				 struct tipc_sock *tsk)
{
	u32 anc_data[3];
	u32 err;
	u32 dest_type;
	int has_name;
	int res;

	if (likely(m->msg_controllen == 0))
		return 0;

	/* Optionally capture errored message object(s) */
	err = msg ? msg_errcode(msg) : 0;
	if (unlikely(err)) {
		anc_data[0] = err;
		anc_data[1] = msg_data_sz(msg);
		res = put_cmsg(m, SOL_TIPC, TIPC_ERRINFO, 8, anc_data);
		if (res)
			return res;
		if (anc_data[1]) {
			res = put_cmsg(m, SOL_TIPC, TIPC_RETDATA, anc_data[1],
				       msg_data(msg));
			if (res)
				return res;
		}
	}

	/* Optionally capture message destination object */
	dest_type = msg ? msg_type(msg) : TIPC_DIRECT_MSG;
	switch (dest_type) {
	case TIPC_NAMED_MSG:
		has_name = 1;
		anc_data[0] = msg_nametype(msg);
		anc_data[1] = msg_namelower(msg);
		anc_data[2] = msg_namelower(msg);
		break;
	case TIPC_MCAST_MSG:
		has_name = 1;
		anc_data[0] = msg_nametype(msg);
		anc_data[1] = msg_namelower(msg);
		anc_data[2] = msg_nameupper(msg);
		break;
	case TIPC_CONN_MSG:
		has_name = (tsk->conn_type != 0);
		anc_data[0] = tsk->conn_type;
		anc_data[1] = tsk->conn_instance;
		anc_data[2] = tsk->conn_instance;
		break;
	default:
		has_name = 0;
	}
	if (has_name) {
		res = put_cmsg(m, SOL_TIPC, TIPC_DESTNAME, 12, anc_data);
		if (res)
			return res;
	}

	return 0;
}

static void tipc_sk_send_ack(struct tipc_sock *tsk, uint ack)
{
	struct sk_buff *skb = NULL;
	struct tipc_msg *msg;
	u32 peer_port = tsk_peer_port(tsk);
	u32 dnode = tsk_peer_node(tsk);

	if (!tsk->connected)
		return;
	skb = tipc_msg_create(CONN_MANAGER, CONN_ACK, INT_H_SIZE, 0, dnode,
			      tipc_own_addr, peer_port, tsk->portid, TIPC_OK);
	if (!skb)
		return;
	msg = buf_msg(skb);
	msg_set_msgcnt(msg, ack);
	tipc_link_xmit_skb(skb, dnode, msg_link_selector(msg));
}

static int tipc_wait_for_rcvmsg(struct socket *sock, long *timeop)
{
	struct sock *sk = sock->sk;
	DEFINE_WAIT(wait);
	long timeo = *timeop;
	int err;

	for (;;) {
		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		if (timeo && skb_queue_empty(&sk->sk_receive_queue)) {
			if (sock->state == SS_DISCONNECTING) {
				err = -ENOTCONN;
				break;
			}
			release_sock(sk);
			timeo = schedule_timeout(timeo);
			lock_sock(sk);
		}
		err = 0;
		if (!skb_queue_empty(&sk->sk_receive_queue))
			break;
		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			break;
		err = -EAGAIN;
		if (!timeo)
			break;
	}
	finish_wait(sk_sleep(sk), &wait);
	*timeop = timeo;
	return err;
}

/**
 * tipc_recvmsg - receive packet-oriented message
 * @iocb: (unused)
 * @m: descriptor for message info
 * @buf_len: total size of user buffer area
 * @flags: receive flags
 *
 * Used for SOCK_DGRAM, SOCK_RDM, and SOCK_SEQPACKET messages.
 * If the complete message doesn't fit in user area, truncate it.
 *
 * Returns size of returned message data, errno otherwise
 */
static int tipc_recvmsg(struct kiocb *iocb, struct socket *sock,
			struct msghdr *m, size_t buf_len, int flags)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct sk_buff *buf;
	struct tipc_msg *msg;
	long timeo;
	unsigned int sz;
	u32 err;
	int res;

	/* Catch invalid receive requests */
	if (unlikely(!buf_len))
		return -EINVAL;

	lock_sock(sk);

	if (unlikely(sock->state == SS_UNCONNECTED)) {
		res = -ENOTCONN;
		goto exit;
	}

	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
restart:

	/* Look for a message in receive queue; wait if necessary */
	res = tipc_wait_for_rcvmsg(sock, &timeo);
	if (res)
		goto exit;

	/* Look at first message in receive queue */
	buf = skb_peek(&sk->sk_receive_queue);
	msg = buf_msg(buf);
	sz = msg_data_sz(msg);
	err = msg_errcode(msg);

	/* Discard an empty non-errored message & try again */
	if ((!sz) && (!err)) {
		tsk_advance_rx_queue(sk);
		goto restart;
	}

	/* Capture sender's address (optional) */
	set_orig_addr(m, msg);

	/* Capture ancillary data (optional) */
	res = tipc_sk_anc_data_recv(m, msg, tsk);
	if (res)
		goto exit;

	/* Capture message data (if valid) & compute return value (always) */
	if (!err) {
		if (unlikely(buf_len < sz)) {
			sz = buf_len;
			m->msg_flags |= MSG_TRUNC;
		}
		res = skb_copy_datagram_msg(buf, msg_hdr_sz(msg), m, sz);
		if (res)
			goto exit;
		res = sz;
	} else {
		if ((sock->state == SS_READY) ||
		    ((err == TIPC_CONN_SHUTDOWN) || m->msg_control))
			res = 0;
		else
			res = -ECONNRESET;
	}

	/* Consume received message (optional) */
	if (likely(!(flags & MSG_PEEK))) {
		if ((sock->state != SS_READY) &&
		    (++tsk->rcv_unacked >= TIPC_CONNACK_INTV)) {
			tipc_sk_send_ack(tsk, tsk->rcv_unacked);
			tsk->rcv_unacked = 0;
		}
		tsk_advance_rx_queue(sk);
	}
exit:
	release_sock(sk);
	return res;
}

/**
 * tipc_recv_stream - receive stream-oriented data
 * @iocb: (unused)
 * @m: descriptor for message info
 * @buf_len: total size of user buffer area
 * @flags: receive flags
 *
 * Used for SOCK_STREAM messages only.  If not enough data is available
 * will optionally wait for more; never truncates data.
 *
 * Returns size of returned message data, errno otherwise
 */
static int tipc_recv_stream(struct kiocb *iocb, struct socket *sock,
			    struct msghdr *m, size_t buf_len, int flags)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct sk_buff *buf;
	struct tipc_msg *msg;
	long timeo;
	unsigned int sz;
	int sz_to_copy, target, needed;
	int sz_copied = 0;
	u32 err;
	int res = 0;

	/* Catch invalid receive attempts */
	if (unlikely(!buf_len))
		return -EINVAL;

	lock_sock(sk);

	if (unlikely(sock->state == SS_UNCONNECTED)) {
		res = -ENOTCONN;
		goto exit;
	}

	target = sock_rcvlowat(sk, flags & MSG_WAITALL, buf_len);
	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

restart:
	/* Look for a message in receive queue; wait if necessary */
	res = tipc_wait_for_rcvmsg(sock, &timeo);
	if (res)
		goto exit;

	/* Look at first message in receive queue */
	buf = skb_peek(&sk->sk_receive_queue);
	msg = buf_msg(buf);
	sz = msg_data_sz(msg);
	err = msg_errcode(msg);

	/* Discard an empty non-errored message & try again */
	if ((!sz) && (!err)) {
		tsk_advance_rx_queue(sk);
		goto restart;
	}

	/* Optionally capture sender's address & ancillary data of first msg */
	if (sz_copied == 0) {
		set_orig_addr(m, msg);
		res = tipc_sk_anc_data_recv(m, msg, tsk);
		if (res)
			goto exit;
	}

	/* Capture message data (if valid) & compute return value (always) */
	if (!err) {
		u32 offset = (u32)(unsigned long)(TIPC_SKB_CB(buf)->handle);

		sz -= offset;
		needed = (buf_len - sz_copied);
		sz_to_copy = (sz <= needed) ? sz : needed;

		res = skb_copy_datagram_msg(buf, msg_hdr_sz(msg) + offset,
					    m, sz_to_copy);
		if (res)
			goto exit;

		sz_copied += sz_to_copy;

		if (sz_to_copy < sz) {
			if (!(flags & MSG_PEEK))
				TIPC_SKB_CB(buf)->handle =
				(void *)(unsigned long)(offset + sz_to_copy);
			goto exit;
		}
	} else {
		if (sz_copied != 0)
			goto exit; /* can't add error msg to valid data */

		if ((err == TIPC_CONN_SHUTDOWN) || m->msg_control)
			res = 0;
		else
			res = -ECONNRESET;
	}

	/* Consume received message (optional) */
	if (likely(!(flags & MSG_PEEK))) {
		if (unlikely(++tsk->rcv_unacked >= TIPC_CONNACK_INTV)) {
			tipc_sk_send_ack(tsk, tsk->rcv_unacked);
			tsk->rcv_unacked = 0;
		}
		tsk_advance_rx_queue(sk);
	}

	/* Loop around if more data is required */
	if ((sz_copied < buf_len) &&	/* didn't get all requested data */
	    (!skb_queue_empty(&sk->sk_receive_queue) ||
	    (sz_copied < target)) &&	/* and more is ready or required */
	    (!(flags & MSG_PEEK)) &&	/* and aren't just peeking at data */
	    (!err))			/* and haven't reached a FIN */
		goto restart;

exit:
	release_sock(sk);
	return sz_copied ? sz_copied : res;
}

/**
 * tipc_write_space - wake up thread if port congestion is released
 * @sk: socket
 */
static void tipc_write_space(struct sock *sk)
{
	struct socket_wq *wq;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (wq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait, POLLOUT |
						POLLWRNORM | POLLWRBAND);
	rcu_read_unlock();
}

/**
 * tipc_data_ready - wake up threads to indicate messages have been received
 * @sk: socket
 * @len: the length of messages
 */
static void tipc_data_ready(struct sock *sk)
{
	struct socket_wq *wq;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (wq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait, POLLIN |
						POLLRDNORM | POLLRDBAND);
	rcu_read_unlock();
}

/**
 * filter_connect - Handle all incoming messages for a connection-based socket
 * @tsk: TIPC socket
 * @msg: message
 *
 * Returns 0 (TIPC_OK) if everything ok, -TIPC_ERR_NO_PORT otherwise
 */
static int filter_connect(struct tipc_sock *tsk, struct sk_buff **buf)
{
	struct sock *sk = &tsk->sk;
	struct socket *sock = sk->sk_socket;
	struct tipc_msg *msg = buf_msg(*buf);
	int retval = -TIPC_ERR_NO_PORT;

	if (msg_mcast(msg))
		return retval;

	switch ((int)sock->state) {
	case SS_CONNECTED:
		/* Accept only connection-based messages sent by peer */
		if (tsk_peer_msg(tsk, msg)) {
			if (unlikely(msg_errcode(msg))) {
				sock->state = SS_DISCONNECTING;
				tsk->connected = 0;
				/* let timer expire on it's own */
				tipc_node_remove_conn(tsk_peer_node(tsk),
						      tsk->portid);
			}
			retval = TIPC_OK;
		}
		break;
	case SS_CONNECTING:
		/* Accept only ACK or NACK message */

		if (unlikely(!msg_connected(msg)))
			break;

		if (unlikely(msg_errcode(msg))) {
			sock->state = SS_DISCONNECTING;
			sk->sk_err = ECONNREFUSED;
			retval = TIPC_OK;
			break;
		}

		if (unlikely(msg_importance(msg) > TIPC_CRITICAL_IMPORTANCE)) {
			sock->state = SS_DISCONNECTING;
			sk->sk_err = EINVAL;
			retval = TIPC_OK;
			break;
		}

		tipc_sk_finish_conn(tsk, msg_origport(msg), msg_orignode(msg));
		msg_set_importance(&tsk->phdr, msg_importance(msg));
		sock->state = SS_CONNECTED;

		/* If an incoming message is an 'ACK-', it should be
		 * discarded here because it doesn't contain useful
		 * data. In addition, we should try to wake up
		 * connect() routine if sleeping.
		 */
		if (msg_data_sz(msg) == 0) {
			kfree_skb(*buf);
			*buf = NULL;
			if (waitqueue_active(sk_sleep(sk)))
				wake_up_interruptible(sk_sleep(sk));
		}
		retval = TIPC_OK;
		break;
	case SS_LISTENING:
	case SS_UNCONNECTED:
		/* Accept only SYN message */
		if (!msg_connected(msg) && !(msg_errcode(msg)))
			retval = TIPC_OK;
		break;
	case SS_DISCONNECTING:
		break;
	default:
		pr_err("Unknown socket state %u\n", sock->state);
	}
	return retval;
}

/**
 * rcvbuf_limit - get proper overload limit of socket receive queue
 * @sk: socket
 * @buf: message
 *
 * For all connection oriented messages, irrespective of importance,
 * the default overload value (i.e. 67MB) is set as limit.
 *
 * For all connectionless messages, by default new queue limits are
 * as belows:
 *
 * TIPC_LOW_IMPORTANCE       (4 MB)
 * TIPC_MEDIUM_IMPORTANCE    (8 MB)
 * TIPC_HIGH_IMPORTANCE      (16 MB)
 * TIPC_CRITICAL_IMPORTANCE  (32 MB)
 *
 * Returns overload limit according to corresponding message importance
 */
static unsigned int rcvbuf_limit(struct sock *sk, struct sk_buff *buf)
{
	struct tipc_msg *msg = buf_msg(buf);

	if (msg_connected(msg))
		return sysctl_tipc_rmem[2];

	return sk->sk_rcvbuf >> TIPC_CRITICAL_IMPORTANCE <<
		msg_importance(msg);
}

/**
 * filter_rcv - validate incoming message
 * @sk: socket
 * @buf: message
 *
 * Enqueues message on receive queue if acceptable; optionally handles
 * disconnect indication for a connected socket.
 *
 * Called with socket lock already taken; port lock may also be taken.
 *
 * Returns 0 (TIPC_OK) if message was consumed, -TIPC error code if message
 * to be rejected, 1 (TIPC_FWD_MSG) if (CONN_MANAGER) message to be forwarded
 */
static int filter_rcv(struct sock *sk, struct sk_buff *buf)
{
	struct socket *sock = sk->sk_socket;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct tipc_msg *msg = buf_msg(buf);
	unsigned int limit = rcvbuf_limit(sk, buf);
	u32 onode;
	int rc = TIPC_OK;

	if (unlikely(msg_user(msg) == CONN_MANAGER))
		return tipc_sk_proto_rcv(tsk, &onode, buf);

	if (unlikely(msg_user(msg) == SOCK_WAKEUP)) {
		kfree_skb(buf);
		tsk->link_cong = 0;
		sk->sk_write_space(sk);
		return TIPC_OK;
	}

	/* Reject message if it is wrong sort of message for socket */
	if (msg_type(msg) > TIPC_DIRECT_MSG)
		return -TIPC_ERR_NO_PORT;

	if (sock->state == SS_READY) {
		if (msg_connected(msg))
			return -TIPC_ERR_NO_PORT;
	} else {
		rc = filter_connect(tsk, &buf);
		if (rc != TIPC_OK || buf == NULL)
			return rc;
	}

	/* Reject message if there isn't room to queue it */
	if (sk_rmem_alloc_get(sk) + buf->truesize >= limit)
		return -TIPC_ERR_OVERLOAD;

	/* Enqueue message */
	TIPC_SKB_CB(buf)->handle = NULL;
	__skb_queue_tail(&sk->sk_receive_queue, buf);
	skb_set_owner_r(buf, sk);

	sk->sk_data_ready(sk);
	return TIPC_OK;
}

/**
 * tipc_backlog_rcv - handle incoming message from backlog queue
 * @sk: socket
 * @skb: message
 *
 * Caller must hold socket lock, but not port lock.
 *
 * Returns 0
 */
static int tipc_backlog_rcv(struct sock *sk, struct sk_buff *skb)
{
	int rc;
	u32 onode;
	struct tipc_sock *tsk = tipc_sk(sk);
	uint truesize = skb->truesize;

	rc = filter_rcv(sk, skb);

	if (likely(!rc)) {
		if (atomic_read(&tsk->dupl_rcvcnt) < TIPC_CONN_OVERLOAD_LIMIT)
			atomic_add(truesize, &tsk->dupl_rcvcnt);
		return 0;
	}

	if ((rc < 0) && !tipc_msg_reverse(skb, &onode, -rc))
		return 0;

	tipc_link_xmit_skb(skb, onode, 0);

	return 0;
}

/**
 * tipc_sk_rcv - handle incoming message
 * @skb: buffer containing arriving message
 * Consumes buffer
 * Returns 0 if success, or errno: -EHOSTUNREACH
 */
int tipc_sk_rcv(struct sk_buff *skb)
{
	struct tipc_sock *tsk;
	struct sock *sk;
	u32 dport = msg_destport(buf_msg(skb));
	int rc = TIPC_OK;
	uint limit;
	u32 dnode;

	/* Validate destination and message */
	tsk = tipc_sk_lookup(dport);
	if (unlikely(!tsk)) {
		rc = tipc_msg_eval(skb, &dnode);
		goto exit;
	}
	sk = &tsk->sk;

	/* Queue message */
	spin_lock_bh(&sk->sk_lock.slock);

	if (!sock_owned_by_user(sk)) {
		rc = filter_rcv(sk, skb);
	} else {
		if (sk->sk_backlog.len == 0)
			atomic_set(&tsk->dupl_rcvcnt, 0);
		limit = rcvbuf_limit(sk, skb) + atomic_read(&tsk->dupl_rcvcnt);
		if (sk_add_backlog(sk, skb, limit))
			rc = -TIPC_ERR_OVERLOAD;
	}
	spin_unlock_bh(&sk->sk_lock.slock);
	sock_put(sk);
	if (likely(!rc))
		return 0;
exit:
	if ((rc < 0) && !tipc_msg_reverse(skb, &dnode, -rc))
		return -EHOSTUNREACH;

	tipc_link_xmit_skb(skb, dnode, 0);
	return (rc < 0) ? -EHOSTUNREACH : 0;
}

static int tipc_wait_for_connect(struct socket *sock, long *timeo_p)
{
	struct sock *sk = sock->sk;
	DEFINE_WAIT(wait);
	int done;

	do {
		int err = sock_error(sk);
		if (err)
			return err;
		if (!*timeo_p)
			return -ETIMEDOUT;
		if (signal_pending(current))
			return sock_intr_errno(*timeo_p);

		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		done = sk_wait_event(sk, timeo_p, sock->state != SS_CONNECTING);
		finish_wait(sk_sleep(sk), &wait);
	} while (!done);
	return 0;
}

/**
 * tipc_connect - establish a connection to another TIPC port
 * @sock: socket structure
 * @dest: socket address for destination port
 * @destlen: size of socket address data structure
 * @flags: file-related flags associated with socket
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_connect(struct socket *sock, struct sockaddr *dest,
			int destlen, int flags)
{
	struct sock *sk = sock->sk;
	struct sockaddr_tipc *dst = (struct sockaddr_tipc *)dest;
	struct msghdr m = {NULL,};
	long timeout = (flags & O_NONBLOCK) ? 0 : tipc_sk(sk)->conn_timeout;
	socket_state previous;
	int res;

	lock_sock(sk);

	/* For now, TIPC does not allow use of connect() with DGRAM/RDM types */
	if (sock->state == SS_READY) {
		res = -EOPNOTSUPP;
		goto exit;
	}

	/*
	 * Reject connection attempt using multicast address
	 *
	 * Note: send_msg() validates the rest of the address fields,
	 *       so there's no need to do it here
	 */
	if (dst->addrtype == TIPC_ADDR_MCAST) {
		res = -EINVAL;
		goto exit;
	}

	previous = sock->state;
	switch (sock->state) {
	case SS_UNCONNECTED:
		/* Send a 'SYN-' to destination */
		m.msg_name = dest;
		m.msg_namelen = destlen;

		/* If connect is in non-blocking case, set MSG_DONTWAIT to
		 * indicate send_msg() is never blocked.
		 */
		if (!timeout)
			m.msg_flags = MSG_DONTWAIT;

		res = tipc_sendmsg(NULL, sock, &m, 0);
		if ((res < 0) && (res != -EWOULDBLOCK))
			goto exit;

		/* Just entered SS_CONNECTING state; the only
		 * difference is that return value in non-blocking
		 * case is EINPROGRESS, rather than EALREADY.
		 */
		res = -EINPROGRESS;
	case SS_CONNECTING:
		if (previous == SS_CONNECTING)
			res = -EALREADY;
		if (!timeout)
			goto exit;
		timeout = msecs_to_jiffies(timeout);
		/* Wait until an 'ACK' or 'RST' arrives, or a timeout occurs */
		res = tipc_wait_for_connect(sock, &timeout);
		break;
	case SS_CONNECTED:
		res = -EISCONN;
		break;
	default:
		res = -EINVAL;
		break;
	}
exit:
	release_sock(sk);
	return res;
}

/**
 * tipc_listen - allow socket to listen for incoming connections
 * @sock: socket structure
 * @len: (unused)
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_listen(struct socket *sock, int len)
{
	struct sock *sk = sock->sk;
	int res;

	lock_sock(sk);

	if (sock->state != SS_UNCONNECTED)
		res = -EINVAL;
	else {
		sock->state = SS_LISTENING;
		res = 0;
	}

	release_sock(sk);
	return res;
}

static int tipc_wait_for_accept(struct socket *sock, long timeo)
{
	struct sock *sk = sock->sk;
	DEFINE_WAIT(wait);
	int err;

	/* True wake-one mechanism for incoming connections: only
	 * one process gets woken up, not the 'whole herd'.
	 * Since we do not 'race & poll' for established sockets
	 * anymore, the common case will execute the loop only once.
	*/
	for (;;) {
		prepare_to_wait_exclusive(sk_sleep(sk), &wait,
					  TASK_INTERRUPTIBLE);
		if (timeo && skb_queue_empty(&sk->sk_receive_queue)) {
			release_sock(sk);
			timeo = schedule_timeout(timeo);
			lock_sock(sk);
		}
		err = 0;
		if (!skb_queue_empty(&sk->sk_receive_queue))
			break;
		err = -EINVAL;
		if (sock->state != SS_LISTENING)
			break;
		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			break;
		err = -EAGAIN;
		if (!timeo)
			break;
	}
	finish_wait(sk_sleep(sk), &wait);
	return err;
}

/**
 * tipc_accept - wait for connection request
 * @sock: listening socket
 * @newsock: new socket that is to be connected
 * @flags: file-related flags associated with socket
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_accept(struct socket *sock, struct socket *new_sock, int flags)
{
	struct sock *new_sk, *sk = sock->sk;
	struct sk_buff *buf;
	struct tipc_sock *new_tsock;
	struct tipc_msg *msg;
	long timeo;
	int res;

	lock_sock(sk);

	if (sock->state != SS_LISTENING) {
		res = -EINVAL;
		goto exit;
	}
	timeo = sock_rcvtimeo(sk, flags & O_NONBLOCK);
	res = tipc_wait_for_accept(sock, timeo);
	if (res)
		goto exit;

	buf = skb_peek(&sk->sk_receive_queue);

	res = tipc_sk_create(sock_net(sock->sk), new_sock, 0, 1);
	if (res)
		goto exit;
	security_sk_clone(sock->sk, new_sock->sk);

	new_sk = new_sock->sk;
	new_tsock = tipc_sk(new_sk);
	msg = buf_msg(buf);

	/* we lock on new_sk; but lockdep sees the lock on sk */
	lock_sock_nested(new_sk, SINGLE_DEPTH_NESTING);

	/*
	 * Reject any stray messages received by new socket
	 * before the socket lock was taken (very, very unlikely)
	 */
	tsk_rej_rx_queue(new_sk);

	/* Connect new socket to it's peer */
	tipc_sk_finish_conn(new_tsock, msg_origport(msg), msg_orignode(msg));
	new_sock->state = SS_CONNECTED;

	tsk_set_importance(new_tsock, msg_importance(msg));
	if (msg_named(msg)) {
		new_tsock->conn_type = msg_nametype(msg);
		new_tsock->conn_instance = msg_nameinst(msg);
	}

	/*
	 * Respond to 'SYN-' by discarding it & returning 'ACK'-.
	 * Respond to 'SYN+' by queuing it on new socket.
	 */
	if (!msg_data_sz(msg)) {
		struct msghdr m = {NULL,};

		tsk_advance_rx_queue(sk);
		tipc_send_packet(NULL, new_sock, &m, 0);
	} else {
		__skb_dequeue(&sk->sk_receive_queue);
		__skb_queue_head(&new_sk->sk_receive_queue, buf);
		skb_set_owner_r(buf, new_sk);
	}
	release_sock(new_sk);
exit:
	release_sock(sk);
	return res;
}

/**
 * tipc_shutdown - shutdown socket connection
 * @sock: socket structure
 * @how: direction to close (must be SHUT_RDWR)
 *
 * Terminates connection (if necessary), then purges socket's receive queue.
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_shutdown(struct socket *sock, int how)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	struct sk_buff *skb;
	u32 dnode;
	int res;

	if (how != SHUT_RDWR)
		return -EINVAL;

	lock_sock(sk);

	switch (sock->state) {
	case SS_CONNECTING:
	case SS_CONNECTED:

restart:
		/* Disconnect and send a 'FIN+' or 'FIN-' message to peer */
		skb = __skb_dequeue(&sk->sk_receive_queue);
		if (skb) {
			if (TIPC_SKB_CB(skb)->handle != NULL) {
				kfree_skb(skb);
				goto restart;
			}
			if (tipc_msg_reverse(skb, &dnode, TIPC_CONN_SHUTDOWN))
				tipc_link_xmit_skb(skb, dnode, tsk->portid);
			tipc_node_remove_conn(dnode, tsk->portid);
		} else {
			dnode = tsk_peer_node(tsk);
			skb = tipc_msg_create(TIPC_CRITICAL_IMPORTANCE,
					      TIPC_CONN_MSG, SHORT_H_SIZE,
					      0, dnode, tipc_own_addr,
					      tsk_peer_port(tsk),
					      tsk->portid, TIPC_CONN_SHUTDOWN);
			tipc_link_xmit_skb(skb, dnode, tsk->portid);
		}
		tsk->connected = 0;
		sock->state = SS_DISCONNECTING;
		tipc_node_remove_conn(dnode, tsk->portid);
		/* fall through */

	case SS_DISCONNECTING:

		/* Discard any unreceived messages */
		__skb_queue_purge(&sk->sk_receive_queue);

		/* Wake up anyone sleeping in poll */
		sk->sk_state_change(sk);
		res = 0;
		break;

	default:
		res = -ENOTCONN;
	}

	release_sock(sk);
	return res;
}

static void tipc_sk_timeout(unsigned long portid)
{
	struct tipc_sock *tsk;
	struct sock *sk;
	struct sk_buff *skb = NULL;
	u32 peer_port, peer_node;

	tsk = tipc_sk_lookup(portid);
	if (!tsk)
		return;

	sk = &tsk->sk;
	bh_lock_sock(sk);
	if (!tsk->connected) {
		bh_unlock_sock(sk);
		goto exit;
	}
	peer_port = tsk_peer_port(tsk);
	peer_node = tsk_peer_node(tsk);

	if (tsk->probing_state == TIPC_CONN_PROBING) {
		/* Previous probe not answered -> self abort */
		skb = tipc_msg_create(TIPC_CRITICAL_IMPORTANCE, TIPC_CONN_MSG,
				      SHORT_H_SIZE, 0, tipc_own_addr,
				      peer_node, portid, peer_port,
				      TIPC_ERR_NO_PORT);
	} else {
		skb = tipc_msg_create(CONN_MANAGER, CONN_PROBE, INT_H_SIZE,
				      0, peer_node, tipc_own_addr,
				      peer_port, portid, TIPC_OK);
		tsk->probing_state = TIPC_CONN_PROBING;
		k_start_timer(&tsk->timer, tsk->probing_interval);
	}
	bh_unlock_sock(sk);
	if (skb)
		tipc_link_xmit_skb(skb, peer_node, portid);
exit:
	sock_put(sk);
}

static int tipc_sk_publish(struct tipc_sock *tsk, uint scope,
			   struct tipc_name_seq const *seq)
{
	struct publication *publ;
	u32 key;

	if (tsk->connected)
		return -EINVAL;
	key = tsk->portid + tsk->pub_count + 1;
	if (key == tsk->portid)
		return -EADDRINUSE;

	publ = tipc_nametbl_publish(seq->type, seq->lower, seq->upper,
				    scope, tsk->portid, key);
	if (unlikely(!publ))
		return -EINVAL;

	list_add(&publ->pport_list, &tsk->publications);
	tsk->pub_count++;
	tsk->published = 1;
	return 0;
}

static int tipc_sk_withdraw(struct tipc_sock *tsk, uint scope,
			    struct tipc_name_seq const *seq)
{
	struct publication *publ;
	struct publication *safe;
	int rc = -EINVAL;

	list_for_each_entry_safe(publ, safe, &tsk->publications, pport_list) {
		if (seq) {
			if (publ->scope != scope)
				continue;
			if (publ->type != seq->type)
				continue;
			if (publ->lower != seq->lower)
				continue;
			if (publ->upper != seq->upper)
				break;
			tipc_nametbl_withdraw(publ->type, publ->lower,
					      publ->ref, publ->key);
			rc = 0;
			break;
		}
		tipc_nametbl_withdraw(publ->type, publ->lower,
				      publ->ref, publ->key);
		rc = 0;
	}
	if (list_empty(&tsk->publications))
		tsk->published = 0;
	return rc;
}

static int tipc_sk_show(struct tipc_sock *tsk, char *buf,
			int len, int full_id)
{
	struct publication *publ;
	int ret;

	if (full_id)
		ret = tipc_snprintf(buf, len, "<%u.%u.%u:%u>:",
				    tipc_zone(tipc_own_addr),
				    tipc_cluster(tipc_own_addr),
				    tipc_node(tipc_own_addr), tsk->portid);
	else
		ret = tipc_snprintf(buf, len, "%-10u:", tsk->portid);

	if (tsk->connected) {
		u32 dport = tsk_peer_port(tsk);
		u32 destnode = tsk_peer_node(tsk);

		ret += tipc_snprintf(buf + ret, len - ret,
				     " connected to <%u.%u.%u:%u>",
				     tipc_zone(destnode),
				     tipc_cluster(destnode),
				     tipc_node(destnode), dport);
		if (tsk->conn_type != 0)
			ret += tipc_snprintf(buf + ret, len - ret,
					     " via {%u,%u}", tsk->conn_type,
					     tsk->conn_instance);
	} else if (tsk->published) {
		ret += tipc_snprintf(buf + ret, len - ret, " bound to");
		list_for_each_entry(publ, &tsk->publications, pport_list) {
			if (publ->lower == publ->upper)
				ret += tipc_snprintf(buf + ret, len - ret,
						     " {%u,%u}", publ->type,
						     publ->lower);
			else
				ret += tipc_snprintf(buf + ret, len - ret,
						     " {%u,%u,%u}", publ->type,
						     publ->lower, publ->upper);
		}
	}
	ret += tipc_snprintf(buf + ret, len - ret, "\n");
	return ret;
}

struct sk_buff *tipc_sk_socks_show(void)
{
	const struct bucket_table *tbl;
	struct rhash_head *pos;
	struct sk_buff *buf;
	struct tlv_desc *rep_tlv;
	char *pb;
	int pb_len;
	struct tipc_sock *tsk;
	int str_len = 0;
	int i;

	buf = tipc_cfg_reply_alloc(TLV_SPACE(ULTRA_STRING_MAX_LEN));
	if (!buf)
		return NULL;
	rep_tlv = (struct tlv_desc *)buf->data;
	pb = TLV_DATA(rep_tlv);
	pb_len = ULTRA_STRING_MAX_LEN;

	rcu_read_lock();
	tbl = rht_dereference_rcu((&tipc_sk_rht)->tbl, &tipc_sk_rht);
	for (i = 0; i < tbl->size; i++) {
		rht_for_each_entry_rcu(tsk, pos, tbl, i, node) {
			spin_lock_bh(&tsk->sk.sk_lock.slock);
			str_len += tipc_sk_show(tsk, pb + str_len,
						pb_len - str_len, 0);
			spin_unlock_bh(&tsk->sk.sk_lock.slock);
		}
	}
	rcu_read_unlock();

	str_len += 1;	/* for "\0" */
	skb_put(buf, TLV_SPACE(str_len));
	TLV_SET(rep_tlv, TIPC_TLV_ULTRA_STRING, NULL, str_len);

	return buf;
}

/* tipc_sk_reinit: set non-zero address in all existing sockets
 *                 when we go from standalone to network mode.
 */
void tipc_sk_reinit(void)
{
	const struct bucket_table *tbl;
	struct rhash_head *pos;
	struct tipc_sock *tsk;
	struct tipc_msg *msg;
	int i;

	rcu_read_lock();
	tbl = rht_dereference_rcu((&tipc_sk_rht)->tbl, &tipc_sk_rht);
	for (i = 0; i < tbl->size; i++) {
		rht_for_each_entry_rcu(tsk, pos, tbl, i, node) {
			spin_lock_bh(&tsk->sk.sk_lock.slock);
			msg = &tsk->phdr;
			msg_set_prevnode(msg, tipc_own_addr);
			msg_set_orignode(msg, tipc_own_addr);
			spin_unlock_bh(&tsk->sk.sk_lock.slock);
		}
	}
	rcu_read_unlock();
}

static struct tipc_sock *tipc_sk_lookup(u32 portid)
{
	struct tipc_sock *tsk;

	rcu_read_lock();
	tsk = rhashtable_lookup(&tipc_sk_rht, &portid);
	if (tsk)
		sock_hold(&tsk->sk);
	rcu_read_unlock();

	return tsk;
}

static int tipc_sk_insert(struct tipc_sock *tsk)
{
	u32 remaining = (TIPC_MAX_PORT - TIPC_MIN_PORT) + 1;
	u32 portid = prandom_u32() % remaining + TIPC_MIN_PORT;

	while (remaining--) {
		portid++;
		if ((portid < TIPC_MIN_PORT) || (portid > TIPC_MAX_PORT))
			portid = TIPC_MIN_PORT;
		tsk->portid = portid;
		sock_hold(&tsk->sk);
		if (rhashtable_lookup_insert(&tipc_sk_rht, &tsk->node))
			return 0;
		sock_put(&tsk->sk);
	}

	return -1;
}

static void tipc_sk_remove(struct tipc_sock *tsk)
{
	struct sock *sk = &tsk->sk;

	if (rhashtable_remove(&tipc_sk_rht, &tsk->node)) {
		WARN_ON(atomic_read(&sk->sk_refcnt) == 1);
		__sock_put(sk);
	}
}

int tipc_sk_rht_init(void)
{
	struct rhashtable_params rht_params = {
		.nelem_hint = 192,
		.head_offset = offsetof(struct tipc_sock, node),
		.key_offset = offsetof(struct tipc_sock, portid),
		.key_len = sizeof(u32), /* portid */
		.hashfn = jhash,
		.max_shift = 20, /* 1M */
		.min_shift = 8,  /* 256 */
	};

	return rhashtable_init(&tipc_sk_rht, &rht_params);
}

void tipc_sk_rht_destroy(void)
{
	/* Wait for socket readers to complete */
	synchronize_net();

	rhashtable_destroy(&tipc_sk_rht);
}

/**
 * tipc_setsockopt - set socket option
 * @sock: socket structure
 * @lvl: option level
 * @opt: option identifier
 * @ov: pointer to new option value
 * @ol: length of option value
 *
 * For stream sockets only, accepts and ignores all IPPROTO_TCP options
 * (to ease compatibility).
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_setsockopt(struct socket *sock, int lvl, int opt,
			   char __user *ov, unsigned int ol)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	u32 value;
	int res;

	if ((lvl == IPPROTO_TCP) && (sock->type == SOCK_STREAM))
		return 0;
	if (lvl != SOL_TIPC)
		return -ENOPROTOOPT;
	if (ol < sizeof(value))
		return -EINVAL;
	res = get_user(value, (u32 __user *)ov);
	if (res)
		return res;

	lock_sock(sk);

	switch (opt) {
	case TIPC_IMPORTANCE:
		res = tsk_set_importance(tsk, value);
		break;
	case TIPC_SRC_DROPPABLE:
		if (sock->type != SOCK_STREAM)
			tsk_set_unreliable(tsk, value);
		else
			res = -ENOPROTOOPT;
		break;
	case TIPC_DEST_DROPPABLE:
		tsk_set_unreturnable(tsk, value);
		break;
	case TIPC_CONN_TIMEOUT:
		tipc_sk(sk)->conn_timeout = value;
		/* no need to set "res", since already 0 at this point */
		break;
	default:
		res = -EINVAL;
	}

	release_sock(sk);

	return res;
}

/**
 * tipc_getsockopt - get socket option
 * @sock: socket structure
 * @lvl: option level
 * @opt: option identifier
 * @ov: receptacle for option value
 * @ol: receptacle for length of option value
 *
 * For stream sockets only, returns 0 length result for all IPPROTO_TCP options
 * (to ease compatibility).
 *
 * Returns 0 on success, errno otherwise
 */
static int tipc_getsockopt(struct socket *sock, int lvl, int opt,
			   char __user *ov, int __user *ol)
{
	struct sock *sk = sock->sk;
	struct tipc_sock *tsk = tipc_sk(sk);
	int len;
	u32 value;
	int res;

	if ((lvl == IPPROTO_TCP) && (sock->type == SOCK_STREAM))
		return put_user(0, ol);
	if (lvl != SOL_TIPC)
		return -ENOPROTOOPT;
	res = get_user(len, ol);
	if (res)
		return res;

	lock_sock(sk);

	switch (opt) {
	case TIPC_IMPORTANCE:
		value = tsk_importance(tsk);
		break;
	case TIPC_SRC_DROPPABLE:
		value = tsk_unreliable(tsk);
		break;
	case TIPC_DEST_DROPPABLE:
		value = tsk_unreturnable(tsk);
		break;
	case TIPC_CONN_TIMEOUT:
		value = tsk->conn_timeout;
		/* no need to set "res", since already 0 at this point */
		break;
	case TIPC_NODE_RECVQ_DEPTH:
		value = 0; /* was tipc_queue_size, now obsolete */
		break;
	case TIPC_SOCK_RECVQ_DEPTH:
		value = skb_queue_len(&sk->sk_receive_queue);
		break;
	default:
		res = -EINVAL;
	}

	release_sock(sk);

	if (res)
		return res;	/* "get" failed */

	if (len < sizeof(value))
		return -EINVAL;

	if (copy_to_user(ov, &value, sizeof(value)))
		return -EFAULT;

	return put_user(sizeof(value), ol);
}

static int tipc_ioctl(struct socket *sk, unsigned int cmd, unsigned long arg)
{
	struct tipc_sioc_ln_req lnr;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SIOCGETLINKNAME:
		if (copy_from_user(&lnr, argp, sizeof(lnr)))
			return -EFAULT;
		if (!tipc_node_get_linkname(lnr.bearer_id & 0xffff, lnr.peer,
					    lnr.linkname, TIPC_MAX_LINK_NAME)) {
			if (copy_to_user(argp, &lnr, sizeof(lnr)))
				return -EFAULT;
			return 0;
		}
		return -EADDRNOTAVAIL;
	default:
		return -ENOIOCTLCMD;
	}
}

/* Protocol switches for the various types of TIPC sockets */

static const struct proto_ops msg_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.release	= tipc_release,
	.bind		= tipc_bind,
	.connect	= tipc_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= tipc_getname,
	.poll		= tipc_poll,
	.ioctl		= tipc_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= tipc_shutdown,
	.setsockopt	= tipc_setsockopt,
	.getsockopt	= tipc_getsockopt,
	.sendmsg	= tipc_sendmsg,
	.recvmsg	= tipc_recvmsg,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage
};

static const struct proto_ops packet_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.release	= tipc_release,
	.bind		= tipc_bind,
	.connect	= tipc_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= tipc_accept,
	.getname	= tipc_getname,
	.poll		= tipc_poll,
	.ioctl		= tipc_ioctl,
	.listen		= tipc_listen,
	.shutdown	= tipc_shutdown,
	.setsockopt	= tipc_setsockopt,
	.getsockopt	= tipc_getsockopt,
	.sendmsg	= tipc_send_packet,
	.recvmsg	= tipc_recvmsg,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage
};

static const struct proto_ops stream_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.release	= tipc_release,
	.bind		= tipc_bind,
	.connect	= tipc_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= tipc_accept,
	.getname	= tipc_getname,
	.poll		= tipc_poll,
	.ioctl		= tipc_ioctl,
	.listen		= tipc_listen,
	.shutdown	= tipc_shutdown,
	.setsockopt	= tipc_setsockopt,
	.getsockopt	= tipc_getsockopt,
	.sendmsg	= tipc_send_stream,
	.recvmsg	= tipc_recv_stream,
	.mmap		= sock_no_mmap,
	.sendpage	= sock_no_sendpage
};

static const struct net_proto_family tipc_family_ops = {
	.owner		= THIS_MODULE,
	.family		= AF_TIPC,
	.create		= tipc_sk_create
};

static struct proto tipc_proto = {
	.name		= "TIPC",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct tipc_sock),
	.sysctl_rmem	= sysctl_tipc_rmem
};

static struct proto tipc_proto_kern = {
	.name		= "TIPC",
	.obj_size	= sizeof(struct tipc_sock),
	.sysctl_rmem	= sysctl_tipc_rmem
};

/**
 * tipc_socket_init - initialize TIPC socket interface
 *
 * Returns 0 on success, errno otherwise
 */
int tipc_socket_init(void)
{
	int res;

	res = proto_register(&tipc_proto, 1);
	if (res) {
		pr_err("Failed to register TIPC protocol type\n");
		goto out;
	}

	res = sock_register(&tipc_family_ops);
	if (res) {
		pr_err("Failed to register TIPC socket type\n");
		proto_unregister(&tipc_proto);
		goto out;
	}
 out:
	return res;
}

/**
 * tipc_socket_stop - stop TIPC socket interface
 */
void tipc_socket_stop(void)
{
	sock_unregister(tipc_family_ops.family);
	proto_unregister(&tipc_proto);
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_add_sk_con(struct sk_buff *skb, struct tipc_sock *tsk)
{
	u32 peer_node;
	u32 peer_port;
	struct nlattr *nest;

	peer_node = tsk_peer_node(tsk);
	peer_port = tsk_peer_port(tsk);

	nest = nla_nest_start(skb, TIPC_NLA_SOCK_CON);

	if (nla_put_u32(skb, TIPC_NLA_CON_NODE, peer_node))
		goto msg_full;
	if (nla_put_u32(skb, TIPC_NLA_CON_SOCK, peer_port))
		goto msg_full;

	if (tsk->conn_type != 0) {
		if (nla_put_flag(skb, TIPC_NLA_CON_FLAG))
			goto msg_full;
		if (nla_put_u32(skb, TIPC_NLA_CON_TYPE, tsk->conn_type))
			goto msg_full;
		if (nla_put_u32(skb, TIPC_NLA_CON_INST, tsk->conn_instance))
			goto msg_full;
	}
	nla_nest_end(skb, nest);

	return 0;

msg_full:
	nla_nest_cancel(skb, nest);

	return -EMSGSIZE;
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_add_sk(struct sk_buff *skb, struct netlink_callback *cb,
			    struct tipc_sock *tsk)
{
	int err;
	void *hdr;
	struct nlattr *attrs;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &tipc_genl_v2_family, NLM_F_MULTI, TIPC_NL_SOCK_GET);
	if (!hdr)
		goto msg_cancel;

	attrs = nla_nest_start(skb, TIPC_NLA_SOCK);
	if (!attrs)
		goto genlmsg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_SOCK_REF, tsk->portid))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_SOCK_ADDR, tipc_own_addr))
		goto attr_msg_cancel;

	if (tsk->connected) {
		err = __tipc_nl_add_sk_con(skb, tsk);
		if (err)
			goto attr_msg_cancel;
	} else if (!list_empty(&tsk->publications)) {
		if (nla_put_flag(skb, TIPC_NLA_SOCK_HAS_PUBL))
			goto attr_msg_cancel;
	}
	nla_nest_end(skb, attrs);
	genlmsg_end(skb, hdr);

	return 0;

attr_msg_cancel:
	nla_nest_cancel(skb, attrs);
genlmsg_cancel:
	genlmsg_cancel(skb, hdr);
msg_cancel:
	return -EMSGSIZE;
}

int tipc_nl_sk_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	int err;
	struct tipc_sock *tsk;
	const struct bucket_table *tbl;
	struct rhash_head *pos;
	u32 prev_portid = cb->args[0];
	u32 portid = prev_portid;
	int i;

	rcu_read_lock();
	tbl = rht_dereference_rcu((&tipc_sk_rht)->tbl, &tipc_sk_rht);
	for (i = 0; i < tbl->size; i++) {
		rht_for_each_entry_rcu(tsk, pos, tbl, i, node) {
			spin_lock_bh(&tsk->sk.sk_lock.slock);
			portid = tsk->portid;
			err = __tipc_nl_add_sk(skb, cb, tsk);
			spin_unlock_bh(&tsk->sk.sk_lock.slock);
			if (err)
				break;

			prev_portid = portid;
		}
	}
	rcu_read_unlock();

	cb->args[0] = prev_portid;

	return skb->len;
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_add_sk_publ(struct sk_buff *skb,
				 struct netlink_callback *cb,
				 struct publication *publ)
{
	void *hdr;
	struct nlattr *attrs;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &tipc_genl_v2_family, NLM_F_MULTI, TIPC_NL_PUBL_GET);
	if (!hdr)
		goto msg_cancel;

	attrs = nla_nest_start(skb, TIPC_NLA_PUBL);
	if (!attrs)
		goto genlmsg_cancel;

	if (nla_put_u32(skb, TIPC_NLA_PUBL_KEY, publ->key))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_PUBL_TYPE, publ->type))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_PUBL_LOWER, publ->lower))
		goto attr_msg_cancel;
	if (nla_put_u32(skb, TIPC_NLA_PUBL_UPPER, publ->upper))
		goto attr_msg_cancel;

	nla_nest_end(skb, attrs);
	genlmsg_end(skb, hdr);

	return 0;

attr_msg_cancel:
	nla_nest_cancel(skb, attrs);
genlmsg_cancel:
	genlmsg_cancel(skb, hdr);
msg_cancel:
	return -EMSGSIZE;
}

/* Caller should hold socket lock for the passed tipc socket. */
static int __tipc_nl_list_sk_publ(struct sk_buff *skb,
				  struct netlink_callback *cb,
				  struct tipc_sock *tsk, u32 *last_publ)
{
	int err;
	struct publication *p;

	if (*last_publ) {
		list_for_each_entry(p, &tsk->publications, pport_list) {
			if (p->key == *last_publ)
				break;
		}
		if (p->key != *last_publ) {
			/* We never set seq or call nl_dump_check_consistent()
			 * this means that setting prev_seq here will cause the
			 * consistence check to fail in the netlink callback
			 * handler. Resulting in the last NLMSG_DONE message
			 * having the NLM_F_DUMP_INTR flag set.
			 */
			cb->prev_seq = 1;
			*last_publ = 0;
			return -EPIPE;
		}
	} else {
		p = list_first_entry(&tsk->publications, struct publication,
				     pport_list);
	}

	list_for_each_entry_from(p, &tsk->publications, pport_list) {
		err = __tipc_nl_add_sk_publ(skb, cb, p);
		if (err) {
			*last_publ = p->key;
			return err;
		}
	}
	*last_publ = 0;

	return 0;
}

int tipc_nl_publ_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	int err;
	u32 tsk_portid = cb->args[0];
	u32 last_publ = cb->args[1];
	u32 done = cb->args[2];
	struct tipc_sock *tsk;

	if (!tsk_portid) {
		struct nlattr **attrs;
		struct nlattr *sock[TIPC_NLA_SOCK_MAX + 1];

		err = tipc_nlmsg_parse(cb->nlh, &attrs);
		if (err)
			return err;

		err = nla_parse_nested(sock, TIPC_NLA_SOCK_MAX,
				       attrs[TIPC_NLA_SOCK],
				       tipc_nl_sock_policy);
		if (err)
			return err;

		if (!sock[TIPC_NLA_SOCK_REF])
			return -EINVAL;

		tsk_portid = nla_get_u32(sock[TIPC_NLA_SOCK_REF]);
	}

	if (done)
		return 0;

	tsk = tipc_sk_lookup(tsk_portid);
	if (!tsk)
		return -EINVAL;

	lock_sock(&tsk->sk);
	err = __tipc_nl_list_sk_publ(skb, cb, tsk, &last_publ);
	if (!err)
		done = 1;
	release_sock(&tsk->sk);
	sock_put(&tsk->sk);

	cb->args[0] = tsk_portid;
	cb->args[1] = last_publ;
	cb->args[2] = done;

	return skb->len;
}
