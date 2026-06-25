#pragma once

// Pure aggregation over audit events — the math behind `mtk stats`.
// Kept separate from the rendering in cmds/meta.cpp so it can be unit
// tested without touching the global audit log or stdout.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "core/audit.hpp"

namespace mtk::core::stats {

// Aggregate counters for one filter (or the overall roll-up).
struct Agg {
    std::size_t count = 0;
    std::size_t bytes_in = 0;
    std::size_t bytes_out = 0;
    std::size_t errors = 0;        // events with non-zero exit_code
    long elapsed_ms_total = 0;

    [[nodiscard]] long avg_ms() const noexcept {
        return count > 0 ? elapsed_ms_total / static_cast<long>(count) : 0;
    }
};

struct Report {
    Agg overall;
    // Per-filter rows, sorted by count descending then name ascending so
    // output (and tests) are deterministic regardless of map iteration order.
    std::vector<std::pair<std::string, Agg>> by_filter;
};

// Savings percentage: 100 - bytes_out/bytes_in * 100. Returns 0 when
// bytes_in is 0. May be NEGATIVE when a filter emitted more than it
// captured (e.g. a filter that adds decoration) — that's a real signal,
// not an error, so we don't clamp it.
[[nodiscard]] long savings_pct(std::size_t bytes_in, std::size_t bytes_out) noexcept;

// Roll events up into overall + per-filter aggregates.
[[nodiscard]] Report summarize(const std::vector<audit::Event>& events);

// Human-readable byte count: "0B", "1.5K", "3.2M".
[[nodiscard]] std::string fmt_bytes(std::size_t n);

}  // namespace mtk::core::stats
