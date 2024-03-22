#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>

#include "opt.h"
#include "printk.h"
#include "kernel.h"
#include "net/dpdk_module.h"
#include "net/net.h"
#include "net/flow.h"
#include "net/sock.h"
#include "net/ethernet.h"
#include "net/tcp.h"
#include "net/udp.h"

#include <rte_ethdev.h>

struct rte_hash * udp_table;

struct net_stats stats;

static struct rte_hash_parameters params = {
	.entries = 2048,
	.key_len = sizeof(struct flowi4),
	.hash_func = rte_jhash,
	.hash_func_init_val = 0,
	.socket_id = 0,
};

void * rx_module(void * arg) {
    int pid = 0, nb_recv;
    uint8_t * pkt;
    int pkt_size;
    // int sec_count = 0;
    // struct timespec curr, last_log;

    // clock_gettime(CLOCK_REALTIME, &last_log);
    // curr = last_log;

    while (1) {
        // clock_gettime(CLOCK_REALTIME, &curr);
        // if (curr.tv_sec - last_log.tv_sec >= 1) {
        //     sec_count = 0;
        //     last_log = curr;
        // }

        nb_recv = dpdk_recv_pkts(pid);
        if (!nb_recv) {
            continue;
        }

        for (int i = 0; i < nb_recv; i++) {
            pkt = dpdk_get_rxpkt(pid, i, &pkt_size);
            ethernet_input(pkt, pkt_size);
            // sec_count++;
        }
    }

    return NULL;
}

void * tx_module(void * arg) {
    int pid = 0, nb_send;
    // int sec_count = 0;
    // struct timespec curr, last_log;

    // clock_gettime(CLOCK_REALTIME, &last_log);
    // curr = last_log;

    while (1) {
        int count = 0;
        struct sock * sk = NULL;

        // clock_gettime(CLOCK_REALTIME, &curr);
        // if (curr.tv_sec - last_log.tv_sec >= 1) {
        //     sec_count = 0;
        //     last_log = curr;
        // }

        count = rte_ring_count(worker_cq);
        for (int i = 0; i < count; i++) {
            if (rte_ring_dequeue(worker_cq, (void **)&sk) == 0) {
                pr_debug(STACK_DEBUG, "%s: sock protocol: %d\n", __func__, sk->sk_protocol);
                lock_sock(sk);
                switch (sk->sk_protocol) {
                    case IPPROTO_IP:
                    case IPPROTO_TCP:
                        pr_warn("Todo: TCP tx\n");
                        break;
                    case IPPROTO_UDP:
                        udp_output(sk);
                        // sec_count++;
                        break;
                }
                sk->sk_tx_pending = 0;
                unlock_sock(sk);
            }
        }

        nb_send = dpdk_send_pkts(pid);
        if (nb_send) {
            pr_debug(STACK_DEBUG, "%s: send %d packets\n", __func__, nb_send);
        }
    }

    return NULL;
}

int __init net_init(void) {
    pthread_t rx_pid, tx_pid;
    pthread_attr_t attr;
    cpu_set_t cpuset;

    params.name = "udp_table";
    udp_table = rte_hash_create(&params);
    assert(udp_table != NULL);

    // Initialize the thread attribute
    pthread_attr_init(&attr);

    // Initialize the CPU set to be empty
    CPU_ZERO(&cpuset);
    // RX core runs on core 0
    CPU_SET(0, &cpuset);

    // Set the CPU affinity in the thread attributes
    if (pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_attr_setaffinity_np");
        exit(EXIT_FAILURE);
    }

    // Create the RX path
    if (pthread_create(&rx_pid, &attr, rx_module, NULL) != 0) {
        perror("Create RX path failed!");
        exit(EXIT_FAILURE);
    }

    // Initialize the CPU set to be empty
    CPU_ZERO(&cpuset);
    // TX core runs on core 1
    CPU_SET(1, &cpuset);

    // Set the CPU affinity in the thread attributes
    if (pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_attr_setaffinity_np");
        exit(EXIT_FAILURE);
    }

    // Create the TX path
    if (pthread_create(&tx_pid, &attr, tx_module, NULL) != 0) {
        perror("Create TX path failed!");
        exit(EXIT_FAILURE);
    }

    // Destroy the thread attributes object, as it's no longer needed
    pthread_attr_destroy(&attr);

    // // Wait for the thread to complete
    // if (pthread_join(rx_pid, NULL) != 0) {
    //     perror("RX join");
    //     exit(EXIT_FAILURE);
    // }

    // // Wait for the thread to complete
    // if (pthread_join(tx_pid, NULL) != 0) {
    //     perror("TX join");
    //     exit(EXIT_FAILURE);
    // }

    return 0;
}