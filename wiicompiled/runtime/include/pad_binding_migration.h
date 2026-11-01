#pragma once

// One-time migration of [gamepad] bindings captured by Switch builds from before the pad
// shim learned to report face buttons by position (spm-nx 2599c2d1).
//
// The frames. SDL gamepad names are positional: a = south, b = east, x = west, y = north.
// The pre-fix Switch shim reported the face buttons by the letter printed on them, and on a
// Nintendo pad the letters sit one seat over from the positions of the same name (printed A
// is east, B is south, X is north, Y is west). A binding captured on a pre-fix build names
// the letter the player pressed; read positionally, as every current build reads it, that
// name is a different physical button. Converting a stored value between the two frames is
// swapping the two pairs, a<->b and x<->y, token by token; the swap is its own inverse.
//
// Only the nintendo_* values can carry the error: an Xbox pad's letters sit at the SDL
// positions of the same names, PlayStation names are shapes, and PC builds always named
// positions. So this migration rewrites nintendo_* values only, inside [gamepad] only.
//
// When it runs. The caller (virtual_remote's BuildPadLayouts, Switch builds only) runs it
// once, before the first layout is built from the file. It rewrites nothing unless the file
// looks like one a Switch build seeded: [video] still carries the Switch seed values
// (resolution_multiplier 1.5, frame_interpolation_fps 0, aspect_mode 1) and none of the
// marks a PC build leaves (window size/position keys, a graphics_api other than "auto", a
// Windows path for dvd_root). A file that fails that test, or already holds the current
// defaults, is left as it is. Either way the file is stamped (the pad_bindings_frame key
// below) so the question is asked exactly once per file; a file with no nintendo bindings
// is not stamped, so bindings a pre-fix build writes into it later can still be migrated.
//
// The known blind spot, stated plainly: the file cannot say WHEN its bindings were
// captured, only where the file began. A file seeded by a pre-fix Switch build whose
// bindings were captured after updating to a fixed build matches the Switch test and its
// (already correct) bindings would be rotated wrongly. The caller backs the file up before
// touching it and logs what it did, so that case is recoverable by hand.

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace PadBindingMigration {

// The stamp written into [gamepad]. "positional": the bindings are in the current frame
// (rotated by the migration, or already the defaults). "unknown": bindings were present
// but the file did not pass the Switch test, so nothing was rotated.
inline constexpr const char* kStampKey = "pad_bindings_frame";
inline constexpr const char* kStampPositional = "positional";
inline constexpr const char* kStampUnknown = "unknown";

// Trimmed in the same way the config reader trims (runtime_config.h's Trim); kept local so
// this header stands alone and the test harness can compile the real code.
inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

// The line up to a '#' outside quotes; mirrors runtime_config.h's RemoveComment.
inline std::string StripComment(std::string_view line) {
    bool inSingle = false;
    bool inDouble = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inDouble && ch == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (ch == '"' && !inSingle && !escaped) {
            inDouble = !inDouble;
        } else if (ch == '#' && !inSingle && !inDouble) {
            return std::string(line.substr(0, i));
        }
        escaped = false;
    }
    return std::string(line);
}

// One binding value, letter frame <-> positional frame. Tokens are comma-separated SDL
// names; a token rotates only when it is exactly one of the four face names once an axis
// direction suffix (+/-) is set aside, so axis names that merely contain a letter
// ("leftx-", "rightx+") and every other name pass through untouched. Empty tokens are
// preserved, so the lone "," that encodes "unbound" round-trips.
inline std::string RotateBindingValue(const std::string& value) {
    std::string out;
    bool first = true;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const size_t length = comma == std::string::npos ? std::string::npos : comma - start;
        std::string token = Trim(std::string_view(value).substr(start, length));
        char sign = 0;
        if (token.size() > 1 && (token.back() == '+' || token.back() == '-')) {
            sign = token.back();
            token.pop_back();
        }
        if (token == "a") {
            token = "b";
        } else if (token == "b") {
            token = "a";
        } else if (token == "x") {
            token = "y";
        } else if (token == "y") {
            token = "x";
        }
        if (sign != 0) {
            token += sign;
        }
        if (!first) {
            out += ',';
        }
        first = false;
        out += token;
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

// A Windows drive path ("D:\...", "D:/...") or UNC path ("\\..."): something the Switch
// build never writes for dvd_root and the PC launcher does.
inline bool LooksLikeWindowsPath(const std::string& value) {
    if (value.size() >= 2 && value[0] == '\\' && value[1] == '\\') {
        return true;
    }
    return value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) &&
           value[1] == ':' && (value[2] == '\\' || value[2] == '/');
}

// What the caller read out of the parsed config. switchSeed: [video] still carries all
// three values a Switch build seeds a fresh Config.toml with. pcEvidence: any mark a PC
// build leaves and a Switch build does not (window keys present, graphics_api other than
// "auto", a Windows dvd_root).
struct Facts {
    bool switchSeed = false;
    bool pcEvidence = false;
};

enum class Outcome {
    kNoBindings,        // no nintendo bindings: nothing done, nothing stamped
    kAlreadyStamped,    // the stamp is already there: nothing done
    kKeptDefaults,      // every binding already equals the current default: stamped only
    kKeptNoProvenance,  // bindings present but the file failed the Switch test: stamped only
    kRotated,           // bindings rotated into the positional frame and stamped
};

struct Result {
    Outcome outcome = Outcome::kNoBindings;
    int rotatedBindings = 0;  // values whose text changed under rotation
    bool changed = false;     // the lines differ (rotation and/or stamp): back up, then write
    // key -> new value for every rotated binding, so the caller can fix up the values it
    // already parsed from the pre-migration file.
    std::vector<std::pair<std::string, std::string>> rotatedValues;
};

// Rewrites `lines` (a whole Config.toml, one entry per line, no line endings) in place.
// actionKeys and nintendoDefaults are index-aligned (the port's Keybinds order).
inline Result MigrateGamepadLines(std::vector<std::string>& lines,
                                  const std::array<std::string_view, 12>& actionKeys,
                                  const std::array<std::string_view, 12>& nintendoDefaults,
                                  const Facts& facts) {
    Result result;

    // Find the [gamepad] section, the same way the config writer finds a section.
    size_t sectionStart = lines.size();
    size_t sectionEnd = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(StripComment(lines[i]));
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            if (sectionStart != lines.size()) {
                sectionEnd = i;
                break;
            }
            if (Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2)) == "gamepad") {
                sectionStart = i;
            }
        }
    }
    if (sectionStart == lines.size()) {
        return result;
    }

    struct Entry {
        size_t line = 0;
        size_t action = 0;
        std::string key;
        std::string value;
    };
    std::vector<Entry> entries;
    bool stamped = false;
    for (size_t i = sectionStart + 1; i < sectionEnd; ++i) {
        const std::string uncommented = StripComment(lines[i]);
        const size_t equals = uncommented.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = Trim(std::string_view(uncommented).substr(0, equals));
        if (key == kStampKey) {
            stamped = true;
            continue;
        }
        if (key.rfind("nintendo_", 0) != 0) {
            continue;
        }
        const std::string_view action = std::string_view(key).substr(9);
        size_t actionIndex = actionKeys.size();
        for (size_t a = 0; a < actionKeys.size(); ++a) {
            if (actionKeys[a] == action) {
                actionIndex = a;
                break;
            }
        }
        if (actionIndex == actionKeys.size()) {
            continue;  // not one of the twelve actions: not a binding this migration owns
        }
        // The value is the quoted string after '='. Bindings are written by the config
        // writer in that form; anything else is left exactly as found.
        const size_t open = uncommented.find('"', equals);
        const size_t close = open == std::string::npos ? std::string::npos : uncommented.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos) {
            continue;
        }
        entries.push_back({i, actionIndex, key,
                           std::string(std::string_view(uncommented).substr(open + 1, close - open - 1))});
    }

    if (stamped) {
        result.outcome = Outcome::kAlreadyStamped;
        return result;
    }
    if (entries.empty()) {
        return result;
    }

    // "Unbound" can be written "" or as a lone ","; both parse to no inputs.
    const auto isUnbound = [](const std::string& value) {
        for (const char ch : value) {
            if (ch != ',' && ch != ' ' && ch != '\t') {
                return false;
            }
        }
        return true;
    };
    bool allDefaults = true;
    for (const Entry& entry : entries) {
        const std::string_view fallback = nintendoDefaults[entry.action];
        const bool equal = fallback.empty() ? isUnbound(entry.value)
                                            : entry.value == fallback;
        if (!equal) {
            allDefaults = false;
            break;
        }
    }

    // The stamp goes after the section's last real line, the config writer's rule.
    const auto stamp = [&](const char* value) {
        size_t insertAt = sectionEnd;
        while (insertAt > sectionStart + 1 && Trim(lines[insertAt - 1]).empty()) {
            --insertAt;
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt),
                     std::string(kStampKey) + " = \"" + value + "\"");
        result.changed = true;
    };

    if (allDefaults) {
        result.outcome = Outcome::kKeptDefaults;
        stamp(kStampPositional);
        return result;
    }
    if (!facts.switchSeed || facts.pcEvidence) {
        result.outcome = Outcome::kKeptNoProvenance;
        stamp(kStampUnknown);
        return result;
    }

    result.outcome = Outcome::kRotated;
    for (const Entry& entry : entries) {
        const std::string rotated = RotateBindingValue(entry.value);
        if (rotated == entry.value) {
            continue;
        }
        // The rotated line is rewritten in the writer's form; a trailing comment survives.
        std::string line = entry.key + " = \"" + rotated + "\"";
        const std::string uncommented = StripComment(lines[entry.line]);
        const size_t close = uncommented.rfind('"');
        if (close != std::string::npos) {
            const std::string tail = Trim(std::string_view(lines[entry.line]).substr(close + 1));
            if (!tail.empty() && tail.front() == '#') {
                line += "  " + tail;
            }
        }
        lines[entry.line] = std::move(line);
        ++result.rotatedBindings;
        result.rotatedValues.emplace_back(entry.key, rotated);
    }
    stamp(kStampPositional);
    return result;
}

} // namespace PadBindingMigration
