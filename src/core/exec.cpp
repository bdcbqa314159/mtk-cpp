#include "core/exec.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

#include "core/limits.hpp"
#include "core/signals.hpp"

// Per perf critic P7: previously we walked PATH ourselves before fork to
// surface "command not found" early. Reproc already reports exec failure
// through its error pipe, so `proc.start()` returns ec from a failed exec
// inside the child; we use that as the authoritative signal. The
// post-spawn "exit=127 with empty pipes" backstop in capture_outcome
// remains as defense-in-depth.

namespace mtk::core::exec {

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

    if (auto ec = proc.start(argv, opts)) {
        if (ec == std::errc::no_such_file_or_directory) {
            std::fprintf(stderr, "mtk %s: command not found in PATH\n", argv[0].c_str());
        } else {
            std::fprintf(stderr, "mtk %s: failed to spawn: %s\n", argv[0].c_str(),
                         ec.message().c_str());
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
        if (!ec || ec.value() != static_cast<int>(std::errc::timed_out)) {
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
    opts.redirect.out.type = reproc::redirect::type::pipe;
    opts.redirect.err.type = reproc::redirect::type::pipe;
    opts.deadline = reproc::milliseconds(mtk::core::limits::capture::kTimeoutMs);
    opts.stop = staged_stop();

    if (!env_extra.empty()) {
        opts.env.behavior = reproc::env::extend;
        opts.env.extra = env_extra;
    }

    if (auto ec = proc.start(argv, opts)) {
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

    int pending_sig = mtk::core::signals::take();
    if (pending_sig != 0) {
        (void)proc.terminate();
        ran.killed_by_signal = pending_sig;
    }

    auto [status, wait_ec] = proc.wait(reproc::infinite);
    ran.exit_code = status;

    // reproc's deadline expired before the child exited cleanly.
    if (wait_ec && wait_ec.value() == static_cast<int>(std::errc::timed_out)) {
        ran.timed_out = true;
        ran.exit_code = 124;  // canonical timeout exit
    }

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
