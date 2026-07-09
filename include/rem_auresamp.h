/**
 * @file rem_auresamp.h Audio Resampling
 *
 * Copyright (C) 2010 Creytiv.com
 */

/**
 * Defines the audio resampler handler
 *
 * @param outv  Output samples
 * @param inv   Input samples
 * @param inc   Number of input samples
 * @param ratio Resample ratio
 */
typedef void (auresamp_h)(int16_t *outv, const int16_t *inv,
			  size_t inc, unsigned ratio);

/** Defines the resampler state */
struct auresamp {
	struct fir fir;        /**< FIR filter state */
	auresamp_h *resample;  /**< Resample handler */
	const int16_t *tapv;   /**< FIR filter taps */
	size_t tapc;           /**< FIR filter tap count */
	uint32_t orate, irate; /**< Input/output sample rate */
	unsigned och, ich;     /**< Input/output channel count */
	unsigned ratio;        /**< Resample ratio */
	bool up;               /**< Up/down sample flag */

	/* Enhanced resampling support */
	void *ext_ctx;         /**< External resampler context (opaque) */
	bool use_external;     /**< Flag to indicate external resampler use */
	bool use_float;        /**< true if set up for float samples (auresampf) */
};

void   auresamp_init(struct auresamp *rs);

/**
 * Configure a resampler object
 *
 * @param rs               Resampler
 * @param irate            Input sample rate
 * @param ich              Input channel count
 * @param orate            Output sample rate
 * @param och              Output channel count
 * @param period_in_frames Maximum number of input frames the caller will
 *                         ever pass to one auresamp()/auresampf() call
 *                         (i.e. one JACK period's worth). Bounds the
 *                         external resampler's internal reservoir so it
 *                         cannot accumulate multiple periods of latency.
 * @param use_float        true to configure for auresampf() (float samples),
 *                         false for auresamp() (int16 samples). Float is
 *                         only ever routed through the external
 *                         (libswresample) resampler.
 *
 * @return 0 if success, otherwise error code
 */
int    auresamp_setup(struct auresamp *rs, uint32_t irate, unsigned ich,
		      uint32_t orate, unsigned och,
		      size_t period_in_frames, bool use_float);
int    auresamp(struct auresamp *rs, int16_t *outv, size_t *outc,
		const int16_t *inv, size_t inc);

/**
 * Resample float samples. Only valid when the resampler was configured
 * with use_float=true in auresamp_setup().
 *
 * @param rs   Resampler
 * @param outv Output samples
 * @param outc Output sample count (in/out)
 * @param inv  Input samples
 * @param inc  Input sample count
 *
 * @return 0 if success, otherwise error code
 */
int    auresampf(struct auresamp *rs, float *outv, size_t *outc,
		 const float *inv, size_t inc);
void   auresamp_close(struct auresamp *rs);
void   auresamp_ext_cleanup(void);
size_t auresamp_calc_output_size(uint32_t irate, uint32_t orate,
				 size_t input_samples, unsigned ch);

/**
 * Return the number of input *frames* (not samples) needed to produce
 * out_frames output frames.  For the external (libswresample) path this
 * accounts for samples already queued inside the resampler via
 * swr_get_delay, so the caller never over-drains the app playout buffer.
 * For the built-in integer-ratio path the ratio is used directly.
 * Returns 0 when out_frames is 0.
 */
size_t auresamp_get_input_frames(const struct auresamp *rs, size_t out_frames);
