#pragma once
#include <string>
#include <vector>

namespace mtk::core::exec {

struct CapturedOutput {
    std::string stdout_data;
    std::string stderr_data;
    int exit_code = 0;
    bool spawned = false;
    std::string spawn_error;
};

using EnvExtra = std::vector<std::pair<std::string, std::string>>;

CapturedOutput capture(const std::vector<std::string>& argv,
                       const EnvExtra& env_extra = {});

int passthrough(const std::vector<std::string>& argv);

}  // namespace mtk::core::exec
