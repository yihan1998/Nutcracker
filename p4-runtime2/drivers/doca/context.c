#include <stdio.h>
#include <stdlib.h>

#include "opt.h"
#include "utils/printk.h"
#include "percpu.h"
#include "kernel/threads.h"

#ifdef CONFIG_DOCA
#include "drivers/doca/common.h"
#include "drivers/doca/context.h"
#include "drivers/doca/compress.h"

int __init docadv_ctx_fetch(struct docadv_context * ctx) {
#ifdef CONFIG_DOCA_COMPRESS
    doca_error_t res;
	struct docadv_compress_ctx * compress_ctx = &ctx->compress_ctx;

    res = doca_buf_inventory_create(NULL, 2, DOCA_BUF_EXTENSION_NONE, &compress_ctx->buf_inv);
	if (res != DOCA_SUCCESS) {
		printf("Unable to create buffer inventory: %s", doca_get_error_string(res));
		return res;
	}
    init_buf(compress_ctx->dev, compress_ctx->buf_inv, &compress_ctx->src_buf, 32768);
    init_buf(compress_ctx->dev, compress_ctx->buf_inv, &compress_ctx->dst_buf, 32768);
#endif  /* CONFIG_DOCA_COMPRESS */

    return DOCA_SUCCESS;
}

int __init docadv_ctx_init(struct docadv_context * ctx) {
    doca_error_t result;

    result = doca_workq_create(WORKQ_DEPTH, &ctx->workq);
	if (result != DOCA_SUCCESS) {
		printf("Unable to create work queue. Reason: %s", doca_get_error_string(result));
		return result;
	}

    return DOCA_SUCCESS;
}

#endif /* CONFIG_DOCA */