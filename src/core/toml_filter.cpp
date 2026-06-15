#include "core/toml_filter.hpp"

#include <sstream>
#include <toml++/toml.hpp>

#include "core/utils.hpp"

namespace mtk::core::toml_filter {

namespace {

bool any_regex_matches(const std::string& line, const std::vector<std::regex>& res) {
    for (const auto& re : res) {
        try {
            if (std::regex_search(line, re)) return true;
        } catch (const std::regex_error&) {
            // Match failure with a compiled regex is rare (stack overflow
            // on pathological input); treat as non-match.
        }
    }
    return false;
}

template <typename T>
std::optional<std::size_t> as_uint(const T& node) {
    if (!node) return std::nullopt;
    if (auto v = node.template value<int64_t>()) {
        if (*v >= 0) return static_cast<std::size_t>(*v);
    }
    return std::nullopt;
}

template <typename View>
std::vector<std::string> read_string_array(View node) {
    std::vector<std::string> out;
    if (auto arr = node.as_array()) {
        for (const auto& e : *arr) {
            if (auto s = e.template value<std::string>()) out.push_back(*s);
        }
    }
    return out;
}

// Compile a single regex, returning std::nullopt on regex_error so we
// silently skip bad patterns (filter must never block the user).
std::optional<std::regex> try_compile(const std::string& pattern) {
    if (pattern.empty()) return std::nullopt;
    try {
        return std::regex(pattern);
    } catch (const std::regex_error&) {
        return std::nullopt;
    }
}

}  // namespace

// Populate the *_re fields from the *_string fields. Public so tests /
// programmatic Filter construction can call it. Called by parse_all()
// automatically.
void compile(Filter& f) {
    f.match_command_re = try_compile(f.match_command_pattern);

    f.replace_re.clear();
    f.replace_re.reserve(f.replace.size());
    for (const auto& [pat, rep] : f.replace) {
        if (auto re = try_compile(pat)) {
            f.replace_re.emplace_back(std::move(*re), rep);
        }
    }

    if (f.match_output) {
        const auto& [pat, msg] = *f.match_output;
        if (auto re = try_compile(pat)) {
            f.match_output_re = std::make_pair(std::move(*re), msg);
        }
    }

    f.strip_lines_re.clear();
    f.strip_lines_re.reserve(f.strip_lines_matching.size());
    for (const auto& pat : f.strip_lines_matching) {
        if (auto re = try_compile(pat)) f.strip_lines_re.push_back(std::move(*re));
    }

    f.keep_lines_re.clear();
    f.keep_lines_re.reserve(f.keep_lines_matching.size());
    for (const auto& pat : f.keep_lines_matching) {
        if (auto re = try_compile(pat)) f.keep_lines_re.push_back(std::move(*re));
    }
}

std::vector<Filter> parse_all(std::string_view toml_source) {
    std::vector<Filter> filters;
    toml::table tbl;
    try {
        tbl = toml::parse(toml_source);
    } catch (const toml::parse_error&) {
        return filters;
    }

    auto root = tbl["filters"].as_table();
    if (!root) return filters;

    for (const auto& [key, node] : *root) {
        auto* section = node.as_table();
        if (!section) continue;
        Filter f;
        f.name = std::string(key.str());
        f.description = (*section)["description"].value_or<std::string>("");
        f.match_command_pattern = (*section)["match_command"].value_or<std::string>("");
        f.filter_stderr = (*section)["filter_stderr"].value_or(false);
        f.strip_ansi = (*section)["strip_ansi"].value_or(false);

        if (auto repl_arr = (*section)["replace"].as_array()) {
            for (const auto& e : *repl_arr) {
                if (auto inner = e.as_array(); inner && inner->size() == 2) {
                    auto p = (*inner)[0].value<std::string>();
                    auto r = (*inner)[1].value<std::string>();
                    if (p && r) f.replace.emplace_back(*p, *r);
                }
            }
        }

        if (auto mo = (*section)["match_output"].as_array(); mo && mo->size() == 2) {
            auto p = (*mo)[0].value<std::string>();
            auto m = (*mo)[1].value<std::string>();
            if (p && m) f.match_output = std::make_pair(*p, *m);
        }

        f.strip_lines_matching = read_string_array((*section)["strip_lines_matching"]);
        f.keep_lines_matching = read_string_array((*section)["keep_lines_matching"]);
        f.truncate_lines_at = as_uint((*section)["truncate_lines_at"]);
        f.head_lines = as_uint((*section)["head_lines"]);
        f.tail_lines = as_uint((*section)["tail_lines"]);
        f.max_lines = as_uint((*section)["max_lines"]);
        if (auto oe = (*section)["on_empty"].value<std::string>()) f.on_empty = *oe;

        compile(f);
        filters.push_back(std::move(f));
    }
    return filters;
}

bool command_matches(const Filter& f, std::string_view cmd_name) {
    if (!f.match_command_re) return false;
    try {
        return std::regex_search(std::string(cmd_name), *f.match_command_re);
    } catch (const std::regex_error&) {
        return false;
    }
}

std::string apply(const Filter& f, std::string_view input) {
    std::string s(input);

    if (f.strip_ansi) {
        s = mtk::core::utils::strip_ansi(s);
    }

    for (const auto& [re, rep] : f.replace_re) {
        try {
            s = std::regex_replace(s, re, rep);
        } catch (const std::regex_error&) {
            // Skip this replacement on runtime error.
        }
    }

    if (f.match_output_re) {
        const auto& [re, msg] = *f.match_output_re;
        try {
            if (std::regex_search(s, re)) return msg;
        } catch (const std::regex_error&) {
        }
    }

    auto lines = mtk::core::utils::split_lines(s);

    if (!f.strip_lines_re.empty()) {
        std::vector<std::string> kept;
        kept.reserve(lines.size());
        for (auto& l : lines) {
            if (!any_regex_matches(l, f.strip_lines_re)) kept.push_back(std::move(l));
        }
        lines = std::move(kept);
    }

    if (!f.keep_lines_re.empty()) {
        std::vector<std::string> kept;
        kept.reserve(lines.size());
        for (auto& l : lines) {
            if (any_regex_matches(l, f.keep_lines_re)) kept.push_back(std::move(l));
        }
        lines = std::move(kept);
    }

    if (f.truncate_lines_at) {
        for (auto& l : lines) {
            l = mtk::core::utils::truncate(l, *f.truncate_lines_at);
        }
    }

    if (f.head_lines && lines.size() > *f.head_lines) {
        lines.resize(*f.head_lines);
    }
    if (f.tail_lines && lines.size() > *f.tail_lines) {
        lines.erase(lines.begin(),
                    lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - *f.tail_lines));
    }
    if (f.max_lines && lines.size() > *f.max_lines) {
        std::size_t keep_each = *f.max_lines / 2;
        std::size_t dropped = lines.size() - 2 * keep_each;
        std::vector<std::string> head(lines.begin(),
                                      lines.begin() + static_cast<std::ptrdiff_t>(keep_each));
        std::vector<std::string> tail(lines.end() - static_cast<std::ptrdiff_t>(keep_each),
                                      lines.end());
        std::ostringstream marker;
        marker << "  ... [+" << dropped << " lines dropped] ...";
        head.push_back(marker.str());
        head.insert(head.end(), std::make_move_iterator(tail.begin()),
                    std::make_move_iterator(tail.end()));
        lines = std::move(head);
    }

    std::string out = mtk::core::utils::join_lines(lines);
    if (out.empty() && f.on_empty) out = *f.on_empty;
    return out;
}

}  // namespace mtk::core::toml_filter
