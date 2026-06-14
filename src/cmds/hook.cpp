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

namespace {

using json = nlohmann::json;

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

    auto reg = mtk::core::build_default_registry();
    auto match = reg.find(argv);
    if (!match.filter) return std::nullopt;
    if (match.filter->name() == std::string_view("_passthrough")) return std::nullopt;

    return "mtk " + cmd;
}

// --- VS Code Copilot Chat schema ---
// {"tool_name": "Bash"|"runTerminalCommand", "tool_input": {"command": "..."}}
bool try_handle_vscode(json& v) {
    auto tool_name_it = v.find("tool_name");
    if (tool_name_it == v.end() || !tool_name_it->is_string()) return false;

    const auto tool_name = tool_name_it->get<std::string>();
    if (tool_name != "Bash" && tool_name != "bash" &&
        tool_name != "runTerminalCommand") {
        // Recognised as VS Code shape but tool isn't a Bash variant — emit
        // unchanged. Return true so the caller doesn't try the CLI shape.
        std::cout << v.dump();
        return true;
    }

    auto& input = v["tool_input"];
    if (!input.is_object()) {
        std::cout << v.dump();
        return true;
    }
    auto cmd_it = input.find("command");
    if (cmd_it == input.end() || !cmd_it->is_string()) {
        std::cout << v.dump();
        return true;
    }

    auto rewritten = decide_rewrite(cmd_it->get<std::string>());
    if (rewritten) input["command"] = *rewritten;
    std::cout << v.dump();
    return true;
}

// --- Copilot CLI schema ---
// {"toolName": "bash", "toolArgs": "{\"command\": \"...\"}"}
// Note: toolArgs is a JSON-encoded STRING, not a nested object.
bool try_handle_copilot_cli(json& v) {
    auto tool_name_it = v.find("toolName");
    if (tool_name_it == v.end() || !tool_name_it->is_string()) return false;
    if (tool_name_it->get<std::string>() != "bash") {
        std::cout << v.dump();
        return true;
    }

    auto args_it = v.find("toolArgs");
    if (args_it == v.end() || !args_it->is_string()) {
        std::cout << v.dump();
        return true;
    }

    json args;
    try {
        args = json::parse(args_it->get<std::string>());
    } catch (const json::exception&) {
        std::cout << v.dump();
        return true;
    }
    if (!args.is_object()) {
        std::cout << v.dump();
        return true;
    }
    auto cmd_it = args.find("command");
    if (cmd_it == args.end() || !cmd_it->is_string()) {
        std::cout << v.dump();
        return true;
    }

    auto rewritten = decide_rewrite(cmd_it->get<std::string>());
    if (rewritten) {
        args["command"] = *rewritten;
        v["toolArgs"] = args.dump();
    }
    std::cout << v.dump();
    return true;
}

}  // namespace

int run_copilot() {
    std::ostringstream buf;
    buf << std::cin.rdbuf();
    std::string raw = buf.str();

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

    if (try_handle_vscode(v)) return 0;
    if (try_handle_copilot_cli(v)) return 0;

    // Unknown shape — passthrough.
    std::cout << v.dump();
    return 0;
}

}  // namespace mtk::cmds::hook
