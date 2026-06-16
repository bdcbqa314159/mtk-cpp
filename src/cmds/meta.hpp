#pragma once
#include <string>
#include <vector>

namespace mtk::cmds::meta {

// `mtk explain [cmd...]` — dump registry contents; if argv given, show
// which filter would match.
int run_explain(const std::vector<std::string>& argv);

// `mtk trust [path]` / `mtk untrust [path]` / `mtk trusted` — manage the
// project-filter allow-list (A2).
int run_trust(const std::vector<std::string>& argv);
int run_untrust(const std::vector<std::string>& argv);
int run_trusted();

// `mtk reload` — invalidate + rebuild the TOML-filter binary cache.
int run_reload();

// `mtk rewrite <cmd>` — emit "mtk <cmd>" if a non-passthrough filter
// would match, else echo unchanged. Used by hook scripts (mtk init).
int run_rewrite(const std::vector<std::string>& argv);

// Audit-reading meta-commands (Phase 3).
int run_stats();
int run_tail(const std::vector<std::string>& argv);
int run_why(const std::vector<std::string>& argv);

}  // namespace mtk::cmds::meta
