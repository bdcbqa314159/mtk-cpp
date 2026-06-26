#include "core/stats.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace mtk::core::stats {

long savings_pct(std::size_t bytes_in, std::size_t bytes_out) noexcept {
    if (bytes_in == 0) return 0;
    return 100 - static_cast<long>(bytes_out * 100 / bytes_in);
}

Report summarize(const std::vector<audit::Event>& events) {
    std::unordered_map<std::string, Agg> by_filter;
    Report report;
    for (const auto& e : events) {
        Agg* buckets[] = {&report.overall, &by_filter[e.filter_name]};
        for (Agg* a : buckets) {
            a->count++;
            a->bytes_in += e.bytes_in;
            a->bytes_out += e.bytes_out;
            if (e.exit_code != 0) a->errors++;
            a->elapsed_ms_total += e.elapsed_ms;
        }
    }

    report.by_filter.assign(by_filter.begin(), by_filter.end());
    std::sort(report.by_filter.begin(), report.by_filter.end(),
              [](const auto& a, const auto& b) {
                  if (a.second.count != b.second.count)
                      return a.second.count > b.second.count;
                  return a.first < b.first;
              });
    return report;
}

std::string fmt_bytes(std::size_t n) {
    char buf[32];
    if (n >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / (1024.0 * 1024.0));
    } else if (n >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(n) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%zuB", n);
    }
    return buf;
}

}  // namespace mtk::core::stats
