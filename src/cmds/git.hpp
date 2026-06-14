#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace mtk::cmds::git {

int run(const std::vector<std::string>& args);

namespace internal {

struct LogOptions {
    bool user_set_format = false;
    bool user_set_count = false;
};

LogOptions detect_log_options(const std::vector<std::string>& args);

std::string filter_log_output(std::string_view raw,
                              std::size_t max_commits,
                              std::size_t header_width,
                              std::size_t max_body_lines);

}  // namespace internal

}  // namespace mtk::cmds::git
