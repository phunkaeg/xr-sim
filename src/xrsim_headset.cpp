// xr-sim: boot-time headset identity, recommended view sizes, and optics.
//
// Clients commonly enumerate view configuration once and immediately size
// their swapchains. The profile therefore loads before the first instance and
// is immutable for the lifetime of the process. Runtime `fov` commands remain
// available for fault injection after that bootstrap decision.

#include "xrsim_internal.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace xrsim {
namespace {

constexpr uint32_t kDimensionLimit = 16384;
constexpr size_t kConfigSizeLimit = 1024 * 1024;

struct ParsedView {
    uint32_t recommendedWidth = 0;
    uint32_t recommendedHeight = 0;
    uint32_t maxWidth = kDimensionLimit;
    uint32_t maxHeight = kDimensionLimit;
    Fov fov{};
    bool sawRecommendedWidth = false;
    bool sawRecommendedHeight = false;
    bool sawFov = false;
};

struct ParsedProfile {
    uint32_t schemaVersion = 0;
    std::string systemName;
    ParsedView views[2];
    bool sawSchemaVersion = false;
    bool sawSystemName = false;
    bool sawViews = false;
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    bool expect(char c, const char* what) {
        skip_ws();
        if (pos_ >= text_.size() || text_[pos_] != c)
            return fail(std::string("expected ") + what);
        ++pos_;
        return true;
    }

    bool member(bool& first, std::string& key, bool& done) {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            done = true;
            return true;
        }
        if (!first && !expect(',', "',' between object members")) return false;
        first = false;
        if (!string(key)) return false;
        if (!expect(':', "':' after object key")) return false;
        done = false;
        return true;
    }

    bool element(bool& first, bool& done) {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            done = true;
            return true;
        }
        if (!first && !expect(',', "',' between array elements")) return false;
        first = false;
        done = false;
        return true;
    }

    bool string(std::string& out) {
        skip_ws();
        if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected string");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= text_.size()) return fail("unfinished string escape");
            const char escaped = text_[pos_++];
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return fail("unsupported string escape (use UTF-8 text directly)");
            }
        }
        return fail("unterminated string");
    }

    bool number(double& out) {
        skip_ws();
        if (pos_ >= text_.size()) return fail("expected number");
        const size_t beginPos = pos_;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) return fail("unfinished number");
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9')
                return fail("leading zero in number");
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            return fail("expected number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            const size_t fractionStart = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == fractionStart) return fail("fraction requires a digit");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            const size_t exponentStart = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == exponentStart) return fail("exponent requires a digit");
        }
        if (pos_ < text_.size()) {
            const char after = text_[pos_];
            if (after != ' ' && after != '\t' && after != '\r' && after != '\n' &&
                after != ',' && after != ']' && after != '}')
                return fail("invalid character after number");
        }
        const std::string token = text_.substr(beginPos, pos_ - beginPos);
        char* end = nullptr;
        errno = 0;
        out = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0') return fail("expected number");
        return true;
    }

    bool uint32(uint32_t& out) {
        double value = 0.0;
        if (!number(value)) return false;
        if (!std::isfinite(value) || value < 0.0 || value > 4294967295.0 ||
            std::floor(value) != value)
            return fail("expected finite unsigned integer");
        out = static_cast<uint32_t>(value);
        return true;
    }

    bool skip_value(unsigned depth = 0) {
        if (depth > 32) return fail("JSON nesting is too deep");
        skip_ws();
        if (pos_ >= text_.size()) return fail("expected value");
        if (text_[pos_] == '"') {
            std::string ignored;
            return string(ignored);
        }
        if (text_[pos_] == '{') {
            ++pos_;
            bool first = true, done = false;
            std::string key;
            while (!done) {
                if (!member(first, key, done)) return false;
                if (!done && !skip_value(depth + 1)) return false;
            }
            return true;
        }
        if (text_[pos_] == '[') {
            ++pos_;
            bool first = true, done = false;
            while (!done) {
                if (!element(first, done)) return false;
                if (!done && !skip_value(depth + 1)) return false;
            }
            return true;
        }
        if (text_.compare(pos_, 4, "true") == 0 || text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return true;
        }
        double ignored = 0.0;
        return number(ignored);
    }

    bool finish() {
        skip_ws();
        return pos_ == text_.size() || fail("trailing data after root object");
    }

    const std::string& error() const { return error_; }

    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message + " at byte " + std::to_string(pos_);
        return false;
    }

private:
    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;
};

bool parse_fov(JsonReader& json, Fov& fov) {
    if (!json.expect('{', "FOV object")) return false;
    bool first = true, done = false;
    bool sawLeft = false, sawRight = false, sawUp = false, sawDown = false;
    std::string key;
    while (!done) {
        if (!json.member(first, key, done)) return false;
        if (done) break;
        double value = 0.0;
        if (key == "angleLeft") {
            if (!json.number(value)) return false;
            fov.angleLeft = static_cast<float>(value);
            sawLeft = true;
        } else if (key == "angleRight") {
            if (!json.number(value)) return false;
            fov.angleRight = static_cast<float>(value);
            sawRight = true;
        } else if (key == "angleUp") {
            if (!json.number(value)) return false;
            fov.angleUp = static_cast<float>(value);
            sawUp = true;
        } else if (key == "angleDown") {
            if (!json.number(value)) return false;
            fov.angleDown = static_cast<float>(value);
            sawDown = true;
        } else if (!json.skip_value()) {
            return false;
        }
    }
    if (!sawLeft || !sawRight || !sawUp || !sawDown)
        return json.fail("FOV requires angleLeft, angleRight, angleUp, and angleDown");
    return true;
}

bool parse_view(JsonReader& json, ParsedView& view) {
    if (!json.expect('{', "view object")) return false;
    bool first = true, done = false;
    std::string key;
    while (!done) {
        if (!json.member(first, key, done)) return false;
        if (done) break;
        if (key == "recommendedImageRectWidth") {
            if (!json.uint32(view.recommendedWidth)) return false;
            view.sawRecommendedWidth = true;
        } else if (key == "recommendedImageRectHeight") {
            if (!json.uint32(view.recommendedHeight)) return false;
            view.sawRecommendedHeight = true;
        } else if (key == "maxImageRectWidth") {
            if (!json.uint32(view.maxWidth)) return false;
        } else if (key == "maxImageRectHeight") {
            if (!json.uint32(view.maxHeight)) return false;
        } else if (key == "fov") {
            if (!parse_fov(json, view.fov)) return false;
            view.sawFov = true;
        } else if (!json.skip_value()) {
            return false;
        }
    }
    if (!view.sawRecommendedWidth || !view.sawRecommendedHeight || !view.sawFov)
        return json.fail("each view requires recommended width, height, and fov");
    return true;
}

bool parse_profile(const std::string& text, ParsedProfile& profile, std::string& error) {
    JsonReader json(text);
    if (!json.expect('{', "root object")) {
        error = json.error();
        return false;
    }
    bool first = true, done = false;
    std::string key;
    while (!done) {
        if (!json.member(first, key, done)) break;
        if (done) break;
        if (key == "schemaVersion") {
            if (!json.uint32(profile.schemaVersion)) break;
            profile.sawSchemaVersion = true;
        } else if (key == "systemName") {
            if (!json.string(profile.systemName)) break;
            profile.sawSystemName = true;
        } else if (key == "views") {
            if (!json.expect('[', "views array")) break;
            bool firstView = true, viewsDone = false;
            uint32_t viewCount = 0;
            while (!viewsDone) {
                if (!json.element(firstView, viewsDone)) break;
                if (viewsDone) break;
                if (viewCount >= 2) {
                    json.fail("views must contain exactly two entries");
                    break;
                }
                if (!parse_view(json, profile.views[viewCount])) break;
                ++viewCount;
            }
            if (!json.error().empty()) break;
            if (viewCount != 2) {
                json.fail("views must contain exactly two entries");
                break;
            }
            profile.sawViews = true;
        } else if (!json.skip_value()) {
            break;
        }
    }
    if (json.error().empty()) json.finish();
    if (!json.error().empty()) {
        error = json.error();
        return false;
    }
    if (!profile.sawSchemaVersion || !profile.sawSystemName || !profile.sawViews) {
        error = "root requires schemaVersion, systemName, and views";
        return false;
    }
    if (profile.schemaVersion != 1) {
        error = "unsupported schemaVersion " + std::to_string(profile.schemaVersion);
        return false;
    }
    if (profile.systemName.empty() || profile.systemName.size() >= XR_MAX_SYSTEM_NAME_SIZE) {
        error = "systemName must contain 1..255 UTF-8 bytes";
        return false;
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const ParsedView& view = profile.views[eye];
        if (view.recommendedWidth == 0 || view.recommendedHeight == 0 ||
            view.maxWidth == 0 || view.maxHeight == 0 ||
            view.recommendedWidth > view.maxWidth || view.recommendedHeight > view.maxHeight ||
            view.maxWidth > kDimensionLimit || view.maxHeight > kDimensionLimit) {
            error = "view " + std::to_string(eye) +
                    " dimensions must be positive, recommended <= max, and max <= 16384";
            return false;
        }
        const Fov& f = view.fov;
        if (!std::isfinite(f.angleLeft) || !std::isfinite(f.angleRight) ||
            !std::isfinite(f.angleUp) || !std::isfinite(f.angleDown)) {
            error = "view " + std::to_string(eye) + " FOV angles must be finite";
            return false;
        }
        if (f.angleLeft <= -kPi || f.angleRight >= kPi ||
            f.angleDown <= -kPi || f.angleUp >= kPi || fov_is_degenerate(f)) {
            error = "view " + std::to_string(eye) +
                    " FOV angles must be ordered, non-degenerate, and inside (-pi, pi)";
            return false;
        }
    }
    return true;
}

bool read_file(const wchar_t* path, std::string& text, std::string& error) {
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open file (Win32 " + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > kConfigSizeLimit) {
        CloseHandle(file);
        error = "file must contain 1..1048576 bytes";
        return false;
    }
    text.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != text.size()) {
        error = "failed to read complete file";
        return false;
    }
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb &&
        static_cast<unsigned char>(text[2]) == 0xbf)
        text.erase(0, 3);
    return true;
}

std::string utf8_path(const wchar_t* path) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return "<profile>";
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}

bool load_profile() {
    wchar_t path[32768] = {};
    const DWORD envLength = GetEnvironmentVariableW(L"XRSIM_HEADSET_CONFIG", nullptr, 0);
    const bool explicitPath = envLength != 0;
    if (explicitPath) {
        if (envLength > static_cast<DWORD>(std::size(path))) {
            XRSIM_LOG("xrsim: XRSIM_HEADSET_CONFIG path is too long");
            return false;
        }
        GetEnvironmentVariableW(L"XRSIM_HEADSET_CONFIG", path, static_cast<DWORD>(std::size(path)));
    } else {
        if (swprintf_s(path, L"%ls\\headset.json", log::dir()) < 0) {
            XRSIM_LOG("xrsim: state directory is too long for headset.json");
            return false;
        }
        const DWORD attributes = GetFileAttributesW(path);
        const DWORD attributeError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            (attributeError == ERROR_FILE_NOT_FOUND || attributeError == ERROR_PATH_NOT_FOUND)) {
            XRSIM_LOG("xrsim: headset profile: built-in Meta Quest 3 geometry");
            return true;
        }
    }

    std::string text, error;
    if (!read_file(path, text, error)) {
        XRSIM_LOG("xrsim: headset profile rejected (%ls): %s", path, error.c_str());
        return false;
    }
    ParsedProfile profile;
    if (!parse_profile(text, profile, error)) {
        XRSIM_LOG("xrsim: headset profile rejected (%ls): %s", path, error.c_str());
        return false;
    }

    strcpy_s(g.systemName, profile.systemName.c_str());
    for (uint32_t eye = 0; eye < 2; ++eye) {
        g.recommendedWidth[eye] = profile.views[eye].recommendedWidth;
        g.recommendedHeight[eye] = profile.views[eye].recommendedHeight;
        g.maxWidth[eye] = profile.views[eye].maxWidth;
        g.maxHeight[eye] = profile.views[eye].maxHeight;
        g.headsetFov[eye] = profile.views[eye].fov;
    }
    const std::string source = utf8_path(path);
    strncpy_s(g.headsetConfigSource, source.c_str(), _TRUNCATE);
    XRSIM_LOG("xrsim: headset profile loaded: '%s', L=%ux%u R=%ux%u (%ls)",
              g.systemName, g.recommendedWidth[0], g.recommendedHeight[0],
              g.recommendedWidth[1], g.recommendedHeight[1], path);
    return true;
}

std::once_flag g_profileOnce;
bool g_profileValid = false;

} // namespace

bool headset_config_load_once() {
    try {
        std::call_once(g_profileOnce, [] {
            try {
                g_profileValid = load_profile();
            } catch (...) {
                XRSIM_LOG("xrsim: headset profile rejected by an internal parser failure");
                g_profileValid = false;
            }
        });
    } catch (...) {
        return false;
    }
    return g_profileValid;
}

} // namespace xrsim
