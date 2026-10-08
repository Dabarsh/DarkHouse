// DarkHouse — tone math shared by the exposure node (global) and the local
// adjustment node (masked). Scene-referred linear RGB. The order is exposure
// gain, then highlight/shadow tone shaping, then contrast. Every step is
// multiplicative or pivots on middle grey, so a grey card at 0.18 keeps its
// value while contrast changes. Mirrored by applyTone() in src/mask_engine.cpp.

const vec3  LUMA_REC709      = vec3(0.2126, 0.7152, 0.0722);
const float MIDDLE_GREY      = 0.18;
const float TONE_RANGE_STOPS = 2.5;    // highlight/shadow masks reach full strength 2.5 EV from grey
const float MAX_TONE_STOPS   = 2.0;    // highlights/shadows = +/-1 changes gain by up to 2 stops
const float TONE_HALF_MAX    = 65504.0;

// exposureEV in stops; highlights, shadows and contrast in [-1, 1].
vec3 applyTone(vec3 rgb, float exposureEV, float highlights, float shadows, float contrast) {
    rgb = max(rgb, vec3(0.0));

    // 1. Exposure: plain linear gain.
    rgb *= exp2(exposureEV);

    // 2. Highlights / shadows: extra gain in stops, weighted by how far the
    //    pixel's luminance is above or below middle grey.
    float luma          = dot(rgb, LUMA_REC709);
    float stopsFromGrey = log2(max(luma, 1e-6) / MIDDLE_GREY);
    float highlightMask = smoothstep(0.0, TONE_RANGE_STOPS,  stopsFromGrey);
    float shadowMask    = smoothstep(0.0, TONE_RANGE_STOPS, -stopsFromGrey);
    float toneStops     = MAX_TONE_STOPS * (highlights * highlightMask + shadows * shadowMask);
    rgb *= exp2(toneStops);

    // 3. Contrast: power curve pivoting on middle grey. It is a slope change in
    //    log space, so grey stays put and black stays black.
    float slope = exp2(contrast);  // [0.5, 2]
    rgb = MIDDLE_GREY * pow(max(rgb / MIDDLE_GREY, vec3(1e-8)), vec3(slope));

    // Clamp so an extreme push cannot write +Inf into the half-float target.
    return min(rgb, vec3(TONE_HALF_MAX));
}
