#pragma once

#include <string>
#include <vector>

namespace ui {

// Semantic version representation and comparison for VMT-Studio.
// Supports formats like "0.1.0", "v0.1.0", "0.2.0-beta.1", "1.0.0.0".
struct AppVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    int build = 0;
    std::string prerelease;

    // Returns the current compiled version of VMT-Studio.
    static AppVersion current();

    // Parses a version string into an AppVersion struct.
    static AppVersion parse(const std::string& str);

    // Formats the version back into a string ("major.minor.patch[-prerelease]").
    std::string to_string() const;

    // Whether this version has at least one positive component.
    bool is_valid() const {
        return major > 0 || minor > 0 || patch > 0 || build > 0;
    }

    bool operator<(const AppVersion& other) const;

    bool operator>(const AppVersion& other) const {
        return other < *this;
    }

    bool operator==(const AppVersion& other) const {
        return major == other.major &&
               minor == other.minor &&
               patch == other.patch &&
               build == other.build &&
               prerelease == other.prerelease;
    }

    bool operator!=(const AppVersion& other) const {
        return !(*this == other);
    }

    bool operator<=(const AppVersion& other) const {
        return !(other < *this);
    }

    bool operator>=(const AppVersion& other) const {
        return !(*this < other);
    }
};

} // namespace ui
