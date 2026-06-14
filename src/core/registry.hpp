#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/filter.hpp"

namespace mtk::core {

// Per A2: dispatch priority tiers. Higher value = higher priority
// (checked first). Built-in C++ filters are `final = true` — project
// filters cannot shadow them by name; user filters can.
enum class Tier : int {
    OrgToml = 0,        // /etc/mtk/filters/*.toml
    Builtin = 1,        // dedicated C++ filters
    UserToml = 2,       // ~/.config/mtk/filters/*.toml
    ProjectToml = 3,    // .mtk/filters/*.toml (opt-in via `mtk trust`)
    Fallback = 4,       // PassthroughFilter (always matches, always last)
};

// Lower numeric value = checked FIRST (org overrides built-in overrides user etc.).
// We iterate in ascending Tier order during dispatch.

struct RegisteredFilter {
    std::unique_ptr<Filter> filter;
    Tier tier;
    bool is_final = false;  // true for built-ins; blocks ProjectToml shadowing
};

class Registry {
public:
    Registry() = default;
    ~Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = default;
    Registry& operator=(Registry&&) = default;

    // Adds a filter to the registry. Rejects (logs to stderr, returns
    // false, drops the filter) if:
    //   - a filter with the same name() exists at the same tier
    //   - the filter is at ProjectToml tier and a Builtin filter with the
    //     same name() exists (A2's shadowing prohibition)
    bool register_filter(std::unique_ptr<Filter> f, Tier tier, bool is_final = false);

    // Find the first matching filter across all tiers in priority order.
    // Returns {filter*, token} or {nullptr, nullptr} if nothing matched
    // (which should be impossible if a Fallback filter is registered).
    // `argv` is the full command (argv[0] is the command name).
    struct Match {
        Filter* filter = nullptr;
        DispatchTokenPtr token;
    };
    [[nodiscard]] Match find(const std::vector<std::string>& argv) const;

    // Enumerate all filters for `mtk explain`. Returns name + tier + source
    // + whether a higher-priority filter would shadow this one.
    struct Entry {
        std::string_view name;
        std::string_view source;
        Tier tier;
        bool shadowed;
    };
    [[nodiscard]] std::vector<Entry> describe() const;

private:
    std::vector<RegisteredFilter> filters_;
};

}  // namespace mtk::core
