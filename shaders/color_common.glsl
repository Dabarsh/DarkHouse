// DarkHouse — colour helpers shared by the colour compute shaders. Must match
// src/color_adjust.cpp (the CPU reference) operation for operation.

// Oklab (Björn Ottosson) on linear sRGB / Rec.709.
vec3 signedCbrt(vec3 v) { return sign(v) * pow(abs(v), vec3(1.0 / 3.0)); }

vec3 linearSrgbToOklab(vec3 c) {
    vec3 lms = signedCbrt(vec3(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b,
                               0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b,
                               0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b));
    return vec3(0.2104542553 * lms.x + 0.7936177850 * lms.y - 0.0040720468 * lms.z,
                1.9779984951 * lms.x - 2.4285922050 * lms.y + 0.4505937099 * lms.z,
                0.0259040371 * lms.x + 0.7827717662 * lms.y - 0.8086757660 * lms.z);
}

vec3 oklabToLinearSrgb(vec3 lab) {
    vec3 lms = vec3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                    lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                    lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    lms = lms * lms * lms;
    return vec3(4.0767416621 * lms.x - 3.3077115913 * lms.y + 0.2309699292 * lms.z,
                -1.2684380046 * lms.x + 2.6097574011 * lms.y - 0.3413193965 * lms.z,
                -0.0041960863 * lms.x - 0.7034186147 * lms.y + 1.7076147010 * lms.z);
}

const float HALF_MAX = 65504.0;

// Clamps to the half-float range; negative (out-of-gamut) light becomes 0.
vec3 storable(vec3 rgb) { return clamp(rgb, vec3(0.0), vec3(HALF_MAX)); }
