#include "core/run_context.hpp"

#include <iostream>
#include <variant>

#include "core/audit.hpp"
#include "core/diagnostic.hpp"
#include "core/exit_codes.hpp"
#include "core/tee.hpp"

namespace mtk::core {

namespace ex = mtk::core::exec;

ex::ExecOutcome RunContext::capture(const std::vector<std::string>& argv,
                                    const ex::EnvExtra& env_extra) noexcept {
    try {
        auto outcome = ex::capture_outcome(argv, env_extra);
        if (auto* ran = std::get_if<ex::Ran>(&outcome)) {
            cumulative_in_bytes_ += ran->stdout_data.size() + ran->stderr_data.size();
        }
        return outcome;
    } catch (const std::exception& e) {
        return ex::SpawnFailed{e.what()};
    } catch (...) {
        return ex::SpawnFailed{"unknown error during capture"};
    }
}

int RunContext::passthrough(const std::vector<std::string>& argv) noexcept {
    try {
        return ex::passthrough(argv);
    } catch (...) {
        return mtk::core::exit_codes::kNotFound;
    }
}

bool RunContext::is_spawn_failed(const ex::ExecOutcome& outcome) noexcept {
    return std::holds_alternative<ex::SpawnFailed>(outcome);
}

bool RunContext::is_ran(const ex::ExecOutcome& outcome) noexcept {
    return std::holds_alternative<ex::Ran>(outcome);
}

const ex::Ran* RunContext::as_ran(const ex::ExecOutcome& outcome) noexcept {
    return std::get_if<ex::Ran>(&outcome);
}

int RunContext::emit(ex::ExecOutcome&& outcome, std::string_view tool) noexcept {
    // std::visit + overloaded is the canonical C++17 idiom for two-arm
    // variant dispatch (and the only place in mtk that's allowed to do
    // this — see CE5).
    if (auto* err = std::get_if<ex::SpawnFailed>(&outcome)) {
        return mtk::core::exit_codes::report_spawn_failure(tool, err->message);
    }
    auto* ran = std::get_if<ex::Ran>(&outcome);
    if (!ran) return mtk::core::exit_codes::kNotFound;  // unreachable; defensive

    // Move the strings out — per A3, emit consumes the outcome and the
    // caller cannot reuse it after.
    std::string stdout_data = std::move(ran->stdout_data);
    std::string stderr_data = std::move(ran->stderr_data);
    int exit_code = ran->exit_code;

    try {
        std::cout << stdout_data;
        if (!stdout_data.empty() && stdout_data.back() != '\n') std::cout << '\n';
        if (!stderr_data.empty()) std::cerr << stderr_data;
    } catch (...) {
        // I/O failure on stdout/stderr — nothing useful we can do, exit
        // code still propagates.
    }
    return exit_code;
}

void RunContext::tee_on_failure(const ex::ExecOutcome& outcome,
                                std::string_view slug) noexcept {
    auto* ran = std::get_if<ex::Ran>(&outcome);
    if (!ran || ran->clean()) return;
    try {
        if (auto hint = mtk::core::tee::tee_and_hint(
                ran->stdout_data, slug, ran->exit_code)) {
            std::cerr << *hint << '\n';
        }
    } catch (...) {
        // Tee writes are best-effort.
    }
}

int RunContext::report_spawn_failure(const ex::ExecOutcome& outcome,
                                     std::string_view tool) noexcept {
    if (auto* err = std::get_if<ex::SpawnFailed>(&outcome)) {
        return mtk::core::exit_codes::report_spawn_failure(tool, err->message);
    }
    return -1;
}

std::string RunContext::audit(AuditEvent event) noexcept {
    mtk::core::audit::Event e;
    e.event_id = event.event_id.empty()
        ? mtk::core::audit::make_event_id()
        : std::move(event.event_id);
    e.argv             = std::move(event.argv);
    e.filter_name      = std::move(event.filter_name);
    e.filter_source    = std::move(event.filter_source);
    e.exit_code        = event.exit_code;
    e.bytes_in         = event.bytes_in != 0 ? event.bytes_in : cumulative_in_bytes_;
    e.bytes_out        = event.bytes_out;
    e.elapsed_ms       = event.elapsed_ms;
    e.bytes_in_capped  = event.bytes_in_capped;
    e.timed_out        = event.timed_out;
    e.killed_by_signal = event.killed_by_signal;
    (void)mtk::core::audit::append(e);
    return e.event_id;
}

std::size_t RunContext::cumulative_bytes_in() const noexcept {
    return cumulative_in_bytes_;
}

}  // namespace mtk::core
