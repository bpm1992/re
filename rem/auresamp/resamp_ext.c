/**
 * @file resamp_ext.c Enhanced Audio Resampler with External Library Support
 *
 * Copyright (C) 2025 - Enhanced for non-integer ratios
 */

#include <string.h>
#include <re.h>
#include <rem_fir.h>
#include <rem_auresamp.h>
#include <rem_auresamp_ext.h>

#ifdef USE_LIBSWRESAMPLE
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#endif

/**
 * Initialize enhanced resampler
 */
void auresamp_ext_init(struct auresamp_ext *rsx)
{
	if (!rsx)
		return;

	memset(rsx, 0, sizeof(*rsx));
	auresamp_init(&rsx->simple);
	rsx->use_external = false;
	rsx->initialized = false;

#ifdef USE_LIBSWRESAMPLE
	rsx->swr_ctx = NULL;
	rsx->src_data = NULL;
	rsx->dst_data = NULL;
#endif
}

/**
 * Check if sample rate conversion requires external library
 */
static bool needs_external_resampler(uint32_t irate, uint32_t orate)
{
	/* If rates are equal, no resampling needed */
	if (irate == orate)
		return false;

	/* Check if either direction is divisible (simple resampler can handle) */
	if (orate >= irate) {
		return (orate % irate) != 0;
	} else {
		return (irate % orate) != 0;
	}
}

#ifdef USE_LIBSWRESAMPLE
/**
 * Setup FFmpeg libswresample context
 */
static int setup_swresample(struct auresamp_ext *rsx, uint32_t irate, 
                           unsigned ich, uint32_t orate, unsigned och)
{
	int ret;

	/* Allocate resampling context */
	rsx->swr_ctx = swr_alloc();
	if (!rsx->swr_ctx) {
		re_printf("auresamp_ext: Failed to allocate SwrContext\n");
		return ENOMEM;
	}

	/* Set input/output options */
	av_opt_set_int(rsx->swr_ctx, "in_channel_layout", 
	               ich == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO, 0);
	av_opt_set_int(rsx->swr_ctx, "in_sample_rate", irate, 0);
	av_opt_set_sample_fmt(rsx->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);

	av_opt_set_int(rsx->swr_ctx, "out_channel_layout",
	               och == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO, 0);
	av_opt_set_int(rsx->swr_ctx, "out_sample_rate", orate, 0);
	av_opt_set_sample_fmt(rsx->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

	/* Initialize the resampling context */
	ret = swr_init(rsx->swr_ctx);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to initialize SwrContext: %d\n", ret);
		swr_free(&rsx->swr_ctx);
		return EINVAL;
	}

	/* Allocate input/output buffers */
	rsx->max_src_samples = 4096;  /* Reasonable buffer size */
	rsx->max_dst_samples = av_rescale_rnd(rsx->max_src_samples, orate, irate, AV_ROUND_UP);

	ret = av_samples_alloc_array_and_samples(&rsx->src_data, &rsx->src_linesize,
	                                         ich, rsx->max_src_samples, AV_SAMPLE_FMT_S16, 0);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to allocate source samples\n");
		swr_free(&rsx->swr_ctx);
		return ENOMEM;
	}

	ret = av_samples_alloc_array_and_samples(&rsx->dst_data, &rsx->dst_linesize,
	                                         och, rsx->max_dst_samples, AV_SAMPLE_FMT_S16, 0);
	if (ret < 0) {
		re_printf("auresamp_ext: Failed to allocate destination samples\n");
		av_freep(&rsx->src_data[0]);
		av_freep(&rsx->src_data);
		swr_free(&rsx->swr_ctx);
		return ENOMEM;
	}

	return 0;
}
#endif

/**
 * Setup enhanced resampler with fallback capability
 */
int auresamp_ext_setup(struct auresamp_ext *rsx, uint32_t irate, unsigned ich,
                       uint32_t orate, unsigned och)
{
	int err;

	if (!rsx || !irate || !ich || !orate || !och)
		return EINVAL;

	/* Cleanup any previous setup */
	if (rsx->initialized) {
		auresamp_ext_close(rsx);
		auresamp_ext_init(rsx);
	}

	/* Store configuration */
	rsx->irate = irate;
	rsx->orate = orate;
	rsx->ich = ich;
	rsx->och = och;

	/* Try simple resampler first (for integer ratios) */
	err = auresamp_setup(&rsx->simple, irate, ich, orate, och);
	if (err == 0) {
		/* Simple resampler can handle this conversion */
		rsx->use_external = false;
		rsx->initialized = true;
		re_printf("auresamp_ext: Using built-in resampler for %u->%u Hz\n",
		          irate, orate);
		return 0;
	}

	/* Check if we need external resampler */
	if (!needs_external_resampler(irate, orate)) {
		/* This shouldn't happen, but just in case */
		re_printf("auresamp_ext: Unexpected failure with integer ratio\n");
		return err;
	}

#ifdef USE_LIBSWRESAMPLE
	/* Setup external resampler */
	err = setup_swresample(rsx, irate, ich, orate, och);
	if (err == 0) {
		rsx->use_external = true;
		rsx->initialized = true;
		re_printf("auresamp_ext: Using libswresample for %u->%u Hz (ratio: %.3f)\n",
		          irate, orate, (double)orate / irate);
		return 0;
	}
#endif

	re_printf("auresamp_ext: No suitable resampler available for %u->%u Hz\n",
	          irate, orate);
	return ENOTSUP;
}

/**
 * Enhanced resampling function
 */
int auresamp_ext(struct auresamp_ext *rsx, int16_t *outv, size_t *outc,
                 const int16_t *inv, size_t inc)
{
	if (!rsx || !rsx->initialized || !outv || !outc || !inv)
		return EINVAL;

	/* Use simple resampler if possible */
	if (!rsx->use_external) {
		return auresamp(&rsx->simple, outv, outc, inv, inc);
	}

#ifdef USE_LIBSWRESAMPLE
	/* Use libswresample for arbitrary ratios */
	if (rsx->swr_ctx) {
		int src_samples = inc / rsx->ich;
		int dst_samples;
		int ret;

		/* Check buffer size limits */
		if (src_samples > rsx->max_src_samples) {
			re_printf("auresamp_ext: Input sample count too large: %d > %d\n",
			          src_samples, rsx->max_src_samples);
			return EINVAL;
		}

		/* Copy input data to libswresample buffer */
		memcpy(rsx->src_data[0], inv, inc * sizeof(int16_t));

		/* Convert samples */
		dst_samples = swr_convert(rsx->swr_ctx, rsx->dst_data, rsx->max_dst_samples,
		                         (const uint8_t**)rsx->src_data, src_samples);
		if (dst_samples < 0) {
			re_printf("auresamp_ext: swr_convert failed: %d\n", dst_samples);
			return EINVAL;
		}

		/* Check output buffer size */
		size_t required_outc = dst_samples * rsx->och;
		if (*outc < required_outc) {
			re_printf("auresamp_ext: Output buffer too small: %zu < %zu\n",
			          *outc, required_outc);
			return ENOMEM;
		}

		/* Copy output data */
		memcpy(outv, rsx->dst_data[0], required_outc * sizeof(int16_t));
		*outc = required_outc;

		return 0;
	}
#endif

	return ENOTSUP;
}

/**
 * Cleanup enhanced resampler
 */
void auresamp_ext_close(struct auresamp_ext *rsx)
{
	if (!rsx)
		return;

#ifdef USE_LIBSWRESAMPLE
	if (rsx->src_data) {
		av_freep(&rsx->src_data[0]);
		av_freep(&rsx->src_data);
	}
	if (rsx->dst_data) {
		av_freep(&rsx->dst_data[0]);
		av_freep(&rsx->dst_data);
	}
	if (rsx->swr_ctx) {
		swr_free(&rsx->swr_ctx);
	}
#endif

	rsx->initialized = false;
	rsx->use_external = false;
}