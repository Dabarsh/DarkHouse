// Lens corrections (CPU, no GPU): the lensfun-format database reader, lens
// and camera matching, calibration interpolation, and the correction
// geometry (distortion, chromatic aberration, vignetting, constrain crop).
// The GPU node is checked against applyLensCorrection in color_gpu_test.cpp.
//
// The XML below imitates the lensfun layout; its coefficients are test
// values, not measurements of real lenses.

#include "color_adjust.hpp"
#include "develop_stack.hpp"
#include "lens_correction.hpp"
#include "lens_database.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace darkhouse;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cout << "FAIL line " << __LINE__ << ": " #condition << '\n';              \
        }                                                                                  \
    } while (false)

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

constexpr const char* kDatabase = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE lensdatabase SYSTEM "lensfun-database.dtd">
<lensdatabase version="1">
    <!-- test cameras -->
    <camera>
        <maker>Testcam</maker>
        <maker lang="de">Testkamera</maker>
        <model>TC-1</model>
        <mount>Test</mount>
        <cropfactor>1.5</cropfactor>
    </camera>
    <camera>
        <maker>Testcam</maker>
        <model>TC-Full</model>
        <mount>Test</mount>
        <cropfactor>1.0</cropfactor>
    </camera>
    <lens>
        <maker>Testcam</maker>
        <model>TX 18-55mm f/3.5-5.6 &amp; Macro</model>
        <model lang="en">TX 18-55 mm Zoom</model>
        <mount>Test</mount>
        <cropfactor>1.5</cropfactor>
        <calibration>
            <distortion model="ptlens" focal="18" a="0.01" b="-0.04" c="0.0"/>
            <distortion model="ptlens" focal="55" a="0.0" b="0.02" c="0.0"/>
            <tca model="poly3" focal="18" vr="1.0004" vb="0.9996" cr="0" cb="0" br="0" bb="0"/>
            <tca model="poly3" focal="55" vr="1.0002" vb="0.9998"/>
            <vignetting model="pa" focal="18" aperture="4" distance="10" k1="-0.9" k2="0.3" k3="0"/>
            <vignetting model="pa" focal="18" aperture="4" distance="1000" k1="-0.8" k2="0.2" k3="0"/>
            <vignetting model="pa" focal="18" aperture="8" distance="1000" k1="-0.2" k2="0.0" k3="0"/>
            <vignetting model="pa" focal="55" aperture="8" distance="1000" k1="-0.1" k2="0.0" k3="0"/>
        </calibration>
    </lens>
    <lens>
        <maker>Othermaker</maker>
        <model>Othermaker 50mm f/1.8</model>
        <mount>Test</mount>
        <calibration>
            <distortion model="poly3" focal="50" k1="-0.01"/>
            <tca model="linear" focal="50" kr="1.0001" kb="0.9999"/>
        </calibration>
    </lens>
</lensdatabase>
)";

void testDatabase() {
    LensDatabase db;
    db.addXml(kDatabase);
    CHECK(db.cameras().size() == 2);
    CHECK(db.lenses().size() == 2);
    const LensProfile& zoom = db.lenses()[0];
    CHECK(zoom.name() == "TX 18-55mm f/3.5-5.6 & Macro");  // entity decoded, untranslated name first
    CHECK(zoom.models.size() == 2);
    CHECK(zoom.distortion.size() == 2 && zoom.tca.size() == 2 && zoom.vignetting.size() == 4);
    CHECK(near(zoom.cropFactor, 1.5f, 1e-6f));
    CHECK(db.cameras()[0].maker == "Testcam");

    // Matching: the same name written differently, the maker left out, or a
    // translated name; unrelated or too-short names do not match.
    CHECK(db.findLens("TX 18-55mm F3.5-5.6 & Macro") == &zoom);
    CHECK(db.findLens("tx18-55mmf/3.5-5.6&macro", "Testcam") == &zoom);
    CHECK(db.findLens("TX 18-55 mm Zoom") == &zoom);
    CHECK(db.findLens("50mm f/1.8", "Othermaker") == &db.lenses()[1]);
    CHECK(db.findLens("Macro f/3.5-5.6 TX 18-55mm") == &zoom);  // same words, another order
    CHECK(db.findLens("TX Macro") == nullptr);                  // no number: too generic
    CHECK(db.findLens("Unknown 35mm f/2") == nullptr);
    CHECK(db.findLens("TX") == nullptr);
    CHECK(db.findLens("") == nullptr);
    CHECK(db.findCamera("TESTCAM", "tc-1") == &db.cameras()[0]);
    CHECK(db.findCamera("Testcam", "TC-9") == nullptr);

    // Malformed documents are rejected.
    for (const char* bad : {"<lensdatabase><lens></lensdatabase>", "<lensdatabase><lens a=1/></lensdatabase>",
                            "<notlensfun/>", "<lensdatabase><!-- open"}) {
        bool threw = false;
        try {
            LensDatabase other;
            other.addXml(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
}

void testResolve() {
    LensDatabase db;
    db.addXml(kDatabase);
    const LensProfile& zoom = db.lenses()[0];

    // Exact focal length, then halfway between two calibrations.
    const ResolvedLensProfile wide = resolveLensProfile(zoom, 18.0f, 4.0f);
    CHECK(wide.name == "Testcam TX 18-55mm f/3.5-5.6 & Macro");
    CHECK(wide.distortionModel == DistortionModel::PTLENS);
    CHECK(near(wide.distortion[0], 0.01f, 1e-6f) && near(wide.distortion[1], -0.04f, 1e-6f));
    CHECK(wide.tcaModel == TcaModel::POLY3 && near(wide.tcaRed[0], 1.0004f, 1e-6f));
    CHECK(wide.hasVignetting && near(wide.vignetting[0], -0.8f, 1e-6f));  // the farthest focus distance
    const ResolvedLensProfile middle = resolveLensProfile(zoom, 36.5f, 8.0f);
    CHECK(near(middle.distortion[1], -0.01f, 1e-5f));
    CHECK(near(middle.vignetting[0], -0.15f, 1e-5f));
    // Apertures between calibrations interpolate in stops; beyond them, clamp.
    CHECK(near(resolveLensProfile(zoom, 18.0f, 5.6568542f).vignetting[0], -0.5f, 1e-4f));
    CHECK(near(resolveLensProfile(zoom, 18.0f, 22.0f).vignetting[0], -0.2f, 1e-6f));
    // Outside the calibrated focal range: the nearest calibration.
    CHECK(near(resolveLensProfile(zoom, 200.0f, 8.0f).distortion[1], 0.02f, 1e-6f));
    // A camera with a smaller sensor sees the middle of the calibration frame.
    CHECK(near(resolveLensProfile(zoom, 18.0f, 4.0f, 2.0f).radiusScale, 0.75f, 1e-6f));
    CHECK(near(resolveLensProfile(zoom, 18.0f, 4.0f, 0.0f).radiusScale, 1.0f, 1e-6f));
    const ResolvedLensProfile prime = resolveLensProfile(db.lenses()[1], 0.0f, 0.0f);
    CHECK(prime.distortionModel == DistortionModel::POLY3 && prime.tcaModel == TcaModel::LINEAR && !prime.hasVignetting);
}

void testGeometry() {
    constexpr std::uint32_t W = 60, H = 40;
    std::vector<float> image(W * H * 4);
    for (std::uint32_t y = 0; y < H; ++y) {
        for (std::uint32_t x = 0; x < W; ++x) {
            float* p = &image[(y * W + x) * 4];
            p[0] = 0.1f + 0.01f * static_cast<float>(x);
            p[1] = 0.2f + 0.005f * static_cast<float>(x + y);
            p[2] = 0.05f + 0.012f * static_cast<float>(y);
            p[3] = 1.0f;
        }
    }

    // Identity: every sample at its own pixel, the image unchanged.
    const LensCorrectionParams none;
    CHECK(isIdentity(none));
    const LensCorrectionPush identity = lensCorrectionPush(none, W, H);
    const LensSample same = lensSourcePosition(identity, 12.5f, 7.5f);
    CHECK(near(same.green[0], 12.5f, 1e-4f) && near(same.green[1], 7.5f, 1e-4f) && near(same.red[0], 12.5f, 1e-4f));
    CHECK(same.gain == 1.0f);
    const std::vector<float> unchanged = applyLensCorrection(identity, W, H, image);
    float worst = 0.0f;
    for (std::size_t i = 0; i < image.size(); ++i) worst = std::max(worst, std::fabs(unchanged[i] - image[i]));
    CHECK(worst < 1e-5f);

    // Barrel distortion (ptlens b < 0 pulls the source edges in): without
    // constrain crop the corners sample from inside the frame, towards the
    // centre; with it, the view zooms so the edges still land in the photo.
    LensCorrectionParams barrel;
    ResolvedLensProfile profile;
    profile.name = "Test barrel";
    profile.distortionModel = DistortionModel::PTLENS;
    profile.distortion = {0.0f, 0.05f, 0.0f};  // source radius grows faster than the corrected one
    applyProfile(barrel, profile);
    CHECK(std::string(barrel.profileName) == "Test barrel");
    CHECK(!isIdentity(barrel));
    barrel.constrainCrop = 0;
    const LensCorrectionPush loose = lensCorrectionPush(barrel, W, H);
    const LensSample corner = lensSourcePosition(loose, 0.5f, 0.5f);
    CHECK(corner.green[0] < 0.5f && corner.green[1] < 0.5f);  // outside: would need clamped edges
    barrel.constrainCrop = 1;
    const LensCorrectionPush constrained = lensCorrectionPush(barrel, W, H);
    CHECK(constrained.manual[3] < 1.0f);
    const LensSample inside = lensSourcePosition(constrained, 0.5f, 0.5f);
    CHECK(inside.green[0] >= 0.5f - 1e-3f && inside.green[1] >= 0.5f - 1e-3f);
    const LensSample centre = lensSourcePosition(constrained, W / 2.0f, H / 2.0f);
    CHECK(near(centre.green[0], W / 2.0f, 1e-4f) && near(centre.green[1], H / 2.0f, 1e-4f));
    // Amount 0 turns the profile distortion off; 200 doubles it.
    barrel.constrainCrop = 0;
    barrel.distortionAmount = 0.0f;
    CHECK(near(lensSourcePosition(lensCorrectionPush(barrel, W, H), 0.5f, 0.5f).green[0], 0.5f, 1e-4f));
    barrel.distortionAmount = 200.0f;
    CHECK(lensSourcePosition(lensCorrectionPush(barrel, W, H), 0.5f, 0.5f).green[0] < corner.green[0]);

    // Manual distortion: positive pulls the edges in (removes barrel).
    LensCorrectionParams manual;
    manual.constrainCrop = 0;
    manual.manualDistortion = 60.0f;
    const LensSample pulled = lensSourcePosition(lensCorrectionPush(manual, W, H), 0.5f, 20.0f);
    CHECK(pulled.green[0] > 0.5f);

    // TCA: red sampled farther out, blue farther in (vr > 1 > vb).
    LensCorrectionParams tca;
    profile = {};
    profile.tcaModel = TcaModel::POLY3;
    profile.tcaRed = {1.01f, 0.0f, 0.0f};
    profile.tcaBlue = {0.99f, 0.0f, 0.0f};
    applyProfile(tca, profile);
    const LensSample fringe = lensSourcePosition(lensCorrectionPush(tca, W, H), 50.5f, 20.0f);
    CHECK(fringe.red[0] > fringe.green[0] && fringe.blue[0] < fringe.green[0]);
    tca.removeChromaticAberration = 0;
    const LensSample kept = lensSourcePosition(lensCorrectionPush(tca, W, H), 50.5f, 20.0f);
    CHECK(near(kept.red[0], kept.green[0], 1e-4f));

    // Vignetting: the profile's falloff is divided out, full at amount 100.
    LensCorrectionParams vignette;
    profile = {};
    profile.hasVignetting = true;
    profile.vignetting = {-0.5f, 0.0f, 0.0f};
    applyProfile(vignette, profile);
    const LensCorrectionPush vp = lensCorrectionPush(vignette, W, H);
    const LensSample cornerGain = lensSourcePosition(vp, 0.0f, 0.0f);  // r = 1 at the corner
    CHECK(near(cornerGain.gain, 2.0f, 1e-3f));
    CHECK(near(lensSourcePosition(vp, W / 2.0f, H / 2.0f).gain, 1.0f, 1e-6f));
    vignette.vignettingAmount = 50.0f;
    CHECK(near(lensSourcePosition(lensCorrectionPush(vignette, W, H), 0.0f, 0.0f).gain, std::sqrt(2.0f), 1e-3f));

    // Manual vignetting: +100 is one stop brighter at the very corner.
    LensCorrectionParams manualVignette;
    manualVignette.manualVignetting = 100.0f;
    const LensCorrectionPush mv = lensCorrectionPush(manualVignette, W, H);
    CHECK(near(lensSourcePosition(mv, 0.0f, 0.0f).gain, 2.0f, 1e-3f));
    CHECK(near(lensSourcePosition(mv, W / 2.0f, H / 2.0f).gain, 1.0f, 1e-6f));

    // Scale 150 % zooms in on top of the constraint.
    LensCorrectionParams zoomed;
    zoomed.scale = 150.0f;
    CHECK(near(lensCorrectionPush(zoomed, W, H).manual[3], 1.0f / 1.5f, 1e-5f));

    // Sanitizing, serialization and the develop order.
    LensCorrectionParams wild;
    wild.distortionAmount = 900.0f;
    wild.manualCaRed = std::nanf("");
    wild.distortion = {std::nanf(""), 0.0f, 0.0f};
    const LensCorrectionParams clean = sanitize(wild);
    CHECK(clean.distortionAmount == 200.0f && clean.manualCaRed == 0.0f && clean.distortion[0] == 0.0f);
    const std::vector<std::byte> bytes = packParams(barrel);
    CHECK(bytes.size() == sizeof(LensCorrectionParams));
    CHECK(std::string(unpackParams<LensCorrectionParams>(bytes, "test").profileName) == "Test barrel");
    CHECK(developStage("lens_correction") > developStage("denoise"));
    CHECK(developStage("lens_correction") < developStage("white_balance"));
}

}  // namespace

int main() {
    testDatabase();
    testResolve();
    testGeometry();
    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "lens corrections: all checks passed\n";
    return 0;
}
