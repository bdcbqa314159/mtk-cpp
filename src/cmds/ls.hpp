#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/registry.hpp"

namespace mtk::cmds::ls {

// Registers all ls-related filters into the given registry at Tier::Builtin
// with is_final=true (per A2: project filters cannot shadow built-ins).
void register_builtins(mtk::core::Registry& reg);

namespace internal {

struct LsOptions {
    bool show_all = false;
    bool show_long = false;
};

LsOptions parse_args(const std::vector<std::string>& args);

struct LsEntry {
    char type = '-';
    std::string perms;
    std::uint64_t size = 0;
    std::string name;
};

bool is_dotdir(std::string_view line);
std::optional<LsEntry> parse_ls_line(const std::string& line);
std::optional<std::string> perms_to_octal(const std::string& perms);
std::string human_size(std::uint64_t bytes);

struct CompactResult {
    std::string entries;
    std::string summary;
    std::size_t parsed_count = 0;
};

CompactResult compact_ls(std::string_view raw, bool show_all, bool show_long);

}  // namespace internal

}  // namespace mtk::cmds::ls
