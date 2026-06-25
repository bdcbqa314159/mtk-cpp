#include "core/passthrough_filter.hpp"

#include <string>
#include <vector>

#include "core/run_context.hpp"

namespace mtk::core {

namespace ex = mtk::core::exec;

ex::ExecOutcome PassthroughFilter::run(DispatchTokenPtr,
                                       const std::vector<std::string>& argv,
                                       RunContext& ctx) {
    int code = ctx.passthrough(argv);

    // Passthrough returns the child's exit code (or 127 for spawn failure).
    // We can't distinguish via the int — but the diagnostic was already
    // emitted by passthrough() itself. Return a Ran with empty data and
    // the propagated code; the dispatcher's emit() will just propagate it.
    return ex::Ran{
        /*stdout_data*/ "",
        /*stderr_data*/ "",
        /*exit_code*/   code,
        /*truncated*/   false,
        /*timed_out*/   false,
        /*killed_by_signal*/ 0,
    };
}

}  // namespace mtk::core
