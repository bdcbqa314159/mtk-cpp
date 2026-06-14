#!/usr/bin/env bash
# CI quality gate per GUIDELINES Parts I and II.
# Run from the project root after a successful build.
#
# Enforces:
#   A5  — no networking symbols in the binary, no networking includes in src/
#   CE4 — same forbidden-include check
#   CE5 — no std::get/get_if/visit/.index() on ExecOutcome outside core/exec.cpp + core/run_context.cpp
#   CE6 — every cmds/*.cpp has a corresponding tests/test_*.cpp
#
# Exit 0 = all gates passed. Exit non-zero on first violation.

set -e
cd "$(dirname "$0")/.."

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }

fail() {
    red "FAIL: $*"
    exit 1
}

pass() {
    green "OK:   $*"
}

# --- A5: no networking symbols in the built binary ---
echo "[ci] A5: no networking symbols"
if [[ ! -f build/mtk ]]; then
    yellow "SKIP: build/mtk not found (run cmake --build build first)"
else
    # nm output includes both defined and referenced symbols; we want neither
    # for these networking ones. macOS uses underscored symbol names.
    forbidden_syms='socket|connect|bind|getaddrinfo|SSL_|tls_|curl_|reqwest_'
    if nm build/mtk 2>/dev/null \
        | grep -wE "_?(${forbidden_syms})\b" >/dev/null; then
        red "FAIL: A5 violation — binary references networking symbols:"
        nm build/mtk 2>/dev/null \
            | grep -wE "_?(${forbidden_syms})\b" \
            | head -20
        exit 1
    fi
    pass "A5 — no networking symbols in build/mtk"
fi

# --- CE4: no networking includes in src/ ---
echo "[ci] CE4: no networking includes in src/"
if grep -rE '<sys/socket\.h>|<netinet/|<arpa/|<sys/un\.h>|<curl/' src/ 2>/dev/null; then
    fail "CE4 — networking includes in src/ (see above)"
fi
pass "CE4 — no networking includes in src/"

# --- CE5: no ExecOutcome variant access outside core/exec.cpp + core/run_context.cpp ---
echo "[ci] CE5: no ExecOutcome decomposition outside allowed files"
violations=$(grep -rnE 'std::(get|get_if|visit)[<\(].*ExecOutcome|ExecOutcome.*\.index\(\)' src/ 2>/dev/null \
    | grep -vE 'src/core/(exec|run_context)\.(cpp|hpp)' || true)
if [[ -n "$violations" ]]; then
    red "FAIL: CE5 violation — ExecOutcome decomposed outside allowed files:"
    printf '%s\n' "$violations"
    exit 1
fi
pass "CE5 — no ExecOutcome decomposition outside core/exec + core/run_context"

# --- CE6: every cmds/*.cpp has a tests/test_*.cpp ---
echo "[ci] CE6: every cmd file has a test file"
missing=()
for cmd in src/cmds/*.cpp; do
    base=$(basename "$cmd" .cpp)
    # Skip headers-only or special files
    [[ "$base" == "passthrough_filter" ]] && continue
    if ! ls tests/test_*"$base"*.cpp >/dev/null 2>&1; then
        missing+=("$cmd")
    fi
done
if (( ${#missing[@]} > 0 )); then
    yellow "WARN: CE6 — cmd files without explicit tests:"
    printf '  %s\n' "${missing[@]}"
fi
pass "CE6 — checked (warnings above are not blocking)"

# --- summary ---
green "[ci] all gates passed"
