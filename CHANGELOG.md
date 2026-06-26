# Changelog

All notable changes to mtk++ are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.2] — 2026-06-26

### Changed

- `mtk stats`: per-filter rows now sort deterministically (by count, then
  filter name) — previously tie order followed unspecified map iteration order.

### Internal

- Extracted the `mtk stats` aggregation into a pure, unit-tested `core/stats`
  module (`summarize`, `savings_pct`, `fmt_bytes`). First audit-backed tests
  (+8 cases / +33 assertions; suite now 66 / 298). No user-facing output change
  beyond the deterministic sort above.

## [0.1.1] — 2026-06-25

### Fixed

- Include hygiene (IWYU): add directly-used standard headers (e.g.
  `<atomic>` in `audit.cpp`) so libstdc++/Linux builds compile clean.
  No API or behavior change; macOS/AppleClang and Windows/MSVC unaffected.

## [0.1.0] — 2026-06-16

First public release.

### Added

#### Core

- Single-binary CLI proxy: `mtk <command> [args...]` dispatches through a
  filter registry and compresses the wrapped command's output before
  emitting it. One subprocess + one audit row + one cache touch per
  invocation; no daemon, no background work.
- `Filter` abstract base — the only command abstraction. Builtins, TOML
  filters, and the fallback all implement it. `try_match(argv)` →
  `optional<DispatchTokenPtr>`; `run(token, argv, ctx)` → `ExecOutcome`.
- 5-tier `Registry` with priority resolution (OrgToml > Builtin >
  UserToml > ProjectToml > Fallback). Same-name shadow attempts at
  lower-priority tiers are rejected at registration time when the
  higher-priority registrant is marked `is_final`.
- `ExecOutcome = variant<SpawnFailed, Ran>` as the single result type
  (A3). Sanctioned access only via `RunContext::is_spawn_failed` /
  `is_ran` / `as_ran` / `emit` — CI gate (CE5) forbids direct variant
  decomposition outside `core/exec.cpp` and `core/run_context.cpp`.
- `RunContext` facade owns spawn, emit, tee-on-failure, audit, and the
  end-to-end `run_and_audit` choreography. Filters never reach into
  `cout`, `getenv`, or reproc directly.
- Bounded capture (A6): 50 MiB stdout+stderr cap, 30 s wall-clock
  deadline, surfaced as `Ran::truncated` / `Ran::timed_out`.
- Signal handling (A11): SIGINT / SIGTERM / SIGHUP into a lock-free
  bitmask; SIGPIPE exits 141; no `SA_RESTART` so reproc's blocking
  reads return EINTR promptly.

#### Builtin filters

- `git log` — commit-list compaction.
- `git status` — porcelain + state-header (rebase / detached HEAD /
  cherry-pick) compaction.
- `git diff` — diffstat + Method-Object hunk compactor; single-spawn
  (`--stat --patch` in one shot).
- `git show` — multi-commit-aware (raw passthrough when more than one
  commit was requested); single-spawn.
- `ls` — bucketed dirs/files with size + extension summary; suppresses
  noise dirs (`node_modules`, `.git`, etc.) unless `--all`.
- `grep` / `rg` — NUL-separated parsing (`-Z` / `--null`); per-file
  aggregation with path compaction and per-file caps.

#### TOML filter system

- Declarative filters via `match_command` regex with `strip_ansi`,
  `strip_lines_matching`, `keep_lines_matching`, `replace`,
  `truncate_lines_at`, `head_lines`, `tail_lines`, `max_lines`,
  `match_output`, `on_empty`.
- Three load locations: `~/.config/mtk/filters/`, `.mtk/filters/`,
  `/etc/mtk/filters/` (overridable via `MTK_ORG_CONFIG`).
- Org filters with `locked = true` block same-name registrations at
  lower tiers (A7).
- Binary filter cache at `~/.cache/mtk/filters.bin` (`kVersion = 2`),
  keyed off a manifest of `(path, mtime, size)` per source TOML.
  Atomic write via temp + `rename(2)`. Org filters bypass the cache so
  policy updates take effect immediately.
- `mtk reload` invalidates and rebuilds the cache.

#### Trust (A2)

- Project filters (`.mtk/filters/*.toml`) gated by an allow-list at
  `~/.config/mtk/allowed_projects`. Default-off — prevents silent-shadow
  attacks via cloned-repo TOMLs.
- Managed via `mtk trust [path]` / `mtk untrust [path]` / `mtk trusted`.
- Override with `MTK_ALLOW_PROJECT_FILTERS=1`.
- Atomic file writes via per-PID temp + `rename(2)`.

#### Audit + introspection (Phase 3)

- JSONL audit log at `~/.local/state/mtk/events.jsonl` (rotation at
  10 MiB, single rotated file kept).
- Hand-rolled JSON serialise (avoids the ~30% per-invocation cost of
  `nlohmann::json` on the hot path).
- Cross-process safe rotation via `flock(LOCK_EX)` + inode-compare.
- `mtk stats` — per-filter savings dashboard (count, bytes_in→out,
  savings %, errors, avg latency).
- `mtk tail [N]` — last N audit events.
- `mtk why <event-id>` — replays the raw stdout of an event, if
  `MTK_AUDIT_PAYLOAD=1` was set when it ran.

#### Agent integration

- `mtk init claude` — installs a Bash PreToolUse hook at
  `~/.claude/hooks/mtk-rewrite.sh`; uses `jq` to rewrite Claude Code's
  `tool_input.command` field. Prints the `~/.claude/settings.json`
  snippet to paste.
- `mtk init copilot` — installs a dual-schema JSON hook
  (`PreToolUse` for VS Code Copilot Chat + `preToolUse` for Copilot CLI)
  at `.github/hooks/mtk-rewrite.json`; upserts an `mtk-instructions`
  block into `.github/copilot-instructions.md`.
- Both bake the absolute path of the mtk binary into the generated hook
  so PATH setup is not required (build-only users supported).
- `mtk rewrite <cmd>` — companion command used by the hook scripts;
  emits `mtk <cmd>` if a non-passthrough filter would match, else
  echoes unchanged.
- `mtk hook copilot` — JSON shape-detecting rewriter for both VS Code
  Copilot Chat (snake_case) and Copilot CLI (camelCase) input.
- TODO stubs in `mtk init` help text for `gemini` / `codex` / `cursor`
  (blocked on each tool's hook spec).

#### Build + CI

- CMake 3.20+, C++17, single static binary. All third-party deps
  (CLI11, toml++, reproc, fmt, doctest, nlohmann/json) are git
  submodules under `third_party/` and built from source — no package
  manager required.
- `cmake --install` target; `release` / `release-static` /
  `RelWithDebInfo` / `debug` presets in `CMakePresets.json`.
- Git SHA + build date injected into `--version` via
  `cmake/version.hpp.in`.
- CI gates in `scripts/ci-check.sh`:
  - **A5** — no networking symbols in the built binary
    (`socket|connect|bind|getaddrinfo|SSL_|tls_|curl_`).
  - **CE4** — forbidden-include grep.
  - **CE5** — `ExecOutcome` variant decomposition only in
    `core/exec.cpp` + `core/run_context.cpp`.
  - **CE6** — `[[nodiscard]]` on value-returning public-header
    functions.
- 58 doctest cases / 265 assertions; integration tests via `ctest`.

### Design

See [GUIDELINES.md](GUIDELINES.md) for the full architectural contract
(A1-A12 invariants + CE1-CR15 idioms) and [README.md](README.md) for
the user-facing overview.

[Unreleased]: https://github.com/bdcbqa314159/mtk-cpp/compare/v0.1.2...HEAD
[0.1.2]: https://github.com/bdcbqa314159/mtk-cpp/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/bdcbqa314159/mtk-cpp/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/bdcbqa314159/mtk-cpp/releases/tag/v0.1.0
