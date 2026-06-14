#include "core/utils.hpp"

#include <algorithm>
#include <regex>
#include <sstream>

namespace mtk::core::utils {

namespace {
const std::regex& ansi_regex() {
    static const std::regex re(R"(\x1b\[[0-9;]*[a-zA-Z])");
    return re;
}
}  // namespace

std::string strip_ansi(std::string_view input) {
    std::string s(input);
    return std::regex_replace(s, ansi_regex(), "");
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
            lines.emplace_back(input.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < input.size()) {
        lines.emplace_back(input.substr(start));
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

}  // namespace mtk::core::utils
