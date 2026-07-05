// monkey_command_registry — the nightly "code monkey" for Smatchet's command registry.
// See docs/plans/active/nightly-monkey-tester.md.
//
// WHAT IT DOES (Layer 1a — pre-handler pipeline fuzz):
// boots a headless AppController, registers every builtin command, then fires a long
// SEEDED sequence of randomized-but-guaranteed-invalid dispatches at
// CommandRegistry::Dispatch — missing-required, un-coercible-type, out-of-bounds,
// over-length, destructive-unconfirmed, and unknown-name probes (see CommandArgSynth.h).
// Each probe is guaranteed to be rejected BEFORE the command's handler runs, so:
//   * no app-mutating handler body executes headless (zero false positives), and
//   * the pipeline's parsing / coercion / bounds / fuzzy-match / confirm-gate code gets
//     hammered with adversarial data that scripted tests never generate.
// Run under ASan+UBSan in CI, a memory/UB bug aborts the process with the reproducing
// seed already on stdout. Handler-body execution (1b) is the allow-list-gated follow-up.
//
// REPRODUCIBILITY CONTRACT: the first stdout line is always `monkey seed=<N>`. A fixed
// --seed with a fixed --steps replays a byte-identical dispatch sequence (all randomness
// comes from one std::mt19937_64; no wall-clock/address/unordered-order feeds a choice).
//
// EXIT CODES: 0 = clean. 2 = an exception escaped Dispatch. 3 = a probe that must be
// rejected returned Ok (a handler ran / the destructive gate was bypassed). 4 = a
// malformed error envelope. Non-zero always prints the exact reproducing step.

#include "AppController.h"
#include "Commands/BuiltinCommands.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "CommandArgSynth.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <random>
#include <string>
#include <vector>

using smatchet::cmd::Command;
using smatchet::cmd::CommandContext;
using smatchet::cmd::CommandResult;
using smatchet::cmd::CommandSource;
using smatchet::cmd::ErrorCodeString;
using smatchet::monkey::ProbeKind;
using smatchet::monkey::ProbeKindName;

namespace {

struct Options {
    std::uint64_t seed = 0;
    bool seedProvided = false;
    long long steps = 2000;
    long long timeBudgetMs = 0; // 0 = no wall-clock cap; bound by --steps only
    std::string mode = "prehandler";
    std::string category; // empty = every category
    bool verbose = false;
};

// Self-referential / process-affecting commands. In prehandler mode their handlers can
// never run (every probe rejects first), so this is intent/forward-compat hygiene rather
// than a safety requirement — but it keeps the 1b handler follow-up honest by design.
bool IsDenied(const std::string& name) {
    return name == "app.quit" || name == "ui_test.run" || name == "scenario.run";
}

const char* SourceName(CommandSource s) { return smatchet::cmd::CommandSourceString(s); }

CommandSource PickSource(std::mt19937_64& rng) {
    switch (smatchet::monkey::PickIndex(rng, 6)) {
    case 0: return CommandSource::Cli;
    case 1: return CommandSource::Palette;
    case 2: return CommandSource::Mcp;
    case 3: return CommandSource::Lua;
    case 4: return CommandSource::Unreal;
    default: return CommandSource::Internal;
    }
}

bool ParseLL(const char* s, long long& out) {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = v;
    return true;
}

bool ParseU64(const char* s, std::uint64_t& out) {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

// Match "--key=value"; returns the value pointer or nullptr.
const char* MatchOpt(const char* arg, const char* key) {
    const std::size_t n = std::strlen(key);
    if (std::strncmp(arg, key, n) == 0 && arg[n] == '=') {
        return arg + n + 1;
    }
    return nullptr;
}

void PrintUsage() {
    std::printf(
        "usage: SmatchetMonkeyCli [--seed=<u64>] [--steps=<n>] [--time-budget-ms=<n>]\n"
        "                         [--mode=prehandler|handlers] [--category=<cat>] [--verbose]\n"
        "  Layer 1a fuzzes the pre-handler dispatch pipeline; a fixed --seed+--steps replays\n"
        "  a byte-identical sequence. mode=handlers is reserved for the allow-list follow-up.\n");
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = nullptr;
        if ((std::strcmp(a, "--help") == 0) || (std::strcmp(a, "-h") == 0)) {
            PrintUsage();
            return 0;
        } else if ((v = MatchOpt(a, "--seed")) != nullptr) {
            if (!ParseU64(v, opts.seed)) { std::fprintf(stderr, "monkey: bad --seed '%s'\n", v); return 64; }
            opts.seedProvided = true;
        } else if ((v = MatchOpt(a, "--steps")) != nullptr) {
            if (!ParseLL(v, opts.steps) || opts.steps < 0) { std::fprintf(stderr, "monkey: bad --steps '%s'\n", v); return 64; }
        } else if ((v = MatchOpt(a, "--time-budget-ms")) != nullptr) {
            if (!ParseLL(v, opts.timeBudgetMs) || opts.timeBudgetMs < 0) { std::fprintf(stderr, "monkey: bad --time-budget-ms '%s'\n", v); return 64; }
        } else if ((v = MatchOpt(a, "--mode")) != nullptr) {
            opts.mode = v;
        } else if ((v = MatchOpt(a, "--category")) != nullptr) {
            opts.category = v;
        } else if (std::strcmp(a, "--verbose") == 0) {
            opts.verbose = true;
        } else {
            std::fprintf(stderr, "monkey: unknown arg '%s' (try --help)\n", a);
            return 64;
        }
    }

    // Seed: explicit --seed reproduces; otherwise draw once from random_device and pin it.
    const std::uint64_t seed = opts.seedProvided ? opts.seed : static_cast<std::uint64_t>(std::random_device{}());
    // CONTRACT: seed line is emitted FIRST and flushed, so any later crash log carries the
    // one-command reproducer even if the process aborts under a sanitizer mid-run.
    std::printf("monkey seed=%llu\n", static_cast<unsigned long long>(seed));
    std::fflush(stdout);

    if (opts.mode != "prehandler") {
        // 1b (handler-body execution) is allow-list-gated and ships empty: running an
        // arbitrary handler headless would deref un-wired subsystems (WireCoreServices is
        // not called by Initialize) and generate false-positive crashes. Reserved seam.
        std::printf("monkey: mode='%s' is reserved for the allow-list-gated handler "
                    "follow-up (1b); the allow-list is empty in this build, so no handler "
                    "dispatches run. Use --mode=prehandler.\n",
                    opts.mode.c_str());
        return 0;
    }

    std::mt19937_64 rng(seed);

    // Headless boot — exactly the BuiltinsFixture recipe (no Initialize, no backend, no
    // threads, no I/O): construct a bare AppController and register every builtin on it.
    AppController app;
    smatchet::cmd::RegisterBuiltinCommands(app.Commands(), app);

    const std::vector<Command> all = app.Commands().All();
    std::vector<Command> pool;
    pool.reserve(all.size());
    for (const Command& c : all) {
        if (IsDenied(c.Name)) continue;
        if (!opts.category.empty() && c.Category != opts.category) continue;
        pool.push_back(c);
    }
    std::printf("monkey: %zu commands registered, %zu in pool (category=%s)\n", all.size(), pool.size(),
                opts.category.empty() ? "*" : opts.category.c_str());

    // A default-constructed command for the UnknownName probe (which ignores its command).
    const Command kNoCmd;

    long long stepsRun = 0;
    long long dispatched = 0;
    long long skippedNoProbe = 0;
    std::map<std::string, long long> codeHist;
    std::map<std::string, long long> probeHist;

    const auto start = std::chrono::steady_clock::now();

    for (long long step = 0; step < opts.steps; ++step) {
        if (opts.timeBudgetMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= opts.timeBudgetMs) break;
        }
        ++stepsRun;

        // ~1 step in 16 fires the global unknown-name probe (fuzzes the fuzzy matcher);
        // also the only option when the pool is empty. The draw is consumed unconditionally
        // so the sequence stays aligned regardless of pool contents.
        const bool doUnknown = (smatchet::monkey::NextU64(rng) % 16) == 0 || pool.empty();

        const Command* chosen = nullptr;
        ProbeKind kind = ProbeKind::UnknownName;
        if (!doUnknown) {
            const Command& c = pool[smatchet::monkey::PickIndex(rng, pool.size())];
            const std::vector<ProbeKind> probes = smatchet::monkey::ApplicableProbes(c);
            if (probes.empty()) {
                ++skippedNoProbe; // zero-param non-destructive: no guaranteed-reject probe
                continue;
            }
            kind = probes[smatchet::monkey::PickIndex(rng, probes.size())];
            chosen = &c;
        }

        const smatchet::monkey::FuzzInput in =
            smatchet::monkey::BuildProbe(chosen != nullptr ? *chosen : kNoCmd, kind, rng);
        const std::string name = in.overrideName.empty() ? (chosen != nullptr ? chosen->Name : in.overrideName)
                                                          : in.overrideName;

        CommandContext ctx;
        ctx.App = &app;
        ctx.Source = PickSource(rng);
        ctx.ConfirmedDestructive = smatchet::monkey::PickBool(rng);
        ctx.DryRun = smatchet::monkey::PickBool(rng);
        ctx.TimeoutMs = static_cast<int>(smatchet::monkey::PickIntInRange(rng, 0, 5000));
        if (kind == ProbeKind::DestructiveGate) {
            // The confirm gate is the stopper ONLY when unconfirmed and not a dry run.
            ctx.ConfirmedDestructive = false;
            ctx.DryRun = false;
        }

        CommandResult r;
        try {
            r = app.Commands().Dispatch(name, in.args, ctx);
        } catch (const std::exception& e) {
            std::string argsDump = in.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::fprintf(stderr,
                         "monkey VIOLATION (exception): seed=%llu step=%lld cmd='%s' probe=%s src=%s\n"
                         "  args=%s\n  what()=%s\n"
                         "  reproduce: SmatchetMonkeyCli --seed=%llu --steps=%lld\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind),
                         SourceName(ctx.Source), argsDump.c_str(), e.what(),
                         static_cast<unsigned long long>(seed), opts.steps);
            return 2;
        } catch (...) {
            std::fprintf(stderr,
                         "monkey VIOLATION (non-std exception): seed=%llu step=%lld cmd='%s' probe=%s\n"
                         "  reproduce: SmatchetMonkeyCli --seed=%llu --steps=%lld\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind),
                         static_cast<unsigned long long>(seed), opts.steps);
            return 2;
        }

        ++dispatched;
        ++probeHist[ProbeKindName(kind)];

        // Oracle 1 — every probe is guaranteed pre-handler-reject: an Ok result means a
        // handler executed (or the destructive gate was bypassed). That is a real defect.
        if (r.Ok) {
            std::string argsDump = in.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::fprintf(stderr,
                         "monkey VIOLATION (unexpected Ok — handler ran / gate bypassed):\n"
                         "  seed=%llu step=%lld cmd='%s' probe=%s src=%s dryRun=%d confirmed=%d\n"
                         "  args=%s\n"
                         "  reproduce: SmatchetMonkeyCli --seed=%llu --steps=%lld\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind),
                         SourceName(ctx.Source), ctx.DryRun ? 1 : 0, ctx.ConfirmedDestructive ? 1 : 0,
                         argsDump.c_str(), static_cast<unsigned long long>(seed), opts.steps);
            return 3;
        }

        // Oracle 2 — the error envelope must be well-formed (this is the contract every
        // frontend relies on). A throw here, an empty message, or a missing code is a break.
        try {
            const nlohmann::json ej = r.Error.ToJson();
            if (r.Error.Message.empty() || !ej.contains("code")) {
                std::fprintf(stderr,
                             "monkey VIOLATION (malformed error envelope): seed=%llu step=%lld cmd='%s' "
                             "probe=%s code=%s msgEmpty=%d\n"
                             "  reproduce: SmatchetMonkeyCli --seed=%llu --steps=%lld\n",
                             static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind),
                             ErrorCodeString(r.Error.Code), r.Error.Message.empty() ? 1 : 0,
                             static_cast<unsigned long long>(seed), opts.steps);
                return 4;
            }
        } catch (...) {
            std::fprintf(stderr,
                         "monkey VIOLATION (error envelope ToJson threw): seed=%llu step=%lld cmd='%s'\n"
                         "  reproduce: SmatchetMonkeyCli --seed=%llu --steps=%lld\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(),
                         static_cast<unsigned long long>(seed), opts.steps);
            return 4;
        }

        ++codeHist[ErrorCodeString(r.Error.Code)];

        if (opts.verbose) {
            std::string argsDump = in.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::printf("step=%lld cmd=%s probe=%s src=%s => code=%s args=%s\n", step, name.c_str(),
                        ProbeKindName(kind), SourceName(ctx.Source), ErrorCodeString(r.Error.Code),
                        argsDump.c_str());
        }
    }

    std::printf("monkey summary: seed=%llu mode=prehandler stepsRun=%lld dispatched=%lld skippedNoProbe=%lld\n",
                static_cast<unsigned long long>(seed), stepsRun, dispatched, skippedNoProbe);
    for (std::map<std::string, long long>::const_iterator it = probeHist.begin(); it != probeHist.end(); ++it) {
        std::printf("  probe %-16s %lld\n", it->first.c_str(), it->second);
    }
    for (std::map<std::string, long long>::const_iterator it = codeHist.begin(); it != codeHist.end(); ++it) {
        std::printf("  code  %-20s %lld\n", it->first.c_str(), it->second);
    }
    std::printf("monkey: OK (no violations)\n");
    return 0;
}
