/**
 * @file auresamp_ext.c External resampler implementation using libswresample
 *
 * Copyright (C) 2025 - Enhanced for non-integer ratios
 */

#include <string.h>
#include <re.h>
#include "auresamp_internal.h"

#ifdef USE_LIBSWRESAMPLE
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/**
 * Destructor for external resampler context
 */
static void auresamp_ext_ctx_destructor(void *data)
{
	struct auresamp_ext_ctx *ctx = data;
	
	if (!ctx)
		return;

#ifdef USE_LIBSWRESAMPLE
	/* Debug: Track cleanup */
	re_printf("auresamp_ext: Cleaning up external context (%u->%u Hz)\n",
	          ctx->irate, ctx->orate);
	
	if (ctx->src_data) {
		av_freep(&ctx->src_data[0]);
		av_freep(&ctx->src_data);
	}
	if (ctx->dst_data) {
		av_freep(&ctx->dst_data[0]);
		av_freep(&ctx->dst_data);
	}
	if (ctx->swr_ctx) {
		swr_free(&ctx->swr_ctx);
	}
#endif
}


/**
 * Setup external resampler context
 */
int auresamp_ext_ctx_setup(struct auresamp_ext_ctx **ctx, uint32_t irate, 
                          unsigned ich, uint32_t orate, unsigned och)
{
#ifdef USE_LIBSWRESAMPLE
	struct auresamp_ext_ctx *ext_ctx;
	int ret;

	if (!ctx)
		return EINVAL;

	ext_ctx = mem_zalloc(sizeof(*ext_ctx), auresamp_ext_ctx_destructor);
	if (!ext_ctx)
		return ENOMEM;

	re_printf("auresamp_ext: Created external context for %u->%u Hz\n", irate, orate);

	ext_ctx->irate = irate;
	ext_ctx->orate = orate;
	ext_ctx->ich = ich;
	ext_ctx->och = och;

	/* Allocate resampling context */
	ext_ctx->swr_ctx = swr_alloc();
	if (!ext_ctx->swr_ctx) {
		re_printf("auresamp_ext: Failed to allocate SwrContext\n");
		mem_deref(ext_ctx);
		return ENOMEM;
	}

	/* Set input/output options */
	av_opt_set_int(ext_ctx->swr_ctx, "in_channel_layout", 
	               ich == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO, 0);
	av_opt_set_int(ext_ctx->swr_ctx, "in_sample_rate", irate, 0);
	av_opt_set_sample_fmt(ext_ctx->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);

	av_opt_set_int(ext_ctx->swr_ctx, "out_channel_layout",
	               och == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO, 0);
	av_opt_set_int(ext_ctx->swr_ctx, "out_sample_rate", orate, 0);
	av_opt_set_sample_fmt(ext_ctx->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

	/* Initialize the resampling context */
	ret = swr_init(ext_ctx->swr_ctx);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to initialize SwrContext: %d\n", ret);
		mem_deref(ext_ctx);
		return EINVAL;
	}

	/* Allocate input/output buffers - use dynamic sizing based on rates */
	ext_ctx->max_src_samples = MAX(8192, (irate * 2) / 10);  /* At least 200ms worth */
	ext_ctx->max_dst_samples = av_rescale_rnd(ext_ctx->max_src_samples, orate, irate, AV_ROUND_UP) + 1024;

	ret = av_samples_alloc_array_and_samples(&ext_ctx->src_data, &ext_ctx->src_linesize,
	                                         ich, ext_ctx->max_src_samples, AV_SAMPLE_FMT_S16, 0);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to allocate source samples\n");
		mem_deref(ext_ctx);
		return ENOMEM;
	}

	ret = av_samples_alloc_array_and_samples(&ext_ctx->dst_data, &ext_ctx->dst_linesize,
	                                         och, ext_ctx->max_dst_samples, AV_SAMPLE_FMT_S16, 0);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to allocate destination samples\n");
		mem_deref(ext_ctx);
		return ENOMEM;
	}

	ext_ctx->initialized = true;
	*ctx = ext_ctx;

	return 0;
#else
	(void)ctx;
	(void)irate;
	(void)ich;
	(void)orate;
	(void)och;
	return ENOTSUP;
#endif
}

/**
 * Convert audio samples using external resampler
 */
int auresamp_ext_ctx_convert(struct auresamp_ext_ctx *ctx, int16_t *outv, 
                            size_t *outc, const int16_t *inv, size_t inc)
{
#ifdef USE_LIBSWRESAMPLE
	int src_samples;
	int dst_samples;
	size_t required_outc;

	if (!ctx || !ctx->initialized || !outv || !outc || !inv)
		return EINVAL;

	src_samples = inc / ctx->ich;

	/* Check buffer size limits */
	if (src_samples > ctx->max_src_samples) {
		re_printf("auresamp_ext: Input sample count too large: %d > %d\n",
		          src_samples, ctx->max_src_samples);
		return EINVAL;
	}

	/* Copy input data to libswresample buffer */
	memcpy(ctx->src_data[0], inv, inc * sizeof(int16_t));

	/* Convert samples */
	dst_samples = swr_convert(ctx->swr_ctx, ctx->dst_data, ctx->max_dst_samples,
	                         (const uint8_t**)ctx->src_data, src_samples);
	if (dst_samples < 0) {
		re_printf("auresamp_ext: swr_convert failed: %d\n", dst_samples);
		return EINVAL;
	}

	/* Check output buffer size */
	required_outc = dst_samples * ctx->och;
	if (*outc < required_outc) {
		/* Provide detailed information for debugging */
		double rate_ratio = (double)ctx->orate / ctx->irate;
		size_t expected_outc = (size_t)(inc * rate_ratio);
		
		re_printf("auresamp_ext: Output buffer too small\n");
		re_printf("  Buffer size: %zu samples\n", *outc);
		re_printf("  Required:    %zu samples\n", required_outc);
		re_printf("  Input:       %zu samples (%d frames)\n", inc, src_samples);
		re_printf("  Rate ratio:  %.6f (%u -> %u Hz)\n", rate_ratio, ctx->irate, ctx->orate);
		re_printf("  Expected:    %zu samples (simple calc)\n", expected_outc);
		re_printf("  Difference:  %+ld samples\n", (long)required_outc - (long)expected_outc);
		re_printf("  Use auresamp_calc_output_size() for proper buffer allocation\n");
		
		return ENOMEM;
	}

	/* Copy output data */
	memcpy(outv, ctx->dst_data[0], required_outc * sizeof(int16_t));
	*outc = required_outc;

	return 0;
#else
	(void)ctx;
	(void)outv;
	(void)outc;
	(void)inv;
	(void)inc;
	return ENOTSUP;
#endif
}

/**
 * Close and cleanup external resampler context
 */
void auresamp_ext_ctx_close(struct auresamp_ext_ctx *ctx)
{
	if (!ctx)
		return;

	/* The destructor will handle libswresample cleanup automatically */
	mem_deref(ctx);
}