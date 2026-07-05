// CommandArgSynth — bucket-A coverage for the nightly command-registry monkey's NEW
// logic: the shared pure arg synthesizer (Source/Core/include/Commands/ParamValueSynthPure.h)
// and the seeded fuzz generators (tests/monkey/CommandArgSynth.h). Header-only, no registry
// link — mirrors ParamBoundsPure.test.cpp / CommandSourceTrust.test.cpp.
//
// Pins the two properties the monkey's safety + reproducibility rest on:
//   1. Reproducibility — a fixed seed replays a byte-identical probe sequence.
//   2. Guaranteed pre-handler reject — every probe genuinely violates the required /
//      type / bounds rule that makes CommandRegistry::Dispatch reject it BEFORE the
//      handler runs, checked against the REAL ParamValueWithinBounds predicate. This is
//      what keeps the monkey false-positive-free (no app-mutating handler runs headless).
// The nightly's full-archive ASan/UBSan run (monkey-nightly.yml) is the integration lane;
// this is the fast pure-logic backstop that runs on every PR in the normal ctest rig.

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Commands/Command.h"
#include "Commands/ParamBoundsPure.h"
#include "Commands/ParamValueSynthPure.h"

#include "CommandArgSynth.h" // tests/monkey — on the SmatchetTests include path

using smatchet::cmd::Command;
using smatchet::cmd::ParamSpec;
using smatchet::cmd::ParamType;
using smatchet::cmd::ParamValueWithinBounds;
using smatchet::cmd::SynthValidValue;
using smatchet::monkey::ApplicableProbes;
using smatchet::monkey::BuildProbe;
using smatchet::monkey::FirstBoundedNumeric;
using smatchet::monkey::FirstBoundedString;
using smatchet::monkey::FuzzInput;
using smatchet::monkey::ProbeKind;
using smatchet::monkey::ProbeKindName;

namespace {

ParamSpec Param(std::string name, ParamType t, bool required) {
    ParamSpec p;
    p.Name = std::move(name);
    p.Type = t;
    p.Required = required;
    return p;
}

// A representative registry covering every param flavour + destructive / zero-param /
// Json-only shapes, so every probe kind and the "unprobeable" classification are hit.
std::vector<Command> MakeCommands() {
    std::vector<Command> v;
    {
        Command c;
        c.Name = "tickets.search";
        c.Category = "tickets";
        c.Params.push_back(Param("query", ParamType::String, true));
        ParamSpec limit = Param("limit", ParamType::Int, false);
        limit.MinInt = std::make_shared<long long>(1);
        limit.MaxInt = std::make_shared<long long>(100);
        c.Params.push_back(limit);
        v.push_back(std::move(c));
    }
    {
        Command c;
        c.Name = "config.set_theme";
        c.Category = "config";
        ParamSpec key = Param("name", ParamType::String, true);
        key.MaxLen = 16;
        c.Params.push_back(key);
        ParamSpec mode = Param("mode", ParamType::String, false);
        mode.Enum = {"light", "dark"};
        c.Params.push_back(mode);
        v.push_back(std::move(c));
    }
    {
        Command c;
        c.Name = "tickets.delete";
        c.Category = "tickets";
        c.Destructive = true;
        c.Params.push_back(Param("id", ParamType::String, true));
        v.push_back(std::move(c));
    }
    {
        Command c;
        c.Name = "perf.set_budget";
        c.Category = "perf";
        ParamSpec ms = Param("ms", ParamType::Number, false);
        ms.MaxInt = std::make_shared<long long>(5000);
        c.Params.push_back(ms);
        v.push_back(std::move(c));
    }
    {
        Command c; // Json-only, non-destructive -> unprobeable
        c.Name = "debug.dump";
        c.Category = "debug";
        c.Params.push_back(Param("payload", ParamType::Json, false));
        v.push_back(std::move(c));
    }
    {
        Command c; // zero-param, non-destructive -> unprobeable
        c.Name = "view.notifications";
        c.Category = "view";
        v.push_back(std::move(c));
    }
    {
        Command c;
        c.Name = "grid.set_rows";
        c.Category = "grid";
        ParamSpec rows = Param("rows", ParamType::Int, true);
        rows.MinInt = std::make_shared<long long>(1);
        c.Params.push_back(rows);
        c.Params.push_back(Param("wrap", ParamType::Bool, false));
        v.push_back(std::move(c));
    }
    return v;
}

// Mirror of the driver's per-step choice + probe build, serialized so two runs can be
// compared for the reproducibility contract.
std::string RunSequence(const std::vector<Command>& cmds, std::uint64_t seed, int steps) {
    std::mt19937_64 rng(seed);
    const Command kNoCmd;
    std::string log;
    for (int step = 0; step < steps; ++step) {
        const bool doUnknown = (smatchet::monkey::NextU64(rng) % 16) == 0 || cmds.empty();
        const Command* chosen = nullptr;
        ProbeKind kind = ProbeKind::UnknownName;
        if (!doUnknown) {
            const Command& c = cmds[smatchet::monkey::PickIndex(rng, cmds.size())];
            const std::vector<ProbeKind> probes = ApplicableProbes(c);
            if (probes.empty()) {
                log += "skip\n";
                continue;
            }
            kind = probes[smatchet::monkey::PickIndex(rng, probes.size())];
            chosen = &c;
        }
        const FuzzInput in = BuildProbe(chosen != nullptr ? *chosen : kNoCmd, kind, rng);
        const std::string name = in.overrideName.empty() ? (chosen != nullptr ? chosen->Name : std::string())
                                                          : in.overrideName;
        log += name + "|" + ProbeKindName(kind) + "|" +
               in.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    }
    return log;
}

} // namespace

TEST_CASE("SynthValidValue produces in-bounds, spec-respecting values") {
    // Enum -> first enum value.
    ParamSpec mode = Param("mode", ParamType::String, false);
    mode.Enum = {"light", "dark"};
    CHECK(SynthValidValue(mode) == nlohmann::json("light"));

    // Default (non-null) wins over type synthesis.
    ParamSpec withDefault = Param("x", ParamType::Int, false);
    withDefault.Default = std::make_shared<nlohmann::json>(7);
    CHECK(SynthValidValue(withDefault) == nlohmann::json(7));

    // A bounded Int synthesizes a value that PASSES the real bounds predicate.
    ParamSpec limit = Param("limit", ParamType::Int, false);
    limit.MinInt = std::make_shared<long long>(5);
    limit.MaxInt = std::make_shared<long long>(9);
    std::string why;
    nlohmann::json detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(limit, SynthValidValue(limit), why, detail) == true);
}

TEST_CASE("BuildProbe is reproducible — a fixed seed replays an identical sequence") {
    const std::vector<Command> cmds = MakeCommands();
    const std::string a = RunSequence(cmds, 42, 3000);
    const std::string b = RunSequence(cmds, 42, 3000);
    const std::string c = RunSequence(cmds, 43, 3000);
    CHECK(a == b);   // determinism contract
    CHECK(a != c);   // a different seed drives a different sequence
    CHECK(a.size() > 0);
}

TEST_CASE("every applicable probe is a genuine pre-handler reject") {
    const std::vector<Command> cmds = MakeCommands();
    long long checked = 0;
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        std::mt19937_64 rng(seed);
        for (const Command& c : cmds) {
            for (ProbeKind k : ApplicableProbes(c)) {
                const FuzzInput in = BuildProbe(c, k, rng);
                CAPTURE(c.Name);
                CAPTURE(ProbeKindName(k));
                REQUIRE(in.args.is_object()); // Dispatch expects an object

                switch (k) {
                case ProbeKind::MissingRequired: {
                    bool anyRequiredMissing = false;
                    for (const ParamSpec& p : c.Params) {
                        if (p.Required && !in.args.contains(p.Name)) {
                            anyRequiredMissing = true;
                            break;
                        }
                    }
                    CHECK(anyRequiredMissing);
                    break;
                }
                case ProbeKind::WrongType: {
                    const ParamSpec* scalar = smatchet::cmd::FirstScalarParam(c);
                    REQUIRE(scalar != nullptr);
                    REQUIRE(in.args.contains(scalar->Name));
                    const nlohmann::json& v = in.args[scalar->Name];
                    CHECK((v.is_array() || v.is_object())); // never coercible to a scalar
                    break;
                }
                case ProbeKind::OutOfBounds: {
                    const ParamSpec* np = FirstBoundedNumeric(c);
                    REQUIRE(np != nullptr);
                    REQUIRE(in.args.contains(np->Name));
                    std::string why;
                    nlohmann::json detail = nlohmann::json::object();
                    CHECK(ParamValueWithinBounds(*np, in.args[np->Name], why, detail) == false);
                    break;
                }
                case ProbeKind::OverLength: {
                    const ParamSpec* sp = FirstBoundedString(c);
                    REQUIRE(sp != nullptr);
                    REQUIRE(in.args.contains(sp->Name));
                    std::string why;
                    nlohmann::json detail = nlohmann::json::object();
                    CHECK(ParamValueWithinBounds(*sp, in.args[sp->Name], why, detail) == false);
                    CHECK(in.args[sp->Name].get<std::string>().size() > sp->MaxLen);
                    break;
                }
                case ProbeKind::DestructiveGate: {
                    CHECK(c.Destructive);
                    for (const ParamSpec& p : c.Params) {
                        if (p.Required) {
                            CHECK(in.args.contains(p.Name)); // valid args -> the gate is the stopper
                        }
                    }
                    break;
                }
                case ProbeKind::UnknownName:
                    CHECK_FALSE(in.overrideName.empty());
                    break;
                }
                ++checked;
            }
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("probeability classification — Json-only / zero-param non-destructive are unprobeable") {
    for (const Command& c : MakeCommands()) {
        CAPTURE(c.Name);
        const std::vector<ProbeKind> probes = ApplicableProbes(c);
        if (c.Name == "debug.dump" || c.Name == "view.notifications") {
            CHECK(probes.empty());
        } else {
            CHECK_FALSE(probes.empty());
        }
    }
}
