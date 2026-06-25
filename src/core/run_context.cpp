#include "core/run_context.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/audit.hpp"
#include "core/diagnostic.hpp"
#include "core/exit_codes.hpp"
#include "core/filter.hpp"
#include "core/tee.hpp"
#include "core/utils.hpp"

namespace mtk::core {

namespace ex = mtk::core::exec;

ex::ExecOutcome RunContext::capture(const std::vector<std::string>& argv,
                                    const ex::EnvExtra& env_extra) noexcept {
    try {
        auto outcome = ex::capture_outcome(argv, env_extra);
        if (auto* ran = std::get_if<ex::Ran>(&outcome)) {
            // bytes_in counts the RAW child output (pre-strip) so audit
            // savings % reflects "what mtk reduced", which includes
            // ANSI-stripping for color-emitting children.
            cumulative_in_bytes_ += ran->stdout_data.size() + ran->stderr_data.size();
            // Per C11: set sticky truncation flag so audit() picks it up
            // even when the caller discards this intermediate Ran.
            if (ran->truncated) any_truncated_ = true;

            // Strip child ANSI escapes from captured output before
            // filters see it. Two reasons:
            //   1. Builtin compactors parse text by structure (line
            //      prefixes, regex). Embedded escape codes break those
            //      parsers when a child force-enables color (git
            //      diff --color=always, rg, eza, etc.).
            //   2. Agents (the default mtk consumer) get color-free
            //      output regardless of what the child decided. mtk
            //      adds its OWN color in the emit path when stdout is
            //      a TTY — separate from passthrough of child colors.
            // strip_ansi has a fast path (no ESC byte → no-op copy),
            // so ANSI-free output costs essentially nothing.
            ran->stdout_data = mtk::core::utils::strip_ansi(ran->stdout_data);
            ran->stderr_data = mtk::core::utils::strip_ansi(ran->stderr_data);
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

    // Per perf critic P10: write directly to stdout/stderr via fwrite
    // rather than `std::cout <<`. iostream formatting + sentry construction
    // + sync_with_stdio glue adds up on multi-KB blobs; fwrite is a single
    // libc call per buffer. Exceptions can't escape from these calls so
    // the try is gone.
    if (!stdout_data.empty()) {
        std::fwrite(stdout_data.data(), 1, stdout_data.size(), stdout);
        if (stdout_data.back() != '\n') std::fputc('\n', stdout);
    }
    if (!stderr_data.empty()) {
        std::fwrite(stderr_data.data(), 1, stderr_data.size(), stderr);
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
    // Single source of truth: audit() owns id generation. Previously
    // run_and_audit pre-generated one and threaded it through an
    // event_id field; the dual path masked a real footgun (audit's
    // payload and audit's JSONL line under different ids if any caller
    // ever forgot to pre-populate).
    mtk::core::audit::Event e;
    e.event_id         = mtk::core::audit::make_event_id();
    e.argv             = std::move(event.argv);
    e.filter_name      = std::move(event.filter_name);
    e.filter_source    = std::move(event.filter_source);
    e.exit_code        = event.exit_code;
    e.bytes_in         = event.bytes_in != 0 ? event.bytes_in : cumulative_in_bytes_;
    e.bytes_out        = event.bytes_out;
    e.elapsed_ms       = event.elapsed_ms;
    e.bytes_in_capped  = event.bytes_in_capped || any_truncated_;
    e.timed_out        = event.timed_out;
    e.killed_by_signal = event.killed_by_signal;
    (void)mtk::core::audit::append(e);
    return e.event_id;
}

std::size_t RunContext::cumulative_bytes_in() const noexcept {
    return cumulative_in_bytes_;
}

bool RunContext::any_capture_truncated() const noexcept {
    return any_truncated_;
}

int RunContext::run_and_audit(Filter& filter,
                              DispatchTokenPtr token,
                              const std::vector<std::string>& argv,
                              std::string_view filter_name,
                              std::string_view filter_source) noexcept {
    auto start = std::chrono::steady_clock::now();

    ex::ExecOutcome outcome;
    try {
        outcome = filter.run(std::move(token), argv, *this);
    } catch (const std::exception& e) {
        outcome = ex::SpawnFailed{e.what()};
    } catch (...) {
        outcome = ex::SpawnFailed{"unknown exception from filter run"};
    }

    // Snapshot the outcome fields the audit event needs BEFORE emit
    // consumes the outcome. The stdout copy is only taken when
    // MTK_AUDIT_PAYLOAD=1 (default off) — common path skips it.
    std::size_t bytes_out = 0;
    bool timed_out = false;
    int killed_by_signal = 0;
    bool bytes_in_capped = false;
    std::string payload_snapshot;
    const char* payload_env = std::getenv("MTK_AUDIT_PAYLOAD");
    const bool want_payload = payload_env && std::string_view(payload_env) == "1";
    if (auto* ran = std::get_if<ex::Ran>(&outcome)) {
        bytes_out = ran->stdout_data.size() + ran->stderr_data.size();
        timed_out = ran->timed_out;
        killed_by_signal = ran->killed_by_signal;
        bytes_in_capped = ran->truncated;
        if (want_payload) payload_snapshot = ran->stdout_data;
    }

    int exit_code = emit(std::move(outcome), filter_name);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto event_id = audit(AuditEvent{
        std::string(filter_name),
        std::string(filter_source),
        argv,
        exit_code,
        /*bytes_in*/ 0,  // 0 = take from cumulative_bytes_in()
        bytes_out,
        static_cast<long>(elapsed),
        bytes_in_capped,
        timed_out,
        killed_by_signal,
    });
    if (want_payload) {
        (void)mtk::core::audit::capture_payload(event_id, payload_snapshot);
    }

    return exit_code;
}

}  // namespace mtk::core
