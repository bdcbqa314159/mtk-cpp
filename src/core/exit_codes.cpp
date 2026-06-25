#include "core/exit_codes.hpp"

#include <string>
#include <string_view>

#include "core/diagnostic.hpp"

namespace mtk::core::exit_codes {

int report_spawn_failure(std::string_view tool, std::string_view error) noexcept {
    try {
        std::string msg = "failed to spawn: ";
        msg.append(error);
        mtk::core::diag::emit(tool, msg);
    } catch (...) {
        // best-effort; if even string construction fails we still want to
        // return the canonical exit code so the caller can propagate it.
    }
    return kNotFound;
}

}  // namespace mtk::core::exit_codes
