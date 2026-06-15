#include "core/audit.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>

namespace mtk::core::audit {

namespace {

// Per A6 defaults; could move to core/limits.hpp under namespace audit{} later.
constexpr std::size_t kRotationBytes   = 10 * 1024 * 1024;   // 10 MiB
constexpr std::size_t kPayloadMaxBytes = 1  * 1024 * 1024;   // 1 MiB

std::filesystem::path state_dir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
        return std::filesystem::path(xdg) / "mtk";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "state" / "mtk";
    }
    return std::filesystem::path(".local") / "state" / "mtk";
}

std::string format_iso_utc_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void rotate_if_needed(const std::filesystem::path& log) noexcept {
    std::error_code ec;
    auto size = std::filesystem::file_size(log, ec);
    if (ec || size < kRotationBytes) return;
    auto rotated = log;
    rotated += ".1";
    std::filesystem::remove(rotated, ec);
    std::filesystem::rename(log, rotated, ec);
}

std::string serialise(const Event& e) {
    using json = nlohmann::json;
    json j = {
        {"ts",                e.ts},
        {"event_id",          e.event_id},
        {"argv",              e.argv},
        {"filter_name",       e.filter_name},
        {"filter_source",     e.filter_source},
        {"exit_code",         e.exit_code},
        {"bytes_in",          e.bytes_in},
        {"bytes_out",         e.bytes_out},
        {"elapsed_ms",        e.elapsed_ms},
        {"bytes_in_capped",   e.bytes_in_capped},
        {"timed_out",         e.timed_out},
        {"killed_by_signal",  e.killed_by_signal},
    };
    return j.dump();
}

std::optional<Event> deserialise(const std::string& line) {
    using json = nlohmann::json;
    try {
        auto j = json::parse(line);
        Event e;
        e.ts               = j.value("ts", std::string{});
        e.event_id         = j.value("event_id", std::string{});
        e.argv             = j.value("argv", std::vector<std::string>{});
        e.filter_name      = j.value("filter_name", std::string{});
        e.filter_source    = j.value("filter_source", std::string{});
        e.exit_code        = j.value("exit_code", 0);
        e.bytes_in         = j.value("bytes_in", static_cast<std::size_t>(0));
        e.bytes_out        = j.value("bytes_out", static_cast<std::size_t>(0));
        e.elapsed_ms       = j.value("elapsed_ms", 0L);
        e.bytes_in_capped  = j.value("bytes_in_capped", false);
        e.timed_out        = j.value("timed_out", false);
        e.killed_by_signal = j.value("killed_by_signal", 0);
        return e;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

std::filesystem::path log_file() {
    return state_dir() / "events.jsonl";
}

std::filesystem::path payload_dir() {
    return state_dir() / "payloads";
}

bool append(const Event& event) noexcept {
    try {
        auto log = log_file();
        std::error_code ec;
        std::filesystem::create_directories(log.parent_path(), ec);
        rotate_if_needed(log);

        Event stamped = event;
        if (stamped.ts.empty()) stamped.ts = format_iso_utc_now();

        // Single-line JSON; trailing newline. std::ofstream::app maps to
        // O_APPEND on POSIX, which means writes are atomic up to PIPE_BUF
        // (typically 4 KiB) — comfortably above our per-event size.
        std::ofstream f(log, std::ios::app);
        if (!f) return false;
        f << serialise(stamped) << '\n';
        return f.good();
    } catch (...) {
        return false;
    }
}

std::vector<Event> read_all() {
    std::vector<Event> events;
    std::ifstream f(log_file());
    if (!f) return events;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (auto e = deserialise(line)) events.push_back(std::move(*e));
    }
    return events;
}

std::vector<Event> tail(std::size_t n) {
    auto all = read_all();
    if (all.size() <= n) return all;
    return std::vector<Event>(all.end() - static_cast<std::ptrdiff_t>(n), all.end());
}

std::string make_event_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    char buf[20];
    std::snprintf(buf, sizeof(buf), "ev_%012llx",
                  static_cast<unsigned long long>(dist(rng) & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::optional<std::filesystem::path> capture_payload(
    const std::string& event_id, std::string_view data) noexcept {
    try {
        const char* enabled = std::getenv("MTK_AUDIT_PAYLOAD");
        if (!enabled || std::string_view(enabled) != "1") return std::nullopt;
        if (event_id.empty()) return std::nullopt;

        auto path = payload_path(event_id);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return std::nullopt;

        auto to_write = data.substr(0, std::min(data.size(), kPayloadMaxBytes));
        f.write(to_write.data(), static_cast<std::streamsize>(to_write.size()));
        if (!f.good()) return std::nullopt;
        return path;
    } catch (...) {
        return std::nullopt;
    }
}

std::filesystem::path payload_path(const std::string& event_id) {
    return payload_dir() / (event_id + ".txt");
}

}  // namespace mtk::core::audit
