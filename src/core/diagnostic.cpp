#include "core/diagnostic.hpp"

#include <iostream>
#include <string>

#include "core/color.hpp"

namespace mtk::core::diag {

void emit(std::string_view tool, std::string_view message) noexcept {
    try {
        // Color the whole "mtk <tool>: <message>" red when emitting to
        // a TTY — agents see plain via colors_enabled()==false. Build
        // the full string first so the color wrapper sees one logical
        // unit (single \x1b[...m...\x1b[0m around everything).
        std::string line = "mtk ";
        line.append(tool);
        line += ": ";
        line.append(message);
        std::cerr << mtk::core::color::red(line) << '\n';
    } catch (...) {
        // stderr write itself failed — nowhere useful to report this.
    }
}

}  // namespace mtk::core::diag
