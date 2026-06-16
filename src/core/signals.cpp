#include "core/signals.hpp"

#include <atomic>
#include <cstdlib>

#if defined(__linux__) || defined(__APPLE__)
#  include <csignal>
#elif defined(_WIN32)
// Windows port: this initial implementation is a deferred-version stub.
// We rely on the CRT's default Ctrl-C handler to terminate the process
// cleanly. No SIGINT/SIGTERM/SIGHUP forwarding to children, no
// SIGPIPE→141 exit (Windows surfaces broken-pipe via WriteFile error
// at the emit site, not as a signal). Upgrading to native
// SetConsoleCtrlHandler-based delivery is a real design call —
// SIGHUP and SIGPIPE have no Windows analog and CTRL_CLOSE_EVENT /
// CTRL_LOGOFF_EVENT only fire in console apps.
#else
#  error "signals: unsupported platform"
#endif

namespace mtk::core::signals {

#if defined(__linux__) || defined(__APPLE__)

namespace {

// Bitmask: bit N = signal N pending. Lock-free per static_assert.
std::atomic<int> g_mask{0};
std::atomic<bool> g_installed{false};

static_assert(std::atomic<int>::is_always_lock_free,
              "signal handler requires lock-free std::atomic<int>");

extern "C" void on_int_term_hup(int signo) noexcept {
    // Async-signal-safe: fetch_or on a lock-free atomic. Bitmask
    // semantics mean concurrent signals OR together — no signal is lost
    // even if two arrive between consecutive take() calls.
    if (signo >= 0 && signo < 32) {
        g_mask.fetch_or(1 << signo, std::memory_order_relaxed);
    }
}

extern "C" void on_sigpipe(int /*signo*/) noexcept {
    // Per A11: user closed the downstream reader; we do not tee, do not
    // audit, do not flush buffers. _Exit is async-signal-safe (unlike
    // exit() which runs atexit handlers and destroys static objects —
    // may touch malloc).
    std::_Exit(141);
}

}  // namespace

void install() noexcept {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    // SIGINT/TERM/HUP: NO SA_RESTART. Blocked syscalls (reproc's pipe
    // reads in particular) return EINTR; reproc's drain surfaces that as
    // an error and we return from the drain promptly, check take(), and
    // terminate the child. Without this, drain blocks until child
    // closes its pipes or the deadline expires — up to 30 s of latency
    // on a non-terminal-delivered signal (correctness critic C1).
    //
    // Trade-off: other syscalls in mtk (file open/write) may also fail
    // with EINTR. The hot path is audit-log append (try/catch in
    // audit::append already swallows) and the dispatch path itself,
    // which doesn't make blocking syscalls outside reproc.
    struct sigaction sa_intr {};
    sa_intr.sa_handler = on_int_term_hup;
    sigemptyset(&sa_intr.sa_mask);
    sa_intr.sa_flags = 0;
    sigaction(SIGINT, &sa_intr, nullptr);
    sigaction(SIGTERM, &sa_intr, nullptr);
    sigaction(SIGHUP, &sa_intr, nullptr);

    // SIGPIPE: handler calls _Exit(141), never returns. SA_RESTART is
    // set but irrelevant.
    struct sigaction sa_pipe {};
    sa_pipe.sa_handler = on_sigpipe;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = SA_RESTART;
    sigaction(SIGPIPE, &sa_pipe, nullptr);
}

int take() noexcept {
    // Take ONE signal (highest priority), clear only that bit. The
    // previous exchange(0) drained all bits but the if-chain returned
    // only one signal — a second pending signal was silently dropped
    // (SIGINT+SIGHUP racing → SIGHUP-as-log-rotate was lost). fetch_and
    // is atomic and lock-free; a signal arriving between the load and
    // the clear is OR'd into the mask by the handler and survives.
    int mask = g_mask.load(std::memory_order_acquire);
    int bit = 0;
    int sig = 0;
    if      (mask & (1 << SIGINT))  { bit = 1 << SIGINT;  sig = SIGINT; }
    else if (mask & (1 << SIGTERM)) { bit = 1 << SIGTERM; sig = SIGTERM; }
    else if (mask & (1 << SIGHUP))  { bit = 1 << SIGHUP;  sig = SIGHUP; }
    else return 0;
    g_mask.fetch_and(~bit, std::memory_order_acq_rel);
    return sig;
}

bool any_pending() noexcept {
    return g_mask.load(std::memory_order_acquire) != 0;
}

#elif defined(_WIN32)

// No-op stubs. CRT default handlers terminate the process on Ctrl-C;
// other "signal-like" events are intentionally not surfaced in this
// initial Windows port.
void install() noexcept {}
int take() noexcept { return 0; }
bool any_pending() noexcept { return false; }

#endif

}  // namespace mtk::core::signals
