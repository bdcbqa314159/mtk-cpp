#include "core/diagnostic.hpp"

#include <iostream>

namespace mtk::core::diag {

void emit(std::string_view tool, std::string_view message) noexcept {
    try {
        std::cerr << "mtk " << tool << ": " << message << '\n';
    } catch (...) {
        // stderr write itself failed — nowhere useful to report this.
    }
}

}  // namespace mtk::core::diag
