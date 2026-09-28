// DarkHouse — helpers shared by the denoise compute shaders. Must match the
// CPU reference in src/denoise.cpp operation for operation.

#include "denoise_config.h"

const float kInvSqrt2 = 0.70710678118654752;
const float kInvSqrt3 = 0.57735026918962576;
const float kInvSqrt6 = 0.40824829046386302;
const float kBinomial[5] = float[](0.0625, 0.25, 0.375, 0.25, 0.0625);

// sqrt (variance stabilisation) followed by the orthonormal opponent transform.
vec3 stabilize(vec3 rgb) {
    vec3 s = sqrt(max(rgb, vec3(0.0)));
    return vec3((s.r + s.g + s.b) * kInvSqrt3, (s.r - s.b) * kInvSqrt2, (s.r - 2.0 * s.g + s.b) * kInvSqrt6);
}

vec3 destabilize(vec3 s) {
    float y = s.x * kInvSqrt3;
    vec3 rgb = vec3(y + s.y * kInvSqrt2 + s.z * kInvSqrt6,
                    y - 2.0 * s.z * kInvSqrt6,
                    y - s.y * kInvSqrt2 + s.z * kInvSqrt6);
    rgb = max(rgb, vec3(0.0));
    return rgb * rgb;
}

// Rounds to the nearest fp16 value (ties to even) in fp32. Vulkan lets fp16
// image stores round either to nearest or toward zero; storing a value that
// is already exactly representable makes every implementation store the
// same bits, and avoids the downward drift truncation would accumulate
// through the pyramid. (Normal fp16 range; subnormals are negligible here.)
vec4 roundToHalf(vec4 v) {
    uvec4 bits = floatBitsToUint(v);
    bits += uvec4(0x0FFFu) + ((bits >> 13u) & uvec4(1u));
    return uintBitsToFloat(bits & uvec4(0xFFFFE000u));
}

float shrink(float d, float threshold) {
    if (threshold <= 0.0) return d;
    float d2 = d * d;
    return d * d2 / (d2 + threshold * threshold);
}

vec3 shrink3(vec3 d, vec3 threshold) {
    return vec3(shrink(d.x, threshold.x), shrink(d.y, threshold.y), shrink(d.z, threshold.z));
}

// Coarse texels feeding the bilinear upsample at fine texel p (texel-centre
// aligned): `a` covers p, `b` is the neighbour on the side p lies towards.
// Weights are fixed: 3/4 for a, 1/4 for b, per axis.
void upsampleTaps(ivec2 p, ivec2 coarseSize, out ivec2 a, out ivec2 b) {
    ivec2 c = p >> 1;
    ivec2 n = c + ((p & 1) * 2 - 1);  // even -> c - 1, odd -> c + 1
    a = clamp(c, ivec2(0), coarseSize - 1);
    b = clamp(n, ivec2(0), coarseSize - 1);
}

vec3 upsampleCombine(vec3 aa, vec3 ba, vec3 ab, vec3 bb) {
    vec3 top = 0.75 * aa + 0.25 * ba;
    vec3 bottom = 0.75 * ab + 0.25 * bb;
    return 0.75 * top + 0.25 * bottom;
}

uint histogramBin(float magnitude) {
    if (!(magnitude > 0.0)) return 0u;
    float position = (log2(magnitude) - float(DH_DENOISE_HIST_MIN_EXP)) * float(DH_DENOISE_HIST_BINS_PER_OCTAVE);
    return uint(clamp(floor(position), 0.0, float(DH_DENOISE_HIST_BINS - 1)));
}
