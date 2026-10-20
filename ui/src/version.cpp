#include "ui/version.h"

#include <algorithm>
#include <sstream>

#ifndef VMTSTUDIO_VERSION_STRING
#define VMTSTUDIO_VERSION_STRING "0.2.0"
#endif

namespace ui {

AppVersion AppVersion::current() {
    return parse(VMTSTUDIO_VERSION_STRING);
}

// Helper functions for SemVer 2.0.0 prerelease precedence comparison (section 11.4)
static bool is_numeric_identifier(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

static std::vector<std::string> split_prerelease(const std::string& pre) {
    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (start < pre.size()) {
        const std::size_t dot = pre.find('.', start);
        tokens.push_back((dot == std::string::npos) ? pre.substr(start) : pre.substr(start, dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return tokens;
}

static int compare_prerelease_identifiers(const std::string& a, const std::string& b) {
    const bool a_num = is_numeric_identifier(a);
    const bool b_num = is_numeric_identifier(b);
    if (a_num && b_num) {
        try {
            const unsigned long long na = std::stoull(a);
            const unsigned long long nb = std::stoull(b);
            if (na < nb) return -1;
            if (na > nb) return 1;
            return 0;
        } catch (...) {
            // fallback
        }
    } else if (a_num && !b_num) {
        // Numeric identifiers always have lower precedence than non-numeric
        return -1;
    } else if (!a_num && b_num) {
        return 1;
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static int compare_prereleases(const std::string& a, const std::string& b) {
    const auto a_parts = split_prerelease(a);
    const auto b_parts = split_prerelease(b);
    const std::size_t min_len = std::min(a_parts.size(), b_parts.size());
    for (std::size_t i = 0; i < min_len; ++i) {
        const int cmp = compare_prerelease_identifiers(a_parts[i], b_parts[i]);
        if (cmp != 0) return cmp;
    }
    if (a_parts.size() < b_parts.size()) return -1;
    if (a_parts.size() > b_parts.size()) return 1;
    return 0;
}

bool AppVersion::operator<(const AppVersion& other) const {
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    if (patch != other.patch) return patch < other.patch;
    if (build != other.build) return build < other.build;

    // SemVer 2.0.0: A version without a prerelease tag has higher precedence than one with a prerelease.
    // E.g. "0.2.0" > "0.2.0-beta"
    if (prerelease.empty() && !other.prerelease.empty()) return false;
    if (!prerelease.empty() && other.prerelease.empty()) return true;

    return compare_prereleases(prerelease, other.prerelease) < 0;
}

AppVersion AppVersion::parse(const std::string& str) {
    AppVersion v;
    if (str.empty()) {
        return v;
    }

    std::string s = str;
    // Trim leading whitespace
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.erase(s.begin());
    }
    // Trim trailing whitespace
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }

    // Trim leading 'v' or 'V'
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) {
        s.erase(s.begin());
    }

    // Separate build metadata (+...) first according to SemVer 2.0.0 spec section 10.
    // Build metadata is ignored when determining version precedence.
    const std::size_t plus = s.find('+');
    if (plus != std::string::npos) {
        s = s.substr(0, plus);
    }

    // Separate prerelease suffix (-...)
    const std::size_t dash = s.find('-');
    if (dash != std::string::npos) {
        v.prerelease = s.substr(dash + 1);
        s = s.substr(0, dash);
    }

    // Parse dot-separated numbers
    std::vector<int> parts;
    std::size_t start = 0;
    while (start < s.size()) {
        const std::size_t dot = s.find('.', start);
        const std::string part = (dot == std::string::npos) ? s.substr(start) : s.substr(start, dot - start);
        try {
            int val = std::stoi(part);
            parts.push_back(val < 0 ? 0 : val);
        } catch (...) {
            parts.push_back(0);
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }

    if (parts.size() > 0) v.major = parts[0];
    if (parts.size() > 1) v.minor = parts[1];
    if (parts.size() > 2) v.patch = parts[2];
    if (parts.size() > 3) v.build = parts[3];

    return v;
}

std::string AppVersion::to_string() const {
    std::ostringstream ss;
    ss << major << "." << minor << "." << patch;
    if (build > 0) {
        ss << "." << build;
    }
    if (!prerelease.empty()) {
        ss << "-" << prerelease;
    }
    return ss.str();
}

} // namespace ui
