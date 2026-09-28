/* DarkHouse — denoise configuration shared by the compute shaders (GLSL) and
 * the C++ reference implementation and node. Plain #defines only, so both
 * languages can include it; this is the single source of truth.
 *
 * Algorithm summary (see include/denoise.hpp for the full description):
 *   linear RGB -> sqrt (variance stabilisation) -> orthonormal opponent
 *   (Y, C1, C2) -> Laplacian pyramid -> Wiener shrinkage of each detail band
 *   with thresholds derived from a robust noise estimate -> reconstruction.
 */
#ifndef DARKHOUSE_DENOISE_CONFIG_H
#define DARKHOUSE_DENOISE_CONFIG_H

/* Detail bands d_0 .. d_(L-1); the pyramid holds G_1 .. G_L below full size. */
#define DH_DENOISE_MAX_LEVELS 5

/* Down passes: 8x8 workgroups, each producing 8x8 texels of level k+1 from a
 * 20x20 tile of level k (2 * 8 + 4 texels of 5-tap apron). */
#define DH_DENOISE_GROUP 8
#define DH_DENOISE_TILE 20

/* Noise estimation: log2 histogram of |Haar HH| of the stabilised image.
 * 8 bins per octave from 2^-16 to 2^0. Only every 4th down0 workgroup (a
 * diagonal pattern) contributes, which keeps global atomics cheap while
 * still sampling ~1/16 of all 2x2 blocks. */
#define DH_DENOISE_HIST_BINS 128
#define DH_DENOISE_HIST_BINS_PER_OCTAVE 8
#define DH_DENOISE_HIST_MIN_EXP (-16)
#define DH_DENOISE_SAMPLE_STRIDE 4

/* MAD -> sigma for Gaussian noise: sigma = median(|x|) / 0.6745. */
#define DH_DENOISE_MAD_TO_SIGMA 1.482602218505602

/* Standard deviation of detail band d_k for unit white noise in the
 * stabilised domain (the Haar HH estimator measures sigma at level 0).
 * Measured with the reference pyramid; tests/denoise_test.cpp re-derives
 * them and fails if they drift. */
#define DH_DENOISE_LEVEL_NOISE 0.9452, 0.2329, 0.1020, 0.0500, 0.0254

/* Per-band threshold weights, finest to coarsest. Luma keeps coarse bands
 * (they carry image structure); chroma noise is blotchy, so chroma
 * thresholds stay high down to coarse bands. */
#define DH_DENOISE_LUMA_WEIGHTS 1.0, 0.85, 0.65, 0.45, 0.30
#define DH_DENOISE_CHROMA_WEIGHTS 1.0, 1.0, 1.0, 0.90, 0.75

/* Threshold = strength * scale * weight[k] * level_noise[k] * sigma. At
 * strength 1 luma bands are shrunk at 3 sigma, chroma bands at 4 sigma. */
#define DH_DENOISE_LUMA_SCALE 3.0
#define DH_DENOISE_CHROMA_SCALE 4.0

/* Storage-buffer layout written by the sigma pass and read by the up/final
 * passes: thresholds[level][4] (Y, C1, C2, unused), then sigma[4]. */
#define DH_DENOISE_PARAMS_FLOATS (DH_DENOISE_MAX_LEVELS * 4 + 4)

#endif /* DARKHOUSE_DENOISE_CONFIG_H */
