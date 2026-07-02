#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/exec.hpp"

#ifdef _WIN32
#include <cstdlib>
#include <filesystem>
#include <fstream>

// The load-bearing case: a bare command that resolves to a .cmd shim on PATH
// must be rewritten to `cmd /c <full-path> <args...>` so CreateProcessW (which
// only auto-appends .exe) can actually launch it.
TEST_CASE("resolve_launcher wraps a .cmd shim in cmd /c") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "mtk_launcher_test";
    fs::create_directories(dir);
    fs::path shim = dir / "mtkshim.cmd";
    { std::ofstream(shim) << "@echo off\n"; }

    std::string old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    _putenv_s("PATH", (dir.string() + ";" + old_path).c_str());

    auto out = mtk::core::exec::resolve_launcher({"mtkshim", "build", "x"});

    _putenv_s("PATH", old_path.c_str());
    fs::remove_all(shim.parent_path());

    REQUIRE(out.size() == 5);          // {cmd.exe, /c, <path>, build, x}
    CHECK(out[1] == "/c");
    // Path carries PATHEXT's casing (".CMD"), so match the stem, not ".cmd".
    CHECK(out[2].find("mtkshim") != std::string::npos);
    CHECK(out[3] == "build");
    CHECK(out[4] == "x");
}

// Regression for the real npm case: the shim lives under a directory with a
// space (`C:\Program Files\nodejs\`). reproc quotes a space-containing arg and
// cmd.exe's `/c` quote-stripping then mangles it (`"C:\Program Files\..."`
// becomes an attempt to run `C:\Program`). resolve_launcher must hand cmd a
// path with no space — it collapses to the 8.3 short form. (Requires 8.3 name
// generation on the volume, which is the Windows default for the system drive.)
TEST_CASE("resolve_launcher gives cmd a space-free shim path") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "mtk launcher spaced";  // note space
    fs::create_directories(dir);
    fs::path shim = dir / "mtkshim2.cmd";
    { std::ofstream(shim) << "@echo off\n"; }

    std::string old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    _putenv_s("PATH", (dir.string() + ";" + old_path).c_str());

    auto out = mtk::core::exec::resolve_launcher({"mtkshim2", "run"});

    _putenv_s("PATH", old_path.c_str());
    fs::remove_all(dir);

    REQUIRE(out.size() == 4);                          // {cmd.exe, /c, <path>, run}
    CHECK(out[2].find("mtkshim2") != std::string::npos);
    CHECK(out[2].find(' ') == std::string::npos);      // the load-bearing check
}
#else
TEST_CASE("resolve_launcher is identity on POSIX") {
    auto out = mtk::core::exec::resolve_launcher({"npm", "install"});
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "npm");
    CHECK(out[1] == "install");
}
#endif
