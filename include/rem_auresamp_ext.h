/**
 * @file rem_auresamp_ext.h Enhanced Audio Resampling with External Libraries
 *
 * Copyright (C) 2025 - Enhanced for non-integer ratios
 */

#ifndef REM_AURESAMP_EXT_H
#define REM_AURESAMP_EXT_H

#ifdef USE_LIBSWRESAMPLE
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#endif

/**
 * Enhanced resampler context for handling arbitrary sample rate conversions
 */
struct auresamp_ext {
	/* Original simple resampler for integer ratios */
	struct auresamp simple;
	
#ifdef USE_LIBSWRESAMPLE
	/* FFmpeg libswresample context for arbitrary ratios */
	SwrContext *swr_ctx;
	uint8_t **src_data;
	uint8_t **dst_data;
	int src_linesize;
	int dst_linesize;
	int max_src_samples;
	int max_dst_samples;
#endif

	/* Current configuration */
	uint32_t irate, orate;
	unsigned ich, och;
	bool use_external;  /* Flag to indicate if external resampler is active */
	bool initialized;
};

/* Initialize enhanced resampler */
void auresamp_ext_init(struct auresamp_ext *rsx);

/* Setup enhanced resampler with fallback capability */
int auresamp_ext_setup(struct auresamp_ext *rsx, uint32_t irate, unsigned ich,
                       uint32_t orate, unsigned och);

/* Enhanced resampling function */
int auresamp_ext(struct auresamp_ext *rsx, int16_t *outv, size_t *outc,
                 const int16_t *inv, size_t inc);

/* Cleanup enhanced resampler */
void auresamp_ext_close(struct auresamp_ext *rsx);

#endif /* REM_AURESAMP_EXT_H */