# mtk++

**Minimal Token Killer** — a single-binary CLI proxy that filters and
compresses shell-command output to reduce tokens spent on agent tool
results.

```sh
$ git log -5                                # raw: ~3 KB of decoration
$ mtk git log -5                            # compacted: ~400 B, same information
```

Same idea for `ls`, `grep`, `git status`, `git diff`, `git show`, and
anything you describe in a TOML filter (`make`, `cmake`, and yours).

---

## Why

- **Tokens cost real money on agent tool calls.** Most CLI output is
  decoration. mtk strips it and leaves the signal.
- **Telemetry-free by construction.** A CI gate (`scripts/ci-check.sh`)
  greps the built binary's symbol table for `socket|connect|getaddrinfo|
  SSL_|tls_|curl_` and fails the build if anything networking-related
  leaks in. There is no phone-home. There is no metrics endpoint.
- **C++17, no Rust toolchain needed.** Drop-in for environments where
  installing a new language runtime is not an option.
- **Single static binary.** No daemon, no shared library to install,
  exits after each invocation.

This is a clean-room C++17 reimplementation of [rtk-ai/rtk][rtk]
(Rust). Same idea, different constraints.

---

## Quick start

```sh
git clone https://github.com/bdcbqa314159/mtk-cpp.git mtk
cd mtk
cmake -B build -DCMAKE_BUILD_TYPE=Release      # auto-fetches submodules on first run
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/mtk --help
```

CMake's configure step auto-runs `git submodule update --init --recursive`
when submodules are missing, so `--recurse-submodules` is not required.
If you can't reach `github.com` directly (air-gapped build), see
[INSTALL.md](INSTALL.md) for the offline tarball-drop fallback.

---

## Use

```sh
mtk git log -10
mtk git status
mtk git diff
mtk git show <sha>
mtk ls /some/path
mtk grep "pattern" src/
mtk make                                    # uses filters/make.toml if installed
```

When no filter matches, mtk passes the command through unchanged. You
can prefix any command with `mtk` safely — it will only compress where
it knows how to.

`mtk explain <cmd>` shows which filter would handle it (dry run).

---

## Install

System install (binary + bundled TOML filters + docs):

```sh
cmake --build build --target install
```

Per-user install — copy or symlink `build/mtk` into `~/.local/bin/` (or
your preferred location). No PATH magic required.

Hook into an agent so it auto-wraps shell commands:

```sh
mtk init claude        # writes ~/.claude/hooks/mtk-rewrite.sh + prints settings snippet
mtk init copilot       # writes .github/hooks/mtk-rewrite.json + upserts copilot-instructions.md
```

Both install the *absolute* path of the mtk binary into the generated
hook, so PATH setup isn't required.

---

## Custom filters

Drop a TOML file into one of:

- `~/.config/mtk/filters/` — your user filters
- `.mtk/filters/` — project-scoped (requires `mtk trust .`)
- `/etc/mtk/filters/` — org-wide policy (set `locked = true` to prevent
  user/project overrides)

Example (`~/.config/mtk/filters/make.toml`):

```toml
[filters.make]
description = "Compress make build output — keep warnings/errors, drop chatter"
match_command = "^make$"
strip_ansi = true
strip_lines_matching = [
    "^make\\[", "^Entering directory", "^Leaving directory",
    "^\\s*CC\\s", "^\\s*LD\\s",
]
keep_lines_matching = ["error", "Error", "warning", "Warning"]
truncate_lines_at = 240
max_lines = 80
on_empty = "[mtk: make succeeded — no errors or warnings]"
```

See [filters/](filters/) for shipped examples.

---

## Introspection

```sh
mtk stats                       # per-filter savings dashboard
mtk tail [N]                    # last N audit events (default 10)
mtk why <event-id>              # re-spool the raw output of an event
mtk trusted                     # list project paths allowed to use .mtk/filters/
mtk reload                      # invalidate + rebuild the TOML-filter cache
```

Audit log: `~/.local/state/mtk/events.jsonl` (JSONL, rotated at 10 MiB).
Set `MTK_AUDIT_PAYLOAD=1` to capture full raw outputs to
`~/.local/state/mtk/payloads/<event-id>.txt` for `mtk why` to replay.

---

## Configuration layers

Filters resolve in priority order (lower number wins):

| # | Tier       | Source                                   |
|---|------------|------------------------------------------|
| 0 | OrgToml    | `/etc/mtk/filters/*.toml` (overridable via `MTK_ORG_CONFIG`) |
| 1 | Builtin    | C++ filters compiled into the binary     |
| 2 | UserToml   | `~/.config/mtk/filters/*.toml`           |
| 3 | ProjectToml| `.mtk/filters/*.toml` (opt-in via `mtk trust`) |
| 4 | Fallback   | passthrough (always last)                |

An org filter with `locked = true` blocks same-name registrations at
any lower tier — admins can enforce a baseline that users can't shadow.

---

## Architecture

The design is documented in [GUIDELINES.md](GUIDELINES.md) — the
project's constitution. A1-A12 are the architectural invariants
(no networking, `ExecOutcome` sum type, 5-tier registry, etc.); CE1-CR15
are the C++ idioms.

In one paragraph: one binary, one invocation. argv → registry lookup →
single Filter abstraction (builtin or TOML) → spawn via reproc with
bounded capture (50 MiB / 30 s) → compaction → emit → audit row.
The `RunContext` facade owns spawn/emit/audit so filters never reach
into `cout`, `getenv`, or reproc directly. No background work, no
persistent state beyond the cache file (`~/.cache/mtk/filters.bin`)
and the audit log.

---

## Environment

| Variable                       | Effect                                        |
|--------------------------------|-----------------------------------------------|
| `MTK_DEBUG=1`                  | Dispatch trace to stderr                      |
| `MTK_ALLOW_PROJECT_FILTERS=1`  | Bypass trust check (load any `.mtk/filters/`) |
| `MTK_AUDIT_PAYLOAD=1`          | Capture full outputs to `~/.local/state/mtk/payloads/` |
| `MTK_ORG_CONFIG=/path`         | Override `/etc/mtk` as the org-config root    |
| `XDG_CONFIG_HOME` / `XDG_CACHE_HOME` / `XDG_STATE_HOME` | Honoured for user paths       |

---

## License

MIT — see [LICENSE](LICENSE).

[rtk]: https://github.com/rtk-ai/rtk
