// DarkHouse — lens profiles in the lensfun database format.
//
// DarkHouse reads the XML files of a lensfun database (version 1 layout, as
// shipped in the lensfun-data packages): cameras with their crop factors, and
// lenses with calibrations per focal length (distortion, transverse chromatic
// aberration) and per focal length / aperture / distance (vignetting). The
// database is searched in, first to last:
//
//   --lens-db <dir> (AppConfig::lensDatabaseDirectory)
//   $DARKHOUSE_LENSFUN_DIR
//   /usr/share/lensfun/version_1, /usr/local/share/lensfun/version_1
//
// A matched profile is resolved for the photo's focal length and aperture
// into the coefficients of the lens_correction node (lens_correction.hpp),
// so an edit keeps its correction even where no database is installed.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace darkhouse {

enum class DistortionModel : std::uint32_t {
    NONE,
    PTLENS,  // Rd = Ru (a Ru^3 + b Ru^2 + c Ru + 1 - a - b - c)
    POLY3,   // Rd = Ru (1 - k1 + k1 Ru^2)
    POLY5,   // Rd = Ru (1 + k1 Ru^2 + k2 Ru^4)
};
enum class TcaModel : std::uint32_t {
    NONE,
    LINEAR,  // Rd = Ru k                       (red and blue against green)
    POLY3,   // Rd = Ru (b Ru^2 + c Ru + v)
};

struct DistortionCalibration {
    float focal = 0.0f;
    DistortionModel model = DistortionModel::NONE;
    std::array<float, 3> k{};  // ptlens a, b, c; poly3 k1; poly5 k1, k2
};
struct TcaCalibration {
    float focal = 0.0f;
    TcaModel model = TcaModel::NONE;
    std::array<float, 3> red{1.0f, 0.0f, 0.0f};   // v, c, b (linear: v = k)
    std::array<float, 3> blue{1.0f, 0.0f, 0.0f};
};
struct VignettingCalibration {
    float focal = 0.0f;
    float aperture = 0.0f;
    float distance = 0.0f;
    std::array<float, 3> k{};  // pa: Cd = Cs (1 + k1 r^2 + k2 r^4 + k3 r^6)
};

struct CameraProfile {
    std::string maker;
    std::string model;
    float cropFactor = 1.0f;
};

struct LensProfile {
    std::string maker;
    std::vector<std::string> models;  // the model name and its translations
    float cropFactor = 1.0f;          // of the camera the lens was calibrated on
    std::vector<DistortionCalibration> distortion;
    std::vector<TcaCalibration> tca;
    std::vector<VignettingCalibration> vignetting;

    [[nodiscard]] const std::string& name() const noexcept { return models.front(); }
};

// Coefficients of one lens at one focal length and aperture.
struct ResolvedLensProfile {
    std::string name;  // "Fujifilm XF16mmF1.4 R WR"
    DistortionModel distortionModel = DistortionModel::NONE;
    std::array<float, 3> distortion{};
    TcaModel tcaModel = TcaModel::NONE;
    std::array<float, 3> tcaRed{1.0f, 0.0f, 0.0f};
    std::array<float, 3> tcaBlue{1.0f, 0.0f, 0.0f};
    bool hasVignetting = false;
    std::array<float, 3> vignetting{};
    float radiusScale = 1.0f;  // calibration crop factor / camera crop factor
};

class LensDatabase {
public:
    // Every *.xml file in `directory`. Unreadable or malformed files are
    // skipped and reported in errors().
    [[nodiscard]] static LensDatabase loadDirectory(const std::filesystem::path& directory);
    // The first directory of the search list above that holds a database;
    // nullopt when there is none.
    [[nodiscard]] static std::optional<std::filesystem::path> findDirectory(const std::filesystem::path& preferred = {});

    // Adds the cameras and lenses of one lensfun XML document. Throws
    // std::invalid_argument for malformed XML.
    void addXml(std::string_view xml);

    // The lens whose model best matches the photo's lens string (EXIF
    // LensModel); `maker` narrows ties. nullptr when nothing matches.
    [[nodiscard]] const LensProfile* findLens(std::string_view lensModel, std::string_view maker = {}) const;
    [[nodiscard]] const CameraProfile* findCamera(std::string_view maker, std::string_view model) const;

    [[nodiscard]] const std::vector<LensProfile>& lenses() const noexcept { return lenses_; }
    [[nodiscard]] const std::vector<CameraProfile>& cameras() const noexcept { return cameras_; }
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    std::vector<LensProfile> lenses_;
    std::vector<CameraProfile> cameras_;
    std::vector<std::string> errors_;
    std::filesystem::path directory_;
};

// Interpolates a lens's calibrations at `focal` (mm) and `aperture` (f-number;
// 0 = unknown, the widest calibrated aperture is used). Focal lengths outside
// the calibrated range use the nearest calibration. `cameraCropFactor` 0 =
// same as the calibration camera.
[[nodiscard]] ResolvedLensProfile resolveLensProfile(const LensProfile& lens, float focal, float aperture,
                                                     float cameraCropFactor = 0.0f);

}  // namespace darkhouse
