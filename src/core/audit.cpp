#include "core/audit.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>  // for fileno
#endif

#include "core/platform/file_lock.hpp"
#include "core/platform/paths.hpp"
#include "core/platform/process_id.hpp"
#include "core/utils.hpp"

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

void append_str_field(std::string& out, std::string_view key, std::string_view val,
                      bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":\"";
    mtk::core::utils::json_escape_into(out, val);
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
        mtk::core::utils::json_escape_into(out, s);
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

// Extract the platform-native file handle from a FILE* so we can pass it
// to platform::lock_exclusive / same_file. Both POSIX `fileno` and the
// Windows `_get_osfhandle(_fileno(...))` pair are documented "give me
// the underlying OS handle"; the FILE* remains owner and free is via
// fclose. The handle is invalidated when the FILE* is closed.
mtk::core::platform::native_file_handle native_handle_of(std::FILE* f) noexcept {
#if defined(_WIN32)
    int fd = ::_fileno(f);
    if (fd < 0) return mtk::core::platform::kInvalidFileHandle;
    auto h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    return (h == reinterpret_cast<HANDLE>(-1)) ? mtk::core::platform::kInvalidFileHandle : h;
#else
    return ::fileno(f);
#endif
}

// Open the log for append, creating it (and the parent dir) if needed.
// Returns nullptr on failure. Caches "directory exists" via an atomic
// flag so the mkdir-p stat chain runs at most once per process (B4).
std::FILE* open_log_append(const std::filesystem::path& log) noexcept {
    static std::atomic<bool> dir_known_good{false};
    // "ab": binary + append. POSIX-mandated O_APPEND semantics (atomic
    // append from concurrent writers); Windows MSVCRT honours the same
    // contract.
    std::FILE* f = std::fopen(log.string().c_str(), "ab");
    if (!f && !dir_known_good.load(std::memory_order_acquire)) {
        std::error_code ec;
        std::filesystem::create_directories(log.parent_path(), ec);
        f = std::fopen(log.string().c_str(), "ab");
    }
    if (f) dir_known_good.store(true, std::memory_order_release);
    return f;
}

// After rotate_if_needed moved the log aside, our FILE* may point at the
// renamed .1 file. Drop it and reopen the canonical path. Returns the
// new FILE* (with the exclusive lock re-acquired) or nullptr on failure.
//
// Per correctness critic C-RoundE-2: identity-compare the open handle
// against the on-disk path. `platform::same_file` handles the POSIX
// inode / Windows file-index check uniformly.
std::FILE* reopen_after_rotate(std::FILE* f, const std::filesystem::path& log) noexcept {
    if (mtk::core::platform::same_file(native_handle_of(f), log)) return f;
    std::fclose(f);
    f = std::fopen(log.string().c_str(), "ab");
    if (!f) return nullptr;
    if (!mtk::core::platform::lock_exclusive(native_handle_of(f))) {
        std::fclose(f);
        return nullptr;
    }
    return f;
}

// Write the full record. fwrite returns items written; modern libcs
// (glibc, libc++, MSVCRT) retry internally on EINTR / partial writes,
// so a single call is sufficient.
bool write_all(std::FILE* f, std::string_view data) noexcept {
    return std::fwrite(data.data(), 1, data.size(), f) == data.size();
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

        std::FILE* f = open_log_append(log);
        if (!f) return false;

        if (!mtk::core::platform::lock_exclusive(native_handle_of(f))) {
            std::fclose(f);
            return false;
        }

        // Rotation must happen under the lock so cross-process moves
        // are safe: process B's pending write either lands in the new
        // (post-rotate) log, or it stalled on lock acquisition until
        // our rotate finished.
        rotate_if_needed(log);
        f = reopen_after_rotate(f, log);
        if (!f) return false;

        const bool ok = write_all(f, line);
        // Drain libc buffer to the kernel before releasing the lock —
        // otherwise another process could acquire the lock and read
        // a partially-written record.
        std::fflush(f);

        mtk::core::platform::unlock(native_handle_of(f));
        std::fclose(f);
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
