// monkey_command_registry — the nightly "code monkey" for Smatchet's command registry.
// See docs/plans/nightly-monkey-tester.md.
//
// Two modes:
//
// --mode=prehandler (Layer 1a, default): boots a BARE headless AppController + registers
//   every builtin (no Initialize), then fires a long SEEDED sequence of randomized-but-
//   guaranteed-invalid dispatches at CommandRegistry::Dispatch — missing-required,
//   un-coercible-type, out-of-bounds, over-length, destructive-unconfirmed, unknown-name
//   (see CommandArgSynth.h). Every probe is rejected BEFORE the handler runs, so no
//   app-mutating handler executes and the pipeline's parse/coerce/bounds/fuzzy-match/
//   confirm-gate code gets hammered with adversarial data. Zero false positives.
//
// --mode=handlers (Layer 1b): boots a FULLY-initialized AppController against a FAKE Jira
//   backend (no network) and actually RUNS handler bodies — but only for a small ALLOW-LIST
//   of read-only commands proven safe headless. The hazard headless is NOT un-wired
//   services (Initialize->InitConfig->WireCoreServices wires ticketSync_/offlineQueue_/
//   fieldEdit_/... — see AppController.cpp:1807), it is `g_ui`/ImGui frame state and
//   UI-thread hops; the allow-list is curated to touch neither. Handlers run on the init
//   thread (so RunOnUiThread lambdas run inline, never blocking), and the main-thread
//   dispatcher is drained after each dispatch. Grown by reading handlers, never by a
//   deny-set — the opposite scoping to 1a.
//
// Run under ASan+UBSan in CI; a memory/UB bug aborts with the reproducing seed already on
// stdout. REPRODUCIBILITY: first stdout line is always `monkey seed=<N>`; a fixed --seed +
// --steps replays a byte-identical sequence (all randomness from one std::mt19937_64).
//
// EXIT CODES: 0 = clean. 2 = an exception escaped Dispatch. 3 = a prehandler probe that
// must be rejected returned Ok (handler ran / gate bypassed). 4 = a malformed error
// envelope. 66 = handler-mode boot failure (fixture/Initialize). Non-zero prints the
// reproducing step.

#include "AppController.h"
#include "Commands/BuiltinCommands.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/ParamValueSynthPure.h"

#include "CommandArgSynth.h"
#include "JiraFakeTrackerFixture.h"        // tests/support — on the include path
#include "ScriptedTrackerBackendFactory.h" // tests/support

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <unistd.h> // getpid — the monkey target is Linux-only (posix-core-check)

using smatchet::cmd::Command;
using smatchet::cmd::CommandContext;
using smatchet::cmd::CommandResult;
using smatchet::cmd::CommandSource;
using smatchet::cmd::ErrorCodeString;
using smatchet::cmd::ParamSpec;
using smatchet::cmd::SynthValidValue;
using smatchet::monkey::ProbeKind;
using smatchet::monkey::ProbeKindName;

namespace {

struct Options {
    std::uint64_t seed = 0;
    bool seedProvided = false;
    long long steps = 2000;
    long long timeBudgetMs = 0; // 0 = no wall-clock cap; bound by --steps only
    std::string mode = "prehandler";
    std::string category;                                                // empty = every category
    std::string fixture = "tests/fixtures/jira_backend/basic-grid.json"; // handler-mode fake backend
    bool verbose = false;
};

// Self-referential / process-affecting commands. In prehandler mode their handlers can
// never run (every probe rejects first); listing them keeps intent clear.
bool IsDenied(const std::string& name) { return name == "app.quit" || name == "ui_test.run" || name == "scenario.run"; }

// Layer 1b ALLOW-LIST: read-only commands whose handlers touch NO g_ui/ImGui frame state
// and never hop to the UI thread (verified by reading their bodies in
// Source/Core/src/Commands/Builtin/BuiltinCommands_{Meta,Config,Perf}.cpp). Grown by
// reading handlers — never auto-expanded. Everything else is skipped-by-default.
bool IsHandlerAllowListed(const std::string& name) {
    return name == "commands.list" || name == "commands.help" || name == "commands.search" ||
           name == "commands.recents" || name == "config.get" || name == "config.path" || name == "perf.snapshot" ||
           name == "perf.frame_count" || name == "debug.thread_dump";
}

const char* SourceName(CommandSource s) { return smatchet::cmd::CommandSourceString(s); }

CommandSource PickSource(std::mt19937_64& rng) {
    switch (smatchet::monkey::PickIndex(rng, 6)) {
    case 0:
        return CommandSource::Cli;
    case 1:
        return CommandSource::Palette;
    case 2:
        return CommandSource::Mcp;
    case 3:
        return CommandSource::Lua;
    case 4:
        return CommandSource::Unreal;
    default:
        return CommandSource::Internal;
    }
}

// Valid args for a handler-mode dispatch: fill every required param with a synth-valid
// value, and each optional param at ~50% so handler bodies see varied valid input.
nlohmann::json BuildValidArgs(const Command& c, std::mt19937_64& rng) {
    nlohmann::json args = nlohmann::json::object();
    for (const ParamSpec& p : c.Params) {
        if (p.Required || smatchet::monkey::PickBool(rng)) {
            args[p.Name] = SynthValidValue(p);
        }
    }
    return args;
}

bool ParseLL(const char* s, long long& out) {
    if (s == nullptr || *s == '\0')
        return false;
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (end == nullptr || *end != '\0')
        return false;
    out = v;
    return true;
}

bool ParseU64(const char* s, std::uint64_t& out) {
    if (s == nullptr || *s == '\0')
        return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == nullptr || *end != '\0')
        return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

const char* MatchOpt(const char* arg, const char* key) {
    const std::size_t n = std::strlen(key);
    if (std::strncmp(arg, key, n) == 0 && arg[n] == '=') {
        return arg + n + 1;
    }
    return nullptr;
}

std::string TempDbPath() {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
    return dir + "/smatchet-monkey-" + std::to_string(static_cast<long long>(getpid())) + ".db";
}

void PrintUsage() {
    std::printf("usage: SmatchetMonkeyCli [--seed=<u64>] [--steps=<n>] [--time-budget-ms=<n>]\n"
                "                         [--mode=prehandler|handlers] [--category=<cat>]\n"
                "                         [--fixture=<jira-fixture.json>] [--verbose]\n"
                "  prehandler (default): fuzz the pre-handler dispatch pipeline (Layer 1a).\n"
                "  handlers: run an allow-list of read-only handlers vs a fake Jira backend (1b).\n"
                "  A fixed --seed+--steps replays a byte-identical sequence.\n");
}

// Well-formed-envelope oracle shared by both modes: a non-Ok result must carry a valid
// CommandError (ToJson works, non-empty message, has a code). Returns false on violation.
bool ErrorEnvelopeWellFormed(const CommandResult& r) {
    if (r.Ok) {
        return true;
    }
    try {
        const nlohmann::json ej = r.Error.ToJson();
        return !r.Error.Message.empty() && ej.contains("code");
    } catch (...) {
        return false;
    }
}

// -------------------------------------------------------------------------------------
// Layer 1a — pre-handler pipeline fuzz.
// -------------------------------------------------------------------------------------
int RunPrehandler(const Options& opts, std::uint64_t seed) {
    std::mt19937_64 rng(seed);

    // Bare headless boot — exactly the BuiltinsFixture recipe (no Initialize, no backend,
    // no threads, no I/O): construct a bare AppController and register every builtin.
    AppController app;
    smatchet::cmd::RegisterBuiltinCommands(app.Commands(), app);

    const std::vector<Command> all = app.Commands().All();
    std::vector<Command> pool;
    pool.reserve(all.size());
    for (const Command& c : all) {
        if (IsDenied(c.Name))
            continue;
        if (!opts.category.empty() && c.Category != opts.category)
            continue;
        pool.push_back(c);
    }
    std::printf("monkey: %zu commands registered, %zu in pool (mode=prehandler, category=%s)\n", all.size(),
                pool.size(), opts.category.empty() ? "*" : opts.category.c_str());

    const Command kNoCmd;
    long long stepsRun = 0, dispatched = 0, skippedNoProbe = 0;
    std::map<std::string, long long> codeHist, probeHist;
    const auto start = std::chrono::steady_clock::now();

    for (long long step = 0; step < opts.steps; ++step) {
        if (opts.timeBudgetMs > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= opts.timeBudgetMs)
                break;
        }
        ++stepsRun;

        const bool doUnknown = (smatchet::monkey::NextU64(rng) % 16) == 0 || pool.empty();
        const Command* chosen = nullptr;
        ProbeKind kind = ProbeKind::UnknownName;
        if (!doUnknown) {
            const Command& c = pool[smatchet::monkey::PickIndex(rng, pool.size())];
            const std::vector<ProbeKind> probes = smatchet::monkey::ApplicableProbes(c);
            if (probes.empty()) {
                ++skippedNoProbe;
                continue;
            }
            kind = probes[smatchet::monkey::PickIndex(rng, probes.size())];
            chosen = &c;
        }

        const smatchet::monkey::FuzzInput in =
            smatchet::monkey::BuildProbe(chosen != nullptr ? *chosen : kNoCmd, kind, rng);
        const std::string name =
            in.overrideName.empty() ? (chosen != nullptr ? chosen->Name : in.overrideName) : in.overrideName;

        CommandContext ctx;
        ctx.ScenarioHost = &app;
        ctx.Threading = &app;
        ctx.Source = PickSource(rng);
        ctx.ConfirmedDestructive = smatchet::monkey::PickBool(rng);
        ctx.DryRun = smatchet::monkey::PickBool(rng);
        ctx.TimeoutMs = static_cast<int>(smatchet::monkey::PickIntInRange(rng, 0, 5000));
        if (kind == ProbeKind::DestructiveGate) {
            ctx.ConfirmedDestructive = false;
            ctx.DryRun = false;
        }

        CommandResult r;
        try {
            r = app.Commands().Dispatch(name, in.args, ctx);
        } catch (const std::exception& e) {
            const std::string argsDump = in.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::fprintf(stderr,
                         "monkey VIOLATION (exception): seed=%llu step=%lld cmd='%s' probe=%s src=%s\n"
                         "  args=%s\n  what()=%s\n  reproduce: SmatchetMonkeyCli --seed=%llu --steps=%lld\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind),
                         SourceName(ctx.Source), argsDump.c_str(), e.what(), static_cast<unsigned long long>(seed),
                         opts.steps);
            return 2;
        } catch (...) {
            std::fprintf(stderr, "monkey VIOLATION (non-std exception): seed=%llu step=%lld cmd='%s' probe=%s\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind));
            return 2;
        }

        ++dispatched;
        ++probeHist[ProbeKindName(kind)];

        if (r.Ok) {
            const std::string argsDump = in.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::fprintf(stderr,
                         "monkey VIOLATION (unexpected Ok — handler ran / gate bypassed):\n"
                         "  seed=%llu step=%lld cmd='%s' probe=%s src=%s dryRun=%d confirmed=%d\n  args=%s\n"
                         "  reproduce: SmatchetMonkeyCli --seed=%llu --steps=%lld\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind),
                         SourceName(ctx.Source), ctx.DryRun ? 1 : 0, ctx.ConfirmedDestructive ? 1 : 0, argsDump.c_str(),
                         static_cast<unsigned long long>(seed), opts.steps);
            return 3;
        }
        if (!ErrorEnvelopeWellFormed(r)) {
            std::fprintf(stderr,
                         "monkey VIOLATION (malformed error envelope): seed=%llu step=%lld cmd='%s' probe=%s code=%s\n",
                         static_cast<unsigned long long>(seed), step, name.c_str(), ProbeKindName(kind),
                         ErrorCodeString(r.Error.Code));
            return 4;
        }
        ++codeHist[ErrorCodeString(r.Error.Code)];

        if (opts.verbose) {
            const std::string argsDump = in.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::printf("step=%lld cmd=%s probe=%s src=%s => code=%s args=%s\n", step, name.c_str(),
                        ProbeKindName(kind), SourceName(ctx.Source), ErrorCodeString(r.Error.Code), argsDump.c_str());
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

// -------------------------------------------------------------------------------------
// Layer 1b — allow-listed handler-body execution vs a fake Jira backend.
// -------------------------------------------------------------------------------------
int RunHandlers(const Options& opts, std::uint64_t seed) {
    std::mt19937_64 rng(seed);

    // Boot a fully-initialized app against the FAKE Jira backend (no network). The fake
    // must be installed BEFORE Initialize (which consumes backendFactory_).
    AppController app;
    const std::string dbPath = TempDbPath();
    try {
        smatchet_tests::JiraFakeTrackerFixture fixture =
            smatchet_tests::JiraFakeTrackerFixture::LoadFromFile(opts.fixture);
        app.SetBackendFactory(std::make_unique<smatchet_tests::ScriptedTrackerBackendFactory>(std::move(fixture)));
        app.Initialize(dbPath, "Jira");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "monkey: handler-mode boot failed (fixture='%s', db='%s'): %s\n", opts.fixture.c_str(),
                     dbPath.c_str(), e.what());
        return 66;
    }

    // Build the allow-listed pool from the REGISTERED commands (so a renamed/removed
    // command is dropped, not a phantom); warn on any allow-list name that isn't present.
    const std::vector<Command> all = app.Commands().All();
    std::vector<Command> pool;
    for (const Command& c : all) {
        if (IsHandlerAllowListed(c.Name) && (opts.category.empty() || c.Category == opts.category)) {
            pool.push_back(c);
        }
    }
    std::printf("monkey: %zu commands registered, %zu allow-listed in pool (mode=handlers, fixture=%s)\n", all.size(),
                pool.size(), opts.fixture.c_str());
    if (pool.empty()) {
        std::printf("monkey: no allow-listed handlers registered — nothing to run (not an error).\n");
        return 0;
    }

    long long stepsRun = 0, dispatched = 0, okCount = 0;
    std::map<std::string, long long> cmdHist, codeHist;
    const auto start = std::chrono::steady_clock::now();

    for (long long step = 0; step < opts.steps; ++step) {
        if (opts.timeBudgetMs > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= opts.timeBudgetMs)
                break;
        }
        ++stepsRun;

        const Command& c = pool[smatchet::monkey::PickIndex(rng, pool.size())];
        const nlohmann::json args = BuildValidArgs(c, rng);

        CommandContext ctx;
        ctx.ScenarioHost = &app;
        ctx.Threading = &app;
        ctx.Source = PickSource(rng);
        ctx.DryRun = smatchet::monkey::PickBool(rng);
        ctx.TimeoutMs = static_cast<int>(smatchet::monkey::PickIntInRange(rng, 0, 5000));
        // The allow-list is non-destructive, so ConfirmedDestructive is irrelevant; leave it
        // false. Handlers run inline (we are on the init/UI thread).

        CommandResult r;
        try {
            r = app.Commands().Dispatch(c.Name, args, ctx);
        } catch (const std::exception& e) {
            const std::string argsDump = args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::fprintf(
                stderr,
                "monkey VIOLATION (handler exception): seed=%llu step=%lld cmd='%s' src=%s\n"
                "  args=%s\n  what()=%s\n  reproduce: SmatchetMonkeyCli --mode=handlers --seed=%llu --steps=%lld\n",
                static_cast<unsigned long long>(seed), step, c.Name.c_str(), SourceName(ctx.Source), argsDump.c_str(),
                e.what(), static_cast<unsigned long long>(seed), opts.steps);
            return 2;
        } catch (...) {
            std::fprintf(stderr, "monkey VIOLATION (handler non-std exception): seed=%llu step=%lld cmd='%s'\n",
                         static_cast<unsigned long long>(seed), step, c.Name.c_str());
            return 2;
        }

        // Flush any incidental main-thread posts (no-op for the inline allow-list, but keeps
        // the loop honest if a future allow-list entry posts work).
        app.mainThreadDispatcher.Drain();

        ++dispatched;
        ++cmdHist[c.Name];

        // Oracle: a read-only handler must not crash and must return a well-formed envelope.
        // Ok OR a clean error are both acceptable (some getters legitimately error on odd args).
        if (!ErrorEnvelopeWellFormed(r)) {
            const std::string argsDump = args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            std::fprintf(
                stderr,
                "monkey VIOLATION (malformed handler result): seed=%llu step=%lld cmd='%s' code=%s\n  args=%s\n",
                static_cast<unsigned long long>(seed), step, c.Name.c_str(), ErrorCodeString(r.Error.Code),
                argsDump.c_str());
            return 4;
        }
        if (r.Ok) {
            ++okCount;
        } else {
            ++codeHist[ErrorCodeString(r.Error.Code)];
        }

        if (opts.verbose) {
            std::printf("step=%lld cmd=%s src=%s => ok=%d\n", step, c.Name.c_str(), SourceName(ctx.Source),
                        r.Ok ? 1 : 0);
        }
    }

    std::printf("monkey summary: seed=%llu mode=handlers stepsRun=%lld dispatched=%lld ok=%lld\n",
                static_cast<unsigned long long>(seed), stepsRun, dispatched, okCount);
    for (std::map<std::string, long long>::const_iterator it = cmdHist.begin(); it != cmdHist.end(); ++it) {
        std::printf("  handler %-18s %lld\n", it->first.c_str(), it->second);
    }
    for (std::map<std::string, long long>::const_iterator it = codeHist.begin(); it != codeHist.end(); ++it) {
        std::printf("  err     %-18s %lld\n", it->first.c_str(), it->second);
    }
    std::printf("monkey: OK (no violations)\n");
    return 0;
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
            if (!ParseU64(v, opts.seed)) {
                std::fprintf(stderr, "monkey: bad --seed '%s'\n", v);
                return 64;
            }
            opts.seedProvided = true;
        } else if ((v = MatchOpt(a, "--steps")) != nullptr) {
            if (!ParseLL(v, opts.steps) || opts.steps < 0) {
                std::fprintf(stderr, "monkey: bad --steps '%s'\n", v);
                return 64;
            }
        } else if ((v = MatchOpt(a, "--time-budget-ms")) != nullptr) {
            if (!ParseLL(v, opts.timeBudgetMs) || opts.timeBudgetMs < 0) {
                std::fprintf(stderr, "monkey: bad --time-budget-ms '%s'\n", v);
                return 64;
            }
        } else if ((v = MatchOpt(a, "--mode")) != nullptr) {
            opts.mode = v;
        } else if ((v = MatchOpt(a, "--category")) != nullptr) {
            opts.category = v;
        } else if ((v = MatchOpt(a, "--fixture")) != nullptr) {
            opts.fixture = v;
        } else if (std::strcmp(a, "--verbose") == 0) {
            opts.verbose = true;
        } else {
            std::fprintf(stderr, "monkey: unknown arg '%s' (try --help)\n", a);
            return 64;
        }
    }

    const std::uint64_t seed = opts.seedProvided ? opts.seed : static_cast<std::uint64_t>(std::random_device{}());
    // CONTRACT: seed line first + flushed, so a crash log always carries the reproducer.
    std::printf("monkey seed=%llu\n", static_cast<unsigned long long>(seed));
    std::fflush(stdout);

    if (opts.mode == "prehandler") {
        return RunPrehandler(opts, seed);
    }
    if (opts.mode == "handlers") {
        return RunHandlers(opts, seed);
    }
    std::fprintf(stderr, "monkey: unknown --mode '%s' (expected prehandler|handlers)\n", opts.mode.c_str());
    return 64;
}
