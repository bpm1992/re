/**
 * @file auresamp_ext.c External resampler implementation using libswresample
 *
 * Copyright (C) 2025 - Enhanced for non-integer ratios
 */

#include <stdlib.h>
#include <string.h>
#include <re.h>
#include <rem_fir.h>
#include <rem_auresamp.h>
#include "auresamp_internal.h"

#ifdef USE_LIBSWRESAMPLE
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#endif

/** Global registry of external resampler contexts */
static struct {
	struct list ctxl;  /**< List of active contexts */
	mtx_t *lock;       /**< Mutex for thread safety */
	bool closing;      /**< Flag to prevent deadlock during cleanup */
} auresamp_ext_registry = {
	.ctxl = LIST_INIT,
	.lock = NULL,
	.closing = false
};

/**
 * Close the external resampler registry and clean up all contexts
 */
static void auresamp_ext_registry_close(void);

/**
 * Initialize the external resampler registry
 */
#ifdef USE_LIBSWRESAMPLE
static int auresamp_ext_registry_init(void)
{
	int err;

	if (auresamp_ext_registry.lock)
		return 0;  /* Already initialized */

	err = mutex_alloc(&auresamp_ext_registry.lock);
	if (err)
		return err;

	/* Register cleanup on process exit */
	atexit(auresamp_ext_registry_close);

	return 0;
}
#endif

/**
 * Close the external resampler registry and clean up all contexts
 */
static void auresamp_ext_registry_close(void)
{
	struct le *le;

	if (!auresamp_ext_registry.lock)
		return;

	mtx_lock(auresamp_ext_registry.lock);
	auresamp_ext_registry.closing = true;
	le = auresamp_ext_registry.ctxl.head;
	while (le) {
		struct auresamp_ext_ctx *ctx = le->data;
		le = le->next;

		mem_deref(ctx);
	}
	mtx_unlock(auresamp_ext_registry.lock);

	mem_deref(auresamp_ext_registry.lock);
	auresamp_ext_registry.lock = NULL;
	auresamp_ext_registry.closing = false;
}

/**
 * Register a context in the global registry
 */
#ifdef USE_LIBSWRESAMPLE
static int auresamp_ext_registry_register(struct auresamp_ext_ctx *ctx)
{
	int err;

	err = auresamp_ext_registry_init();
	if (err)
		return err;

	mtx_lock(auresamp_ext_registry.lock);
	list_append(&auresamp_ext_registry.ctxl, &ctx->le, ctx);
	mtx_unlock(auresamp_ext_registry.lock);

	return 0;
}
#endif

/**
 * Unregister a context from the global registry
 */
#ifdef USE_LIBSWRESAMPLE
static void auresamp_ext_registry_unregister(struct auresamp_ext_ctx *ctx)
{
	if (!auresamp_ext_registry.lock || auresamp_ext_registry.closing)
		return;

	mtx_lock(auresamp_ext_registry.lock);
	list_unlink(&ctx->le);
	mtx_unlock(auresamp_ext_registry.lock);
}
#endif

/**
 * Destructor for external resampler context
 */
#ifdef USE_LIBSWRESAMPLE
static void auresamp_ext_ctx_destructor(void *data)
{
	struct auresamp_ext_ctx *ctx = data;
    
	if (!ctx)
		return;

	/* Unregister from global registry */
	auresamp_ext_registry_unregister(ctx);

	/* Debug: Track cleanup */
    
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
}
#endif


/**
 * Setup external resampler context
 */
int auresamp_ext_ctx_setup(struct auresamp_ext_ctx **ctx, uint32_t irate,
                          unsigned ich, uint32_t orate, unsigned och,
                          size_t period_in_frames, bool use_float)
{
#ifdef USE_LIBSWRESAMPLE
	struct auresamp_ext_ctx *ext_ctx;
	AVChannelLayout in_ch, out_ch;
	int ret;

	if (!ctx || !period_in_frames)
		return EINVAL;

	ext_ctx = mem_zalloc(sizeof(*ext_ctx), auresamp_ext_ctx_destructor);
	if (!ext_ctx)
		return ENOMEM;

	re_printf("auresamp_ext: Created external context for %u->%u Hz"
	          " (%s, period %zu frames)\n", irate, orate,
	          use_float ? "float" : "s16", period_in_frames);

	ext_ctx->irate = irate;
	ext_ctx->orate = orate;
	ext_ctx->ich = ich;
	ext_ctx->och = och;
	ext_ctx->fmt = use_float ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
	ext_ctx->sample_size = av_get_bytes_per_sample(ext_ctx->fmt);

	av_channel_layout_default(&in_ch, ich);
	av_channel_layout_default(&out_ch, och);

	ext_ctx->swr_ctx = NULL;
	ret = swr_alloc_set_opts2(&ext_ctx->swr_ctx,
	                          &out_ch, ext_ctx->fmt, orate,
	                          &in_ch, ext_ctx->fmt, irate,
	                          0, NULL);

	av_channel_layout_uninit(&in_ch);
	av_channel_layout_uninit(&out_ch);

	if (ret < 0 || !ext_ctx->swr_ctx) {
		re_printf("auresamp_ext: Failed to allocate SwrContext: %d\n", ret);
		mem_deref(ext_ctx);
		return ENOMEM;
	}

	/* Initialize the resampling context */
	ret = swr_init(ext_ctx->swr_ctx);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to initialize SwrContext: %d\n", ret);
		mem_deref(ext_ctx);
		return EINVAL;
	}

	/*
	 * Bound the reservoir to a single JACK period (plus a small guard).
	 * The previous 200ms-worth reservoir let libswresample buffer far
	 * more than the caller's output buffer could ever absorb; when it
	 * eventually flushed, the flush overran the output buffer and the
	 * frame was dropped. Constraining max_src_samples means the caller
	 * (jack_src.c/jack_play.c) never passes more than this in one call,
	 * and combined with the per-period drain in *_convert(), the
	 * reservoir cannot carry state across periods.
	 */
	ext_ctx->max_src_samples = (int)period_in_frames + 8;

	/*
	 * Worst case per-period output: the expected output for one period
	 * plus room for a full drain-flush of anything the primary
	 * swr_convert() call didn't emit. Sized once here (RT thread never
	 * allocates).
	 */
	ext_ctx->max_dst_samples = (int)(av_rescale_rnd(
		(int64_t)period_in_frames, orate, irate, AV_ROUND_UP) * 2 + 1024);

	ret = av_samples_alloc_array_and_samples(&ext_ctx->src_data, &ext_ctx->src_linesize,
	                                         ich, ext_ctx->max_src_samples, ext_ctx->fmt, 0);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to allocate source samples\n");
		mem_deref(ext_ctx);
		return ENOMEM;
	}

	ret = av_samples_alloc_array_and_samples(&ext_ctx->dst_data, &ext_ctx->dst_linesize,
	                                         och, ext_ctx->max_dst_samples, ext_ctx->fmt, 0);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to allocate destination samples\n");
		mem_deref(ext_ctx);
		return ENOMEM;
	}

	ext_ctx->initialized = true;
	*ctx = ext_ctx;

	/* Register in global registry for emergency cleanup */
	ret = auresamp_ext_registry_register(ext_ctx);
	if (ret) {
		mem_deref(ext_ctx);
		return ret;
	}

	return 0;
#else
	(void)ctx;
	(void)irate;
	(void)ich;
	(void)orate;
	(void)och;
	(void)period_in_frames;
	(void)use_float;
	return ENOTSUP;
#endif
}

#ifdef USE_LIBSWRESAMPLE
/**
 * Shared conversion body for both the int16 and float entry points.
 * No logging, no allocation -- this is reachable from the JACK RT thread.
 */
static int convert_generic(struct auresamp_ext_ctx *ctx, void *outv,
                            size_t *outc, const void *inv, size_t inc)
{
	int src_samples;
	int dst_samples;
	size_t required_outc;

	if (!ctx || !ctx->initialized || !outv || !outc || !inv)
		return EINVAL;

	src_samples = (int)(inc / ctx->ich);

	/* Check buffer size limits */
	if (src_samples > ctx->max_src_samples)
		return EINVAL;

	/* Copy input data to libswresample buffer */
	memcpy(ctx->src_data[0], inv, inc * (size_t)ctx->sample_size);

	/* Convert samples */
	dst_samples = swr_convert(ctx->swr_ctx, ctx->dst_data, ctx->max_dst_samples,
	                         (const uint8_t**)ctx->src_data, src_samples);
	if (dst_samples < 0)
		return EINVAL;

	/*
	 * Drain any residual samples buffered inside libswresample so the
	 * reservoir never carries latency into the next JACK period.
	 */
	if (swr_get_delay(ctx->swr_ctx, ctx->orate) > 0 &&
	    dst_samples < ctx->max_dst_samples) {
		uint8_t *drain_dst[1];
		int drained;

		drain_dst[0] = ctx->dst_data[0] +
			(size_t)dst_samples * ctx->och * (size_t)ctx->sample_size;

		drained = swr_convert(ctx->swr_ctx, drain_dst,
		                      ctx->max_dst_samples - dst_samples,
		                      NULL, 0);
		if (drained > 0)
			dst_samples += drained;
	}

	/* Check output buffer size */
	required_outc = (size_t)dst_samples * ctx->och;
	if (*outc < required_outc)
		return ENOMEM;

	/* Copy output data */
	memcpy(outv, ctx->dst_data[0], required_outc * (size_t)ctx->sample_size);
	*outc = required_outc;

	return 0;
}
#endif

/**
 * Convert audio samples using external resampler (int16 path)
 */
int auresamp_ext_ctx_convert(struct auresamp_ext_ctx *ctx, int16_t *outv,
                            size_t *outc, const int16_t *inv, size_t inc)
{
#ifdef USE_LIBSWRESAMPLE
	if (ctx && ctx->fmt != AV_SAMPLE_FMT_S16)
		return EINVAL;
	return convert_generic(ctx, outv, outc, inv, inc);
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
 * Convert audio samples using external resampler (float path)
 */
int auresamp_ext_ctx_convert_f(struct auresamp_ext_ctx *ctx, float *outv,
                            size_t *outc, const float *inv, size_t inc)
{
#ifdef USE_LIBSWRESAMPLE
	if (ctx && ctx->fmt != AV_SAMPLE_FMT_FLT)
		return EINVAL;
	return convert_generic(ctx, outv, outc, inv, inc);
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
 * Compute input frames needed to produce out_frames output frames.
 * Uses swr_get_delay so the caller never over- or under-requests app audio.
 */
int64_t auresamp_ext_needed_input_frames(const struct auresamp_ext_ctx *ctx,
                                         int64_t out_frames)
{
#ifdef USE_LIBSWRESAMPLE
	int64_t delay, needed;

	if (!ctx || !ctx->initialized || !out_frames)
		return out_frames;

	/* Delay is expressed in units of irate (input sample rate). */
	delay = swr_get_delay(ctx->swr_ctx, (int64_t)ctx->irate);

	/* Frames needed = ceil(out_frames * irate / orate) - already buffered */
	needed = av_rescale_rnd(out_frames, (int64_t)ctx->irate,
	                        (int64_t)ctx->orate, AV_ROUND_UP) - delay;

	/* Always request at least 1 frame when output is required, so the
	 * caller never passes inc=0 to swr_convert in the normal data path. */
	return needed > 0 ? needed : 1;
#else
	(void)ctx;
	return out_frames;
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

#ifdef __GNUC__
/**
 * Library destructor - clean up any unfreed contexts on library unload
 */
static void __attribute__((destructor)) auresamp_ext_lib_destructor(void)
{
	auresamp_ext_registry_close();
}
#endif

/**
 * Public cleanup function for emergency cleanup of all external resampler contexts
 * Call this from libre_close() to prevent memory leaks
 */
void auresamp_ext_cleanup(void)
{
	auresamp_ext_registry_close();
}