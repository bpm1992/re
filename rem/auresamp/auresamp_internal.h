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
	enum AVSampleFormat fmt;  /**< AV_SAMPLE_FMT_S16 or AV_SAMPLE_FMT_FLT */
	int sample_size;          /**< bytes per sample, av_get_bytes_per_sample(fmt) */
#endif
	uint32_t irate, orate;
	unsigned ich, och;
	bool initialized;
	struct le le;  /**< List element for registry */
};

/* Internal functions */
int auresamp_ext_ctx_setup(struct auresamp_ext_ctx **ctx, uint32_t irate,
                          unsigned ich, uint32_t orate, unsigned och,
                          size_t period_in_frames, bool use_float);
int auresamp_ext_ctx_convert(struct auresamp_ext_ctx *ctx, int16_t *outv,
                            size_t *outc, const int16_t *inv, size_t inc);
int auresamp_ext_ctx_convert_f(struct auresamp_ext_ctx *ctx, float *outv,
                            size_t *outc, const float *inv, size_t inc);
void auresamp_ext_ctx_close(struct auresamp_ext_ctx *ctx);

/**
 * Compute how many input frames are needed to produce out_frames output
 * frames, accounting for samples already buffered inside libswresample.
 * Returns 0 when out_frames is 0; never returns negative.
 */
int64_t auresamp_ext_needed_input_frames(const struct auresamp_ext_ctx *ctx,
                                         int64_t out_frames);

#endif /* AURESAMP_INTERNAL_H */