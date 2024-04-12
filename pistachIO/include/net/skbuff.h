#ifndef _SKBUFF_H_
#define _SKBUFF_H_

#include "list.h"

// #include <netinet/ip.h>
// #include <netinet/tcp.h>
// #include <netinet/udp.h>

struct msghdr;
struct rte_mbuf;
struct sock;

struct sk_buff {
    struct list_head list;

    struct sock * sk;
    struct rte_mbuf * m;

	char    cb[64];

    int len;

    union {
    	uint8_t buf[0];
        uint8_t * ptr;
    };
};

extern struct sk_buff * alloc_skb(uint8_t * data, unsigned int size);
extern void free_skb(struct sk_buff * skb);

extern struct sk_buff * __skb_try_recv_from_queue(struct sock * sk, unsigned int flags, int * err);
extern int __skb_wait_for_more_packets(struct sock *sk, int *err, long *timeo_p);

extern int skb_copy_datagram_msg(struct sk_buff * from, struct msghdr * msg, int size);

#endif  /* _SKBUFF_H_ */