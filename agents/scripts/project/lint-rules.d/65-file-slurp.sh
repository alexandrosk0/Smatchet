#!/usr/bin/env bash
# 65-file-slurp.sh — unbounded-file-slurp rule (advisory) (sourced by test-lint-rules.sh, not run directly).

# unbounded-file-slurp — WARN-first over first-party C++ (calibration; never blocks).
# A whole-file slurp with no byte cap (`ss << f.rdbuf()`, `std::istreambuf_iterator<char>`
# construction) reads an arbitrarily large file into memory before any validation — the class
# behind the Win32-vs-POSIX config-read asymmetry (SECURITY_AUDIT.md #33: the Win32 arm caps at
# 64 MiB, the POSIX arm slurps unbounded) and CPP_CODE_AUDIT #31's unbounded accumulator. Most
# residual sites read developer-authored fixtures — hence WARN-first, diff-scoped, deviation-
# suppressible (`// SMATCHET_DEVIATION(rule=unbounded-file-slurp; ...)` above the read). Prefer a
# size-capped read (stat/tellg + reject, or LoadJsonFile which already caps) for new code.

scan_file_slurp_file() {
    # $1 = a first-party C++ file. Emits `unbounded-file-slurp\t<f>:<line>` per uncapped slurp.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    grep -qE 'rdbuf\(\)|istreambuf_iterator' "$f" 2>/dev/null || return 0
    local lineno=0 prev_dev_rule=""
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        if [[ "$line" =~ ^[[:space:]]*(//|\*|/\*) ]]; then continue; fi
        # Comment-stripped view: a trailing `// ... "rdbuf()" ...` mention can neither fire nor
        # mask a real slurp on the same line.
        local code_only="${line%%//*}"
        case "$code_only" in *'"'*rdbuf*'"'*|*'"'*istreambuf*'"'*) continue ;; esac
        if [[ "$code_only" =~ (\<\<[[:space:]]*[A-Za-z_][A-Za-z0-9_]*(\.|-\>)rdbuf\(\)|istreambuf_iterator\<[[:space:]]*char[[:space:]]*\>) ]]; then
            if [ "$suppress" != "unbounded-file-slurp" ]; then
                printf 'unbounded-file-slurp\t%s:%s\n' "$f" "$lineno"
            fi
        fi
    done < "$f"
}

compute_file_slurp_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(list_first_party_cpp_files)
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_file_slurp_file "$f"; done
}
