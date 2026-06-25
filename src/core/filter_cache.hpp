#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/toml_filter.hpp"

namespace mtk::core::filter_cache {

// Per perf critic P2: skip directory scan + TOML parse on every invocation
// by caching the parsed Filter structs to a binary file. Cache is keyed on
// (mtime, size) of every source TOML file. On invalidation (any source
// changed / new file appeared / file removed) the cache is rebuilt.
//
// The compiled regex fields (*_re) are NOT serialised — they're rebuilt
// via toml_filter::compile() after deserialisation, so the cache saves
// the TOML parse cost but not the regex compile cost. (TOML parse dominates
// on typical configs.)

[[nodiscard]] std::filesystem::path cache_file();

// Bundle of sources contributing to a cache snapshot.
struct SourceManifest {
    struct Entry {
        std::string path;            // absolute, for cross-process stability
        std::int64_t mtime_seconds;  // POSIX mtime (epoch seconds)
        std::uint64_t size_bytes;
    };
    std::vector<Entry> sources;
};

// Returns true if the cache at `cache_file()` is consistent with the given
// manifest — every entry's mtime+size matches the on-disk file.
// Returns false on missing cache, mismatched magic/version, manifest
// mismatch, or any I/O error.
[[nodiscard]] bool is_valid(const SourceManifest& current);

// Cached filters split by tier so the caller registers them at the right
// priority. compile() is run on each Filter at load time.
struct LoadResult {
    std::vector<toml_filter::Filter> user_filters;
    std::vector<toml_filter::Filter> project_filters;
};

[[nodiscard]] LoadResult load();

// Writes the given manifest + filter sets to the cache. Best-effort
// (silent on I/O error). Concurrent writers are serialised via flock.
void save(const SourceManifest& manifest,
          const std::vector<toml_filter::Filter>& user_filters,
          const std::vector<toml_filter::Filter>& project_filters) noexcept;

// Removes the cache file. Used by `mtk reload` to force a rebuild on next
// invocation. Returns true if a file was removed.
bool invalidate() noexcept;

// Builds a manifest by stat()ing each path. Files that don't exist are
// recorded with mtime=0, size=0 — the manifest still encodes their absence
// so a later create-of-that-file invalidates the cache.
[[nodiscard]] SourceManifest make_manifest(const std::vector<std::filesystem::path>& paths);

}  // namespace mtk::core::filter_cache
