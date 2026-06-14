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

    // ProjectToml may not shadow a Builtin by name (A2).
    if (tier == Tier::ProjectToml) {
        for (const auto& rf : filters_) {
            if (rf.tier == Tier::Builtin && rf.is_final &&
                rf.filter->name() == new_name) {
                std::cerr << "mtk registry: rejected project filter '" << new_name
                          << "' — would shadow final built-in (source="
                          << f->source() << ")\n";
                return false;
            }
        }
    }

    filters_.push_back(RegisteredFilter{std::move(f), tier, is_final});
    return true;
}

Registry::Match Registry::find(const std::vector<std::string>& argv) const {
    // Build a tier-ordered view of filter indices.
    std::vector<std::size_t> order(filters_.size());
    for (std::size_t i = 0; i < filters_.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [this](std::size_t a, std::size_t b) {
                         return static_cast<int>(filters_[a].tier) <
                                static_cast<int>(filters_[b].tier);
                     });

    for (std::size_t i : order) {
        auto maybe_token = filters_[i].filter->try_match(argv);
        if (maybe_token.has_value()) {
            return Match{filters_[i].filter.get(), std::move(*maybe_token)};
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
