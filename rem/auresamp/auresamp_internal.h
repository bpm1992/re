/**
 * @file auresamp_internal.h Internal structures for enhanced audio resampling
 *
 * Copyright (C) 2025 - Enhanced for non-integer ratios
 */

#ifndef AURESAMP_INTERNAL_H
#define AURESAMP_INTERNAL_H

#ifdef USE_LIBSWRESAMPLE
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#endif

/**
 * External resampler context (opaque to main interface)
 */
struct auresamp_ext_ctx {
#ifdef USE_LIBSWRESAMPLE
	SwrContext *swr_ctx;
	uint8_t **src_data;
	uint8_t **dst_data;
	int src_linesize;
	int dst_linesize;
	int max_src_samples;
	int max_dst_samples;
#endif
	uint32_t irate, orate;
	unsigned ich, och;
	bool initialized;
	struct le le;  /**< List element for registry */
};

/* Internal functions */
int auresamp_ext_ctx_setup(struct auresamp_ext_ctx **ctx, uint32_t irate, 
                          unsigned ich, uint32_t orate, unsigned och);
int auresamp_ext_ctx_convert(struct auresamp_ext_ctx *ctx, int16_t *outv, 
                            size_t *outc, const int16_t *inv, size_t inc);
void auresamp_ext_ctx_close(struct auresamp_ext_ctx *ctx);

#endif /* AURESAMP_INTERNAL_H */