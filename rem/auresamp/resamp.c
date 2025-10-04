/**
 * @file resamp.c Audio Resampler
 *
 * Copyright (C) 2010 Creytiv.com
 */

#include <string.h>
#include <re.h>
#include <rem_fir.h>
#include <rem_auresamp.h>
#include "auresamp_internal.h"


/* 48kHz sample-rate, 4kHz cutoff (pass 0-3kHz, stop 5-24kHz) */
static const int16_t fir_48_4[] = {
	 62,   -176,   -329,   -556,   -802,  -1005,  -1090,   -985,
       -636,    -23,    826,   1837,   2894,   3859,   4595,   4994,
       4994,   4595,   3859,   2894,   1837,    826,    -23,   -636,
       -985,  -1090,  -1005,   -802,   -556,   -329,   -176,     62
};

/* 48kHz sample-rate, 8kHz cutoff (pass 0-7kHz, stop 9-24kHz) */
static const int16_t fir_48_8[] = {
	238,    198,   -123,   -738,  -1268,  -1204,   -380,    714,
       1164,    376,  -1220,  -2206,  -1105,   2395,   6909,  10069,
      10069,   6909,   2395,  -1105,  -2206,  -1220,    376,   1164,
	714,   -380,  -1204,  -1268,   -738,   -123,    198,    238
};

/* 16kHz sample-rate, 4kHz cutoff and 32kHz sample-rate, 8 kHz cutoff */
static const int16_t fir_16_4[] = {
		22, 60, -41, -157, -9, 322, 195, -490, -613, 539, 1362, -229,
		-2657, -1101, 6031, 13167, 13167, 6031, -1101, -2657, -229,
		1362, 539, -613, -490, 195, 322, -9, -157, -41, 60, 22
};

static void upsample_mono2mono(int16_t *outv, const int16_t *inv,
			       size_t inc, unsigned ratio)
{
	unsigned i;

	while (inc >= 1) {

		for (i=0; i<ratio; i++)
			*outv++ = *inv;

		++inv;
		--inc;
	}
}


static void upsample_mono2stereo(int16_t *outv, const int16_t *inv,
				 size_t inc, unsigned ratio)
{
	unsigned i;

	ratio *= 2;

	while (inc >= 1) {

		for (i=0; i<ratio; i++)
			*outv++ = *inv;

		++inv;
		--inc;
	}
}


static void upsample_stereo2mono(int16_t *outv, const int16_t *inv,
				 size_t inc, unsigned ratio)
{
	unsigned i;

	while (inc >= 2) {

		const int16_t s = inv[0]/2 + inv[1]/2;

		for (i=0; i<ratio; i++)
			*outv++ = s;

		inv += 2;
		inc -= 2;
	}
}


static void upsample_stereo2stereo(int16_t *outv, const int16_t *inv,
				   size_t inc, unsigned ratio)
{
	unsigned i;

	while (inc >= 2) {

		for (i=0; i<ratio; i++) {
			*outv++ = inv[0];
			*outv++ = inv[1];
		}

		inv += 2;
		inc -= 2;
	}
}


static void downsample_mono2mono(int16_t *outv, const int16_t *inv,
				 size_t inc, unsigned ratio)
{
	while (inc >= ratio) {

		*outv++ = *inv;

		inv += ratio;
		inc -= ratio;
	}
}


static void downsample_mono2stereo(int16_t *outv, const int16_t *inv,
				   size_t inc, unsigned ratio)
{
	while (inc >= ratio) {

		*outv++ = *inv;
		*outv++ = *inv;

		inv += ratio;
		inc -= ratio;
	}
}


static void downsample_stereo2mono(int16_t *outv, const int16_t *inv,
				   size_t inc, unsigned ratio)
{
	ratio *= 2;

	while (inc >= ratio) {

		*outv++ = inv[0]/2 + inv[1]/2;

		inv += ratio;
		inc -= ratio;
	}
}


static void downsample_stereo2stereo(int16_t *outv, const int16_t *inv,
				     size_t inc, unsigned ratio)
{
	ratio *= 2;

	while (inc >= ratio) {

		*outv++ = inv[0];
		*outv++ = inv[1];

		inv += ratio;
		inc -= ratio;
	}
}


/**
 * Initialize a resampler object
 *
 * @param rs Resampler to initialize
 */
void auresamp_init(struct auresamp *rs)
{
	if (!rs)
		return;

	memset(rs, 0, sizeof(*rs));
	fir_reset(&rs->fir);
	rs->ext_ctx = NULL;
	rs->use_external = false;
}


/**
 * Configure a resampler object
 *
 * @note The sample rate ratio must be an integer
 *
 * @param rs    Resampler
 * @param irate Input sample rate
 * @param ich   Input channel count
 * @param orate Output sample rate
 * @param och   Output channel count
 *
 * @return 0 if success, otherwise error code
 */
int auresamp_setup(struct auresamp *rs, uint32_t irate, unsigned ich,
		   uint32_t orate, unsigned och)
{
	int err;
	
	if (!rs || !irate || !ich || !orate || !och)
		return EINVAL;

	/* Clean up any existing external context */
	if (rs->ext_ctx) {
		auresamp_ext_ctx_close((struct auresamp_ext_ctx *)rs->ext_ctx);
		rs->ext_ctx = NULL;
		rs->use_external = false;
	}

	if (orate == irate && och == ich) {
		auresamp_init(rs);
		return 0;
	}

	/* First, try the built-in simple resampler */
	bool can_use_simple = true;
	
	if (orate >= irate) {
		if (orate % irate)
			can_use_simple = false;
	} else {
		if (irate % orate)
			can_use_simple = false;
	}

	if (can_use_simple) {
		/* Use original simple resampler logic */
		if (orate >= irate) {
			if (ich == 1 && och == 1)
				rs->resample = upsample_mono2mono;
			else if (ich == 1 && och == 2)
				rs->resample = upsample_mono2stereo;
			else if (ich == 2 && och == 1)
				rs->resample = upsample_stereo2mono;
			else if (ich == 2 && och == 2)
				rs->resample = upsample_stereo2stereo;
			else
				can_use_simple = false;

			if (can_use_simple) {
				if (!rs->up || orate != rs->orate || och != rs->och)
					fir_reset(&rs->fir);

				rs->ratio = orate / irate;
				rs->up    = true;

				if (orate == irate) {
					rs->tapv = NULL;
					rs->tapc = 0;
				}
				else if (orate == 48000 && irate == 16000) {
					rs->tapv = fir_48_8;
					rs->tapc = RE_ARRAY_SIZE(fir_48_8);
				}
				else if ((orate == 16000 && irate == 8000) ||
		                         (orate == 32000 && irate == 16000)) {
					rs->tapv = fir_16_4;
					rs->tapc = RE_ARRAY_SIZE(fir_16_4);
				}
				else {
					rs->tapv = fir_48_4;
					rs->tapc = RE_ARRAY_SIZE(fir_48_4);
				}
			}
		}
		else {
			if (ich == 1 && och == 1)
				rs->resample = downsample_mono2mono;
			else if (ich == 1 && och == 2)
				rs->resample = downsample_mono2stereo;
			else if (ich == 2 && och == 1)
				rs->resample = downsample_stereo2mono;
			else if (ich == 2 && och == 2)
				rs->resample = downsample_stereo2stereo;
			else
				can_use_simple = false;

			if (can_use_simple) {
				if (rs->up || irate != rs->irate || ich != rs->ich)
					fir_reset(&rs->fir);

				rs->ratio = irate / orate;
				rs->up    = false;

				if (irate == 48000 && orate == 16000) {
					rs->tapv = fir_48_8;
					rs->tapc = RE_ARRAY_SIZE(fir_48_8);
				}
				else if ((irate == 16000 && orate == 8000) ||
		                         (irate == 32000 && orate == 16000)) {
					rs->tapv = fir_16_4;
					rs->tapc = RE_ARRAY_SIZE(fir_16_4);
				}
				else {
					rs->tapv = fir_48_4;
					rs->tapc = RE_ARRAY_SIZE(fir_48_4);
				}
			}
		}

		if (can_use_simple) {
			rs->orate = orate;
			rs->och   = och;
			rs->irate = irate;
			rs->ich   = ich;
			rs->use_external = false;
			
			re_printf("auresamp: Using built-in resampler for %u->%u Hz\n",
			          irate, orate);
			return 0;
		}
	}

	/* Simple resampler can't handle this, try external resampler */
	struct auresamp_ext_ctx *ext_ctx = NULL;
	err = auresamp_ext_ctx_setup(&ext_ctx, irate, ich, orate, och);
	if (err == 0) {
		rs->ext_ctx = ext_ctx;
		rs->use_external = true;
		rs->orate = orate;
		rs->och   = och;
		rs->irate = irate;
		rs->ich   = ich;
		
		re_printf("auresamp: Using external resampler for %u->%u Hz (ratio: %.3f)\n",
		          irate, orate, (double)orate / irate);
		return 0;
	}

	re_printf("auresamp: No suitable resampler available for %u->%u Hz\n",
	          irate, orate);
	return ENOTSUP;
}


/**
 * Resample
 *
 * @note When downsampling, the input count must be divisible by rate ratio
 *
 * @param rs   Resampler
 * @param outv Output samples
 * @param outc Output sample count (in/out)
 * @param inv  Input samples
 * @param inc  Input sample count
 *
 * @return 0 if success, otherwise error code
 */
int auresamp(struct auresamp *rs, int16_t *outv, size_t *outc,
	     const int16_t *inv, size_t inc)
{
	size_t incc, outcc;

	if (!rs || !outv || !outc || !inv)
		return EINVAL;

	/* Use external resampler if active */
	if (rs->use_external && rs->ext_ctx) {
		return auresamp_ext_ctx_convert((struct auresamp_ext_ctx *)rs->ext_ctx,
		                               outv, outc, inv, inc);
	}

	/* Use built-in simple resampler */
	if (!rs->resample)
		return EINVAL;

	incc = inc / rs->ich;

	if (rs->up) {
		outcc = incc * rs->ratio;

		if (*outc < outcc * rs->och)
			return ENOMEM;

		rs->resample(outv, inv, inc, rs->ratio);

		*outc = outcc * rs->och;

		if (rs->tapv)
			fir_filter(&rs->fir, outv, outv, *outc, rs->och,
				   rs->tapv, rs->tapc);
	}
	else {
		outcc = incc / rs->ratio;

		if (*outc < outcc * rs->och || *outc < inc)
			return ENOMEM;

		fir_filter(&rs->fir, outv, inv, inc, rs->ich,
			   rs->tapv, rs->tapc);

		rs->resample(outv, outv, inc, rs->ratio);

		*outc = outcc * rs->och;
	}

	return 0;
}


/**
 * Close and cleanup a resampler object
 *
 * @param rs Resampler to close
 */
void auresamp_close(struct auresamp *rs)
{
	if (!rs)
		return;

	if (rs->ext_ctx) {
		re_printf("auresamp: Closing external resampler context\n");
		auresamp_ext_ctx_close((struct auresamp_ext_ctx *)rs->ext_ctx);
		rs->ext_ctx = NULL;
		rs->use_external = false;
	}
}


/**
 * Calculate required output buffer size for resampling
 *
 * This function calculates the maximum possible output size for a given
 * input size and sample rate conversion, with appropriate safety margins
 * for fractional resampling.
 *
 * @param irate Input sample rate
 * @param orate Output sample rate  
 * @param input_samples Number of input samples (includes all channels)
 * @param ch Number of channels
 *
 * @return Required output buffer size in samples
 */
size_t auresamp_calc_output_size(uint32_t irate, uint32_t orate, 
				 size_t input_samples, unsigned ch)
{
	size_t output_samples;
	
	if (!irate || !orate || !input_samples || !ch)
		return 0;
	
	/* Convert total samples to per-channel samples */
	size_t input_frames = input_samples / ch;
	
	if (irate == orate) {
		/* No resampling needed */
		return input_samples;
	}
	
	/* Calculate base output frames using 64-bit arithmetic to avoid overflow */
	uint64_t output_frames = ((uint64_t)input_frames * orate) / irate;
	
	/* Add safety margin based on resampling direction and ratio type */
	if (orate > irate) {
		/* Upsampling: add margin for rounding and internal buffering */
		if ((orate % irate) == 0) {
			/* Integer ratio - minimal margin needed */
			output_frames += 2;
		} else {
			/* Fractional ratio - add proportional margin */
			output_frames += (output_frames * 5) / 100 + 2; /* 5% + 2 frames */
		}
	} else {
		/* Downsampling: usually more predictable, smaller margin */
		if ((irate % orate) == 0) {
			/* Integer ratio - minimal margin */
			output_frames += 1;
		} else {
			/* Fractional ratio - small margin */
			output_frames += (output_frames * 2) / 100 + 1; /* 2% + 1 frame */
		}
	}
	
	/* Convert back to total samples */
	output_samples = output_frames * ch;
	
	/* Ensure minimum reasonable size - don't allocate less than input for downsampling
	 * as some resamplers may need temp space */
	if (orate < irate && output_samples < input_samples) {
		output_samples = input_samples;
	}
	
	return output_samples;
}
