#include "cmds/hook.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/registry.hpp"

namespace mtk::cmds::hook {

namespace internal {

// Strip a leading UTF-8 BOM (some Windows hosts prepend one to hook stdin).
std::string_view strip_bom(std::string_view s) noexcept {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.remove_prefix(3);
    }
    return s;
}

// Naïve shell tokenisation — splits on whitespace, respecting double and
// single quotes. Enough to feed `mtk rewrite`'s registry lookup. Does NOT
// handle shell features (pipes, redirects, expansions, heredocs) — if any
// of those appear, the caller falls through unchanged.
std::vector<std::string> tokenise(std::string_view cmd) {
    std::vector<std::string> tokens;
    std::string cur;
    char quote = 0;
    for (char c : cmd) {
        if (quote == 0 && (c == '"' || c == '\'')) {
            quote = c;
            continue;
        }
        if (quote != 0 && c == quote) {
            quote = 0;
            continue;
        }
        if (quote == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

bool contains_shell_metachar(std::string_view cmd) noexcept {
    // Conservative: any of these means we shouldn't try to rewrite (the
    // shell will run it inside its own context). Returning false here
    // forces a passthrough — the agent runs the command unmodified.
    for (char c : cmd) {
        if (c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
            c == '`' || c == '$' || c == '(' || c == ')' || c == '\n') {
            return true;
        }
    }
    return false;
}

// Returns true if `tok` looks like a shell environment-variable assignment
// (NAME=value) — i.e. starts with [A-Za-z_], contains '=' before any other
// special char. Per POSIX shell grammar, these prefixes don't change the
// real command (they just set env for the spawned process).
bool is_env_assignment(std::string_view tok) noexcept {
    if (tok.empty()) return false;
    char c0 = tok.front();
    if (!(std::isalpha(static_cast<unsigned char>(c0)) || c0 == '_')) return false;
    auto eq = tok.find('=');
    if (eq == std::string_view::npos) return false;
    for (std::size_t i = 0; i < eq; ++i) {
        char c = tok[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

// True if `tok` is a "wrapper" that's semantically transparent for the
// purposes of deciding whether the real command would benefit from filtering.
// `env` and `sudo` are the common ones; `nice`/`time`/`/usr/bin/env` too.
bool is_transparent_wrapper(std::string_view tok) noexcept {
    return tok == "env" || tok == "/usr/bin/env" ||
           tok == "sudo" || tok == "doas" ||
           tok == "nice" || tok == "time" ||
           tok == "stdbuf";
}

// Strip leading env-assignments and transparent wrappers, returning the
// index of the actual command in `argv`. Per correctness critic C5:
// `FOO=bar git log`, `env FOO=bar git log`, `sudo git log` should all
// trigger the git_log filter, not silently passthrough.
std::size_t skip_leading_noops(const std::vector<std::string>& argv) noexcept {
    std::size_t i = 0;
    while (i < argv.size()) {
        if (is_env_assignment(argv[i])) { ++i; continue; }
        if (is_transparent_wrapper(argv[i])) {
            ++i;
            // After `env`/`sudo`, more env-assignments may follow.
            while (i < argv.size() && is_env_assignment(argv[i])) ++i;
            // For sudo specifically, common flags like `-E`/`-u user` precede
            // the real command. Skip a small set of well-known no-arg flags;
            // give up if we'd guess wrong (return current index, treat next
            // token as the command — may miss rewrite but never wrong-wrap).
            while (i < argv.size() && !argv[i].empty() && argv[i][0] == '-' &&
                   (argv[i] == "-E" || argv[i] == "-H" || argv[i] == "-S" ||
                    argv[i] == "-n" || argv[i] == "-A" || argv[i] == "--")) {
                if (argv[i] == "--") { ++i; break; }
                ++i;
            }
            continue;
        }
        break;
    }
    return i;
}

// Decide whether the given shell command would benefit from being routed
// through mtk. Returns the rewritten string, or std::nullopt if no rewrite
// is warranted.
std::optional<std::string> decide_rewrite(const std::string& cmd) {
    if (cmd.empty()) return std::nullopt;
    if (contains_shell_metachar(cmd)) return std::nullopt;

    auto argv = tokenise(cmd);
    if (argv.empty()) return std::nullopt;

    // Avoid rewriting commands already prefixed with mtk (idempotent hook).
    if (argv[0] == "mtk") return std::nullopt;

    // Peel `FOO=bar`, `env FOO=bar`, `sudo -E`, etc. — the filter should
    // match against the REAL command, not the wrapper.
    std::size_t cmd_idx = skip_leading_noops(argv);
    if (cmd_idx >= argv.size()) return std::nullopt;

    // Match against the effective argv (without the wrapper prefix).
    std::vector<std::string> effective(argv.begin() + cmd_idx, argv.end());
    if (effective[0] == "mtk") return std::nullopt;  // mtk already in the middle

    auto reg = mtk::core::build_default_registry();
    auto match = reg.find(effective);
    if (!match.filter) return std::nullopt;
    if (match.filter->name() == std::string_view("_passthrough")) return std::nullopt;

    // Wrap by injecting `mtk` immediately before the effective command —
    // preserving any leading env-assignments / sudo / etc.
    // For simplicity we rebuild as: <prefix> mtk <real cmd...>
    std::string out;
    for (std::size_t j = 0; j < cmd_idx; ++j) {
        if (j > 0) out += ' ';
        out += argv[j];
    }
    if (cmd_idx > 0) out += ' ';
    out += "mtk";
    for (const auto& tok : effective) {
        out += ' ';
        out += tok;
    }
    return out;
}

}  // namespace internal

namespace {

using json = nlohmann::json;
using internal::decide_rewrite;
using internal::strip_bom;

// Schema handlers: each returns `true` if it recognises the shape
// (i.e. `v` is its format). If so, it may mutate `v` to apply a
// rewrite; if the shape is recognised but no rewrite applies (wrong
// tool, missing command field, etc.), it leaves `v` unchanged and
// still returns true. Returns `false` only when the shape is not its
// schema — letting the caller try the next handler. The handlers do
// NOT emit; emission is a single point in run_copilot.

// VS Code Copilot Chat:
// {"tool_name": "Bash"|"runTerminalCommand", "tool_input": {"command": "..."}}
bool try_handle_vscode(json& v) {
    auto tool_name_it = v.find("tool_name");
    if (tool_name_it == v.end() || !tool_name_it->is_string()) return false;

    const auto tool_name = tool_name_it->get<std::string>();
    if (tool_name != "Bash" && tool_name != "bash" &&
        tool_name != "runTerminalCommand") {
        return true;  // VS Code shape, but not a Bash variant — leave `v` as-is.
    }

    auto& input = v["tool_input"];
    if (!input.is_object()) return true;
    auto cmd_it = input.find("command");
    if (cmd_it == input.end() || !cmd_it->is_string()) return true;

    if (auto rewritten = decide_rewrite(cmd_it->get<std::string>())) {
        input["command"] = *rewritten;
    }
    return true;
}

// Copilot CLI:
// {"toolName": "bash", "toolArgs": "{\"command\": \"...\"}"}
// Note: toolArgs is a JSON-encoded STRING, not a nested object.
bool try_handle_copilot_cli(json& v) {
    auto tool_name_it = v.find("toolName");
    if (tool_name_it == v.end() || !tool_name_it->is_string()) return false;
    if (tool_name_it->get<std::string>() != "bash") return true;

    auto args_it = v.find("toolArgs");
    if (args_it == v.end() || !args_it->is_string()) return true;

    json args;
    try {
        args = json::parse(args_it->get<std::string>());
    } catch (const json::exception&) {
        return true;  // malformed inner JSON — pass through unchanged.
    }
    if (!args.is_object()) return true;
    auto cmd_it = args.find("command");
    if (cmd_it == args.end() || !cmd_it->is_string()) return true;

    if (auto rewritten = decide_rewrite(cmd_it->get<std::string>())) {
        args["command"] = *rewritten;
        v["toolArgs"] = args.dump();
    }
    return true;
}

}  // namespace

int run_copilot() {
    // Correctness critic C6: cap stdin so an adversarial / accidentally-
    // huge tool-input JSON can't OOM the hook (which would fail every
    // Bash invocation in the session). Cap = 1 MiB; typical hook input
    // is < 4 KiB. On overflow, emit the (truncated) input back unchanged
    // so the agent gets a passthrough rather than a wedge.
    constexpr std::size_t kMaxStdinBytes = 1 * 1024 * 1024;
    std::string raw;
    raw.reserve(8 * 1024);
    char buf[8192];
    while (raw.size() < kMaxStdinBytes && std::cin.read(buf, sizeof(buf))) {
        raw.append(buf, static_cast<std::size_t>(std::cin.gcount()));
    }
    // Drain any trailing partial-read on EOF.
    if (std::cin.gcount() > 0 && raw.size() < kMaxStdinBytes) {
        std::size_t remaining = kMaxStdinBytes - raw.size();
        std::size_t take = std::min(remaining, static_cast<std::size_t>(std::cin.gcount()));
        raw.append(buf, take);
    }
    if (raw.size() >= kMaxStdinBytes) {
        std::cerr << "[mtk hook copilot] input exceeded " << kMaxStdinBytes
                  << " bytes — passing through unchanged\n";
        std::cout << raw;
        return 0;
    }

    auto stripped = strip_bom(raw);
    // Trim trailing whitespace/newlines.
    while (!stripped.empty() &&
           std::isspace(static_cast<unsigned char>(stripped.back()))) {
        stripped.remove_suffix(1);
    }
    if (stripped.empty()) return 0;

    json v;
    try {
        v = json::parse(stripped);
    } catch (const json::exception& e) {
        std::cerr << "[mtk hook copilot] failed to parse JSON input: "
                  << e.what() << '\n';
        // Emit the raw input back so the agent doesn't lose state.
        std::cout << raw;
        return 0;
    }

    // Try each schema; the first that recognises the shape may have
    // mutated `v` (rewriting the embedded bash command). Per audit:
    // emission is a SINGLE point at the end of this function — the
    // handlers no longer touch stdout themselves, so adding a new
    // schema doesn't introduce another duplicated `std::cout` site.
    (void)(try_handle_vscode(v) || try_handle_copilot_cli(v));

    // Single emission point. Covers all paths: rewrite applied, shape
    // recognised-but-no-rewrite, shape entirely unknown (`v` unchanged).
    std::cout << v.dump();
    return 0;
}

}  // namespace mtk::cmds::hook
