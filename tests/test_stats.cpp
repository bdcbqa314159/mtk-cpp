#include <doctest/doctest.h>

#include "core/stats.hpp"

namespace {

mtk::core::audit::Event ev(const std::string& filter, std::size_t in,
                           std::size_t out, int exit_code = 0, long ms = 0) {
    mtk::core::audit::Event e;
    e.filter_name = filter;
    e.bytes_in = in;
    e.bytes_out = out;
    e.exit_code = exit_code;
    e.elapsed_ms = ms;
    return e;
}

}  // namespace

TEST_CASE("savings_pct: normal reduction") {
    CHECK(mtk::core::stats::savings_pct(1000, 400) == 60);
    CHECK(mtk::core::stats::savings_pct(1000, 1000) == 0);
}

TEST_CASE("savings_pct: zero bytes_in yields 0 (no divide-by-zero)") {
    CHECK(mtk::core::stats::savings_pct(0, 0) == 0);
    CHECK(mtk::core::stats::savings_pct(0, 500) == 0);
}

TEST_CASE("savings_pct: full reduction and negative when output grows") {
    CHECK(mtk::core::stats::savings_pct(100, 0) == 100);
    // A filter that emits MORE than it captured → negative savings, not clamped.
    CHECK(mtk::core::stats::savings_pct(100, 200) == -100);
}

TEST_CASE("summarize: empty input gives an empty report") {
    auto r = mtk::core::stats::summarize({});
    CHECK(r.overall.count == 0);
    CHECK(r.by_filter.empty());
    CHECK(r.overall.avg_ms() == 0);  // no divide-by-zero
}

TEST_CASE("summarize: overall roll-up sums every field") {
    std::vector<mtk::core::audit::Event> events = {
        ev("git_log", 1000, 400, 0, 10),
        ev("ls",       200,  50, 1, 30),
        ev("git_log",  500, 100, 0, 20),
    };
    auto r = mtk::core::stats::summarize(events);
    CHECK(r.overall.count == 3);
    CHECK(r.overall.bytes_in == 1700);
    CHECK(r.overall.bytes_out == 550);
    CHECK(r.overall.errors == 1);              // the ls event exited non-zero
    CHECK(r.overall.elapsed_ms_total == 60);
    CHECK(r.overall.avg_ms() == 20);           // 60 / 3
}

TEST_CASE("summarize: per-filter grouping is correct") {
    std::vector<mtk::core::audit::Event> events = {
        ev("git_log", 1000, 400, 0, 10),
        ev("ls",       200,  50, 1, 30),
        ev("git_log",  500, 100, 0, 20),
    };
    auto r = mtk::core::stats::summarize(events);
    REQUIRE(r.by_filter.size() == 2);

    // Sorted by count desc: git_log (2) before ls (1).
    CHECK(r.by_filter[0].first == "git_log");
    CHECK(r.by_filter[0].second.count == 2);
    CHECK(r.by_filter[0].second.bytes_in == 1500);
    CHECK(r.by_filter[0].second.bytes_out == 500);
    CHECK(r.by_filter[0].second.errors == 0);

    CHECK(r.by_filter[1].first == "ls");
    CHECK(r.by_filter[1].second.count == 1);
    CHECK(r.by_filter[1].second.errors == 1);
}

TEST_CASE("summarize: ties broken by filter name for deterministic output") {
    std::vector<mtk::core::audit::Event> events = {
        ev("zeta", 10, 5),
        ev("alpha", 10, 5),
        ev("mid", 10, 5),
    };
    auto r = mtk::core::stats::summarize(events);
    REQUIRE(r.by_filter.size() == 3);
    CHECK(r.by_filter[0].first == "alpha");
    CHECK(r.by_filter[1].first == "mid");
    CHECK(r.by_filter[2].first == "zeta");
}

TEST_CASE("fmt_bytes: scales to B / K / M") {
    CHECK(mtk::core::stats::fmt_bytes(0) == "0B");
    CHECK(mtk::core::stats::fmt_bytes(512) == "512B");
    CHECK(mtk::core::stats::fmt_bytes(1024) == "1.0K");
    CHECK(mtk::core::stats::fmt_bytes(1536) == "1.5K");
    CHECK(mtk::core::stats::fmt_bytes(1024 * 1024) == "1.0M");
}
