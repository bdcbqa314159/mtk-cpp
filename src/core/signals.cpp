#include "core/signals.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>

namespace mtk::core::signals {

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
    // exchange returns the OLD value and atomically resets to 0 — closes
    // the lost-signal-between-load-and-clear race that the previous
    // pending()/clear() pair had.
    int mask = g_mask.exchange(0, std::memory_order_acq_rel);
    if (mask & (1 << SIGINT))  return SIGINT;
    if (mask & (1 << SIGTERM)) return SIGTERM;
    if (mask & (1 << SIGHUP))  return SIGHUP;
    return 0;
}

bool any_pending() noexcept {
    return g_mask.load(std::memory_order_acquire) != 0;
}

}  // namespace mtk::core::signals
