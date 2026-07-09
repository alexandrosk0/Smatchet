#!/usr/bin/env bash
# 40-unused-config-guard.sh — unused-symbol-under-config-guard rule (sourced by test-lint-rules.sh, not run directly).

# unused-symbol-under-config-guard — ABSOLUTE-0 over first-party C++ .cpp TUs.
# The config-skew -Werror,-Wunused-function class that produced #863: a free
# function DEFINITION sits at file scope UNGUARDED while EVERY reference to it
# lives inside a `#if[def] ... SMATCHET_WITH_* ...` block. In the feature-OFF
# build the definition has zero callers -> Clang's /WX -Werror promotes
# -Wunused-function to a hard error and the sanitized binary never links. MSVC
# /W4 (the PR-time iter/publish presets) does not warn the same way, so the
# break is invisible until the nightly Lua-OFF sanitizer job -- a config-skew
# gate escape (#863, fixed by 61b17427 / #945; this lint is the preventing gate).
#
# Heuristic (NOT a compiler / full AST parse -- a deliberately narrow text+
# preprocessor-depth proxy for the -Wunused-function shape, scoped to keep
# false positives near zero):
#   * Track config-guard depth: +1 on a `#if`/`#ifdef`/`#ifndef` whose header
#     line mentions SMATCHET_WITH_, -1 on the matching `#endif` (full #if/#endif
#     nesting is tracked so a non-config #if inside a config block still closes
#     correctly). A line is "config-guarded" iff config-depth > 0.
#   * A DEFINITION candidate is a COLUMN-0 (file/namespace scope -- methods are
#     indented) line shaped like a free-function definition `... name(... ) {`
#     (or `... name(...)` whose body-brace opens on the next non-blank line),
#     captured ONLY when it sits at config-depth 0 (unguarded). Column-0 + the
#     return-type-then-name-then-paren shape is what restricts this to the
#     -Wunused-function free-function class and excludes calls, declarations
#     (trailing `;`), control-flow (`if (`/`for (`/`while (`/`switch (`), and
#     macro lines.
#   * For each candidate name, scan EVERY OTHER line for a whole-word reference.
#     If there is >=1 reference and ALL references are config-guarded (depth>0)
#     while the definition is unguarded (depth 0) -> flag the definition line.
#   * A `// SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard; ...)` in the
#     preceding-line window above the definition escapes (legit asymmetry).
# Limitation (documented, accepted per the plan section Risks): this is a
# per-file proxy, not the compiler -- it cannot see cross-TU linkage or the exact
# set of SMATCHET_WITH_* the nightly toggles, and only catches the single-file
# def-unguarded / all-refs-guarded shape. The nightly Lua-OFF sanitizer build
# stays the authoritative backstop; this is the fast PR-time fail.
USCG_CONFIG_RE='SMATCHET_WITH_'   # config-flag family the guard family keys on

usc_is_def_line() {
    # $1 = the COLUMN-0 raw line. Sets the global USC_DEF_NAME to the function
    # name + returns 0 if the line looks like a FREE-function DEFINITION header;
    # returns 1 otherwise. NB: uses a global (not stdout) so the hot Pass-1 caller
    # need not fork a `$(...)` subshell per column-0 line (that fork cost ~100s
    # tree-wide before this change).
    USC_DEF_NAME=""
    local line="$1"
    # Must contain `name(` and must NOT end in `;` (that's a declaration/call stmt).
    case "$line" in
        *';'*) return 1 ;;                       # declaration / statement, not a def
        '#'*) return 1 ;;                        # preprocessor line
        '//'*|'/*'*|'*'*) return 1 ;;            # comment
    esac
    # The "signature head" is everything up to the first `(`.
    local head="${line%%(*}"
    # Reject QUALIFIED definitions (`Type::method`, `ns::fn`): an out-of-line
    # member / namespace-qualified definition is NOT the -Wunused-function
    # free-function class — the symbol is declared in a header and referenced from
    # OTHER TUs, so it is never "unused" the way a file-local free function is.
    # This is the discriminator that keeps AppController::AddAiContext (a public
    # member whose only IN-FILE refs happen to be SMATCHET_WITH_AI-guarded) from
    # firing while LogLuaScriptFileProbe (an unqualified free function — the #863
    # shape) still does. (Real-tree false-positive caught pre-ship; see plan log.)
    case "$head" in *'::'*) return 1 ;; esac
    # Reject type/template/operator declarations that can also open column-0 with `(`.
    case "$head" in
        *operator*|*template*|*'class '*|*'struct '*|*'enum '*|*'union '*|*'namespace '*) return 1 ;;
    esac
    # Extract the LAST identifier in `head` (the token immediately before `(`),
    # in PURE BASH (no sed/printf/head subprocess — this runs per column-0 line
    # over every config-bearing TU; a 3-process pipeline here cost ~100s tree-wide).
    # Trim trailing whitespace, then strip everything up to the last char that is
    # not an identifier char, leaving the trailing run of [A-Za-z0-9_].
    local h="${head%"${head##*[![:space:]]}"}"   # rstrip
    local name="$h"
    # Strip a leading run that ends at the last non-identifier char.
    case "$name" in
        *[!A-Za-z0-9_]*) name="${name##*[!A-Za-z0-9_]}" ;;
    esac
    # name now = trailing identifier run (possibly empty if head ended in punct).
    case "$name" in
        ''|*[!A-Za-z0-9_]*) return 1 ;;          # no clean trailing identifier
    esac
    # Reject control-flow / type-system keywords as the "function name".
    case "$name" in
        if|for|while|switch|return|sizeof|catch|else|do|case|new|delete) return 1 ;;
    esac
    # Require a return-type token BEFORE the name (a leading identifier/`*`/`&`/
    # `>` token), i.e. `void name(` / `Foo* name(` / `Result<T> name(` -- a bare
    # `name(args)` with nothing before it is a CALL, not a definition.
    case "$line" in
        "$name"*) return 1 ;;                    # name at column 0 with no return type = call
    esac
    USC_DEF_NAME="$name"
    return 0
}

scan_unused_under_config_guard_file() {
    # $1 = a first-party .cpp file. Emits
    # `unused-symbol-under-config-guard\t<f>:<defline>` per unguarded definition
    # whose every reference is config-guarded. .cpp only (the -Wunused-function
    # class is a TU-local free function; headers declare, they don't define-then-
    # leave-unused the same way).
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp) ;; *) return 0 ;; esac

    # Fast early-exit: a file with no config-flag token at all has no possible
    # config-guarded reference, so no candidate can ever qualify. Skips ~83% of
    # first-party .cpp with one cheap fixed-string grep, before the per-line walk
    # (the def+ref double-scan is O(defs x lines) per file — too slow tree-wide
    # without this gate; only ~47 of ~275 .cpp carry SMATCHET_WITH_).
    grep -qF "$USCG_CONFIG_RE" "$f" 2>/dev/null || return 0

    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i

    # Pass 1: compute POSITIVE-config-guard depth per line + collect unguarded def
    # candidates. A line counts as config-guarded ONLY when its nearest enclosing
    # config `#if` is a POSITIVE one — `#if defined(SMATCHET_WITH_X)` / `#ifdef
    # SMATCHET_WITH_X` / `#if SMATCHET_WITH_X` — AND we are in that #if's TRUE
    # branch (not its `#else`/`#elif`). This is the discriminator that matches the
    # exact #863 shape (call under `#if defined(SMATCHET_WITH_LUA_AUTOMATION)`) while
    # rejecting the common false-positive idiom of a real impl in the `#else` of a
    # `#if !defined(SMATCHET_WITH_X)` (the call then sits in the FLAG-ON branch, and
    # the def is never unused — e.g. SmatchetWhisperSetupBanner's ComputeBannerHeight).
    local -a cfg_depth=()           # cfg_depth[i] = positive-config-guard depth AT line i
    local -a def_name=()            # parallel: candidate names (unguarded defs)
    local -a def_line=()            # parallel: 1-based line numbers
    local -a def_dev=()             # parallel: 1 if an in-window DEVIATION suppresses
    local depth=0                   # full #if/#endif nesting depth
    local cfg=0                     # positive-config-guard depth (subset of `depth`)
    # per-#if frame: "active" = this frame currently contributes +1 to cfg (a
    # positive config #if whose TRUE branch we are in). On `#else`/`#elif` the
    # frame flips inactive (we've left the positive true-branch).
    local -a frame_iscfg=()         # 1 if this #if header is a positive config guard
    local -a frame_active=()        # 1 if currently counted in cfg
    for ((i = 0; i < n; i++)); do
        local raw="${lines[$i]}"
        # Leading-whitespace-stripped view for preprocessor detection.
        local stripped="${raw#"${raw%%[![:space:]]*}"}"
        case "$stripped" in
            '#if'*|'#ifdef'*|'#ifndef'*)
                depth=$((depth + 1))
                # Positive config guard: mentions SMATCHET_WITH_ and is NOT a
                # negation (`#ifndef`, or `#if !defined`/`#if !` form).
                local iscfg=0
                case "$stripped" in
                    '#ifndef'*) : ;;                                  # negative — not positive
                    *"$USCG_CONFIG_RE"*)
                        case "$stripped" in
                            *'!defined'*|*'! defined'*|*'!'*"$USCG_CONFIG_RE"*) : ;;  # negation
                            *) iscfg=1 ;;
                        esac ;;
                esac
                frame_iscfg+=("$iscfg"); frame_active+=("$iscfg")
                [ "$iscfg" = "1" ] && cfg=$((cfg + 1))
                cfg_depth[$i]=$cfg
                continue ;;
            '#else'*|'#elif'*)
                # Leaving the current frame's TRUE branch: if it was an active
                # positive config guard, drop its contribution for the rest of the
                # frame. (A positive config #elif could re-activate, but that idiom
                # is vanishingly rare for SMATCHET_WITH_ flags — treat #elif as
                # leaving the positive branch, conservative = fewer FPs.)
                local fi=$(( ${#frame_active[@]} - 1 ))
                if [ "$fi" -ge 0 ] && [ "${frame_active[$fi]:-0}" = "1" ]; then
                    [ "$cfg" -gt 0 ] && cfg=$((cfg - 1))
                    frame_active[$fi]=0
                fi
                cfg_depth[$i]=$cfg
                continue ;;
            '#endif'*)
                if [ "$depth" -gt 0 ]; then
                    local fi=$(( ${#frame_active[@]} - 1 ))
                    if [ "$fi" -ge 0 ] && [ "${frame_active[$fi]:-0}" = "1" ] && [ "$cfg" -gt 0 ]; then
                        cfg=$((cfg - 1))
                    fi
                    unset 'frame_active[${#frame_active[@]}-1]'; frame_active=("${frame_active[@]}")
                    unset 'frame_iscfg[${#frame_iscfg[@]}-1]'; frame_iscfg=("${frame_iscfg[@]}")
                    depth=$((depth - 1))
                fi
                cfg_depth[$i]=$cfg
                continue ;;
            *) cfg_depth[$i]=$cfg ;;
        esac

        # Candidate definition: column-0 (no leading whitespace), at config-depth 0,
        # def-shaped, with a `{` opening the body on this line OR the next non-blank.
        [ "$cfg" -eq 0 ] || continue
        case "$raw" in [![:space:]]*) ;; *) continue ;; esac     # column-0 only
        # Cheap pre-filter before the def-shape check: a candidate def line must
        # contain a `(` and must not be a preprocessor / comment / statement line.
        case "$raw" in *'('*) ;; *) continue ;; esac
        local name
        usc_is_def_line "$raw" || continue
        name="$USC_DEF_NAME"
        # Require the body brace `{` here or on the next non-blank line (a real
        # definition, not a multi-line declaration). Scan forward briefly.
        local has_brace=0 j
        case "$raw" in *'{'*) has_brace=1 ;; esac
        if [ "$has_brace" -eq 0 ]; then
            for ((j = i + 1; j < n && j <= i + 4; j++)); do
                local nx="${lines[$j]}"
                case "$nx" in
                    '') continue ;;
                    *';'*) break ;;          # declaration terminator before any brace
                    *'{'*) has_brace=1; break ;;
                esac
            done
        fi
        [ "$has_brace" -eq 1 ] || continue

        # In-window SMATCHET_DEVIATION suppression (preceding up-to-3 non-blank lines).
        local dev=0 k cnt=0
        for ((k = i - 1; k >= 0 && cnt < 3; k--)); do
            case "${lines[$k]}" in '') continue ;; esac
            cnt=$((cnt + 1))
            case "${lines[$k]}" in
                *'SMATCHET_DEVIATION(rule=unused-symbol-under-config-guard'*) dev=1; break ;;
            esac
        done

        def_name+=("$name"); def_line+=("$((i + 1))"); def_dev+=("$dev")
    done

    [ "${#def_name[@]}" -gt 0 ] || return 0

    # Pass 2: for each candidate, classify every OTHER reference by config-guard.
    # Reference lookup is delegated to `grep -nwF` (whole-word, fixed-string) — a
    # single C-fast scan per candidate — rather than a bash per-line `=~` regex
    # loop (which was O(defs x lines): catastrophically slow on the 2000+-line
    # monoliths like AppController.cpp, several minutes tree-wide). grep -n gives
    # `<lineno>:<content>`; cfg_depth[lineno-1] (precomputed in Pass 1) tells us
    # whether that reference is config-guarded. -w is grep's own non-identifier
    # word boundary (digits/letters/underscore), the same whole-word semantics the
    # old regex enforced.
    local d
    for ((d = 0; d < ${#def_name[@]}; d++)); do
        local nm="${def_name[$d]}" dl="${def_line[$d]}"
        [ "${def_dev[$d]}" = "1" ] && continue
        local refs=0 guarded_refs=0 gline gno gtext gs
        while IFS= read -r gline; do
            [ -n "$gline" ] || continue
            gno="${gline%%:*}"
            [ "$gno" = "$dl" ] && continue                  # skip the def line itself
            gtext="${gline#*:}"
            # Skip pure-comment / preprocessor-conditional lines as references
            # (a `#if defined(name)` is config plumbing, a `// name` is prose).
            gs="${gtext#"${gtext%%[![:space:]]*}"}"
            case "$gs" in '//'*|'*'*|'/*'*|'#if'*|'#ifdef'*|'#ifndef'*|'#endif'*|'#else'*|'#elif'*) continue ;; esac
            refs=$((refs + 1))
            [ "${cfg_depth[$((gno - 1))]:-0}" -gt 0 ] && guarded_refs=$((guarded_refs + 1))
        done < <(grep -nwF -- "$nm" "$f" 2>/dev/null || true)
        # Flag iff there is >=1 reference and EVERY reference is config-guarded
        # while the definition is unguarded (depth 0, by construction above).
        if [ "$refs" -ge 1 ] && [ "$refs" -eq "$guarded_refs" ]; then
            printf 'unused-symbol-under-config-guard\t%s:%s\n' "$f" "$dl"
        fi
    done
}

compute_unused_under_config_guard_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/**' 'Source/Plugins/**' 'Source/Standalone/**' \
            2>/dev/null \
            | grep -E '\.cpp$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_unused_under_config_guard_file "$f"; done
}
