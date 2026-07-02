#include "core/exec.hpp"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#ifdef _WIN32
#  include <atomic>
#  include <fstream>
#endif
#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

#ifdef _WIN32
#  include <windows.h>  // GetShortPathNameW, WAIT_TIMEOUT
#endif

#include "core/diagnostic.hpp"
#include "core/limits.hpp"
#include "core/signals.hpp"

// Per perf critic P7: previously we walked PATH ourselves before fork to
// surface "command not found" early. Reproc already reports exec failure
// through its error pipe, so `proc.start()` returns ec from a failed exec
// inside the child; we use that as the authoritative signal. The
// post-spawn "exit=127 with empty pipes" backstop in capture_outcome
// remains as defense-in-depth.

namespace mtk::core::exec {

// True if `ec` is a reproc wait/deadline timeout. reproc sets REPROC_ETIMEDOUT
// and the C++ wrapper surfaces it as {-REPROC_ETIMEDOUT, system_category()}.
// On POSIX that is ETIMEDOUT, which compares equal to std::errc::timed_out; on
// Windows it is WAIT_TIMEOUT (258), which MSVC's system_category does NOT map
// to std::errc::timed_out — so the generic comparison silently fails there and
// a still-running child gets misread as "exited". Match both encodings.
static bool is_wait_timeout(const std::error_code& ec) {
    if (ec == std::errc::timed_out) return true;  // POSIX (and any mapped)
#ifdef _WIN32
    // WAIT_TIMEOUT in the Win32 system category — the value REPROC_ETIMEDOUT
    // negates on Windows. Kept explicit because MSVC won't map it for us.
    if (ec.category() == std::system_category() && ec.value() == WAIT_TIMEOUT) {
        return true;
    }
#endif
    return false;
}

#ifdef _WIN32
// Collapse a Windows path to its 8.3 short form (no spaces). reproc joins argv
// into one command line and quotes any element containing a space; cmd.exe's
// `/c` quote-stripping then mangles a quoted program path (turning
// `"C:\Program Files\...\npm.cmd"` into an attempt to run `C:\Program`). A
// short path sidesteps the whole quoting problem. Returns the input unchanged
// if 8.3 generation is disabled on the volume or the call fails.
static std::string to_short_path(const std::string& path) {
    std::filesystem::path p(path);
    std::wstring w = p.wstring();
    DWORD n = GetShortPathNameW(w.c_str(), nullptr, 0);
    if (n == 0) return path;
    std::wstring buf(n, L'\0');
    DWORD got = GetShortPathNameW(w.c_str(), buf.data(), n);
    if (got == 0 || got >= n) return path;
    buf.resize(got);
    return std::filesystem::path(buf).string();
}

// A unique temp path for one capture stream. pid + a monotonic counter avoids
// collisions between concurrent mtk invocations and between the two streams of
// one invocation, without needing Date/random (unavailable / undesirable here).
static std::string capture_temp_path(const char* tag) {
    static std::atomic<unsigned> counter{0};
    unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path p = std::filesystem::temp_directory_path() /
        ("mtk_cap_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(n) + "_" + tag + ".tmp");
    return p.string();
}

// Read a capture file back into `target`, honouring the shared byte cap (A6).
// Mirrors BoundedSink's semantics for the file-redirect capture path.
static void read_capped(const std::string& path, std::string& target,
                        std::size_t& total, std::size_t cap, bool& truncated) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    char buf[8192];
    while (f) {
        f.read(buf, sizeof(buf));
        std::streamsize got = f.gcount();
        if (got <= 0) break;
        std::size_t n = static_cast<std::size_t>(got);
        if (total >= cap) { truncated = true; break; }
        std::size_t room = cap - total;
        std::size_t to_write = (n < room) ? n : room;
        target.append(buf, to_write);
        total += to_write;
        if (to_write < n) { truncated = true; break; }
    }
}
#endif

// On Windows, reproc spawns via CreateProcessW(NULL, cmdline), which only
// auto-appends .exe — so .cmd/.bat shims (npm, yarn, pnpm, tsc, ...) are never
// found. We search PATH ourselves; on a batch-file match we relaunch through
// `cmd /c`, which CAN exec a .cmd. .exe/.com matches and POSIX are returned
// unchanged (CreateProcess / execvp resolve those itself). The resolved shim
// path is collapsed to its 8.3 short form so cmd.exe's quote handling doesn't
// choke on a spaced install dir (e.g. C:\Program Files\nodejs).
//
// ponytail: handles bare command names only. An explicit `mtk npm.cmd ...`,
// and shim *arguments* that contain spaces on volumes without 8.3 names, are
// out of scope — add outer-quote wrapping if hit.
std::vector<std::string> resolve_launcher(const std::vector<std::string>& argv) {
#ifndef _WIN32
    return argv;
#else
    if (argv.empty()) return argv;
    std::filesystem::path cmd(argv[0]);
    if (cmd.has_extension() || cmd.has_parent_path()) return argv;  // explicit

    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) return argv;
    const char* ext_env = std::getenv("PATHEXT");
    std::string exts = ext_env ? ext_env : ".COM;.EXE;.BAT;.CMD";

    auto split = [](std::string_view s) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= s.size()) {
            std::size_t pos = s.find(';', start);
            if (pos == std::string_view::npos) pos = s.size();
            if (pos > start) parts.emplace_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return parts;
    };

    for (const auto& dir : split(path_env)) {
        for (const auto& ext : split(exts)) {
            std::filesystem::path cand =
                std::filesystem::path(dir) / (argv[0] + ext);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(cand, ec)) continue;

            std::string lower = ext;
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == ".cmd" || lower == ".bat") {
                const char* comspec = std::getenv("COMSPEC");
                std::vector<std::string> out = {
                    comspec ? comspec : "cmd.exe", "/c",
                    to_short_path(cand.string())};
                out.insert(out.end(), argv.begin() + 1, argv.end());
                return out;
            }
            return argv;  // .exe/.com etc. — CreateProcess resolves it
        }
    }
    return argv;  // not found — let reproc surface ENOENT as before
#endif
}

// Staged termination: SIGTERM, wait kGraceMs, then SIGKILL.
reproc::stop_actions staged_stop() {
    reproc::stop_actions sa{};
    sa.first.action = reproc::stop::terminate;
    sa.first.timeout = reproc::milliseconds(mtk::core::limits::signal_::kGraceMs);
    sa.second.action = reproc::stop::kill;
    sa.second.timeout = reproc::milliseconds(0);
    return sa;
}

int passthrough(const std::vector<std::string>& argv) {
    if (argv.empty()) return 127;

    reproc::process proc;
    reproc::options opts;
    opts.redirect.parent = true;
    opts.stop = staged_stop();

    if (auto ec = proc.start(resolve_launcher(argv), opts)) {
        // Route through diag::emit so output picks up the color policy
        // — was direct fprintf, which bypassed the color wrapper.
        if (ec == std::errc::no_such_file_or_directory) {
            mtk::core::diag::emit(argv[0], "command not found in PATH");
        } else {
            std::string msg = "failed to spawn: ";
            msg.append(ec.message());
            mtk::core::diag::emit(argv[0], msg);
        }
        return 127;
    }

    // Poll loop so we can react to signals (the child shares our process group
    // so it gets the signal too; we still want to wait for it to exit and
    // propagate the exit code rather than dying first).
    while (true) {
        if (int sig = mtk::core::signals::take(); sig != 0) {
            // proc::~process / .stop() will run the staged termination.
            (void)proc.terminate();
            auto [status, _ec] = proc.wait(
                reproc::milliseconds(mtk::core::limits::signal_::kGraceMs));
            (void)status;
            return 128 + sig;
        }
        auto [status, ec] = proc.wait(reproc::milliseconds(100));
        // Anything that isn't the 100 ms poll timeout means the child exited
        // (or a real wait error) — return. A timeout just loops. Using the
        // category-aware is_wait_timeout matters on Windows: the old raw-value
        // compare misfired, returned mid-run, and let the destructor's staged
        // stop fire CTRL_BREAK at the child ("Terminate batch job (Y/N)?").
        if (!ec || !is_wait_timeout(ec)) {
            return status;
        }
    }
}

// Bounded sink: appends to `target` while a shared byte-counter stays under
// `cap`. Stops the drain (non-zero error_code) when the cap is reached. The
// counter is shared between stdout and stderr sinks so the cap is the total
// captured bytes per A6.
class BoundedSink {
public:
    BoundedSink(std::string& target, std::size_t& total, std::size_t cap,
                bool& truncated) noexcept
        : target_(target), total_(total), cap_(cap), truncated_(truncated) {}

    std::error_code operator()(reproc::stream, const uint8_t* buf, std::size_t size) {
        if (total_ >= cap_) {
            truncated_ = true;
            return std::make_error_code(std::errc::file_too_large);
        }
        std::size_t room = cap_ - total_;
        std::size_t to_write = (size < room) ? size : room;
        target_.append(reinterpret_cast<const char*>(buf), to_write);
        total_ += to_write;
        if (to_write < size) {
            truncated_ = true;
            return std::make_error_code(std::errc::file_too_large);
        }
        return {};
    }

private:
    std::string& target_;
    std::size_t& total_;
    std::size_t cap_;
    bool& truncated_;
};

ExecOutcome capture_outcome(const std::vector<std::string>& argv,
                            const EnvExtra& env_extra) {
    if (argv.empty()) {
        return SpawnFailed{"empty argv"};
    }

    reproc::process proc;
    reproc::options opts;
    opts.redirect.in.type = reproc::redirect::type::discard;
    opts.deadline = reproc::milliseconds(mtk::core::limits::capture::kTimeoutMs);
    opts.stop = staged_stop();

#ifdef _WIN32
    // reproc's Windows "pipes" are AF_INET sockets (so it can WSAPoll them).
    // Node/libuv inspects its inherited stdio handle, sees a socket, routes it
    // through net.Socket and dies with `Error: open EISDIR` — so npm/npx/tsc
    // and any node-backed shim can't be captured over reproc pipes. Redirect
    // the child's stdout/stderr to temp files instead (node treats a file
    // handle as UV_FILE and is happy), then read them back below. The strings
    // must outlive start(): reproc keeps the `const char*`.
    std::string out_path = capture_temp_path("out");
    std::string err_path = capture_temp_path("err");
    opts.redirect.out.type = reproc::redirect::type::path_;
    opts.redirect.out.path = out_path.c_str();
    opts.redirect.err.type = reproc::redirect::type::path_;
    opts.redirect.err.path = err_path.c_str();
#else
    opts.redirect.out.type = reproc::redirect::type::pipe;
    opts.redirect.err.type = reproc::redirect::type::pipe;
#endif

    if (!env_extra.empty()) {
        opts.env.behavior = reproc::env::extend;
        opts.env.extra = env_extra;
    }

    if (auto ec = proc.start(resolve_launcher(argv), opts)) {
#ifdef _WIN32
        // reproc's redirect_path opens (creates) the files before spawning, so
        // a spawn failure can leave empties behind — clean them up.
        std::error_code rmec;
        std::filesystem::remove(out_path, rmec);
        std::filesystem::remove(err_path, rmec);
#endif
        // Reproc surfaces exec failures (ENOENT for missing binary, EACCES
        // for non-executable) via the in-child error pipe — normalise the
        // ENOENT case to the friendlier "command not found in PATH" shape
        // we used before P7.
        if (ec == std::errc::no_such_file_or_directory) {
            return SpawnFailed{"command not found in PATH: " + argv[0]};
        }
        return SpawnFailed{ec.message()};
    }

    Ran ran;
    std::size_t total_bytes = 0;
    bool truncated = false;

#ifndef _WIN32
    BoundedSink out_sink(ran.stdout_data, total_bytes,
                         mtk::core::limits::capture::kMaxBytes, truncated);
    BoundedSink err_sink(ran.stderr_data, total_bytes,
                         mtk::core::limits::capture::kMaxBytes, truncated);

    (void)reproc::drain(proc, out_sink, err_sink);
    ran.truncated = truncated;

    // If we cut the drain short because of the byte cap, the child is still
    // running. Terminate it via the staged stop_actions configured in opts.
    if (truncated) {
        (void)proc.terminate();
    }
#endif

    int pending_sig = mtk::core::signals::take();
    if (pending_sig != 0) {
        (void)proc.terminate();
        ran.killed_by_signal = pending_sig;
    }

    // POSIX: the deadline was enforced during drain, so we can wait forever for
    // the (already-finished or drain-terminated) child. Windows: there was no
    // drain, so we must wait on the deadline sentinel or a runaway child would
    // block us indefinitely.
#ifdef _WIN32
    auto [status, wait_ec] = proc.wait(reproc::deadline);
#else
    auto [status, wait_ec] = proc.wait(reproc::infinite);
#endif
    ran.exit_code = status;

    // reproc's deadline expired before the child exited cleanly. Category-aware
    // check — on Windows the deadline surfaces as WAIT_TIMEOUT/system_category,
    // which a raw std::errc::timed_out compare would miss (see is_wait_timeout).
    if (wait_ec && is_wait_timeout(wait_ec)) {
        ran.timed_out = true;
        ran.exit_code = 124;  // canonical timeout exit
    }

#ifdef _WIN32
    // On a deadline hit the child is still running and holding the file open;
    // stop it (staged terminate/kill) before we read, so the handle is released
    // and the file content is flushed.
    if (ran.timed_out) {
        (void)proc.terminate();
        (void)proc.wait(reproc::milliseconds(mtk::core::limits::signal_::kGraceMs));
    }
    read_capped(out_path, ran.stdout_data, total_bytes,
                mtk::core::limits::capture::kMaxBytes, truncated);
    read_capped(err_path, ran.stderr_data, total_bytes,
                mtk::core::limits::capture::kMaxBytes, truncated);
    ran.truncated = truncated;
    std::error_code rmec;
    std::filesystem::remove(out_path, rmec);
    std::filesystem::remove(err_path, rmec);
#endif

    if (ran.killed_by_signal != 0) {
        ran.exit_code = 128 + ran.killed_by_signal;
    }

    // Per audit: the previous "exit=127 + empty pipes → SpawnFailed"
    // backstop was a defense-in-depth check for execvp-failure-in-child
    // scenarios that reproc supposedly couldn't catch. It produced a
    // false positive for any program that legitimately exits 127 with
    // no output (`bash -c 'exit 127'`, shell sentinels, makefile
    // wrappers). Reproc's `start()` error pipe reliably reports exec
    // failures via ec — see the start() check above — so the backstop
    // was both defensive and wrong. Dropped: a real exit=127 from a
    // running program now flows through as Ran with exit_code=127,
    // matching the user's mental model.
    return ran;
}

}  // namespace mtk::core::exec
