#include "core/default_registry.hpp"

#include "cmds/git.hpp"
#include "cmds/grep.hpp"
#include "cmds/ls.hpp"
#include "core/config.hpp"
#include "core/filter_cache.hpp"
#include "core/passthrough_filter.hpp"
#include "core/toml_filter_adapter.hpp"

namespace mtk::core {

namespace {

void register_org_filters(Registry& reg, std::vector<toml_filter::Filter>& tomls) {
    for (auto& t : tomls) {
        // Per A7: `locked = true` in an org TOML promotes is_final, which
        // blocks every lower-priority tier (Builtin/User/Project) from
        // registering the same name.
        const bool is_final = t.locked;
        std::string source = "org:" + t.name;
        reg.register_filter(
            std::make_unique<TomlFilterAdapter>(std::move(t), std::move(source)),
            Tier::OrgToml, is_final);
    }
}

void register_user_filters(Registry& reg, std::vector<toml_filter::Filter>& tomls) {
    for (auto& t : tomls) {
        std::string source = "user:" + t.name;
        reg.register_filter(
            std::make_unique<TomlFilterAdapter>(std::move(t), std::move(source)),
            Tier::UserToml);
    }
}

void register_project_filters(Registry& reg, std::vector<toml_filter::Filter>& tomls) {
    for (auto& t : tomls) {
        std::string source = "project:" + t.name;
        reg.register_filter(
            std::make_unique<TomlFilterAdapter>(std::move(t), std::move(source)),
            Tier::ProjectToml);
    }
}

}  // namespace

Registry build_default_registry() {
    Registry reg;

    // Tier::Builtin — dedicated C++ filters, final (cannot be shadowed by
    // project TOML per A2).
    mtk::cmds::git::register_builtins(reg);
    mtk::cmds::ls::register_builtins(reg);
    mtk::cmds::grep::register_builtins(reg);

    // Per A7: org filters always loaded fresh — they're root-owned in
    // /etc and shouldn't be cached in the user's cache dir (a stale
    // cache could mask a policy update). They're typically a small
    // handful, parse cost is negligible.
    auto org_tomls = mtk::core::config::load_org_filters();
    register_org_filters(reg, org_tomls);

    // Per perf critic P2: try the binary cache first. Manifest covers
    // every TOML file we'd load — any mtime/size change invalidates,
    // forcing a fresh parse.
    auto user_paths = mtk::core::config::user_filter_paths();
    auto project_paths = mtk::core::config::project_filter_paths();
    std::vector<std::filesystem::path> all_paths;
    all_paths.reserve(user_paths.size() + project_paths.size());
    all_paths.insert(all_paths.end(), user_paths.begin(), user_paths.end());
    all_paths.insert(all_paths.end(), project_paths.begin(), project_paths.end());

    auto manifest = mtk::core::filter_cache::make_manifest(all_paths);

    std::vector<mtk::core::toml_filter::Filter> user_tomls;
    std::vector<mtk::core::toml_filter::Filter> project_tomls;

    if (mtk::core::filter_cache::is_valid(manifest)) {
        auto loaded = mtk::core::filter_cache::load();
        user_tomls = std::move(loaded.user_filters);
        project_tomls = std::move(loaded.project_filters);
    } else {
        // Slow path: parse the TOML files fresh, then write the cache so
        // the next invocation hits the fast path.
        user_tomls = mtk::core::config::load_user_filters();
        // load_project_filters does the trust check + nag; matches our
        // project_paths empty/non-empty.
        project_tomls = mtk::core::config::load_project_filters();
        mtk::core::filter_cache::save(manifest, user_tomls, project_tomls);
    }

    register_user_filters(reg, user_tomls);
    register_project_filters(reg, project_tomls);

    // Tier::Fallback — always-match passthrough.
    reg.register_filter(std::make_unique<PassthroughFilter>(), Tier::Fallback);

    return reg;
}

}  // namespace mtk::core
