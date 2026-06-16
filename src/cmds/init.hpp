#pragma once
#include <string>
#include <vector>

namespace mtk::cmds::init {

// `mtk init <agent>` — install the agent's hook configuration. Empty argv
// prints the supported-agent list. Returns the exit code to propagate.
int run_init(const std::vector<std::string>& argv);

}  // namespace mtk::cmds::init
