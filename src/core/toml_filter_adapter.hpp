#pragma once
#include <string>

#include "core/filter.hpp"
#include "core/toml_filter.hpp"

namespace mtk::core {

// Adapts an existing `mtk::core::toml_filter::Filter` (the 8-stage
// pipeline data) into the `Filter` interface used by the registry.
// One TomlFilterAdapter is constructed per loaded TOML filter at registry
// build time. The underlying toml_filter::Filter is held by value so the
// adapter owns its state; the source path is recorded for `mtk explain`.
class TomlFilterAdapter final : public Filter {
public:
    TomlFilterAdapter(mtk::core::toml_filter::Filter data, std::string source_path);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::string_view source() const noexcept override;
    [[nodiscard]] std::string_view literal_first_token() const noexcept override;
    [[nodiscard]] std::optional<DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept override;
    [[nodiscard]] mtk::core::exec::ExecOutcome
    run(DispatchTokenPtr token,
        const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override;

private:
    mtk::core::toml_filter::Filter data_;
    std::string source_path_;
    std::string literal_token_;  // extracted from `^literal$` patterns; empty otherwise
};

}  // namespace mtk::core
