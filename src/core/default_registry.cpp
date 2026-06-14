#include "core/default_registry.hpp"

#include "core/config.hpp"
#include "core/passthrough_filter.hpp"
#include "core/toml_filter_adapter.hpp"

namespace mtk::core {

Registry build_default_registry() {
    Registry reg;

    // User TOML filters from ~/.config/mtk/filters and .mtk/filters.
    // (Today: load_all_filters bundles both directories without source
    // distinction. Phase 4's settings refactor splits them into proper
    // user/project tiers per A2.)
    auto tomls = mtk::core::config::load_all_filters();
    for (auto& t : tomls) {
        std::string source = "toml:" + t.name;
        reg.register_filter(
            std::make_unique<TomlFilterAdapter>(std::move(t), std::move(source)),
            Tier::UserToml);
    }

    // Final fallback: always-match passthrough.
    reg.register_filter(std::make_unique<PassthroughFilter>(), Tier::Fallback);

    return reg;
}

}  // namespace mtk::core
