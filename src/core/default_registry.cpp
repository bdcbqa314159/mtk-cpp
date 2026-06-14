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

    // Tier::UserToml — TOML filters from ~/.config/mtk/filters and
    // .mtk/filters. (Phase 4's settings refactor splits these into proper
    // user vs project tiers per A2's allow-list.)
    auto tomls = mtk::core::config::load_all_filters();
    for (auto& t : tomls) {
        std::string source = "toml:" + t.name;
        reg.register_filter(
            std::make_unique<TomlFilterAdapter>(std::move(t), std::move(source)),
            Tier::UserToml);
    }

    // Tier::Fallback — always-match passthrough.
    reg.register_filter(std::make_unique<PassthroughFilter>(), Tier::Fallback);

    return reg;
}

}  // namespace mtk::core
