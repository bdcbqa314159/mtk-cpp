#include "core/signals.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>

namespace mtk::core::signals {

namespace {

// Async-signal-safe state. std::atomic<int> on this platform is lock-free
// (verified by static_assert below) so writes from a signal handler are safe.
std::atomic<int> g_pending{0};
std::atomic<bool> g_installed{false};

static_assert(std::atomic<int>::is_always_lock_free,
              "signal handler requires lock-free std::atomic<int>");

extern "C" void on_signal(int signo) noexcept {
    if (signo == SIGPIPE) {
        // Per A11: user closed the downstream reader; we do not tee/audit/
        // buffer-flush. _exit is async-signal-safe (unlike exit() which runs
        // atexit handlers and destroys static objects — may touch malloc).
        std::_Exit(141);
    }
    g_pending.store(signo, std::memory_order_release);
}

}  // namespace

void install() noexcept {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART so blocked syscalls (read, wait, etc.) resume rather than
    // failing with EINTR — we react to the signal at our polling boundary,
    // not via interrupt-driven control flow.
    sa.sa_flags = SA_RESTART;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGPIPE, &sa, nullptr);
}

int pending() noexcept {
    return g_pending.load(std::memory_order_acquire);
}

void clear() noexcept {
    g_pending.store(0, std::memory_order_release);
}

}  // namespace mtk::core::signals
