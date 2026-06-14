#pragma once
#include "core/filter.hpp"

namespace mtk::core {

// Per A2: the final fallback. Always matches; runs `ctx.passthrough(argv)`.
// Registered at Tier::Fallback so it's checked last and only fires when no
// other filter claimed the command.
//
// Returns a SpawnFailed-style ExecOutcome only when passthrough returns a
// non-zero exit indicating spawn failure (127). Otherwise returns a Ran
// with empty stdout/stderr (since passthrough inherits stdio and writes
// directly) and the propagated exit code.
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
