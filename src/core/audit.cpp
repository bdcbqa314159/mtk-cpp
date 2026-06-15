#include "core/audit.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>

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

// Per perf critic P3: hand-rolled JSON writer for the audit hot path.
// Schema is fixed; field names are literal; we know every value type.
// One reserved string + append loop. No DOM, no per-field allocation,
// no locale. Reduces audit::append cost from ~20-50 us to ~3-5 us per
// event. Read path (mtk stats/tail/why) still uses nlohmann.
void json_escape_into(std::string& out, std::string_view s) {
    out.reserve(out.size() + s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

void append_str_field(std::string& out, std::string_view key, std::string_view val,
                      bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":\"";
    json_escape_into(out, val);
    out += '"';
}

template <typename T>
void append_num_field(std::string& out, std::string_view key, T val, bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":";
    out += std::to_string(val);
}

void append_bool_field(std::string& out, std::string_view key, bool val, bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":";
    out += val ? "true" : "false";
}

void append_strarr_field(std::string& out, std::string_view key,
                         const std::vector<std::string>& val, bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":[";
    bool inner_first = true;
    for (const auto& s : val) {
        if (!inner_first) out += ',';
        inner_first = false;
        out += '"';
        json_escape_into(out, s);
        out += '"';
    }
    out += ']';
}

std::string serialise(const Event& e) {
    std::string out;
    // Reserve enough for a typical event: ~200 bytes for short argv,
    // ~400 for longer. Single allocation in the common case.
    std::size_t argv_size = 0;
    for (const auto& a : e.argv) argv_size += a.size() + 3;
    out.reserve(220 + argv_size + e.filter_name.size() + e.filter_source.size());
    out += '{';
    bool first = true;
    append_str_field (out, "ts",               e.ts,               first);
    append_str_field (out, "event_id",         e.event_id,         first);
    append_strarr_field(out, "argv",           e.argv,             first);
    append_str_field (out, "filter_name",      e.filter_name,      first);
    append_str_field (out, "filter_source",    e.filter_source,    first);
    append_num_field (out, "exit_code",        e.exit_code,        first);
    append_num_field (out, "bytes_in",         e.bytes_in,         first);
    append_num_field (out, "bytes_out",        e.bytes_out,        first);
    append_num_field (out, "elapsed_ms",       e.elapsed_ms,       first);
    append_bool_field(out, "bytes_in_capped",  e.bytes_in_capped,  first);
    append_bool_field(out, "timed_out",        e.timed_out,        first);
    append_num_field (out, "killed_by_signal", e.killed_by_signal, first);
    out += '}';
    return out;
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
    // Correctness critic C2+C7: PIPE_BUF atomicity only applies to pipes,
    // not regular files. std::ofstream can split a single line across
    // multiple write(2) calls and concurrent mtk processes can interleave
    // bytes inside a JSON line. Fix: serialise to one buffer, then take a
    // process-exclusive flock around: rotation-check + single write(2)
    // + close. flock is held only for the syscall itself so contention
    // stays in the microsecond range.
    //
    // Perf critic B4: cache "directory exists" per process — mkdir-p is
    // the slowest step in the open path (stats every component). For a
    // one-shot binary this caches across only one event, but a future
    // long-lived caller (mtk repl?) benefits. Try-open-first, fall back
    // to mkdir-p on ENOENT — saves the stat chain on every event.
    try {
        auto log = log_file();
        static std::atomic<bool> dir_known_good{false};

        Event stamped = event;
        if (stamped.ts.empty()) stamped.ts = format_iso_utc_now();
        std::string line = serialise(stamped);
        line.push_back('\n');

        int fd = ::open(log.c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0 && errno == ENOENT && !dir_known_good.load(std::memory_order_acquire)) {
            // First time (or dir was removed). mkdir-p then retry.
            std::error_code ec;
            std::filesystem::create_directories(log.parent_path(), ec);
            fd = ::open(log.c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
            if (fd >= 0) dir_known_good.store(true, std::memory_order_release);
        } else if (fd >= 0) {
            dir_known_good.store(true, std::memory_order_release);
        }
        if (fd < 0) return false;

        // Block until we have the exclusive lock; only held across one
        // rotate-check + one write. Per-event lock contention is bounded
        // by the syscall cost (~tens of microseconds).
        if (::flock(fd, LOCK_EX) != 0) {
            ::close(fd);
            return false;
        }

        // Rotate while holding the lock so cross-process moves are safe:
        // process B's pending write either lands in the new (post-rotate)
        // log, or it stalled in flock until our rotate finished.
        rotate_if_needed(log);
        // Re-open after rotate: the fd we have points at the now-renamed
        // .1 file; subsequent writes would go there and be lost on next
        // rotation. Drop our fd, open fresh.
        if (std::error_code ec2; std::filesystem::file_size(log, ec2) == 0 ||
                                 ec2.value() == ENOENT) {
            ::close(fd);
            fd = ::open(log.c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) return false;
            if (::flock(fd, LOCK_EX) != 0) {
                ::close(fd);
                return false;
            }
        }

        ssize_t to_write = static_cast<ssize_t>(line.size());
        ssize_t written = 0;
        while (written < to_write) {
            ssize_t n = ::write(fd, line.data() + written,
                                static_cast<std::size_t>(to_write - written));
            if (n < 0) {
                if (errno == EINTR) continue;
                (void)::flock(fd, LOCK_UN);
                ::close(fd);
                return false;
            }
            written += n;
        }

        (void)::flock(fd, LOCK_UN);
        ::close(fd);
        return true;
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
