#include "core/default_registry.hpp"

#include "cmds/git.hpp"
#include "cmds/grep.hpp"
#include "cmds/ls.hpp"
#include "core/config.hpp"
#include "core/passthrough_filter.hpp"
#include "core/toml_filter_adapter.hpp"

namespace mtk::core {

Registry build_default_registry() {
    Registry reg;

    // Tier::Builtin — dedicated C++ filters, final (cannot be shadowed by
    // project TOML per A2).
    mtk::cmds::git::register_builtins(reg);
    mtk::cmds::ls::register_builtins(reg);
    mtk::cmds::grep::register_builtins(reg);

    // Tier::UserToml — TOML filters from ~/.config/mtk/filters/. Always loaded.
    auto user_tomls = mtk::core::config::load_user_filters();
    for (auto& t : user_tomls) {
        std::string source = "user:" + t.name;
        reg.register_filter(
            std::make_unique<TomlFilterAdapter>(std::move(t), std::move(source)),
            Tier::UserToml);
    }

    // Tier::ProjectToml — TOML filters from <cwd>/.mtk/filters/. Per A2:
    // gated on trust (mtk trust . / mtk untrust . / MTK_ALLOW_PROJECT_FILTERS=1).
    // Registry::register_filter rejects any project filter whose name() matches
    // a final built-in — A2's shadow-prohibition.
    auto project_tomls = mtk::core::config::load_project_filters();
    for (auto& t : project_tomls) {
        std::string source = "project:" + t.name;
        reg.register_filter(
            std::make_unique<TomlFilterAdapter>(std::move(t), std::move(source)),
            Tier::ProjectToml);
    }

    // Tier::Fallback — always-match passthrough.
    reg.register_filter(std::make_unique<PassthroughFilter>(), Tier::Fallback);

    return reg;
}

}  // namespace mtk::core
