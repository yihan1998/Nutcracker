#include "fs/fs.h"
#include "net/net.h"
#include "net/skbuff.h"

struct sk_buff * alloc_skb(uint8_t * data, unsigned int data_len) {
    struct sk_buff * skb = NULL;

    rte_mempool_get(skb_mp, (void *)&skb);
    if (!skb) {
        return NULL;
    }

    skb->len = data_len;
    skb->ptr = data;

    return skb;
}

void free_skb(struct sk_buff * skb) {
    rte_mempool_put(skb_mp, skb);
}