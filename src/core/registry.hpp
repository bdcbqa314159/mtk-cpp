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

// Convention: `a` has higher priority than `b` when its enumerator is
// numerically smaller. Centralised so the convention is named in one
// place — a sign flip in this function is the kind of bug that would
// silently invert the entire dispatch hierarchy.
[[nodiscard]] constexpr bool higher_priority_than(Tier a, Tier b) noexcept {
    return static_cast<int>(a) < static_cast<int>(b);
}

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
    // false, drops the filter) when EITHER:
    //   - a filter with the same name() is already registered at the
    //     same tier (duplicate per-tier names are never allowed); OR
    //   - an `is_final` filter with the same name() is already
    //     registered at any HIGHER-priority tier (lower numeric Tier
    //     value). This is the generalised A7 shadow rule: it blocks a
    //     same-name registration further down the priority chain.
    //
    // Concretely:
    //   - Builtins are registered with is_final=true, so a ProjectToml
    //     filter trying the same name is rejected (A2 silent-shadow
    //     protection).
    //   - An OrgToml filter with `locked = true` is registered with
    //     is_final=true, so a same-name Builtin / UserToml / ProjectToml
    //     is rejected. NOTE: this requires the org filter to be
    //     registered first — default_registry.cpp loads org BEFORE
    //     builtins to make this work at registration time, not just at
    //     find-time.
    //
    // The check inspects only filters already registered when this is
    // called — registration order matters. A later same-name attempt at
    // a higher-priority tier does NOT retroactively evict a prior
    // lower-priority registration; instead it succeeds and is_final
    // (if set) starts gating subsequent attempts.
    //
    // Return value is documentary — call sites don't currently check it
    // (registration happens at startup; failure logs to stderr). Leaving
    // it without [[nodiscard]] preserves the existing "best-effort
    // register" call pattern in default_registry / cmds/*.cpp.
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
