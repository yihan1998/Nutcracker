#include <assert.h>
#include <stdlib.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "printk.h"
#include "list.h"
#include "net/dpdk_module.h"

#define MEMPOOL_CACHE_SIZE  256
#define N_MBUF              8192
#define BUF_SIZE            2048
#define MBUF_SIZE           (BUF_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)

#define RX_DESC_DEFAULT    4096
#define TX_DESC_DEFAULT    4096

/* -------------------------------------------------------------------------- */
/* RX queue configuration */
static struct rte_eth_rxconf rx_conf = {
    .rx_thresh = {
        .pthresh = 8,
        .hthresh = 8,
        .wthresh = 4,
    },
    .rx_free_thresh = 32,
#if RTE_VERSION >= RTE_VERSION_NUM(20, 11, 0, 0)
    .rx_deferred_start = 1,
#endif
};

/* TX queue configuration */
static struct rte_eth_txconf tx_conf = {
    .tx_thresh = {
        .pthresh = 36,
        .hthresh = 0,
        .wthresh = 0,
    },
    .tx_free_thresh = 0,
#if RTE_VERSION >= RTE_VERSION_NUM(20, 11, 0, 0)
    .tx_deferred_start = 1,
#endif
};

/* Port configuration */
struct rte_eth_conf port_conf = {
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 0, 0)
    .rxmode = {
        .mq_mode        = ETH_MQ_RX_NONE,
        .split_hdr_size = 0,
    },
#else
    .rxmode = {
        .mq_mode        = RTE_ETH_MQ_RX_RSS,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf =
                RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP,
        },
    },
#endif
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 0, 0)
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
        .offloads = (DEV_TX_OFFLOAD_IPV4_CKSUM |
                DEV_TX_OFFLOAD_UDP_CKSUM |
                DEV_TX_OFFLOAD_TCP_CKSUM),
    },
#else
    .txmode = {
        .mq_mode = RTE_ETH_MQ_TX_NONE,
        .offloads = (RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                RTE_ETH_TX_OFFLOAD_TCP_CKSUM),
    },
#endif
};

/* -------------------------------------------------------------------------- */
struct mbuf_table {
    int len;
    struct rte_mbuf * mtable[MAX_PKT_BURST];
} __rte_cache_aligned;

/* Global packet pool */
struct rte_mempool * pkt_mempool;
/* RX mbuf and TX mbuf */
struct mbuf_table rx_mbufs[RTE_MAX_ETHPORTS];
__thread struct mbuf_table tx_mbufs[RTE_MAX_ETHPORTS];

/**
 * mempool_init - init packet mempool on each port for RX/TX queue
 */
static int mempool_init(void) {
    pkt_mempool = rte_pktmbuf_pool_create("pkt_mempool", N_MBUF,
                    RTE_MEMPOOL_CACHE_MAX_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    assert(pkt_mempool);
    return 0;
}

/**
 * queue_init - init RX/TX queue for each interface
 */
static int queue_init(void) {
    int ret;
    int pid;
    int nb_rx_queue, nb_tx_queue;

    nb_rx_queue = nb_tx_queue = 1;

    RTE_ETH_FOREACH_DEV(pid) {
        /* Get Ethernet device info */
        struct rte_eth_dev_info dev_info;
        ret = rte_eth_dev_info_get(pid, &dev_info);
	    if (ret != 0) {
            pr_emerg("Error during getting device (port %u) info: %s\n", pid, strerror(-ret));
        }

        /* Configure # of RX and TX queue for port */
        ret = rte_eth_dev_configure(pid, nb_rx_queue, nb_tx_queue, &port_conf);
    	if (ret < 0) {
	    	pr_emerg("cannot configure device: err=%d, port=%u\n", ret, pid);
	    }

        /* Set up rx queue with pakcet mempool */
        for (int i = 0; i < nb_rx_queue; i++) {
	    	ret = rte_eth_rx_queue_setup(pid, i, RX_DESC_DEFAULT,
                    rte_eth_dev_socket_id(pid), &rx_conf, pkt_mempool);
    		if (ret < 0) {
	    		pr_emerg("Rx queue setup failed: err=%d, port=%u\n", ret, pid);
		    }
	    }

        /* Set up tx queue with pakcet mempool */
    	for (int i = 0;i < nb_tx_queue;i++) {
	    	ret = rte_eth_tx_queue_setup(pid, i, TX_DESC_DEFAULT,
		    		rte_eth_dev_socket_id(pid), &tx_conf);
    		if (ret < 0) {
	    		pr_emerg("Tx queue setup failed: err=%d, port=%u\n", ret, pid);
		    }
	    }

        pr_info("Port %d has %d RX queue and %d TX queue\n", pid, nb_rx_queue, nb_tx_queue);

        ret = rte_eth_promiscuous_enable(pid);
        if (ret != 0) {
            pr_emerg("rte_eth_promiscuous_enable:err = %d, port = %u\n", ret, (unsigned) pid);
        }

        /* Start Ethernet device */
        ret = rte_eth_dev_start(pid);
        if (ret < 0) {
            pr_emerg("rte_eth_dev_start:err = %d, port = %u\n", ret, (unsigned) pid);
        }
    }

    return 0;
}

uint8_t * dpdk_get_rxpkt(int pid, struct rte_mbuf ** pkts, int index, int * pkt_size) {
    struct rte_mbuf * rx_pkt = pkts[index];
    *pkt_size = rx_pkt->pkt_len;
    return rte_pktmbuf_mtod(rx_pkt, uint8_t *);
}

uint32_t dpdk_recv_pkts(int pid, struct rte_mbuf ** pkts) {
    return rte_eth_rx_burst((uint8_t)pid, 0, pkts, MAX_PKT_BURST);
}

struct rte_mbuf * dpdk_alloc_txpkt(int pkt_size) {
    struct rte_mbuf * tx_pkt = rte_pktmbuf_alloc(pkt_mempool);
    if (!tx_pkt) return NULL;
    tx_pkt->pkt_len = tx_pkt->data_len = pkt_size;
    tx_pkt->nb_segs = 1;
    tx_pkt->next = NULL;
    return tx_pkt;
}

int dpdk_insert_txpkt(int pid, struct rte_mbuf * m) {
    if (unlikely(tx_mbufs[pid].len == MAX_PKT_BURST)) {
        return -1;
    }

    int next_pkt = tx_mbufs[pid].len;
    tx_mbufs[pid].mtable[next_pkt] = m;
    tx_mbufs[pid].len++;
    return 0;
}

uint32_t dpdk_send_pkts(int pid) {
    int total_pkt, pkt_cnt;
    total_pkt = pkt_cnt = tx_mbufs[pid].len;

    struct rte_mbuf ** pkts = tx_mbufs[pid].mtable;

    if (pkt_cnt > 0) {
        int ret;
        do {
            /* Send packets until there is none in TX queue */
            ret = rte_eth_tx_burst(pid, 0, pkts, pkt_cnt);
            pkts += ret;
            pkt_cnt -= ret;
        } while (pkt_cnt > 0);

        tx_mbufs[pid].len = 0;
    }

    return total_pkt;
}

int __init dpdk_init(int argc, char ** argv) {
    int ret;

    if ((ret = rte_eal_init(argc, argv)) < 0) {
        pr_emerg("rte_eal_init() failed! ret: %d\n", ret);
        return -1;
    }

    /* Init packet mempool and mbuf for each core */
    mempool_init();

    /* Init queue for each core */
    queue_init();

    return 0;
}