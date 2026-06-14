#include "core/toml_filter_adapter.hpp"

#include "core/run_context.hpp"

namespace mtk::core {

namespace ex = mtk::core::exec;

TomlFilterAdapter::TomlFilterAdapter(mtk::core::toml_filter::Filter data,
                                     std::string source_path)
    : data_(std::move(data)), source_path_(std::move(source_path)) {}

std::string_view TomlFilterAdapter::name() const noexcept {
    return data_.name;
}

std::string_view TomlFilterAdapter::source() const noexcept {
    return source_path_;
}

std::optional<DispatchTokenPtr>
TomlFilterAdapter::try_match(const std::vector<std::string>& argv) const noexcept {
    if (argv.empty()) return std::nullopt;
    try {
        if (mtk::core::toml_filter::command_matches(data_, argv[0])) {
            return DispatchTokenPtr{};  // match, no state needed
        }
    } catch (...) {
        // try_match is noexcept; swallow any regex errors as non-match.
    }
    return std::nullopt;
}

ex::ExecOutcome TomlFilterAdapter::run(DispatchTokenPtr,
                                       const std::vector<std::string>& argv,
                                       RunContext& ctx) {
    auto outcome = ctx.capture(argv);
    const auto* ran = ctx.as_ran(outcome);
    if (!ran) return outcome;  // SpawnFailed flows through

    // Per A4: tee the RAW output (not the filtered version) on non-zero
    // exit, before we apply the filter. The user can recover the unfiltered
    // bytes from the tee file if our compression dropped something they
    // needed.
    ctx.tee_on_failure(outcome, argv.empty() ? std::string_view{name()}
                                             : std::string_view(argv[0]));

    const std::string blob = data_.filter_stderr
        ? (ran->stdout_data + ran->stderr_data)
        : ran->stdout_data;

    std::string filtered;
    try {
        filtered = mtk::core::toml_filter::apply(data_, blob);
    } catch (const std::exception& e) {
        std::string warning = "mtk ";
        warning.append(name());
        warning.append(": filter warning: ");
        warning.append(e.what());
        warning.push_back('\n');
        // We don't have a direct way to emit warnings through emit() for a
        // Ran outcome; fall back to raw and put a note in stderr_data so
        // the dispatcher's emit() surfaces it.
        return ex::Ran{
            blob,
            std::string(ran->stderr_data) + warning,
            ran->exit_code,
            ran->truncated, ran->timed_out, ran->killed_by_signal,
        };
    }

    return ex::Ran{
        std::move(filtered),
        data_.filter_stderr ? std::string{} : ran->stderr_data,
        ran->exit_code,
        ran->truncated, ran->timed_out, ran->killed_by_signal,
    };
}

}  // namespace mtk::core
