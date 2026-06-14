# mtk++ — Design & Code Guidelines (v2)

**This document is the contract.** Code that violates it doesn't merge.
Updates to Parts I–II happen via PR with rationale and a one-line entry
in the change log; the rules are not advisory.

These guidelines are the synthesis of two design reviews + two adversarial
stress-tests conducted on 2026-06-14, plus lessons from the day-1
implementation. Parts I and II describe the system as it must *become* —
the current code violates many rules and Phase 0–4 in Part III is the
work that makes the rules true.

---

## Part I — Architectural Invariants (non-negotiable)

### A1. The `Filter` abstraction is the only command abstraction

Every command — dedicated C++ filter, TOML filter, or passthrough — is a
concrete implementation of one interface:

```cpp
// Polymorphic base. Each Filter subclass defines its own derived token
// type and static_casts inside run(). Pairing is structural — the filter
// that produced the token is the filter that consumes it. For filters
// needing no match state, return DispatchTokenPtr{} (null) and ignore
// the parameter in run().
struct DispatchToken {
    virtual ~DispatchToken() = default;
};
using DispatchTokenPtr = std::unique_ptr<DispatchToken>;

class Filter {
public:
    virtual ~Filter() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view source() const noexcept = 0;  // "builtin", path to .toml, etc.

    // Cheap. Must not throw. All expensive setup (regex compile, glob
    // compile, TOML schema validation) MUST happen at registry
    // construction, not here. Derived DispatchToken types may NOT store
    // views/spans into try_match's arguments — copy what they need
    // (those views' lifetimes end when the dispatch loop moves on).
    [[nodiscard]] virtual std::optional<DispatchTokenPtr>
    try_match(std::string_view subcommand,
              std::span<const std::string> args) const noexcept = 0;

    // May throw. A4's RunContext wrapper catches and falls back to raw.
    [[nodiscard]] virtual ExecOutcome
    run(DispatchTokenPtr token,
        std::span<const std::string> args,
        RunContext& ctx) = 0;
};
```

- **Why `try_match` returns `optional<DispatchTokenPtr>`, not `bool`**:
  a filter that matched with capture groups or parsed flags can carry
  that state forward to `run` without re-parsing.
- **Why setup is forbidden in `try_match`**: dispatch cost scales with
  `O(N_filters × per_call_setup)`. Compile regexes and validate TOML at
  registry-construction; `try_match` does the cheap match only.
- **Registry construction is failable**. Bad TOML at startup is a loud
  error with file:line:col, not a silent disable.
- **Concrete `Filter` subclasses are marked `final`** to enable
  devirtualization when the static type is known (tests, etc.).
- **`RunContext` owns** spawn primitives, tee policy, stderr handling,
  stdout sinks, audit emission, settings (limits, env). Filters never
  reach into `std::cout`, `std::getenv`, or `reproc::process` directly.
  - **For test isolation**: `RunContext` exposes its constituents as
    `const`-ref accessors (`settings() noexcept`, `auditor() noexcept`).
    Production filter code uses the unified helper methods
    (`ctx.capture()`, `ctx.emit()`, `ctx.audit()`); tests can substitute
    individual collaborators via the accessors.
  - **Helpers visit `ExecOutcome` at most once**: a `RunContext` helper
    does not call another `RunContext` helper that re-decomposes the
    same outcome. Chained visits pay the sum-type tax twice.
- **Name uniqueness within priority tier**: registry rejects (logs to
  stderr, returns false) a filter whose `name()` already exists in the
  same tier. Cross-tier collision is allowed *and is how override
  works*; `mtk explain` lists shadowed filters with a `[SHADOWED]` marker.
- **`source()` is required** so `mtk explain` can show provenance —
  "matched filter `git_log` (source: builtin), shadowing `git_log`
  (source: ~/.config/mtk/filters/git.toml [DISALLOWED])."

**Enforcement**: any new command added under `cmds/*` is a `Filter`
subclass. No free `int run(args)` functions in command files. Any direct
use of `reproc::process` outside `core/exec.cpp` is a review block.

### A2. Single dispatch registry with explicit priority — built-ins are protected

There is one dispatcher. Hardcoded `if (sub == "git")` chains anywhere
are forbidden.

**Priority order (highest first):**
1. `/etc/mtk/filters/*.toml` — org filters (root-owned, sysadmin tier).
2. **Built-in C++ filters** (`git log`, `git status`, etc.) — registered
   at startup as `final = true`. Project filters cannot override these
   by name; user filters can.
3. `~/.config/mtk/filters/*.toml` — user filters.
4. `.mtk/filters/*.toml` — **project filters, opt-in only**. Loaded only
   if the project path is in `~/.config/mtk/allowed_projects` *or*
   `MTK_ALLOW_PROJECT_FILTERS=1` is set. Silently ignored otherwise,
   with one-time stderr nag per cwd on first skip ("project filters at
   `.mtk/filters/` were not loaded; allow with `mtk trust .`").
5. Passthrough (always last, always matches).

**Project filters may never shadow built-ins by name**: registry rejects
a project-tier filter whose `name()` matches a built-in. This prevents
the security class where a cloned repo ships `.mtk/filters/git.toml`
and silently rewrites `mtk git status` to do something malicious.

Project filters *can* add new filters (commands the built-ins don't
cover) or extend the registry, just not replace.

**Why**: a project-relative directory is untrusted input. Direnv solved
this with explicit allow-listing; we adopt the same model.

**Commands**: `mtk trust [path]` adds path to `allowed_projects`;
`mtk untrust [path]` removes; `mtk explain` shows which tier matched.

### A3. `ExecOutcome` is a two-arm sum type

Subprocess outcomes have two structurally distinct shapes:

```cpp
struct SpawnFailed {
    std::string message;
};

struct Ran {
    std::string stdout_data;
    std::string stderr_data;
    int exit_code;  // 0 = clean, non-zero = child failed
    [[nodiscard]] bool clean() const noexcept { return exit_code == 0; }
};

using ExecOutcome = [[nodiscard]] std::variant<SpawnFailed, Ran>;
```

- **Why two arms, not three**: the structural distinction is
  "spawn-failed vs ran." Whether the ran child exited zero is a runtime
  predicate (`r.clean()`), not a type-level distinction — three arms
  duplicates the stdout/stderr payload across `RanClean`/`RanWithFailure`
  for no structural gain. This shape is `tl::expected`-shape without the
  dependency.
- **`ExecOutcome` is `[[nodiscard]]`**: ignoring a captured outcome is
  always a bug (the entire point of the sum type is exhaustive handling).
- **`emit(ExecOutcome&&)` consumes**: `RunContext::emit` takes the
  outcome by rvalue reference and moves the strings into the sink. A
  40 MiB capture failure must not pay for a 40 MiB copy. Callers cannot
  reuse the outcome after `emit`.
- **No `std::get`/`std::get_if`/`std::visit`/`index()` outside
  `core/exec.cpp`**: call sites use the helpers in `RunContext`
  (`ctx.emit(outcome)`, `ctx.tee_on_failure(outcome, slug)`,
  `ctx.report_spawn_failure(outcome, tool)`). Forbidden patterns are a
  CI-enforced block.

**Enforcement**: CI grep (CE5) flags
`std::get`/`std::get_if`/`std::visit`/`\.index\(\)` on `ExecOutcome`
outside `src/core/exec.cpp`. Any direct variant access in command code
is a build break.

### A4. The fallback pattern is non-negotiable

Every filter degrades to raw output on failure:

1. **Spawn failure** → emit canonical `mtk <tool>: <message>` diagnostic
   to stderr, return exit code 127 via `report_spawn_failure`.
2. **Ran with non-zero exit** → tee the raw output (subject to A6
   bounds), emit raw stdout, propagate the exit code, append
   `[mtk: full output saved to <path>]` to stderr.
3. **Filter threw or returned malformed output** → emit raw output
   unchanged, emit `mtk: filter warning [<name>]: <what>` to stderr,
   propagate the underlying exit code.
4. **Capture exceeded `max_capture_bytes`** → see A6 — switch to
   passthrough for the rest of the invocation; emit a one-line warning.

**Enforcement**: every `Filter::run` goes through `RunContext` helpers
that own these rules. No per-filter reimplementation. Per-filter try/catch
is permitted around the filter logic itself; everything else is in
`RunContext`.

### A5. No telemetry, ever. Local audit IS allowed.

"No telemetry" means **no network egress, ever**. Enforced *structurally*:

- The `mtk` binary does not link against any networking library. CI
  step: `nm build/mtk | grep -E '\b(socket|connect|bind|getaddrinfo|SSL_|tls_|curl_|reqwest_)' && exit 1`. Build fails if any networking symbol is referenced.
- `#include <sys/socket.h>`, `<netinet/in.h>`, `<sys/un.h>`,
  `<arpa/inet.h>`, and `<curl/*>` are forbidden in `src/`. CI lint
  checks this.
- **No user-invoked upload commands**, regardless of intent. `mtk share`,
  `mtk report-bug`, `mtk export-to-server` and equivalents are
  explicitly out of scope for this binary. If a user wants to share
  audit data, they `cat ~/.local/state/mtk/events.jsonl` themselves and
  use their own tools.

"No telemetry" does **not** mean "no observability." A local-only audit
log is required (see Phase 3):

- **Location**: `~/.local/state/mtk/events.jsonl` (Unix; XDG-compliant).
- **Schema (committed in Phase 1; do not change without major version)**:
  ```json
  {"ts": "2026-06-14T09:01:23Z", "argv": ["git","log","-5"],
   "filter_name": "git_log", "filter_source": "builtin",
   "exit_code": 0, "bytes_in": 4823, "bytes_out": 612,
   "elapsed_ms": 12, "bytes_in_capped": false, "timed_out": false}
  ```
- **Default**: metadata only, no payloads. Payload capture is opt-in via
  `MTK_AUDIT_PAYLOAD=1` (per-invocation) or `audit.capture_payload = true`
  in settings.
- **Audit log rotation**: size-cap (default 10 MiB; setting:
  `audit.max_log_bytes`). FIFO drop of oldest entries on overflow.
- **Audit log writes are O_APPEND + single `write(2)` per event** —
  atomic on POSIX up to PIPE_BUF (4 KiB), which is comfortably larger
  than our metadata schema. No `fsync` in the hot path; `fsync` on
  rotation only.
- **Commands**: `mtk stats` (aggregations), `mtk tail [N]` (recent
  events), `mtk why <event-id>` (re-spool raw output if payload capture
  was on for that event).

**The distinction to internalize**: exfiltration is forbidden;
introspection is required. The audit file never leaves the user's
machine via mtk; what the user does with it is the user's choice.

### A6. All I/O is bounded — and the budget rules are explicit

Three interacting budgets. Each is independent of the others; cross-budget
interactions are spelled out below.

| Budget | Setting | Default | What happens at limit |
|---|---|---|---|
| Per-capture buffer | `capture.max_bytes` | 50 MiB | Stop capture mid-stream; **switch to passthrough from cap point** with one-line stderr warning `[mtk: capture exceeded N MiB at byte X, falling through to passthrough]`. Buffered prefix is **discarded** (printed-as-passthrough mode does not replay history). Audit event records `bytes_in_capped = true, bytes_in_capped_at = X`. |
| Per-capture wall-clock | `capture.timeout_ms` | 30000 | SIGTERM child → `signal_grace_ms` wait → SIGKILL. Whatever was captured so far is emitted as if exit succeeded; audit records `timed_out = true`. |
| Per-invocation tee file | `tee.max_bytes` | = `capture.max_bytes` | Tee is bounded by the same limit as capture — a 40 MiB failed capture produces a 40 MiB tee. |
| Tee directory total | `tee.dir_max_bytes` | 100 MiB | FIFO drop of oldest tee files, **except** tee files referenced by audit events emitted in the last `tee.retention_invocations` events (default 50). This prevents the lying-hint failure mode where `[mtk: full output saved to <path>]` is printed and the file is then deleted by rotation. |
| Audit log total | `audit.max_log_bytes` | 10 MiB | FIFO drop of oldest events. Audit retention is independent of tee retention; an event can survive after its tee file was evicted (the `mtk why <event-id>` command shows "[tee no longer available]"). |
| Audit payload per event | `audit.payload_max_bytes` | 1 MiB | Truncate; record `payload_truncated = true`. |

**Passthrough mode is exempt** from all of the above — we're not
capturing, we're not budgeting.

**Enforcement**: all caps live in `core/limits.hpp` constants and are
read via `Settings`. Hardcoded literals at call sites are review blocks.

### A7. Layered config (org policy is a first-class concept)

```
/etc/mtk/mtk.toml          (org, read-only, sets baseline + locks)
↓ overridden by
~/.config/mtk/mtk.toml     (user)
↓ overridden by
.mtk/mtk.toml              (project, cwd-relative — opt-in like project filters; see A2)
↓ overridden by
MTK_* environment variables
```

The org layer can mark settings `locked = true` to prevent lower layers
from overriding. This is what makes "the org rolls out mtk with
audit-always-on and payload-capture-off" possible.

**Loaded once** at startup into a single `Settings` struct, threaded
through `RunContext`. No `std::getenv` calls outside the loader.

`Settings` is failable on parse error (loud; doesn't silently fall back).

### A8. Color suppression at the source, not after

Children launched under capture inherit:
- `NO_COLOR=1`, `CLICOLOR=0`, `TERM=dumb` in the env.
- Tool-specific flags injected when known: `git` → `-c color.ui=never
  -c color.diff=never`; `rg`/`grep` → `--color=never`; `cargo` →
  `--color=never`; `npm`/`pnpm`/`yarn` → `--color=false` if supported.

**Why**: stripping ANSI after the fact is brittle (escape sequences can
span buffer boundaries) and wastes work. Preventing color at source is
correct and free.

Passthrough mode is exempt — the user asked for raw behavior.

### A9. Cross-platform from day one

Unix-isms behind `#ifdef _WIN32` guards live in `core/platform.{hpp,cpp}`.
No `getenv("HOME")` or hardcoded `~/.config` outside that file.

Applies even when not actively building Windows binaries — we keep the
door open, not block it with technical debt.

### A10. The fallback pattern beats the perfect pattern

When in doubt, prefer:
- **Passthrough over partial filtering** if the parser doesn't fully
  understand the input.
- **Raw output + tee hint over compressed-but-misleading output**.
- **Inherited stdio over capture** when the filter has no clear value.

A user who sees full git output is mildly annoyed. A user who sees
silently-broken git output stops trusting mtk forever.

### A11. Subprocess lifecycle and signals

- `mtk` forwards `SIGINT`, `SIGTERM`, `SIGHUP` to the active child;
  waits up to `signal.grace_ms` (default 2000); then SIGKILL.
- Exit code on signal-killed child: `128 + signum` (consistent with C7).
- **SIGPIPE on stdout**: `mtk` exits 141 (`128 + 13`) and does **not**
  tee/audit (user explicitly stopped reading; tee would be lying about
  "full output saved"). The audit log records `sigpiped = true` but no
  payload regardless of `audit.capture_payload`.
- **`RunContext` is not thread-safe; filters are single-threaded by
  default.** Multi-spawn (`run_status`, `run_show`, `run_diff`) is
  sequential. Parallel multi-spawn is opt-in via
  `ctx.spawn_parallel({...})` returning `std::vector<ExecOutcome>`,
  added only when a concrete filter demonstrates need.
- **Audit writes during shutdown** are atomic (single `write(2)` per
  event; `O_APPEND`). Events in flight when SIGKILL hits the parent
  are lost — this is fine; we're metadata, not a database.

### A12. Streaming is opt-in, never the default

Today (and throughout Phase 1–4) capture is buffered: full subprocess
output into `std::string`. Streaming (emit-as-we-go) is an *optimization*
to be added under measurement, never speculation.

Any streaming path:
- Must preserve A4's fallback semantics (filter failure → raw).
- Must respect A6's `capture.max_bytes` bound (streamed output is also
  capped; bytes past the limit get passthrough'd directly to stdout).
- Must not fork the `Filter` abstraction. A streaming filter is a
  `Filter` whose `run` consumes a `RunContext::stream_capture(...)`
  call instead of `ctx.capture(...)`. Same interface, different
  internals.

**Why this rule exists**: without naming it, a future contributor will
ship "Streaming Filter v2" as a separate abstraction and we'll have two
parallel codebases. Forbid that explicitly.

---

## Part II — Code Idioms

Split into **enforced** (CI gate fails the build) and **reviewed**
(human judgment; reviewer must explicitly call out).

### Enforced rules (CI gate)

#### CE1. String parameter conventions (clang-tidy lint)

| Usage | Type |
|---|---|
| Read-only input | `std::string_view` |
| Will be stored as a `std::string` | `std::string` (take by value) |
| Will be passed to `std::regex_*` | `const std::string&` (unavoidable) |
| Modified in place | `std::string&` |

**Returns**: prefer `std::vector<std::string_view>` when the caller
owns the source buffer for the slices' lifetime (document the contract
in the header). Otherwise `std::vector<std::string>`.

If a function takes `const std::string&` purely for regex, document:
`// const std::string& required by std::regex` next to the param.

#### CE2. Function size — hard cap 80 LOC (clang-tidy `readability-function-size`)

`LineThreshold=80, BranchThreshold=8, ParameterThreshold=6`. Fails the
build at 80 LOC. The soft threshold of 50 LOC is a *reviewer prompt*,
not a CI rule — see CR2 below.

**Multi-spawn orchestrators are exempt** from the soft cap but not the
hard cap. `run_status`/`run_show`/`run_diff` each sequence 2–3 captures
and naturally land at 45–70 LOC even with `RunContext` doing the
boilerplate. They must stay under 80; if they grow past, decompose.

#### CE3. Include order (clang-format `IncludeBlocks: Regroup`)

Four blocks, blank line between, alphabetized within each:
```cpp
#include "cmds/git.hpp"       // own header for this TU (must be first)
                              //
#include <algorithm>          // std
#include <iostream>           //
                              //
#include <fmt/format.h>       // third-party
#include <reproc++/reproc.hpp>
                              //
#include "core/exec.hpp"      // own implementation includes
#include "core/utils.hpp"
```

Enforced via `.clang-format` shipped in repo + pre-commit hook that
runs `clang-format --dry-run -Werror`. Format-on-save in IDE
configurations is encouraged but not required.

#### CE4. No networking includes (CI grep)

```sh
grep -rE '<sys/socket\.h>|<netinet/|<arpa/|<sys/un\.h>|<curl/' src/ && exit 1
```

#### CE5. No direct `ExecOutcome` decomposition outside `core/exec.cpp`

```sh
grep -rE 'std::(get|get_if|visit).*ExecOutcome|\.index\(\)' src/ | \
    grep -v 'src/core/exec\.cpp' && exit 1
```

`.index()` is the back door around the `std::get`/`visit` rule and is
forbidden by the same gate.

#### CE6. `[[nodiscard]]` and `noexcept` are mandatory where applicable

**`[[nodiscard]]`** on every return whose discarding is a bug:
`ExecOutcome`, `DispatchTokenPtr`, every `optional<T>` parse/extract
result (CR14), every `RunContext` method that returns a value, every
`std::variant` alias whose discard is meaningful.

**`noexcept`** on:
- `Filter::try_match` (must not throw — throwing is a contract bug;
  expensive setup is at registry-construction).
- `Filter::name()`, `Filter::source()`.
- All `RunContext` accessors (`settings()`, `auditor()`, etc.).
- `RunContext::emit`, `RunContext::tee_on_failure`,
  `RunContext::report_spawn_failure` (these own the A4 fallback and
  must not propagate failure to the dispatcher).

**NOT `noexcept`** on:
- `Filter::run` — filter logic can legitimately throw; A4's wrapper
  catches and falls back to raw.
- `RunContext::capture` — child-spawn errors are reported via
  `ExecOutcome::SpawnFailed`, but pathological-arg errors (bad UTF-8,
  etc.) can throw.

Documented per-method in the relevant headers; reviewer enforces.

#### CE7. Tests required for new filters

Every `Filter` subclass added under `src/cmds/` requires:
- `tests/test_<name>.cpp` (greppable check in CI).
- A fixture in `tests/fixtures/` referenced by name.
- A token-savings assertion.

CI: a script checks that `find src/cmds -name '*.cpp' | wc -l` ≥
`find tests -name 'test_*.cpp' | wc -l` (minus the count of
non-filter cpps).

### Reviewed rules (judgment, reviewer must call out)

#### CR1. Function size soft cap 50 LOC

If a function exceeds 50 LOC, the reviewer must comment: "this is past
50 LOC — confirm the split is awkward or extract." Acceptable answers:
"multi-spawn orchestrator" (see CE2 exemption), "the alternative is
deeper nesting that hurts readability." Unacceptable: "it's only 60."

#### CR2. Two or more *related* booleans → struct

Mode flags that travel together (e.g., `{strip_ansi: bool, color:
bool}`) → struct. **Orthogonal** booleans stay as parameters; call
sites use **named locals** or **designated initializers** (C++20):

```cpp
// Acceptable:
const bool show_all = opts.show_all;
const bool show_long = opts.show_long;
compact_ls(raw, show_all, show_long);

// Acceptable (C++20):
compact_ls(raw, /*ignored*/, .show_all=true, .show_long=false);

// Forbidden:
compact_ls(raw, /*show_all=*/true, /*show_long=*/false);
```

The forbidden form is the smell — inline-comment positional bools
defeat code review.

#### CR3. Three is the threshold for DRY

Two similar lines are coincidence. Three are a pattern — extract.
Don't extract prematurely on the second instance.

#### CR4. Magic numbers and policy strings live in `core/limits.hpp` / `core/constants.hpp`

Numbers: path-compaction threshold (50), tee slug truncation (40),
byte boundaries (1024, 1048576), default max-counts.

Strings encoding policy contracts: the `"---END---"` sentinel for
`git log` is a contract with our own injected `--pretty=format`
string. **The format string and its sentinel are `constexpr` siblings
in the same `constants.hpp` block** so a future edit to one catches
the other.

#### CR5. Error diagnostics via `diagnostic()` helper

```cpp
namespace mtk::core::diag {
    void emit(std::string_view tool, std::string_view message);
}
```

Produces the canonical `mtk <tool>: <message>` to stderr. No more
"mtk: failed to spawn git: …" / "mtk grep: missing pattern" /
"mtk: failed to spawn grep/rg: …" variants. One shape.

#### CR6. Exit code conventions

| Code | Meaning |
|---|---|
| 0 | success |
| 2 | usage error (bad CLI args) |
| 126 | command found but not executable |
| 127 | command not found (spawn failed) |
| 128+N | killed by signal N |
| 141 | SIGPIPE specifically (`128 + 13`) — see A11 |
| 1, 3..125, 129..140, 142..255 | propagated from wrapped child |

Helper `report_spawn_failure(tool, err) -> int` enforces. No `return
127;` literals outside that helper.

#### CR7. Tests — branch coverage of caps is in-PR-only

Every cap added in a PR ships with a test that exercises the boundary
in the same PR. **Pre-existing uncovered caps are tech-debt tickets,
not merge blockers** — fix them in dedicated cleanup PRs.

`read_fixture` lives in **one** place: `tests/support/fixture.hpp`.
Test files import the support header; never copy-paste the helper.

#### CR8. Helpers live in `core/utils.hpp`

`starts_with`, `trim_copy`, `to_lower`, `ltrim`/`rtrim`, `split_lines`,
`join_lines`, `count_tokens`. Each helper has exactly one definition.

When C++20 is reachable, drop `starts_with` in favor of
`std::string::starts_with`.

#### CR9. No `try/catch` around non-throwing code

The legitimate places for try/catch are: regex (`std::regex_error`),
`std::stoull` (`std::invalid_argument` / `std::out_of_range`), TOML
parse (`toml::parse_error`), filesystem (`std::filesystem::filesystem_error`),
and the top-level filter wrapper (catch any escaping exception, log,
fall back to raw per A4).

Wrapping pure `std::string` manipulation in try/catch is noise.

#### CR10. Comments restate WHY, not WHAT — and *must* document non-obvious

**Forbidden**: comments that restate what well-named code already says.

```cpp
// Loop over the lines        ← delete this
for (const auto& line : lines) { ... }
```

**Required**: any code that exists because of a workaround, a fallback,
a unicode edge, a non-obvious invariant, or a recovered-from bug
**must** carry a WHY-comment naming the failure mode it prevents:

```cpp
// NUL-mode is preferred because filenames containing ':' (Windows drive
// letters, weird names like "badly_named:52:file.txt") defeat colon-mode
// parsing. ugrep on macOS doesn't emit NUL even with -Z; the colon
// fallback exists for that case. Both modes share parse_at(...).
auto p = parse_at(line, nul_pos);
```

Reviewers ask "why is this here?" on every unusual branch. If the
answer isn't in the source, the PR doesn't merge.

#### CR11. `auto` rule — reviewer cost beats writer convenience

- Use `auto` for genuinely opaque types: lambdas, iterators, template
  returns, unutterable types.
- Spell out types that appear in arithmetic, comparison, or stream-out:
  `std::string`, `std::size_t`, `bool`, integer types. The reviewer
  reading the diff cold needs to know the conversions involved.
- `auto&` and `const auto&` in range-for loops are **exempt** — they
  read clearly because the container's element type is right there.
- `decltype(auto)` for forwarding return shape is **required** when
  forwarding; never spell out unutterable types.

The cost of `auto` is not in the writer; it's in the diff reviewer.

#### CR12. `internal::` namespace = test-visible; anonymous namespace = file-private

Anything tested from `tests/` lives in `mtk::cmds::<name>::internal::`.
Anything file-private uses an anonymous namespace inside `internal::`
(or at file scope if not visible from the header). Don't mix freely
— if a helper might need a test in the future, put it in `internal::`
from day one.

#### CR13. Hot-path regex is function-static + has a pattern-validity test

```cpp
const std::regex& ls_date_regex() {
    static const std::regex re(R"(\s+(Jan|Feb|...)\s+\d{1,2}\s+...)");
    return re;
}
```

The function-static form compiles on first call (thread-safe under
C++11). Pattern errors crash on first use — to catch them at startup
instead, add a test that *calls* every regex helper at least once:

```cpp
TEST_CASE("all hot-path regexes compile cleanly") {
    (void)mtk::cmds::ls::internal::ls_date_regex();
    (void)mtk::cmds::grep::internal::match_line_regex();
    // ... one line per regex helper
}
```

#### CR14. Parse/extract failure → `std::optional<T>`

`parse_ls_line`, `extract_state_header`, `parse_match_line`,
`extract_detached_head` all return `std::optional<T>` for "didn't
match." Empty-but-valid results (an empty compact diff) are returned
as the empty value, not `nullopt`.

Don't use sentinel values (`-1`, empty string-meaning-failure) where
`optional` fits.

#### CR15. Format-injection strings + sentinels are co-located

Anywhere we inject a format string into a wrapped command and then
parse the output against a sentinel we chose, both live as `constexpr`
siblings in `core/constants.hpp` (or a per-tool constants block):

```cpp
namespace mtk::constants::git_log {
    constexpr const char* kPrettyFormat =
        "--pretty=format:%h %s (%ar) <%an>%n%b%n---END---";
    constexpr std::string_view kCommitSeparator = "---END---";
}
```

Editing one without the other breaks parsing silently.

---

## Part III — The Plan (phased, checkable)

Phases are sequential. Don't start phase N+1 until phase N is green.
The order maximizes leverage — earlier phases make later phases cheaper.

### Phase 0 — Latent correctness bugs + zero-risk extractions (today, ~2 hours)

Bugs from the day-1 review:
- [ ] **Fix `was_truncated` over-reporting in `compact_diff`**
  (`src/cmds/git.cpp:285–349`). Set the flag only inside the actual
  truncation branch, not in the lambda's unconditional post-loop flush.
- [ ] **Fix unchecked `plain.spawned` in `run_status`**
  (`src/cmds/git.cpp:570`). Guard `extract_state_header` and
  `extract_detached_head` behind a `plain.exit_code == 0` check.

Zero-risk helper extractions (used by Phase 1 immediately):
- [ ] **`core/diagnostic.{hpp,cpp}`** — `diag::emit(tool, message)`
  helper per CR5. Migrate all "mtk: failed to spawn …" sites.
- [ ] **`core/exit_codes.{hpp,cpp}`** — `report_spawn_failure(tool, err)`
  helper per CR6. Migrate all `return 127;` sites.
- [ ] **`.clang-format` + `.clang-tidy`** in repo root + a `scripts/
  precommit.sh` hook.

### Phase 1.0 — Spawn primitives + audit interface (1–2 days)

Migrates the spawn return type and *commits the audit interface shape*
so Phase 3 doesn't re-touch every filter.

- [ ] **A3: `ExecOutcome` three-arm variant** in `core/exec.{hpp,cpp}`.
  Migrate every consumer. Delete `CapturedOutput`.
- [ ] **`RunContext` skeleton** in `core/run_context.{hpp,cpp}` with
  the committed API surface: `capture()`, `passthrough()`, `emit()`,
  `tee_on_failure()`, `report_spawn_failure()`, **`audit({...})`** (stub
  that no-ops in Phase 1, fills in Phase 3 — schema per A5 is committed
  here).
- [ ] **A11: signal handling** in `core/exec.cpp` — SIGINT/TERM/HUP
  forwarding, signal grace period, SIGPIPE → exit 141.
- [ ] **A6: `capture.max_bytes` enforcement** + **`capture.timeout_ms`**
  in `core/exec.cpp`. Settings live in `core/limits.hpp`.
- [ ] **`core/limits.hpp`** with every numeric cap (existing + new).
- [ ] **`core/utils.hpp`** consolidation — kill the four `starts_with`,
  three `trim_copy`, two `to_lower` reimplementations.

### Phase 1.5 — `Filter` interface + dispatch registry (2–3 days)

Now that `RunContext` exists, migrate command code onto it.

- [ ] **A1: `Filter` interface + `DispatchHandle`** in
  `core/filter.{hpp,cpp}`.
- [ ] **A2: dispatch registry** in `core/registry.{hpp,cpp}` with the
  priority order from A2 (including the `final = true` bit for built-ins
  and the `allowed_projects` allow-list).
- [ ] **`mtk trust` / `mtk untrust`** commands.
- [ ] **`MTK_DEBUG=1`** env var.
- [ ] **`mtk explain <argv...>`** command — dry-run dispatch, emit
  "matched filter X (source: Y, tier: Z) → would call ctx.capture(argv).
  bytes_in_estimate: N." Lists shadowed filters with `[SHADOWED]` marker.
- [ ] Migrate `cmds/exec.cpp` (simplest) → `cmds/ls.cpp` → `cmds/grep.cpp`
  → `cmds/git.cpp` (split each `run_*` into its own `Filter` subclass;
  `internal::filter_*` pure helpers stay untouched).

### Phase 2 — Deployment unlock + Phase 4 config plumbing (2 days)

This is the actual rollout blocker. Wire the *paths* now so Phase 4
just populates the data layer.

- [ ] **CMake `install` target**: `install(TARGETS mtk RUNTIME
  DESTINATION bin)`, `install(DIRECTORY filters/ DESTINATION
  share/mtk/filters)`, `install(FILES INSTALL.md GUIDELINES.md
  DESTINATION share/doc/mtk)`.
- [ ] **`release-static` CMake preset** with `-static-libstdc++
  -static-libgcc`.
- [ ] **`CMakePresets.json`** with `release`, `release-static`,
  `release-debug-symbols`, `release-musl` (Linux).
- [ ] **Git-SHA into `--version`** via `configure_file` →
  `version.hpp` with `MTK_VERSION = "0.1.0 (abcd1234, built 2026-06-14)"`.
- [ ] **CPack DEB/RPM + tar.gz** generators; CI emits SHA256SUMS.
- [ ] **Homebrew tap formula** for Mac developers.
- [ ] **CI linker-map check for A5** (`nm build/mtk | grep …`).
- [ ] **CI grep for forbidden includes** per CE4.
- [ ] **`mtk init` hook installer** — copies hooks for claude/copilot/
  gemini/codex/cursor from `share/mtk/hooks/` to the right directories.
- [ ] **Wire `~/.config/mtk/` skeleton and `.mtk/filters/` lookup
  paths** (even though Phase 4 fills the data layer — the lookup wiring
  itself is wired now, walks an empty dir if nothing's there).

### Phase 3 — Audit implementation (1.5 days)

The schema and call sites are committed in Phase 1.0/1.5; Phase 3 fills
the implementation.

- [ ] **`core/audit.{hpp,cpp}`** — JSONL writer (O_APPEND, single
  `write` per event), tee rotation per A6, schema per A5.
- [ ] **`mtk stats`** — per-filter aggregations from the JSONL.
- [ ] **`mtk tail [N]`** — last N events.
- [ ] **`mtk why <event-id>`** — re-spool raw output if
  `audit.capture_payload` was on for that event; "[tee no longer
  available]" otherwise.
- [ ] **`MTK_AUDIT_PAYLOAD=1`** env opt-in.
- [ ] Replace existing ad-hoc `tee.cpp` with the audit-backed version.

### Phase 4 — Layered config + code-smell cleanup (2 days)

- [ ] **A7: layered config** (`/etc/mtk` < `~/.config/mtk` < `.mtk`
  < env) in `core/settings.{hpp,cpp}` with `locked = true` support.
- [ ] **`mtk reload`** — validates all TOML filters, compiles regexes,
  emits errors with file:line:col, optionally writes a compiled cache
  to `~/.cache/mtk/filters.bin`.
- [ ] **A8: color suppression at source** — env + tool-specific flags.
- [ ] **`mtk init` writes default `~/.config/mtk/mtk.toml`** with
  inline comments showing every available setting.
- [ ] **Code-smell pass** (each its own commit):
  - [ ] Decompose `compact_ls` into parse/classify/render-entries/
    render-summary (CE2 / CR1).
  - [ ] Decompose `parse_match_line` NUL-vs-colon duplication into
    `try_parse_at(line, file_end_pos)` (CR3) + add the CR10 WHY-comment
    explaining why the fallback exists.
  - [ ] `run_show` orchestration — should already be cleaner post
    Phase 1.5, but verify it's under CE2's hard cap; if not, extract.
  - [ ] Apply CE1 string conventions repo-wide.
  - [ ] Apply CR11 `auto` conventions.
  - [ ] Apply CR12 `internal::` vs anon namespace conventions.
  - [ ] Apply CR13 — add the regex-compile test.
  - [ ] Consolidate `read_fixture` into `tests/support/fixture.hpp` (CR7).
  - [ ] Add missing branch-coverage tests per CR7 (only the ones added
    in this PR; pre-existing gaps become tickets).

### Phase 5 — Filter expansion (measured, not speculated)

Don't port filters speculatively. Phase 3 makes audit data possible —
run mtk against real agent traffic for a week, surface the top
token-leaking passthrough commands, then port those.

Likely candidates from the earlier strategic plan:
- [ ] **Test-runner filter** (one C++ module covering cargo test /
  pytest / vitest / jest / go test / playwright) — highest speculative
  value.
- [ ] **Bundled TOML filters**: cargo, npm, pip, tsc, eslint, kubectl,
  docker, terraform, ninja, psql. Each is a `*.toml` file in `filters/`;
  `mtk init` copies them.
- [ ] **Optional C++** for high-volume tabular output: `terraform
  plan`, `kubectl get`, `docker ps`, `git blame` — only if Phase 3 data
  shows they're hot.

### Phase 6 — Cross-platform completion (1 day)

- [ ] **A9: `core/platform.{hpp,cpp}`** with Windows path conventions
  (`%APPDATA%\mtk`, `%LOCALAPPDATA%\mtk`).
- [ ] **CI matrix**: macOS + Linux + Windows.

---

## Part IV — Operational rules

### Commit hygiene

- One logical change per commit. No "fix X + add Y" bundles.
- Commit messages: `<area>: <imperative verb> <what>`. Examples:
  - `git: fix was_truncated over-reporting on last-hunk cap`
  - `core/filter: introduce Filter interface and RunContext`
  - `tests: consolidate read_fixture into tests/support/fixture.hpp`
- No co-author lines. No emojis. Body explains rationale.

### PR review gate

CI enforces (build fails on violation):
- `cmake --build && ctest`
- CE2 function-size hard cap (clang-tidy)
- CE3 include order (clang-format `--dry-run -Werror`)
- CE4 no networking includes
- CE5 no `ExecOutcome` decomposition outside `core/exec.cpp`
- CE6 every cmds file has a corresponding test file
- A5 linker-map check (no networking symbols)

Reviewer enforces (block PR if violated):
- All CR1–CR15 rules
- Plus the spirit of Parts I and III

PR description confirms: "no new network dependency (A5); no
hardcoded subcommand dispatch (A1/A2); fallback pattern preserved
(A4); new caps have boundary tests (CR7)."

### When this document changes

Updates to Parts I–II require a one-line entry in the change log at
the bottom of this file with date, what changed, and rationale.
Part III is allowed to evolve freely as we work through it (checkboxes
flip).

---

## Change log

- **2026-06-14 (v1)**: Initial document. Synthesized from app-designer
  + clean-code-expert reviews + day-1 implementation lessons.
- **2026-06-14 (v3)**: `cpp-expert` design-patterns review confronted
  v2; load-bearing changes:
  - **A1 `DispatchToken`** — previous `DispatchHandle` ("struct with
    opaque contents") was unimplementable as written. Replaced with
    polymorphic base + `std::unique_ptr<DispatchToken>`; each filter
    defines its own derived type and `static_cast`s inside `run()`.
    Lifetime contract added: derived tokens may not store views/spans
    into `try_match`'s arguments.
  - **A3 collapsed to two arms** — `variant<SpawnFailed, Ran>` where
    `Ran { stdout, stderr, exit_code; clean(); }`. The structural
    distinction is spawn-failed-vs-ran; "exited non-zero" is a runtime
    predicate. Three arms duplicated payload for no structural gain.
  - **A1 `RunContext` test-isolation accessors** added — `settings()`,
    `auditor()` `const`-ref accessors so tests can substitute
    collaborators without constructing the whole environment.
  - **A1 "helpers visit at most once" rule** — prevents chained
    `RunContext` helpers that re-decompose the same outcome.
  - **CE5 extended** to forbid `.index()` (the back door).
  - **CE6 (new)** — `[[nodiscard]]` and `noexcept` are mandatory where
    applicable, with explicit per-method list.
  - **`emit(ExecOutcome&&)` consumes** — committed rvalue reference so
    a 40 MiB failure doesn't pay for a 40 MiB copy.
  - **Concrete `Filter` subclasses are `final`** — enables
    devirtualization where the static type is known.
- **2026-06-14 (v2)**: Adversarial stress-test by both review agents
  produced 18 concerns; load-bearing changes:
  - **A1**: `try_match()` returning `optional<DispatchHandle>` instead
    of `matches()` returning `bool` — carries match state forward, makes
    setup-at-registry-load enforceable, eliminates re-parse in `run()`.
    Name uniqueness within tier enforced.
  - **A2**: priority order inverted; built-ins are `final`; project
    `.mtk/` is opt-in via `mtk trust` (closes silent-shadow attack
    surface).
  - **A3**: changed to three-arm variant (SpawnFailed / RanClean /
    RanWithFailure); `std::get`/`visit` outside `core/exec.cpp`
    forbidden.
  - **A5**: enforced by linker-map CI check + forbidden-include CI grep,
    not just docstring; user-invoked upload commands explicitly out of
    scope.
  - **A6**: full budget interaction table — capture vs tee vs audit
    bounds spelled out, including the "lying hint" failure mode (tee
    eviction blocked for recently-referenced files).
  - **A11 (new)**: signal handling, RunContext threading, SIGPIPE
    behavior, audit-during-shutdown semantics.
  - **A12 (new)**: streaming is opt-in, must reuse `Filter` interface
    (prevents future abstraction fork).
  - **Part II split** into CE-enforced (CI gate) vs CR-reviewed
    (judgment), with each rule's enforcement mechanism named.
  - **CE2 + CR1**: 80-LOC hard cap is CI; 50-LOC soft cap is reviewer
    prompt only. Multi-spawn orchestrators exempt from soft cap.
  - **CR2**: orthogonal booleans stay as params with named locals or
    designated initializers; only related booleans require a struct.
  - **CR10**: WHY-comments now have a *positive obligation* for
    workarounds / fallbacks / non-obvious unicode / recovered-from bugs.
  - **CR12–CR15 (new)**: `internal::` vs anon namespace rule;
    function-static regex pattern; `optional<T>` for parse failure;
    format-injection-string + sentinel co-location.
  - **Phase 0**: added zero-risk extractions (`diagnostic`, `report_
    spawn_failure`, clang-format/tidy configs) so Phase 1 doesn't churn
    every filter twice.
  - **Phase 1 split** into 1.0 (spawn + audit interface stub) and 1.5
    (Filter + registry) so audit schema is committed *before* every
    filter is migrated — eliminates the "Phase 3 re-touches every
    filter" trap.
  - **Phase 2**: wired up Phase 4's config lookup paths early so the
    layered config has somewhere to land without re-migration.
  - **Phase 4**: dropped the `print_captured(outcome, slug) -> int`
    omnibus helper in favor of single-purpose `ctx.emit(outcome)` +
    `ctx.tee_on_failure(outcome, slug)`.
