// DarkHouse — mask component evaluation, shared by the mask shaders. Must
// match componentAlpha() / combineMask() in src/mask_engine.cpp.

// darkhouse::MaskComponentGpu (80 bytes, std430).
struct MaskComponentGpu {
    uint shape;       // darkhouse::MaskShape
    uint mode;        // darkhouse::MaskMode
    uint invert;
    uint brushSlot;   // BRUSH: coverage in brush layer slot / 4, channel slot % 4
    float opacity;
    float angle;      // radians
    float feather;
    float falloff;
    vec4 startEnd;    // start.xy, end.xy (normalized image coordinates)
    vec4 sizeRange;   // radii.xy (fractions of width / height), low, high
    vec4 hueParams;   // hue, hue width (degrees), 0, 0
};

const uint SHAPE_BRUSH = 0u;
const uint SHAPE_LINEAR = 1u;
const uint SHAPE_RADIAL = 2u;
const uint SHAPE_LUMINANCE = 3u;
const uint SHAPE_COLOR = 4u;
const uint SHAPE_SUBJECT = 5u;
const uint SHAPE_SKY = 6u;

const uint MODE_ADD = 0u;
const uint MODE_SUBTRACT = 1u;
const uint MODE_INTERSECT = 2u;

float hueDistance(float a, float b) { return abs(mod(a - b + 540.0, 360.0) - 180.0); }

// `pixel` is the texel centre in pixels; `lab` the source colour in Oklab.
float componentAlpha(MaskComponentGpu c, vec2 pixel, vec2 size, vec3 lab, float brush) {
    float alpha = 0.0;
    if (c.shape == SHAPE_BRUSH) {
        alpha = brush;
    } else if (c.shape == SHAPE_LINEAR) {
        vec2 s = c.startEnd.xy * size;
        vec2 d = c.startEnd.zw * size - s;
        float t = dot(pixel - s, d) / max(dot(d, d), 1e-6);
        alpha = 1.0 - smoothstep(0.0, 1.0, t);
    } else if (c.shape == SHAPE_RADIAL) {
        vec2 q = pixel - c.startEnd.xy * size;
        float cs = cos(c.angle), sn = sin(c.angle);
        vec2 rotated = vec2(cs * q.x + sn * q.y, -sn * q.x + cs * q.y);
        vec2 radii = max(c.sizeRange.xy * size, vec2(1e-3));
        float f = clamp(c.feather, 0.001, 1.0);
        alpha = 1.0 - smoothstep(1.0 - f, 1.0, length(rotated / radii));
    } else if (c.shape == SHAPE_LUMINANCE) {
        float l = clamp(lab.x, 0.0, 1.0);
        float f = max(c.falloff, 1e-3);
        alpha = smoothstep(c.sizeRange.z - f, c.sizeRange.z, l) * (1.0 - smoothstep(c.sizeRange.w, c.sizeRange.w + f, l));
    } else if (c.shape == SHAPE_COLOR) {
        float chroma = length(lab.yz);
        if (chroma > 0.0) {
            float hue = degrees(atan(lab.z, lab.y));
            float halfWidth = 0.5 * c.hueParams.y;
            float soft = max(c.falloff * 90.0, 1.0);
            alpha = (1.0 - smoothstep(halfWidth, halfWidth + soft, hueDistance(hue, c.hueParams.x))) *
                    smoothstep(c.sizeRange.z, c.sizeRange.z + 0.03, chroma);
        }
    } else if (c.shape == SHAPE_SUBJECT) {
        // Placeholder until subject segmentation: a centre-weighted ellipse.
        vec2 q = (pixel / size - 0.5) / vec2(0.32, 0.42);
        alpha = 1.0 - smoothstep(0.6, 1.0, length(q));
    } else if (c.shape == SHAPE_SKY) {
        // Placeholder until sky segmentation: blue or bright near-white, in the upper frame.
        float chroma = length(lab.yz);
        float blue = 0.0;
        if (chroma > 0.0) {
            float hue = degrees(atan(lab.z, lab.y));
            blue = (1.0 - smoothstep(40.0, 70.0, hueDistance(hue, 240.0))) * smoothstep(0.02, 0.06, chroma);
        }
        float bright = smoothstep(0.8, 0.95, lab.x) * (1.0 - smoothstep(0.02, 0.05, chroma));
        float upper = 1.0 - smoothstep(0.35, 0.7, pixel.y / size.y);
        alpha = max(blue, bright) * upper;
    }
    return c.invert != 0u ? 1.0 - alpha : alpha;
}

float combineMask(float mask, MaskComponentGpu c, float alpha) {
    if (c.mode == MODE_SUBTRACT) return mask * (1.0 - c.opacity * alpha);
    if (c.mode == MODE_INTERSECT) return mask * (1.0 - c.opacity * (1.0 - alpha));
    float a = c.opacity * alpha;
    return mask + a - mask * a;  // ADD: union (screen)
}
