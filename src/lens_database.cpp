#include "lens_database.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace darkhouse {
namespace {

// --- A small XML reader: elements, attributes, text, comments, entities. ---------------
// Enough for the lensfun files; not a general-purpose parser (no namespaces,
// no DTD processing).

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;
    std::vector<XmlElement> children;

    [[nodiscard]] const std::string* attribute(std::string_view key) const {
        for (const auto& [k, v] : attributes) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

class XmlReader {
public:
    explicit XmlReader(std::string_view xml) : xml_(xml) {}

    XmlElement document() {
        XmlElement root;
        root.name = "#document";
        readContent(root, 0);
        if (pos_ < xml_.size()) fail("unexpected closing tag");
        return root;
    }

private:
    static constexpr int kMaxDepth = 64;

    [[noreturn]] void fail(const std::string& what) const {
        throw std::invalid_argument("XML: " + what + " at offset " + std::to_string(pos_));
    }
    bool startsWith(std::string_view prefix) const { return xml_.substr(pos_, prefix.size()) == prefix; }
    void skipPast(std::string_view terminator) {
        const std::size_t end = xml_.find(terminator, pos_);
        if (end == std::string_view::npos) fail("unterminated construct");
        pos_ = end + terminator.size();
    }
    void skipSpace() {
        while (pos_ < xml_.size() && std::isspace(static_cast<unsigned char>(xml_[pos_]))) ++pos_;
    }
    std::string readName() {
        const std::size_t start = pos_;
        while (pos_ < xml_.size()) {
            const char c = xml_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) fail("expected a name");
        return std::string(xml_.substr(start, pos_ - start));
    }

    static std::string decode(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            const std::size_t end = raw.find(';', i);
            if (end == std::string_view::npos) {
                out += raw[i];
                continue;
            }
            const std::string_view entity = raw.substr(i + 1, end - i - 1);
            if (entity == "amp") out += '&';
            else if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (!entity.empty() && entity[0] == '#') {
                const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                const long code = std::strtol(std::string(entity.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10);
                out += code > 0 && code < 128 ? static_cast<char>(code) : '?';
            } else {
                out += '?';
            }
            i = end;
        }
        return out;
    }

    // Children and text of `parent` up to its closing tag (or the end).
    void readContent(XmlElement& parent, int depth) {
        if (depth > kMaxDepth) fail("elements nested too deeply");
        while (pos_ < xml_.size()) {
            if (startsWith("<!--")) {
                skipPast("-->");
            } else if (startsWith("<![CDATA[")) {
                pos_ += 9;
                const std::size_t end = xml_.find("]]>", pos_);
                if (end == std::string_view::npos) fail("unterminated CDATA");
                parent.text += xml_.substr(pos_, end - pos_);
                pos_ = end + 3;
            } else if (startsWith("<?") || startsWith("<!")) {
                skipPast(">");
            } else if (startsWith("</")) {
                return;  // the caller checks the name
            } else if (xml_[pos_] == '<') {
                parent.children.push_back(readElement(depth + 1));
            } else {
                const std::size_t end = xml_.find('<', pos_);
                const std::size_t stop = end == std::string_view::npos ? xml_.size() : end;
                parent.text += decode(xml_.substr(pos_, stop - pos_));
                pos_ = stop;
            }
        }
    }

    XmlElement readElement(int depth) {
        XmlElement element;
        ++pos_;  // '<'
        element.name = readName();
        for (;;) {
            skipSpace();
            if (pos_ >= xml_.size()) fail("unterminated tag <" + element.name + ">");
            if (startsWith("/>")) {
                pos_ += 2;
                return element;
            }
            if (xml_[pos_] == '>') {
                ++pos_;
                break;
            }
            std::string key = readName();
            skipSpace();
            if (pos_ >= xml_.size() || xml_[pos_] != '=') fail("expected '=' after attribute " + key);
            ++pos_;
            skipSpace();
            if (pos_ >= xml_.size() || (xml_[pos_] != '"' && xml_[pos_] != '\'')) fail("expected a quoted value");
            const char quote = xml_[pos_++];
            const std::size_t end = xml_.find(quote, pos_);
            if (end == std::string_view::npos) fail("unterminated attribute value");
            element.attributes.emplace_back(std::move(key), decode(xml_.substr(pos_, end - pos_)));
            pos_ = end + 1;
        }
        readContent(element, depth);
        if (!startsWith("</")) fail("missing </" + element.name + ">");
        pos_ += 2;
        const std::string closing = readName();
        if (closing != element.name) fail("</" + closing + "> closes <" + element.name + ">");
        skipSpace();
        if (pos_ >= xml_.size() || xml_[pos_] != '>') fail("malformed closing tag");
        ++pos_;
        return element;
    }

    std::string_view xml_;
    std::size_t pos_ = 0;
};

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

float number(const XmlElement& element, std::string_view key, float fallback) {
    const std::string* value = element.attribute(key);
    if (!value) return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value->c_str(), &end);
    return end != value->c_str() && std::isfinite(parsed) ? parsed : fallback;
}

// The first child text of `name`, preferring the untranslated one.
std::string childText(const XmlElement& element, std::string_view name) {
    std::string translated;
    for (const XmlElement& child : element.children) {
        if (child.name != name) continue;
        if (!child.attribute("lang")) return trim(child.text);
        if (translated.empty()) translated = trim(child.text);
    }
    return translated;
}

// Lower-case letters and digits only: "XF 16mm f/1.4 R WR" -> "xf16mmf14rwr".
std::string compact(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string withoutPrefix(const std::string& s, const std::string& prefix) {
    return !prefix.empty() && s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0 ? s.substr(prefix.size()) : s;
}

// Words, each compacted: "AF-S NIKKOR 24-70mm f/2.8G" -> {afs, nikkor, 2470mm, f28g}.
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        std::string word = compact(s.substr(start, i - start));
        if (!word.empty()) out.push_back(std::move(word));
    }
    return out;
}

// Every word of the shorter name appears in the longer one, in any order
// ("AF-S NIKKOR 24-70mm f/2.8G ED" in "Nikon AF-S Zoom-Nikkor 24-70mm
// f/2.8G ED"). Needs two words and a number, so generic words never match.
float wordScore(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const auto& shorter = a.size() < b.size() ? a : b;
    const auto& longer = a.size() < b.size() ? b : a;
    if (shorter.size() < 2) return 0.0f;
    bool number = false;
    for (const std::string& word : shorter) {
        if (std::find(longer.begin(), longer.end(), word) == longer.end()) return 0.0f;
        number = number || std::any_of(word.begin(), word.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    }
    return number ? 0.7f + 0.2f * static_cast<float>(shorter.size()) / static_cast<float>(longer.size()) : 0.0f;
}

// 1 for the same name, less for one containing the other, 0 for unrelated names.
float matchScore(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0f;
    if (a == b) return 1.0f;
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    if (shorter.size() < 6 || longer.find(shorter) == std::string::npos) return 0.0f;
    return 0.9f * static_cast<float>(shorter.size()) / static_cast<float>(longer.size());
}

std::array<float, 3> lerp3(const std::array<float, 3>& a, const std::array<float, 3>& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// Calibrations sorted by focal length: the two around `focal` and the blend.
template <class Calibration>
std::pair<const Calibration*, std::pair<const Calibration*, float>> bracket(const std::vector<Calibration>& list, float focal) {
    const Calibration* below = nullptr;
    const Calibration* above = nullptr;
    for (const Calibration& c : list) {
        if (c.focal <= focal && (!below || c.focal > below->focal)) below = &c;
        if (c.focal >= focal && (!above || c.focal < above->focal)) above = &c;
    }
    if (!below) return {above, {above, 0.0f}};
    if (!above || above->focal == below->focal) return {below, {below, 0.0f}};
    return {below, {above, (focal - below->focal) / (above->focal - below->focal)}};
}

// pa coefficients at one focal length, interpolated in log2(f-number).
std::array<float, 3> vignettingAt(const std::vector<VignettingCalibration>& list, float focal, float aperture) {
    // Farthest focus distance per aperture.
    std::map<float, const VignettingCalibration*> byAperture;
    for (const VignettingCalibration& v : list) {
        if (v.focal != focal) continue;
        auto [it, inserted] = byAperture.emplace(v.aperture, &v);
        if (!inserted && v.distance > it->second->distance) it->second = &v;
    }
    if (byAperture.empty()) return {};
    if (aperture <= byAperture.begin()->first) return byAperture.begin()->second->k;
    if (aperture >= byAperture.rbegin()->first) return byAperture.rbegin()->second->k;
    const auto upper = byAperture.lower_bound(aperture);
    const auto lower = std::prev(upper);
    const float t = (std::log2(aperture) - std::log2(lower->first)) / (std::log2(upper->first) - std::log2(lower->first));
    return lerp3(lower->second->k, upper->second->k, t);
}

}  // namespace

// -----------------------------------------------------------------------------
// LensDatabase
// -----------------------------------------------------------------------------

std::optional<std::filesystem::path> LensDatabase::findDirectory(const std::filesystem::path& preferred) {
    std::vector<std::filesystem::path> candidates;
    if (!preferred.empty()) candidates.push_back(preferred);
    if (const char* env = std::getenv("DARKHOUSE_LENSFUN_DIR"); env && *env) candidates.emplace_back(env);
    candidates.emplace_back("/usr/share/lensfun/version_1");
    candidates.emplace_back("/usr/local/share/lensfun/version_1");
    for (const std::filesystem::path& dir : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".xml") return dir;
        }
    }
    return std::nullopt;
}

LensDatabase LensDatabase::loadDirectory(const std::filesystem::path& directory) {
    LensDatabase database;
    database.directory_ = directory;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.path().extension() == ".xml") files.push_back(entry.path());
    }
    if (ec) database.errors_.push_back(directory.string() + ": " + ec.message());
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        if (!in && !in.eof()) {
            database.errors_.push_back(file.string() + ": unreadable");
            continue;
        }
        try {
            database.addXml(text.str());
        } catch (const std::exception& e) {
            database.errors_.push_back(file.filename().string() + ": " + e.what());
        }
    }
    return database;
}

void LensDatabase::addXml(std::string_view xml) {
    const XmlElement document = XmlReader(xml).document();
    const XmlElement* root = nullptr;
    for (const XmlElement& child : document.children) {
        if (child.name == "lensdatabase") root = &child;
    }
    if (!root) throw std::invalid_argument("not a lensfun database (no <lensdatabase>)");

    for (const XmlElement& element : root->children) {
        if (element.name == "camera") {
            CameraProfile camera;
            camera.maker = childText(element, "maker");
            camera.model = childText(element, "model");
            const std::string crop = childText(element, "cropfactor");
            camera.cropFactor = crop.empty() ? 1.0f : std::strtof(crop.c_str(), nullptr);
            if (!camera.model.empty() && camera.cropFactor > 0.0f) cameras_.push_back(std::move(camera));
        } else if (element.name == "lens") {
            LensProfile lens;
            lens.maker = childText(element, "maker");
            const std::string primary = childText(element, "model");
            if (primary.empty()) continue;
            lens.models.push_back(primary);
            for (const XmlElement& child : element.children) {
                if (child.name == "model" && trim(child.text) != primary) lens.models.push_back(trim(child.text));
            }
            const std::string crop = childText(element, "cropfactor");
            lens.cropFactor = crop.empty() ? 1.0f : std::strtof(crop.c_str(), nullptr);
            if (!(lens.cropFactor > 0.0f)) lens.cropFactor = 1.0f;
            for (const XmlElement& calibration : element.children) {
                if (calibration.name != "calibration") continue;
                for (const XmlElement& c : calibration.children) {
                    const std::string* model = c.attribute("model");
                    const float focal = number(c, "focal", 0.0f);
                    if (!model || !(focal > 0.0f)) continue;
                    if (c.name == "distortion") {
                        DistortionCalibration d;
                        d.focal = focal;
                        if (*model == "ptlens") {
                            d.model = DistortionModel::PTLENS;
                            d.k = {number(c, "a", 0.0f), number(c, "b", 0.0f), number(c, "c", 0.0f)};
                        } else if (*model == "poly3") {
                            d.model = DistortionModel::POLY3;
                            d.k = {number(c, "k1", 0.0f), 0.0f, 0.0f};
                        } else if (*model == "poly5") {
                            d.model = DistortionModel::POLY5;
                            d.k = {number(c, "k1", 0.0f), number(c, "k2", 0.0f), 0.0f};
                        } else {
                            continue;
                        }
                        lens.distortion.push_back(d);
                    } else if (c.name == "tca") {
                        TcaCalibration t;
                        t.focal = focal;
                        if (*model == "linear") {
                            t.model = TcaModel::LINEAR;
                            t.red = {number(c, "kr", 1.0f), 0.0f, 0.0f};
                            t.blue = {number(c, "kb", 1.0f), 0.0f, 0.0f};
                        } else if (*model == "poly3") {
                            t.model = TcaModel::POLY3;
                            t.red = {number(c, "vr", 1.0f), number(c, "cr", 0.0f), number(c, "br", 0.0f)};
                            t.blue = {number(c, "vb", 1.0f), number(c, "cb", 0.0f), number(c, "bb", 0.0f)};
                        } else {
                            continue;
                        }
                        lens.tca.push_back(t);
                    } else if (c.name == "vignetting" && *model == "pa") {
                        VignettingCalibration v;
                        v.focal = focal;
                        v.aperture = number(c, "aperture", 0.0f);
                        v.distance = number(c, "distance", 1000.0f);
                        v.k = {number(c, "k1", 0.0f), number(c, "k2", 0.0f), number(c, "k3", 0.0f)};
                        if (v.aperture > 0.0f) lens.vignetting.push_back(v);
                    }
                }
            }
            lenses_.push_back(std::move(lens));
        }
    }
}

const LensProfile* LensDatabase::findLens(std::string_view lensModel, std::string_view maker) const {
    const std::string wanted = compact(lensModel);
    const std::string wantedMaker = compact(maker);
    const std::vector<std::string> wantedWords = words(lensModel);
    if (wanted.empty()) return nullptr;
    const LensProfile* best = nullptr;
    float bestScore = 0.0f;
    for (const LensProfile& lens : lenses_) {
        const std::string lensMaker = compact(lens.maker);
        for (const std::string& model : lens.models) {
            const std::string name = compact(model);
            // Database names often carry the maker ("Canon EF 24-105mm..."),
            // EXIF lens strings often do not ("EF24-105mm...").
            float score = std::max({matchScore(wanted, name), matchScore(wanted, withoutPrefix(name, lensMaker)),
                                    matchScore(withoutPrefix(wanted, lensMaker), withoutPrefix(name, lensMaker)),
                                    wordScore(wantedWords, words(model))});
            if (score > 0.0f && !wantedMaker.empty() &&
                (lensMaker == wantedMaker || wantedMaker.find(lensMaker) == 0 || lensMaker.find(wantedMaker) == 0)) {
                score += 0.05f;  // same maker wins a tie
            }
            if (score > bestScore) {
                bestScore = score;
                best = &lens;
            }
        }
    }
    return bestScore >= 0.6f ? best : nullptr;
}

const CameraProfile* LensDatabase::findCamera(std::string_view maker, std::string_view model) const {
    const std::string wantedModel = compact(model);
    const std::string wantedMaker = compact(maker);
    const CameraProfile* fallback = nullptr;
    for (const CameraProfile& camera : cameras_) {
        if (compact(camera.model) != wantedModel) continue;
        const std::string cameraMaker = compact(camera.maker);
        if (wantedMaker.empty() || cameraMaker.find(wantedMaker) == 0 || wantedMaker.find(cameraMaker) == 0) return &camera;
        if (!fallback) fallback = &camera;
    }
    return fallback;
}

// -----------------------------------------------------------------------------
// Resolving a profile
// -----------------------------------------------------------------------------

ResolvedLensProfile resolveLensProfile(const LensProfile& lens, float focal, float aperture, float cameraCropFactor) {
    ResolvedLensProfile out;
    const std::string& model = lens.name();
    out.name = compact(model).rfind(compact(lens.maker), 0) == 0 || lens.maker.empty() ? model : lens.maker + " " + model;
    out.radiusScale = cameraCropFactor > 0.0f ? lens.cropFactor / cameraCropFactor : 1.0f;
    if (!(focal > 0.0f)) {
        // Unknown focal length: the first calibration's.
        focal = !lens.distortion.empty() ? lens.distortion.front().focal
              : !lens.vignetting.empty() ? lens.vignetting.front().focal
              : !lens.tca.empty()        ? lens.tca.front().focal
                                         : 0.0f;
    }

    if (!lens.distortion.empty()) {
        const auto [below, next] = bracket(lens.distortion, focal);
        const auto [above, t] = next;
        out.distortionModel = below->model;
        out.distortion = below->model == above->model ? lerp3(below->k, above->k, t) : (t < 0.5f ? below->k : above->k);
        if (below->model != above->model && t >= 0.5f) out.distortionModel = above->model;
    }
    if (!lens.tca.empty()) {
        const auto [below, next] = bracket(lens.tca, focal);
        const auto [above, t] = next;
        const TcaCalibration& pick = t < 0.5f ? *below : *above;
        out.tcaModel = pick.model;
        if (below->model == above->model) {
            out.tcaRed = lerp3(below->red, above->red, t);
            out.tcaBlue = lerp3(below->blue, above->blue, t);
        } else {
            out.tcaRed = pick.red;
            out.tcaBlue = pick.blue;
        }
    }
    if (!lens.vignetting.empty()) {
        // Unknown aperture: f/5.6, a middle setting.
        const float f = aperture > 0.0f ? aperture : 5.6f;
        const auto [below, next] = bracket(lens.vignetting, focal);
        const auto [above, t] = next;
        const std::array<float, 3> low = vignettingAt(lens.vignetting, below->focal, f);
        const std::array<float, 3> high = vignettingAt(lens.vignetting, above->focal, f);
        out.vignetting = lerp3(low, high, t);
        out.hasVignetting = true;
    }
    return out;
}

}  // namespace darkhouse
