#pragma once
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/exec.hpp"

namespace mtk::core {

// Forward declaration: defined in core/run_context.hpp.
class RunContext;

// Per A1: opaque per-filter match state. Each Filter subclass defines its
// own derived token type and `static_cast`s to its concrete type inside
// `run()`. Pairing is structural — the filter that produced the token is
// the filter that consumes it.
//
// Derived tokens MUST NOT store views or spans into the arguments passed
// to `try_match` — those arguments have lifetimes that end when the
// dispatch loop moves on. Copy what you need.
struct DispatchToken {
    virtual ~DispatchToken() = default;
};
using DispatchTokenPtr = std::unique_ptr<DispatchToken>;

// Per A1: the only command abstraction. Every dedicated C++ filter, every
// TOML filter, and the final-fallback passthrough all implement this.
class Filter {
public:
    virtual ~Filter() = default;

    // Stable name used for audit emission, `mtk explain`, and registry
    // collision detection. Must be unique within a registry tier
    // (Registry::register_filter enforces).
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Provenance string surfaced by `mtk explain`. Values: "builtin",
    // path of the originating TOML file, etc.
    [[nodiscard]] virtual std::string_view source() const noexcept = 0;

    // Cheap. Must not throw (noexcept). Expensive setup (regex compile,
    // glob compile, TOML schema validation) MUST happen at registry
    // construction, not here. `argv` is the full command (argv[0] is the
    // command name). Returns a non-null token on match (the token may be
    // the default-constructed DispatchToken if the filter needs no
    // per-match state); returns std::nullopt on non-match.
    [[nodiscard]] virtual std::optional<DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept = 0;

    // May throw — A4's RunContext-level wrapper catches and falls back to
    // raw passthrough. `argv` is the same full command try_match saw.
    // Returns the captured-and-filtered ExecOutcome; the dispatcher calls
    // ctx.emit() on it.
    [[nodiscard]] virtual mtk::core::exec::ExecOutcome
    run(DispatchTokenPtr token,
        const std::vector<std::string>& argv,
        RunContext& ctx) = 0;
};

}  // namespace mtk::core
