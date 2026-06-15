#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cmds/hook.hpp"

using namespace mtk::cmds::hook::internal;

TEST_CASE("strip_bom removes UTF-8 BOM when present") {
    std::string with_bom = "\xEF\xBB\xBFhello";
    std::string no_bom = "hello";
    CHECK(strip_bom(with_bom) == "hello");
    CHECK(strip_bom(no_bom) == "hello");
    CHECK(strip_bom("") == "");
    CHECK(strip_bom("\xEF\xBB") == "\xEF\xBB");  // partial BOM, untouched
}

TEST_CASE("tokenise splits on whitespace and respects quotes") {
    auto v = tokenise("git log -n 5");
    REQUIRE(v.size() == 4);
    CHECK(v[0] == "git");
    CHECK(v[1] == "log");
    CHECK(v[2] == "-n");
    CHECK(v[3] == "5");

    v = tokenise(R"(git log --grep="fix bug" -n 5)");
    REQUIRE(v.size() == 5);
    CHECK(v[2] == "--grep=fix bug");

    v = tokenise("git log --grep='fix bug'");
    REQUIRE(v.size() == 3);
    CHECK(v[2] == "--grep=fix bug");

    v = tokenise("");
    CHECK(v.empty());
}

TEST_CASE("contains_shell_metachar flags shell features that need passthrough") {
    CHECK(!contains_shell_metachar("git log"));
    CHECK(!contains_shell_metachar("git log -n 5 --oneline"));

    CHECK(contains_shell_metachar("git log | head"));
    CHECK(contains_shell_metachar("git log && echo done"));
    CHECK(contains_shell_metachar("git log; ls"));
    CHECK(contains_shell_metachar("ls > out.txt"));
    CHECK(contains_shell_metachar("cat < file"));
    CHECK(contains_shell_metachar("git log $(date)"));
    CHECK(contains_shell_metachar("echo `pwd`"));
    CHECK(contains_shell_metachar("foo &"));
    CHECK(contains_shell_metachar("ls (a)"));
}

TEST_CASE("is_env_assignment recognises shell env-var prefixes") {
    CHECK(is_env_assignment("FOO=bar"));
    CHECK(is_env_assignment("FOO="));
    CHECK(is_env_assignment("_FOO=bar"));
    CHECK(is_env_assignment("MIX3D_99=value-with-stuff"));

    CHECK(!is_env_assignment(""));
    CHECK(!is_env_assignment("git"));
    CHECK(!is_env_assignment("=bar"));
    CHECK(!is_env_assignment("9FOO=bar"));      // digit-leading not allowed
    CHECK(!is_env_assignment("-FOO=bar"));      // dash-leading is a flag
    CHECK(!is_env_assignment("foo bar=baz"));   // space before '='
}

TEST_CASE("is_transparent_wrapper covers expected env/sudo/timing wrappers") {
    CHECK(is_transparent_wrapper("env"));
    CHECK(is_transparent_wrapper("/usr/bin/env"));
    CHECK(is_transparent_wrapper("sudo"));
    CHECK(is_transparent_wrapper("doas"));
    CHECK(is_transparent_wrapper("nice"));
    CHECK(is_transparent_wrapper("time"));
    CHECK(is_transparent_wrapper("stdbuf"));

    CHECK(!is_transparent_wrapper("git"));
    CHECK(!is_transparent_wrapper("strace"));
    CHECK(!is_transparent_wrapper(""));
}

TEST_CASE("skip_leading_noops peels env-assignments and wrappers (C5)") {
    using V = std::vector<std::string>;

    // No prefix — first token IS the command.
    CHECK(skip_leading_noops(V{"git", "log"}) == 0);

    // FOO=bar prefix.
    CHECK(skip_leading_noops(V{"FOO=bar", "git", "log"}) == 1);

    // `env FOO=bar git log` — skip env and the env-assignment.
    CHECK(skip_leading_noops(V{"env", "FOO=bar", "git", "log"}) == 2);

    // `sudo -E git log`.
    CHECK(skip_leading_noops(V{"sudo", "-E", "git", "log"}) == 2);

    // `sudo -E -H git log`.
    CHECK(skip_leading_noops(V{"sudo", "-E", "-H", "git", "log"}) == 3);

    // `sudo --` then command.
    CHECK(skip_leading_noops(V{"sudo", "--", "git", "log"}) == 2);

    // `nice git log`.
    CHECK(skip_leading_noops(V{"nice", "git", "log"}) == 1);

    // Unknown sudo flag short-circuits — return current index, command
    // dispatch is best-effort.
    auto i = skip_leading_noops(V{"sudo", "-Q", "git", "log"});
    CHECK(i == 1);  // stops at unknown flag

    // Pure-noop input: only wrappers, no command. Returns argv.size().
    CHECK(skip_leading_noops(V{"sudo"}) == 1);
    CHECK(skip_leading_noops(V{"env", "FOO=bar"}) == 2);
}

TEST_CASE("decide_rewrite returns nullopt for empty / shell-metachar / mtk-prefixed cmds") {
    CHECK(!decide_rewrite("").has_value());
    CHECK(!decide_rewrite("git log | head").has_value());
    CHECK(!decide_rewrite("mtk git log").has_value());
    CHECK(!decide_rewrite("FOO=bar mtk git log").has_value());
}

TEST_CASE("decide_rewrite injects mtk for recognised commands (registry-dispatched)") {
    // git log is wired in the builtin registry, so it should rewrite.
    auto r = decide_rewrite("git log -n 5");
    REQUIRE(r.has_value());
    CHECK(r->find("mtk") != std::string::npos);
    CHECK(r->find("git") != std::string::npos);
    CHECK(r->find("log") != std::string::npos);

    // With env prefix preserved.
    r = decide_rewrite("FOO=bar git log");
    REQUIRE(r.has_value());
    CHECK(r->rfind("FOO=bar", 0) == 0);
    CHECK(r->find("mtk git log") != std::string::npos);

    // With sudo prefix preserved.
    r = decide_rewrite("sudo -E git log");
    REQUIRE(r.has_value());
    CHECK(r->rfind("sudo", 0) == 0);
    CHECK(r->find("mtk git log") != std::string::npos);
}

TEST_CASE("decide_rewrite returns nullopt for commands no filter wants") {
    // Random uncommon binary — no filter, passthrough only → no rewrite.
    auto r = decide_rewrite("zzzunknown_cmd --flag");
    CHECK(!r.has_value());
}
