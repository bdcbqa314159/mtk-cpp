#include "cmds/meta.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "core/audit.hpp"
#include "core/color.hpp"
#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/filter_cache.hpp"
#include "core/registry.hpp"
#include "core/trust.hpp"

namespace mtk::cmds::meta {

namespace {

const char* tier_label(mtk::core::Tier t) noexcept {
    switch (t) {
        case mtk::core::Tier::OrgToml:     return "org";
        case mtk::core::Tier::Builtin:     return "builtin";
        case mtk::core::Tier::UserToml:    return "user";
        case mtk::core::Tier::ProjectToml: return "project";
        case mtk::core::Tier::Fallback:    return "fallback";
    }
    return "unknown";
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

std::string fmt_argv(const std::vector<std::string>& argv, std::size_t max_len = 60) {
    std::string s;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) s += ' ';
        s += argv[i];
        if (s.size() > max_len) {
            s.resize(max_len);
            s += "…";
            break;
        }
    }
    return s;
}

}  // namespace

int run_explain(const std::vector<std::string>& argv) {
    auto reg = mtk::core::build_default_registry();

    if (!argv.empty()) {
        auto match = reg.find(argv);
        std::cout << "Would dispatch:\n";
        if (match.filter) {
            std::cout << "  filter:  " << match.filter->name() << '\n'
                      << "  source:  " << match.filter->source() << '\n';
        } else {
            std::cout << "  (no filter matched -- registry misconfigured)\n";
        }
        std::cout << '\n';
    }

    std::cout << "Registry contents (priority order):\n";
    auto entries = reg.describe();
    for (const auto& e : entries) {
        std::cout << "  " << (e.shadowed ? "[SHADOWED] " : "           ")
                  << e.name
                  << "  (tier=" << tier_label(e.tier)
                  << ", source=" << e.source << ")\n";
    }
    return 0;
}

int run_trust(const std::vector<std::string>& argv) {
    std::filesystem::path target = argv.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(argv[0]);
    auto resolved = mtk::core::trust::canonicalise(target);
    if (resolved.empty()) return mtk::core::exit_codes::kUsage;
    if (mtk::core::trust::add(resolved)) {
        std::cout << "mtk trust: added " << resolved << '\n';
    } else {
        std::cout << "mtk trust: " << resolved << " already trusted (or write failed)\n";
    }
    return 0;
}

int run_untrust(const std::vector<std::string>& argv) {
    std::filesystem::path target = argv.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(argv[0]);
    auto resolved = mtk::core::trust::canonicalise(target);
    if (resolved.empty()) return mtk::core::exit_codes::kUsage;
    if (mtk::core::trust::remove(resolved)) {
        std::cout << "mtk untrust: removed " << resolved << '\n';
    } else {
        std::cout << "mtk untrust: " << resolved << " not in allow-list\n";
    }
    return 0;
}

int run_trusted() {
    auto list = mtk::core::trust::list();
    if (list.empty()) {
        std::ostringstream msg;
        msg << "(no trusted paths; "
            << mtk::core::trust::allowed_projects_file() << " is empty or absent)";
        std::cout << mtk::core::color::dim(msg.str()) << '\n';
        return 0;
    }
    std::cout << "Trusted paths (from "
              << mtk::core::trust::allowed_projects_file() << "):\n";
    for (const auto& p : list) std::cout << "  " << p << '\n';
    return 0;
}

int run_reload() {
    // Per perf critic P2: invalidate the cache. Next mtk invocation will
    // re-parse all TOMLs and rebuild the binary cache on the way out.
    bool removed = mtk::core::filter_cache::invalidate();
    if (removed) {
        std::cout << "mtk reload: cache invalidated ("
                  << mtk::core::filter_cache::cache_file()
                  << " removed). Next invocation re-parses all TOML filters.\n";
    } else {
        std::cout << "mtk reload: no cache to invalidate ("
                  << mtk::core::filter_cache::cache_file()
                  << " does not exist). Next invocation will build one.\n";
    }
    // Eagerly rebuild now so the user sees parse errors immediately
    // rather than on next dispatch.
    (void)mtk::core::build_default_registry();
    std::cout << "mtk reload: cache rebuilt.\n";
    return 0;
}

int run_rewrite(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cerr << "mtk rewrite: no command given\n";
        return mtk::core::exit_codes::kUsage;
    }
    auto reg = mtk::core::build_default_registry();
    auto match = reg.find(argv);
    bool useful_match = match.filter &&
        match.filter->name() != std::string_view("_passthrough");

    if (useful_match) std::cout << "mtk ";
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) std::cout << ' ';
        std::cout << argv[i];
    }
    std::cout << '\n';
    return 0;
}

int run_tail(const std::vector<std::string>& argv) {
    std::size_t n = 10;
    if (!argv.empty()) {
        try { n = std::stoul(argv[0]); } catch (...) {
            std::cerr << "mtk tail: usage: mtk tail [N]\n";
            return mtk::core::exit_codes::kUsage;
        }
    }
    auto events = mtk::core::audit::tail(n);
    if (events.empty()) {

        std::ostringstream msg;
        msg << "(no audit events — "
            << mtk::core::audit::log_file() << " is empty or absent)";
        std::cout << mtk::core::color::dim(msg.str()) << '\n';

        return 0;
    }
    for (const auto& e : events) {
        long savings_pct = 0;
        if (e.bytes_in > 0) {
            savings_pct = 100 - static_cast<long>(
                e.bytes_out * 100 / e.bytes_in);
        }
        std::cout << e.ts << "  " << e.event_id
                  << "  " << e.filter_name
                  << "  exit=" << e.exit_code
                  << "  " << fmt_bytes(e.bytes_in) << "→" << fmt_bytes(e.bytes_out)
                  << " (" << savings_pct << "%)"
                  << "  " << e.elapsed_ms << "ms"
                  << "  " << fmt_argv(e.argv)
                  << '\n';
    }
    return 0;
}

int run_stats() {
    auto events = mtk::core::audit::read_all();
    if (events.empty()) {

        std::ostringstream msg;
        msg << "(no audit events — "
            << mtk::core::audit::log_file() << " is empty or absent)";
        std::cout << mtk::core::color::dim(msg.str()) << '\n';
        
        return 0;
    }

    struct Agg {
        std::size_t count = 0;
        std::size_t bytes_in = 0;
        std::size_t bytes_out = 0;
        std::size_t errors = 0;
        long elapsed_ms_total = 0;
    };
    std::unordered_map<std::string, Agg> by_filter;
    Agg overall;
    for (const auto& e : events) {
        auto& f = by_filter[e.filter_name];
        f.count++;
        f.bytes_in += e.bytes_in;
        f.bytes_out += e.bytes_out;
        if (e.exit_code != 0) f.errors++;
        f.elapsed_ms_total += e.elapsed_ms;

        overall.count++;
        overall.bytes_in += e.bytes_in;
        overall.bytes_out += e.bytes_out;
        if (e.exit_code != 0) overall.errors++;
        overall.elapsed_ms_total += e.elapsed_ms;
    }

    auto pct = [](std::size_t in, std::size_t out) -> long {
        return in > 0 ? 100 - static_cast<long>(out * 100 / in) : 0;
    };

    std::cout << "mtk stats -- " << events.size() << " events in "
              << mtk::core::audit::log_file() << "\n\n";
    std::cout << "Overall:\n"
              << "  bytes_in:   " << fmt_bytes(overall.bytes_in) << "\n"
              << "  bytes_out:  " << fmt_bytes(overall.bytes_out)
              << "  (savings " << pct(overall.bytes_in, overall.bytes_out) << "%)\n"
              << "  errors:     " << overall.errors << " / " << overall.count
              << " (" << (overall.count > 0 ? overall.errors * 100 / overall.count : 0)
              << "%)\n"
              << "  avg time:   "
              << (overall.count > 0 ? overall.elapsed_ms_total / static_cast<long>(overall.count) : 0)
              << "ms\n\n";

    std::vector<std::pair<std::string, Agg>> sorted(by_filter.begin(), by_filter.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

    std::cout << "By filter (sorted by count):\n";
    std::cout << "  count  filter         bytes_in→out      sav%  err  avg_ms\n";
    for (const auto& [name, f] : sorted) {
        std::printf("  %5zu  %-12s  %6s→%-6s  %4ld%% %4zu  %5ld\n",
                    f.count,
                    name.c_str(),
                    fmt_bytes(f.bytes_in).c_str(),
                    fmt_bytes(f.bytes_out).c_str(),
                    pct(f.bytes_in, f.bytes_out),
                    f.errors,
                    f.count > 0 ? f.elapsed_ms_total / static_cast<long>(f.count) : 0);
    }
    return 0;
}

int run_why(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cerr << "mtk why: usage: mtk why <event-id>\n";
        return mtk::core::exit_codes::kUsage;
    }
    auto path = mtk::core::audit::payload_path(argv[0]);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::cout << "[mtk why: no payload captured for " << argv[0] << "]\n"
                  << "  Payload capture is opt-in via MTK_AUDIT_PAYLOAD=1.\n"
                  << "  Re-run the command with that env set to capture future\n"
                  << "  events; today's outputs aren't recoverable.\n"
                  << "  Expected at: " << path << "\n";
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "mtk why: failed to read " << path << '\n';
        return mtk::core::exit_codes::kNotFound;
    }
    std::cout << f.rdbuf();
    return 0;
}

}  // namespace mtk::cmds::meta
