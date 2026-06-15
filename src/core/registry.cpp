#include "core/registry.hpp"

#include <algorithm>
#include <iostream>

namespace mtk::core {

namespace {

const char* tier_name(Tier t) noexcept {
    switch (t) {
        case Tier::OrgToml:     return "org";
        case Tier::Builtin:     return "builtin";
        case Tier::UserToml:    return "user";
        case Tier::ProjectToml: return "project";
        case Tier::Fallback:    return "fallback";
    }
    return "unknown";
}

}  // namespace

bool Registry::register_filter(std::unique_ptr<Filter> f, Tier tier, bool is_final) {
    if (!f) return false;
    const auto new_name = f->name();

    // Same-tier name collision → reject (loud) per A1.
    for (const auto& rf : filters_) {
        if (rf.tier == tier && rf.filter->name() == new_name) {
            std::cerr << "mtk registry: rejected duplicate filter '" << new_name
                      << "' at tier=" << tier_name(tier)
                      << " (already registered from source="
                      << rf.filter->source() << ")\n";
            return false;
        }
    }

    // Per A7: generalised shadowing prohibition. An `is_final` filter at
    // ANY higher-priority tier (lower Tier number) blocks lower-priority
    // tiers from registering the same name. Two existing uses today:
    //   - Builtin (is_final=true via cmds/*.cpp register_builtins) blocks
    //     ProjectToml from defining e.g. its own `git_log` (closes A2's
    //     silent-shadow attack).
    //   - OrgToml (is_final=true via `locked = true` in /etc/mtk TOML)
    //     blocks Builtin, UserToml, and ProjectToml from same-name
    //     registration — lets an org pin an authoritative pipeline.
    for (const auto& rf : filters_) {
        if (static_cast<int>(rf.tier) < static_cast<int>(tier) &&
            rf.is_final &&
            rf.filter->name() == new_name) {
            std::cerr << "mtk registry: rejected '" << new_name
                      << "' at tier=" << tier_name(tier)
                      << " — would shadow final filter from tier="
                      << tier_name(rf.tier) << " (source="
                      << rf.filter->source() << ")\n";
            return false;
        }
    }

    // Per perf critic B5: keep filters in tier-priority order at insertion
    // time so find() can do a single linear scan instead of allocating +
    // stable-sorting a side index on every invocation. Insertion point is
    // the first entry whose tier number is strictly greater than this one
    // (stable: equal-tier entries stay in registration order).
    auto pos = std::find_if(filters_.begin(), filters_.end(),
                            [tier](const RegisteredFilter& rf) {
                                return static_cast<int>(rf.tier) > static_cast<int>(tier);
                            });
    filters_.insert(pos, RegisteredFilter{std::move(f), tier, is_final});
    return true;
}

Registry::Match Registry::find(const std::vector<std::string>& argv) const {
    // Per perf critic B5: filters_ is already kept in tier-priority order
    // by register_filter, so find() is a single linear scan — no per-call
    // allocation or stable_sort.
    //
    // Per perf critic B6: pre-check literal_first_token() so we don't pay
    // the try_match cost (string compare, regex search) for builtins
    // whose argv[0] mismatches obviously. Filters with no literal (TOML
    // regex match, Passthrough) participate normally.
    std::string_view first = argv.empty() ? std::string_view{} : std::string_view{argv[0]};
    for (const auto& rf : filters_) {
        auto lit = rf.filter->literal_first_token();
        if (!lit.empty() && lit != first) continue;  // cheap mismatch skip

        auto maybe_token = rf.filter->try_match(argv);
        if (maybe_token.has_value()) {
            return Match{rf.filter.get(), std::move(*maybe_token)};
        }
    }
    return Match{};
}

std::vector<Registry::Entry> Registry::describe() const {
    std::vector<Entry> out;
    out.reserve(filters_.size());

    // For shadow detection: for each filter, check whether another filter
    // at a strictly higher-priority tier has the same name.
    for (std::size_t i = 0; i < filters_.size(); ++i) {
        bool shadowed = false;
        for (std::size_t j = 0; j < filters_.size(); ++j) {
            if (i == j) continue;
            if (static_cast<int>(filters_[j].tier) <
                    static_cast<int>(filters_[i].tier) &&
                filters_[j].filter->name() == filters_[i].filter->name()) {
                shadowed = true;
                break;
            }
        }
        out.push_back(Entry{
            filters_[i].filter->name(),
            filters_[i].filter->source(),
            filters_[i].tier,
            shadowed,
        });
    }
    return out;
}

}  // namespace mtk::core
