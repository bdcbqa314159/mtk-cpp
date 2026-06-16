#include "core/filter_cache.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace mtk::core::filter_cache {

namespace {

// File format magic + version. Bump version when the on-disk layout
// changes; old caches with wrong version are treated as invalid (rebuilt).
constexpr char kMagic[4] = {'M', 'T', 'K', 'F'};
// v2: added Filter.locked field (A7 layered config). Older caches are
// invalidated and rebuilt — single per-invocation parse, no migration.
constexpr std::uint32_t kVersion = 2;

std::filesystem::path cache_dir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path(xdg) / "mtk";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".cache" / "mtk";
    }
    return std::filesystem::path(".cache") / "mtk";
}

// --- Tiny binary reader/writer ---

struct Writer {
    std::string buf;

    void u8(std::uint8_t v)       { buf.push_back(static_cast<char>(v)); }
    void u32(std::uint32_t v)     { buf.append(reinterpret_cast<const char*>(&v), 4); }
    void u64(std::uint64_t v)     { buf.append(reinterpret_cast<const char*>(&v), 8); }
    void i64(std::int64_t v)      { buf.append(reinterpret_cast<const char*>(&v), 8); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); buf.append(s); }
    void bytes(const char* p, std::size_t n) { buf.append(p, n); }
};

struct Reader {
    const std::string& data;
    std::size_t pos = 0;
    bool ok = true;

    explicit Reader(const std::string& d) : data(d) {}

    bool need(std::size_t n) {
        if (!ok || pos + n > data.size()) { ok = false; return false; }
        return true;
    }
    std::uint8_t u8()      { if (!need(1)) return 0; return static_cast<std::uint8_t>(data[pos++]); }
    std::uint32_t u32()    { if (!need(4)) return 0; std::uint32_t v; std::memcpy(&v, data.data()+pos, 4); pos += 4; return v; }
    std::uint64_t u64()    { if (!need(8)) return 0; std::uint64_t v; std::memcpy(&v, data.data()+pos, 8); pos += 8; return v; }
    std::int64_t i64()     { if (!need(8)) return 0; std::int64_t v; std::memcpy(&v, data.data()+pos, 8); pos += 8; return v; }
    std::string str() {
        auto n = u32();
        if (!need(n)) return {};
        std::string s(data.data() + pos, n);
        pos += n;
        return s;
    }
};

void write_filter(Writer& w, const toml_filter::Filter& f) {
    w.str(f.name);
    w.str(f.description);
    w.str(f.match_command_pattern);
    w.u8(f.filter_stderr ? 1 : 0);
    w.u8(f.strip_ansi ? 1 : 0);
    w.u8(f.locked ? 1 : 0);

    w.u32(static_cast<std::uint32_t>(f.replace.size()));
    for (const auto& [p, r] : f.replace) { w.str(p); w.str(r); }

    w.u8(f.match_output ? 1 : 0);
    if (f.match_output) { w.str(f.match_output->first); w.str(f.match_output->second); }

    w.u32(static_cast<std::uint32_t>(f.strip_lines_matching.size()));
    for (const auto& p : f.strip_lines_matching) w.str(p);

    w.u32(static_cast<std::uint32_t>(f.keep_lines_matching.size()));
    for (const auto& p : f.keep_lines_matching) w.str(p);

    auto opt_u64 = [&](const std::optional<std::size_t>& v) {
        w.u8(v ? 1 : 0);
        if (v) w.u64(static_cast<std::uint64_t>(*v));
    };
    opt_u64(f.truncate_lines_at);
    opt_u64(f.head_lines);
    opt_u64(f.tail_lines);
    opt_u64(f.max_lines);

    w.u8(f.on_empty ? 1 : 0);
    if (f.on_empty) w.str(*f.on_empty);
}

toml_filter::Filter read_filter(Reader& r) {
    toml_filter::Filter f;
    f.name = r.str();
    f.description = r.str();
    f.match_command_pattern = r.str();
    f.filter_stderr = r.u8() != 0;
    f.strip_ansi = r.u8() != 0;
    f.locked = r.u8() != 0;

    auto n = r.u32();
    f.replace.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto p = r.str(); auto rep = r.str();
        f.replace.emplace_back(std::move(p), std::move(rep));
    }

    if (r.u8()) {
        auto p = r.str(); auto m = r.str();
        f.match_output = std::make_pair(std::move(p), std::move(m));
    }

    n = r.u32();
    f.strip_lines_matching.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) f.strip_lines_matching.push_back(r.str());

    n = r.u32();
    f.keep_lines_matching.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) f.keep_lines_matching.push_back(r.str());

    auto read_opt_u64 = [&]() -> std::optional<std::size_t> {
        if (r.u8()) return static_cast<std::size_t>(r.u64());
        return std::nullopt;
    };
    f.truncate_lines_at = read_opt_u64();
    f.head_lines = read_opt_u64();
    f.tail_lines = read_opt_u64();
    f.max_lines = read_opt_u64();

    if (r.u8()) f.on_empty = r.str();
    return f;
}

bool read_file_contents(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) f.read(out.data(), size);
    return f.good() || f.eof();
}

bool check_manifest_against(const std::string& raw, const SourceManifest& current) {
    if (raw.size() < 4) return false;
    if (std::memcmp(raw.data(), kMagic, 4) != 0) return false;

    Reader r(raw);
    r.pos = 4;  // skip magic
    auto ver = r.u32();
    if (!r.ok || ver != kVersion) return false;

    auto src_count = r.u32();
    if (!r.ok) return false;
    if (src_count != current.sources.size()) return false;

    for (std::uint32_t i = 0; i < src_count; ++i) {
        auto path = r.str();
        auto mtime = r.i64();
        auto size = r.u64();
        if (!r.ok) return false;
        const auto& cur = current.sources[i];
        if (path != cur.path || mtime != cur.mtime_seconds || size != cur.size_bytes) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::filesystem::path cache_file() {
    return cache_dir() / "filters.bin";
}

SourceManifest make_manifest(const std::vector<std::filesystem::path>& paths) {
    SourceManifest m;
    m.sources.reserve(paths.size());
    for (const auto& p : paths) {
        SourceManifest::Entry e;
        e.path = p.string();
        struct stat st {};
        if (::stat(p.c_str(), &st) == 0) {
            e.mtime_seconds = static_cast<std::int64_t>(st.st_mtime);
            e.size_bytes = static_cast<std::uint64_t>(st.st_size);
        } else {
            e.mtime_seconds = 0;
            e.size_bytes = 0;
        }
        m.sources.push_back(std::move(e));
    }
    return m;
}

bool is_valid(const SourceManifest& current) {
    std::string raw;
    if (!read_file_contents(cache_file(), raw)) return false;
    return check_manifest_against(raw, current);
}

namespace {

std::vector<toml_filter::Filter> read_filter_list(Reader& r) {
    std::vector<toml_filter::Filter> out;
    auto n = r.u32();
    if (!r.ok) return out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto f = read_filter(r);
        if (!r.ok) { out.clear(); return out; }
        toml_filter::compile(f);
        out.push_back(std::move(f));
    }
    return out;
}

void write_filter_list(Writer& w, const std::vector<toml_filter::Filter>& v) {
    w.u32(static_cast<std::uint32_t>(v.size()));
    for (const auto& f : v) write_filter(w, f);
}

}  // namespace

LoadResult load() {
    LoadResult result;
    std::string raw;
    if (!read_file_contents(cache_file(), raw)) return result;
    if (raw.size() < 4) return result;
    if (std::memcmp(raw.data(), kMagic, 4) != 0) return result;

    Reader r(raw);
    r.pos = 4;
    auto ver = r.u32();
    if (!r.ok || ver != kVersion) return result;

    auto src_count = r.u32();
    if (!r.ok) return result;
    for (std::uint32_t i = 0; i < src_count; ++i) {
        (void)r.str();
        (void)r.i64();
        (void)r.u64();
    }
    if (!r.ok) return result;

    result.user_filters = read_filter_list(r);
    if (!r.ok) { result.user_filters.clear(); return result; }
    result.project_filters = read_filter_list(r);
    if (!r.ok) {
        result.user_filters.clear();
        result.project_filters.clear();
    }
    return result;
}

void save(const SourceManifest& manifest,
          const std::vector<toml_filter::Filter>& user_filters,
          const std::vector<toml_filter::Filter>& project_filters) noexcept {
    try {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir(), ec);

        Writer w;
        w.bytes(kMagic, 4);
        w.u32(kVersion);
        w.u32(static_cast<std::uint32_t>(manifest.sources.size()));
        for (const auto& e : manifest.sources) {
            w.str(e.path);
            w.i64(e.mtime_seconds);
            w.u64(e.size_bytes);
        }
        write_filter_list(w, user_filters);
        write_filter_list(w, project_filters);

        // Per correctness critic C-RoundE-1: write to per-PID tmp then
        // rename(2). The previous O_TRUNC + streaming write left a window
        // where a crash mid-write produced an intact magic+version+manifest
        // but a truncated filter payload; load() then silently dropped
        // every TOML filter until `mtk reload` was run. Same pattern as
        // trust.cpp:50.
        auto path = cache_file();
        auto tmp = path;
        tmp += ".tmp." + std::to_string(static_cast<long>(::getpid()));

        int fd = ::open(tmp.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return;

        ssize_t remaining = static_cast<ssize_t>(w.buf.size());
        const char* p = w.buf.data();
        bool write_ok = true;
        while (remaining > 0) {
            ssize_t n = ::write(fd, p, static_cast<std::size_t>(remaining));
            if (n < 0) {
                if (errno == EINTR) continue;
                write_ok = false;
                break;
            }
            p += n;
            remaining -= n;
        }
        ::close(fd);

        if (!write_ok) {
            std::filesystem::remove(tmp, ec);  // best-effort cleanup
            return;
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);  // best-effort cleanup
        }
    } catch (...) {
    }
}

bool invalidate() noexcept {
    std::error_code ec;
    bool existed = std::filesystem::exists(cache_file(), ec);
    if (existed) std::filesystem::remove(cache_file(), ec);
    return existed && !ec;
}

}  // namespace mtk::core::filter_cache
