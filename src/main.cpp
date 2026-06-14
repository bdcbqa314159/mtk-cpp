#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <vector>

#include "cmds/exec.hpp"
#include "cmds/git.hpp"
#include "cmds/grep.hpp"
#include "cmds/ls.hpp"

int main(int argc, char** argv) {
    CLI::App app{"mtk — Minimal Token Killer"};
    app.set_version_flag("--version", std::string{"0.1.0"});
    app.allow_extras();

    int exit_code = 0;

    auto* git_cmd = app.add_subcommand("git", "Filter git output");
    git_cmd->allow_extras();
    git_cmd->callback([&] {
        exit_code = mtk::cmds::git::run(git_cmd->remaining());
    });

    auto* ls_cmd = app.add_subcommand("ls", "Compact ls -la output");
    ls_cmd->allow_extras();
    ls_cmd->callback([&] {
        exit_code = mtk::cmds::ls::run(ls_cmd->remaining());
    });

    auto* grep_cmd = app.add_subcommand("grep", "Grouped grep/rg results");
    grep_cmd->allow_extras();
    grep_cmd->callback([&] {
        exit_code = mtk::cmds::grep::run(grep_cmd->remaining());
    });

    auto* exec_cmd = app.add_subcommand("exec", "Run any command (TOML filter or passthrough)");
    exec_cmd->allow_extras();
    exec_cmd->callback([&] {
        exit_code = mtk::cmds::exec::run(exec_cmd->remaining());
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (app.get_subcommands().empty()) {
        auto extras = app.remaining();
        if (extras.empty()) {
            std::cerr << app.help();
            return 0;
        }
        exit_code = mtk::cmds::exec::run(extras);
    }

    return exit_code;
}
