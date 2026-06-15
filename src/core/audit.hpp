#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mtk::core::audit {

// Per A5: schema is COMMITTED. Adding fields is allowed (consumers must
// tolerate unknown keys, which nlohmann::json::value()-with-default does).
// Removing or renaming fields is a breaking change requiring a major
// version bump.
struct Event {
    std::string event_id;            // "ev_" + 12 hex chars
    std::string ts;                  // ISO 8601 UTC, e.g. "2026-06-14T13:00:00Z"
    std::vector<std::string> argv;   // full command (argv[0] is the cmd name)
    std::string filter_name;         // Filter::name() that handled it
    std::string filter_source;       // Filter::source() (builtin / user:X / project:X)
    int exit_code = 0;
    std::size_t bytes_in = 0;        // cumulative captured bytes (stdout+stderr)
    std::size_t bytes_out = 0;       // bytes emitted (after filter)
    long elapsed_ms = 0;             // wall-clock for Filter::run
    bool bytes_in_capped = false;    // A6: capture exceeded max_bytes
    bool timed_out = false;          // A6: capture exceeded timeout_ms
    int killed_by_signal = 0;        // A11: signal that killed child (0 = clean)
};

// Path to the audit log file (`~/.local/state/mtk/events.jsonl` by default,
// honours $XDG_STATE_HOME). Directory is created on first append.
[[nodiscard]] std::filesystem::path log_file();

// Path to the payload directory (`~/.local/state/mtk/payloads/`).
[[nodiscard]] std::filesystem::path payload_dir();

// Single-line JSON append to log_file(). Rotates (moves to .1, dropping
// any prior .1) when the log exceeds `audit.max_log_bytes` (10 MiB default).
// Returns true on success. Noexcept — failures are silent because audit
// must never break the user's command.
bool append(const Event& event) noexcept;

// Read all events from the log (excluding the rotated .1 file). Skips
// malformed lines silently. O(file size).
[[nodiscard]] std::vector<Event> read_all();

// Read the last N events. Returns at most N.
[[nodiscard]] std::vector<Event> tail(std::size_t n);

// Generate a unique event_id. Form: "ev_" + 12 hex chars (48 bits of
// entropy from std::random_device-seeded mt19937_64). Collision risk
// negligible for any practical event volume.
[[nodiscard]] std::string make_event_id();

// If MTK_AUDIT_PAYLOAD=1 is set, write `data` (truncated to
// audit.payload_max_bytes, 1 MiB default) to `payload_dir()/<event_id>.txt`
// and return the path. Otherwise returns nullopt (no payload captured).
// Noexcept — silent on failure.
[[nodiscard]] std::optional<std::filesystem::path> capture_payload(
    const std::string& event_id, std::string_view data) noexcept;

// Returns the canonical payload path for an event_id, regardless of
// whether the file actually exists. Caller can `std::filesystem::exists`
// to check.
[[nodiscard]] std::filesystem::path payload_path(const std::string& event_id);

}  // namespace mtk::core::audit
