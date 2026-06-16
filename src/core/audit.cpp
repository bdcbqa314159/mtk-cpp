#include "core/audit.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "core/platform/file_lock.hpp"
#include "core/platform/paths.hpp"
#include "core/platform/process_id.hpp"

namespace mtk::core::audit {

namespace {

// Per A6 defaults; could move to core/limits.hpp under namespace audit{} later.
constexpr std::size_t kRotationBytes   = 10 * 1024 * 1024;   // 10 MiB
constexpr std::size_t kPayloadMaxBytes = 1  * 1024 * 1024;   // 1 MiB

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
    return mtk::core::platform::state_dir() / "events.jsonl";
}

std::filesystem::path payload_dir() {
    return mtk::core::platform::state_dir() / "payloads";
}

namespace {

// Open the log for append, creating it (and the parent dir) if needed.
// Returns -1 on failure. Caches "directory exists" via an atomic_flag
// so the mkdir-p stat chain runs at most once per process (B4).
int open_log_append(const std::filesystem::path& log) noexcept {
    static std::atomic<bool> dir_known_good{false};
    int fd = ::open(log.c_str(),
                    O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0 && errno == ENOENT &&
        !dir_known_good.load(std::memory_order_acquire)) {
        std::error_code ec;
        std::filesystem::create_directories(log.parent_path(), ec);
        fd = ::open(log.c_str(),
                    O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    }
    if (fd >= 0) dir_known_good.store(true, std::memory_order_release);
    return fd;
}

// After rotate_if_needed moved the log aside, our fd may point at the
// renamed .1 file. Drop it and reopen the canonical path. Returns the
// new fd (with the exclusive lock re-acquired) or -1 on failure.
// Caller must release the lock on the returned fd.
//
// Per correctness critic C-RoundE-2: identity-compare the open handle
// against the on-disk path. The previous file_size==0 || ENOENT check
// only caught the case where *we* did the rotation. A different process
// that rotated between our open() and our lock acquisition leaves the
// canonical log non-empty with our fd still pointing at .1; the loser
// then writes events into the rotated file where they're invisible to
// readers. `platform::same_file` handles the POSIX inode / Windows
// file-index check uniformly.
int reopen_after_rotate(int fd, const std::filesystem::path& log) noexcept {
    if (mtk::core::platform::same_file(fd, log)) return fd;
    ::close(fd);
    fd = ::open(log.c_str(),
                O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (!mtk::core::platform::lock_exclusive(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// EINTR-safe write of `data` to `fd`. Returns true if all bytes
// landed, false on any non-EINTR write error.
bool write_all(int fd, std::string_view data) noexcept {
    auto remaining = data.size();
    const char* p = data.data();
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

// Append a single JSONL event. The locking protocol guarantees that
// concurrent mtk processes do not interleave bytes inside a record:
// each holds LOCK_EX across rotate-check + write(2) + close. Per
// audit, the orchestration is split into three named helpers
// (open_log_append, reopen_after_rotate, write_all) so the protocol
// reads as a transcript.
bool append(const Event& event) noexcept {
    try {
        auto log = log_file();

        Event stamped = event;
        if (stamped.ts.empty()) stamped.ts = format_iso_utc_now();
        std::string line = serialise(stamped);
        line.push_back('\n');

        int fd = open_log_append(log);
        if (fd < 0) return false;

        if (!mtk::core::platform::lock_exclusive(fd)) {
            ::close(fd);
            return false;
        }

        // Rotation must happen under the lock so cross-process moves
        // are safe: process B's pending write either lands in the new
        // (post-rotate) log, or it stalled on lock acquisition until
        // our rotate finished.
        rotate_if_needed(log);
        fd = reopen_after_rotate(fd, log);
        if (fd < 0) return false;

        const bool ok = write_all(fd, line);

        mtk::core::platform::unlock(fd);
        ::close(fd);
        return ok;
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
    // Per perf critic P2 (Round F): SplitMix64 with a clock^pid seed
    // costs one uint64_t of state and no syscalls. The previous
    // mt19937_64 + random_device cost ~5-10 µs on first use (random_device
    // opens /dev/urandom on macOS+libc++; the 5008-byte mt state init is
    // a separate hit). Event-id collision space (48 bits) is unchanged;
    // statistical quality is fine for non-cryptographic uniqueness.
    static thread_local std::uint64_t state = []() noexcept -> std::uint64_t {
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        return static_cast<std::uint64_t>(t)
             ^ (static_cast<std::uint64_t>(mtk::core::platform::current_pid()) << 32)
             ^ 0x9E3779B97F4A7C15ULL;
    }();
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    char buf[20];
    std::snprintf(buf, sizeof(buf), "ev_%012llx",
                  static_cast<unsigned long long>(z & 0xFFFFFFFFFFFFULL));
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
