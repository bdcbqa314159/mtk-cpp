#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/filter.hpp"

namespace mtk::core {

// Per A2: the final fallback. Always matches; runs `ctx.passthrough(argv)`.
// Registered at Tier::Fallback so it's checked last and only fires when no
// other filter claimed the command.
//
// Always returns a Ran with empty stdout/stderr (passthrough inherits stdio
// and writes directly to the user's terminal) and the propagated exit code.
// Spawn-failure diagnostics are owned by RunContext::passthrough, which
// writes to stderr and returns 127 — we can't distinguish that from a real
// 127 exit at this layer and don't try.
class PassthroughFilter final : public Filter {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "_passthrough";
    }
    [[nodiscard]] std::string_view source() const noexcept override {
        return "builtin";
    }
    [[nodiscard]] std::optional<DispatchTokenPtr>
    try_match(const std::vector<std::string>&) const noexcept override {
        return DispatchTokenPtr{};  // always matches, no token state
    }
    [[nodiscard]] mtk::core::exec::ExecOutcome
    run(DispatchTokenPtr,
        const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override;
};

}  // namespace mtk::core
