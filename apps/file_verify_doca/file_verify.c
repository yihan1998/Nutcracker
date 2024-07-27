#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/skbuff.h>

int verify_pkt(uint8_t * data, int len) {
#ifdef CONFIG_DOCA
    struct sha256pkt * request;
    int res;
	struct doca_event event = {0};
    void * mbuf_data;
	uint8_t *resp_head;
	struct doca_sha_ctx * sha_ctx = &ctx->sha_ctx;
	// char orig_sha[DOCA_SHA256_BYTE_COUNT * 2 + 1] = {0};
	// char sha_output[DOCA_SHA256_BYTE_COUNT * 2 + 1] = {0};

    request = (struct sha256pkt *)data;

    memcpy(sha_ctx->src_data_buffer, request->data, request->len);

    doca_buf_get_data(sha_ctx->src_buf, &mbuf_data);
    doca_buf_set_data(sha_ctx->src_buf, mbuf_data, request->len);

    /* Construct sha partial job */
	struct doca_sha_job sha_job = {
		.base = (struct doca_job) {
			.type = DOCA_SHA_JOB_SHA256,
			.flags = DOCA_JOB_FLAGS_NONE,
			.ctx = doca_sha_as_ctx(sha_ctx->doca_sha),
        },
		.resp_buf = sha_ctx->dst_buf,
		.req_buf = sha_ctx->src_buf,
		.flags = DOCA_SHA_JOB_FLAGS_SHA_PARTIAL_FINAL,
	};

    // double elapsed_time;
    // struct timespec start, end;
    // clock_gettime(CLOCK_MONOTONIC, &start);

    res = doca_workq_submit(ctx->workq, (struct doca_job *)&sha_job);
    if (res != DOCA_SUCCESS) {
        printf("Unable to enqueue job. [%s]\n", doca_get_error_string(res));
        return -1;
    }

    do {
		res = doca_workq_progress_retrieve(ctx->workq, &event, DOCA_WORKQ_RETRIEVE_FLAGS_NONE);
	    if (res != DOCA_SUCCESS) {
            if (res == DOCA_ERROR_AGAIN) {
                continue;
            } else {
                printf("Unable to dequeue results. [%s]\n", doca_get_error_string(res));
            }
        }
	} while (res != DOCA_SUCCESS);

    // clock_gettime(CLOCK_MONOTONIC, &end);
    // elapsed_time = (end.tv_sec - start.tv_sec) * 1e9;    // seconds to nanoseconds
    // elapsed_time += (end.tv_nsec - start.tv_nsec);       // add nanoseconds
    // fprintf(stderr, "%.9f\n", elapsed_time);

    doca_buf_get_data(sha_job.resp_buf, (void **)&resp_head);

    if (memcmp(resp_head, request->hash, DOCA_SHA256_BYTE_COUNT) != 0) {
        printf("Verification failed\n");
    }

    doca_buf_reset_data_len(sha_ctx->dst_buf);

	return 0;
#else
    struct sha256pkt * request;
    unsigned char hash[EVP_MAX_MD_SIZE];

    request = (struct sha256pkt *)data;

    calculate_sha256(mdctx, request->data, request->len, hash);

    if (memcmp(hash, request->hash, DOCA_SHA256_BYTE_COUNT) != 0) {
        printf("Verification failed\n");
    }
#endif
}

static unsigned int hook_func(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct ethhdr * ethhdr;
    struct iphdr * iphdr;
    uint16_t iphdr_hlen;
    uint8_t iphdr_protocol;
    struct udphdr * udphdr;
	uint16_t ulen, len;
	uint8_t * data;

    ethhdr = (struct ethhdr *)skb->ptr;

    iphdr = (struct iphdr *)&ethhdr[1];
    iphdr_hlen = iphdr->ihl;
    iphdr_hlen <<= 2;

    udphdr = (struct udphdr *)((uint8_t *)iphdr + iphdr_hlen);
    ulen = ntohs(udphdr->len);
	len = ulen - sizeof(struct udphdr);
	data = (uint8_t *)udphdr + sizeof(struct udphdr);

    return parse_dns_query(data, len);
}

static unsigned int check_cond(struct sk_buff *skb) {
    struct ethhdr * ethhdr;
    struct iphdr * iphdr;
    uint16_t iphdr_hlen;
    uint8_t iphdr_protocol;
    struct udphdr * udphdr;

    ethhdr = (struct ethhdr *)skb->ptr;

    iphdr = (struct iphdr *)&ethhdr[1];
    iphdr_hlen = iphdr->ihl;
    iphdr_hlen <<= 2;
    iphdr_protocol = iphdr->protocol;

    if (iphdr_protocol != IPPROTO_UDP) return NF_MISS;

    udphdr = (struct udphdr *)((uint8_t *)iphdr + iphdr_hlen);
    if (ntohs(udphdr->dest) != 4321) return NF_MISS;

    return NF_MATCH; // Accept the packet
}

static struct nf_hook_ops nfho = {
    .cond       = check_cond,
    .hook       = hook_func,
    .hooknum    = NF_INET_PRE_ROUTING,
    .pf         = NFPROTO_INET,
};

int nf_dns_doca_init(void) {
    if (load_regex_rules() != 0) {
        return -1;
    }
    printf("RegEx rule loaded!\n");
    nf_register_net_hook(NULL, &nfho);
    return 0;
}
