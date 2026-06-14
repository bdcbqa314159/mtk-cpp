#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "core/exec.hpp"

namespace mtk::core {

// RunContext is the per-invocation execution environment a filter operates
// in. Owns spawn, tee, audit, settings, and sinks (per A1). Filters never
// reach into std::cout, std::getenv, or reproc::process directly — they go
// through this facade.
//
// Phase 1.0 ships the API surface; audit is a no-op stub that Phase 3 fills
// in (per the GUIDELINES Phase 1.0 commitment).
class RunContext {
public:
    RunContext() = default;
    ~RunContext() = default;

    RunContext(const RunContext&) = delete;
    RunContext& operator=(const RunContext&) = delete;

    // --- spawn ---

    [[nodiscard]] mtk::core::exec::ExecOutcome
    capture(const std::vector<std::string>& argv,
            const mtk::core::exec::EnvExtra& env_extra = {}) noexcept;

    [[nodiscard]] int passthrough(const std::vector<std::string>& argv) noexcept;

    // --- outcome predicates (sole sanctioned access to variant outside core/exec.cpp) ---

    [[nodiscard]] bool is_spawn_failed(const mtk::core::exec::ExecOutcome& outcome) noexcept;
    [[nodiscard]] bool is_ran(const mtk::core::exec::ExecOutcome& outcome) noexcept;

    // Returns nullptr if the outcome is SpawnFailed. Caller may inspect the
    // Ran fields but must not mutate them via this view (use std::move on
    // the outcome via emit() if you need to consume).
    [[nodiscard]] const mtk::core::exec::Ran*
    as_ran(const mtk::core::exec::ExecOutcome& outcome) noexcept;

    // --- emit / error / tee ---

    // Consumes the outcome. Writes Ran.stdout_data to stdout. Writes
    // Ran.stderr_data to stderr. For SpawnFailed, writes the diagnostic
    // and returns the canonical 127 exit code via report_spawn_failure().
    // Returns the exit code the caller should propagate (per A4).
    [[nodiscard]] int emit(mtk::core::exec::ExecOutcome&& outcome,
                           std::string_view tool) noexcept;

    // If outcome is Ran with non-zero exit, writes raw stdout to a tee
    // file and emits a "[mtk: full output saved to <path>]" hint to
    // stderr. No-op for clean runs or spawn failures (per A4).
    void tee_on_failure(const mtk::core::exec::ExecOutcome& outcome,
                        std::string_view slug) noexcept;

    // Canonical spawn-failure reporter: extracts the message from
    // SpawnFailed (if present), emits the standard "mtk <tool>: failed to
    // spawn: <message>" diagnostic, returns exit_codes::kNotFound (127).
    // Returns -1 if the outcome is Ran (caller misuse — but we don't
    // throw; A11 says RunContext helpers are noexcept).
    [[nodiscard]] int report_spawn_failure(const mtk::core::exec::ExecOutcome& outcome,
                                           std::string_view tool) noexcept;

    // --- audit (stub for Phase 1.0; implementation lands in Phase 3) ---

    struct AuditEvent {
        std::string filter_name;
        std::string filter_source;
        std::vector<std::string> argv;
        int exit_code = 0;
        std::size_t bytes_in = 0;
        std::size_t bytes_out = 0;
        long elapsed_ms = 0;
        bool bytes_in_capped = false;
        bool timed_out = false;
    };

    void audit(const AuditEvent& event) noexcept;
};

}  // namespace mtk::core
