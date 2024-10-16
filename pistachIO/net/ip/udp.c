#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>

#include "opt.h"
#include "printk.h"
#include "kernel.h"
#include "net/dpdk_module.h"
#include "net/netfmt.h"
#include "net/net.h"
#include "net/skbuff.h"
#include "net/flow.h"
#include "net/sock.h"
#include "net/ethernet.h"
#include "net/udp.h"

static inline void udp_close(struct sock * sk, long timeout) {
	return;
}

static inline int udp_init_sock(struct sock * sk) {
	struct udp_sock * udp = udp_sk(sk);

	init_list_head(&udp->receive_queue);
	init_list_head(&udp->transmit_queue);

	return 0;
}

static inline int udp_bind(struct sock * sk, const struct sockaddr * uaddr, int addr_len) {
	int pos;
	uint16_t snum;
	struct inet_sock * inet = inet_sk(sk);
	struct sockaddr_in *addr = (struct sockaddr_in *)uaddr;
    struct flowi4 key = {0};
	key.saddr 		= sk->sk_rcv_saddr;
	key.daddr 		= 0;
	key.fl4_sport 	= 0;
	key.fl4_dport 	= 0;
	key.flowi4_proto 	= IPPROTO_UDP;

	snum = addr->sin_port;
	inet->inet_rcv_saddr = inet->inet_saddr = addr->sin_addr.s_addr;

	if (ntohs(snum)) {
		/* TODO: Lookup for collide */
		key.fl4_sport = snum;
		pr_debug(UDP_DEBUG, "snum: %d, hash table: %p, saddr: " IP_STRING ", sport: %u, daddr: " IP_STRING ", dport: %u\n",
			ntohs(snum), flow_table, NET_IP_FMT(key.saddr), ntohs(key.fl4_sport), NET_IP_FMT(key.daddr), ntohs(key.fl4_dport));

		pos = rte_hash_add_key_data(flow_table, (const void *)&key, (void *)sk);
		if (pos < 0) {
			pr_warn("Failed to add key (pos0=%d)", pos);
		}

		inet->inet_sport = inet->inet_num = addr->sin_port;
		inet->inet_daddr = 0;
		inet->inet_dport = 0;

		// return pos;
		return 0;
	} else {
		/* TODO */
	}

	return -1;	
}

static int udp_sendmsg(struct sock * sk, struct msghdr * msg, size_t len) {
    struct inet_sock * inet = inet_sk(sk);
    struct udp_sock * udp = udp_sk(sk);
	DECLARE_SOCKADDR(struct sockaddr_in *, usin, msg->msg_name);
	uint32_t daddr, saddr;
	uint16_t dport, sport;
	struct sk_buff * skb;
	int err;
	void * src_buf = msg->msg_iov->iov_base;

	if (usin) {
		daddr = usin->sin_addr.s_addr;
		dport = usin->sin_port;
	} else {
		daddr = inet->inet_daddr;
		dport = inet->inet_dport;
	}

	saddr = inet->inet_saddr;
	sport = inet->inet_sport;

	skb = alloc_skb(NULL, len);
	if (!skb) {
		pr_warn("Failed to make skb for message!\n");
		err = -ENOMEM;
		goto out;
	}

	skb->sk = sk;

	UDP_SKB_CB(skb)->header.saddr = saddr;
	UDP_SKB_CB(skb)->header.daddr = daddr;
	UDP_SKB_CB(skb)->source = sport;
	UDP_SKB_CB(skb)->dest = dport;

	memcpy(skb->buf, src_buf, len);

	lock_sock(sk);

	list_add_tail(&skb->list, &udp->transmit_queue);

	if (!sock_is_pending(sk)) {
		rte_ring_enqueue(worker_cq, sk);
	    sk->sk_tx_pending = 1;
	}

	unlock_sock(sk);

out:
	return err;
}

struct sk_buff * __skb_recv_udp(struct sock * sk, unsigned int flags, int noblock, int * err) {
	struct sk_buff * skb;
	long timeo;
	int error;

	flags |= noblock ? MSG_DONTWAIT : 0;
	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

	do {
		error = -EAGAIN;
		skb = __skb_try_recv_from_queue(sk, flags, err);
		if (skb) {
			list_del_init(&skb->list);
			return skb;
		}
	} while (timeo && !__skb_wait_for_more_packets(sk, &error, &timeo));

	*err = error;
	return NULL;
}

static int udp_recvmsg(struct sock * sk, struct msghdr * msg, size_t len, int noblock, int flags, int * addr_len) {
	DECLARE_SOCKADDR(struct sockaddr_in *, sin, msg->msg_name);
	struct sk_buff * skb;
	int err;
	unsigned int data_len, copied;

	lock_sock(sk);
	skb = __skb_recv_udp(sk, flags, noblock, &err);
	unlock_sock(sk);

	if (!skb) {
		return err;
	}

	data_len = skb->len;
	copied = data_len;
	if (copied < data_len) {
		msg->msg_flags |= MSG_TRUNC;
	}

	if (copied > data_len) {
		copied = data_len;
	}

	err = skb_copy_datagram_msg(skb, msg, copied);
	if (err < 0) {
		free_skb(skb);
		return err;
	}

	if (sin) {
		sin->sin_family = AF_INET;
		sin->sin_port = UDP_SKB_CB(skb)->source;
		sin->sin_addr.s_addr = UDP_SKB_CB(skb)->header.saddr;
		memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
		*addr_len = sizeof(*sin);
	}

	err = copied;
	free_skb(skb);
	return err;
}

struct proto udp_prot = {
	.name			= "UDP",
	.close			= udp_close,
	.init			= udp_init_sock,
	.bind			= udp_bind,
	// .destroy		= udp_destroy_sock,
	.sendmsg		= udp_sendmsg,
	.recvmsg		= udp_recvmsg,
	// .hash			= udp_hash,
	// .unhash			= udp_unhash,
	// .get_port		= udp_get_port,
	.obj_size		= sizeof(struct udp_sock),
};

__poll_t udp_poll(struct file * file, struct socket * sock, poll_table * wait) {
	return datagram_poll(file, sock, wait);
}

/**
 * udp_lookup_sock - Try to look up socket for a incoming packet in UDP table
 *
 * @param saddr: source IP address
 * @param sport: source UDP port
 * @param daddr: dest IP address
 * @param dport: dest UDP port
 *
 * @return sock if 
 */
static struct sock * udp_lookup_sock(uint32_t saddr, uint32_t sport, uint32_t daddr, uint32_t dport) {
	struct sock * try_sk = NULL;
	struct flowi4 key = {0};
	key.saddr = daddr;
    key.daddr = 0;
    key.fl4_sport = dport;
    key.fl4_dport = 0;
	key.flowi4_proto 	= IPPROTO_UDP;

	pr_debug(UDP_DEBUG, "Hash table: %p, saddr: " IP_STRING ", sport: %u, daddr: " IP_STRING ", dport: %u\n",
			flow_table, NET_IP_FMT(key.saddr), ntohs(key.fl4_sport), NET_IP_FMT(key.daddr), ntohs(key.fl4_dport));

	rte_hash_lookup_data(flow_table, (const void *)&key, (void **)&try_sk);

	if (try_sk) {
		return try_sk;
	}

	return NULL;
}

/* wrapper for udp_queue_rcv_skb tacking care of csum conversion and
 * return code conversion for ip layer consumption
 */
static inline int udp_unicast_rcv_skb(struct sock * sk, struct iphdr * iphdr, struct udphdr * udphdr, struct sk_buff * skb) {
	struct udp_sock * udp = udp_sk(sk);

	lock_sock(sk);
	list_add_tail(&skb->list, &udp->receive_queue);

	if (!sock_flag(sk, SOCK_DEAD)) {
		sk->sk_data_ready(sk);
	}
	unlock_sock(sk);

	return NET_RX_SUCCESS;
}

int udp_input(struct sk_buff * skb, struct iphdr * iphdr, struct udphdr * udphdr) {
    struct sock * sk = NULL;
	uint16_t ulen, len;
	uint8_t * data;

	ulen = ntohs(udphdr->len);
	len = ulen - sizeof(struct udphdr);
	data = (uint8_t *)udphdr + sizeof(struct udphdr);

	/* Try to look up with dest addr */
	sk = udp_lookup_sock(iphdr->saddr, udphdr->source, iphdr->daddr, udphdr->dest);
	if (!sk) {
		sk = udp_lookup_sock(iphdr->saddr, udphdr->source, INADDR_ANY, udphdr->dest);
		if (!sk) {
			// pr_warn("Failed to lookup sock!\n");
			return NET_RX_DROP;
		}
	}

	skb->sk = sk;
	pr_debug(UDP_DEBUG, "%s: saddr: " IP_STRING ", sport: %u, daddr: " IP_STRING ", dport: %u, len: %d\n",
			__func__, NET_IP_FMT(UDP_SKB_CB(skb)->header.saddr), ntohs(UDP_SKB_CB(skb)->source), 
			NET_IP_FMT(UDP_SKB_CB(skb)->header.daddr), ntohs(UDP_SKB_CB(skb)->dest), len);

	memcpy(skb->buf, data, len);

	return udp_unicast_rcv_skb(sk, iphdr, udphdr, skb);
}

int udp_output(struct sock * sk) {
	int pid = 0, ret = -ENODATA;
	struct sk_buff * skb, * tmp;
	struct rte_mbuf * m;
	uint8_t * pkt;
	int pkt_len;
	struct udp_sock * udp = udp_sk(sk);
	struct udphdr * udphdr;
	uint8_t * p;
	uint32_t saddr, daddr;

    list_for_each_entry_safe(skb, tmp, &udp->transmit_queue, list) {
		pkt_len = skb->len + sizeof(struct udphdr) + sizeof(struct iphdr) + sizeof(struct ethhdr);
		m = dpdk_alloc_txpkt(pkt_len);
		if (!m) {
			return 0;
		}

		pkt = rte_pktmbuf_mtod(m, uint8_t *);
		udphdr = (struct udphdr *)(pkt + sizeof(struct ethhdr) + sizeof(struct iphdr));
		udphdr->source = UDP_SKB_CB(skb)->source;
		udphdr->dest = UDP_SKB_CB(skb)->dest;
		udphdr->len = htons(skb->len + sizeof(struct udphdr));
		udphdr->check = 0;

		p = (uint8_t *)(pkt + sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr));
		memcpy(p, skb->buf, skb->len);

		 if (UDP_SKB_CB(skb)->header.saddr == INADDR_ANY) {
			saddr = inet_addr("10.0.0.1");
		} else {
			saddr = UDP_SKB_CB(skb)->header.saddr;
		}
		daddr = UDP_SKB_CB(skb)->header.daddr;

		ip4_output(sk, skb, saddr, daddr, pkt, pkt_len);

		// pthread_spin_lock(&tx_lock);
		dpdk_insert_txpkt(pid, m);
		// pthread_spin_unlock(&tx_lock);

		list_del_init(&skb->list);
		free_skb(skb);
	}

    return ret;
}