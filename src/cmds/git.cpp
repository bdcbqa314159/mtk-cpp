#include "cmds/git.hpp"

// Per audit: the four git filters live in dedicated translation units
// (git_log.cpp / git_status.cpp / git_diff.cpp / git_show.cpp) so a fix
// to one filter doesn't recompile the others. This file is now just
// the orchestrator that calls the four per-filter registrars.

namespace mtk::cmds::git {

void register_builtins(mtk::core::Registry& reg) {
    register_log(reg);
    register_status(reg);
    register_diff(reg);
    register_show(reg);
}

}  // namespace mtk::cmds::git
