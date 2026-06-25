#include "core/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mtk::core::utils {

namespace {
const std::regex& ansi_regex() {
    static const std::regex re(R"(\x1b\[[0-9;]*[a-zA-Z])");
    return re;
}
}  // namespace

std::string strip_ansi(std::string_view input) {
    // Per perf critic P12: search-first guard. Most captured output has
    // zero ANSI escape sequences (any tool whose stdout is a pipe sees
    // isatty()==false and skips colouring). std::regex_replace is the
    // heaviest path in the library: building a match_results, walking the
    // input via the NFA, and rebuilding a string. Probing for ESC (0x1b)
    // with memchr first lets the common case return without instantiating
    // any of that.
    if (input.find('\x1b') == std::string_view::npos) {
        return std::string(input);
    }
    std::string s(input);
    return std::regex_replace(s, ansi_regex(), "");
}

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

std::string trim_copy(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return std::string(s.substr(i, j - i));
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string truncate(std::string_view input, std::size_t max_len) {
    if (input.size() <= max_len) return std::string(input);
    if (max_len <= 3) return std::string(input.substr(0, max_len));

    std::size_t cut = max_len - 3;
    while (cut > 0 && (static_cast<unsigned char>(input[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    std::string out(input.substr(0, cut));
    out += "...";
    return out;
}

std::vector<std::string> split_lines(std::string_view input) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\n') {
            auto end = i;
            if (end > start && input[end - 1] == '\r') --end;  // strip \r\n
            lines.emplace_back(input.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < input.size()) {
        auto len = input.size() - start;
        if (len > 0 && input.back() == '\r') --len;
        lines.emplace_back(input.substr(start, len));
    }
    return lines;
}

std::vector<std::string_view> split_lines_view(std::string_view input) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\n') {
            auto end = i;
            if (end > start && input[end - 1] == '\r') --end;  // strip \r\n
            lines.emplace_back(input.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < input.size()) {
        auto len = input.size() - start;
        if (len > 0 && input.back() == '\r') --len;
        lines.emplace_back(input.substr(start, len));
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream os;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        os << lines[i];
        if (i + 1 < lines.size()) os << '\n';
    }
    return os.str();
}

std::size_t count_tokens(std::string_view input) {
    std::size_t n = 0;
    bool in_word = false;
    for (char c : input) {
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!ws && !in_word) {
            ++n;
            in_word = true;
        } else if (ws) {
            in_word = false;
        }
    }
    return n;
}

void json_escape_into(std::string& out, std::string_view s) {
    out.reserve(out.size() + s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

}  // namespace mtk::core::utils
