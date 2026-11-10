using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Xunit;

namespace Translator.Tests;

// Pins the f6/f12 lane discrepancy (MultVec per-site follow-up, Job 1):
// a modelled helper definition whose destination register is also one of
// its arguments (the standard FP accumulator shape) must keep the demand
// it roots for the INCOMING value of that register. The pre-fix analyzer
// rooted argument demands before killing the destination, so the kill
// erased exactly the rooted pass-through demand and reported such
// registers as not-required inputs (MultVec f6/f12 -> planned RBW 0).
public sealed class LaneDemandSelfReferenceTests
{
    private static readonly IReadOnlyDictionary<uint, GuestAbiContract> NoCallees =
        new Dictionary<uint, GuestAbiContract>();

    private static IrValue R(string name) => IrValue.Register(name);
    private static IrValue Imm(long value) => IrValue.Imm(value);

    private static IrFunction Fn(params IrInstruction[] instructions) =>
        new("test_fn", "entry", new[] { new IrBasicBlock("entry", instructions) });

    private static IrCall PsqL(string dest, string addr) =>
        new(dest, "PPC_PsqL", new[] { R(addr), Imm(0), Imm(0) });

    private static IrCall PsqStW1(string addr, string source) =>
        new("", "PPC_PsqSt", new[] { R(addr), R(source), Imm(1), Imm(0) });

    // The MultVec f6 chain (canonical IR of func_80278354, addresses
    // 0x80278370 / 0x8027837C / 0x8027839C / 0x802783A0): two ps_sum0
    // definitions, the second consuming the first's result as its C
    // operand, the first consuming the entry value f6_0 the same way.
    private static IrFunction MultVecF6Chain() => Fn(
        PsqL("f5_0", "r3_0"),
        new IrCall("f6_1", "PPC_PsSum0", new[] { R("f5_0"), R("f5_0"), R("f6_0") }),
        PsqStW1("r5_0", "f6_1"),
        new IrCall("f5_1", "PPC_PsMul", new[] { R("f5_0"), R("f5_0") }),
        new IrCall("f6_2", "PPC_PsSum0", new[] { R("f5_1"), R("f5_1"), R("f6_1") }),
        PsqStW1("r5_0", "f6_2"),
        new IrReturn(null));

    [Fact]
    public void PsSum0Chain_SelfReferentialDefs_RequiresEntryF6()
    {
        var rbw = GuestStateLaneLivenessAnalyzer.RequiredFprInputs(
            MultVecF6Chain(), NoCallees, demandedFprOutputs: 0x40u);
        Assert.Equal(0x40u, rbw);
    }

    [Fact]
    public void PsSum0_SingleSelfReferentialDef_RequiresEntryF12()
    {
        var fn = Fn(
            PsqL("f11_0", "r3_0"),
            new IrCall("f12_1", "PPC_PsSum0", new[] { R("f11_0"), R("f11_0"), R("f12_0") }),
            PsqStW1("r5_0", "f12_1"),
            new IrReturn(null));
        var rbw = GuestStateLaneLivenessAnalyzer.RequiredFprInputs(fn, NoCallees, 0x1000u);
        Assert.Equal(0x1000u, rbw);
    }

    [Fact]
    public void PsSum0_NonSelfReferentialC_RequiresThatRegister()
    {
        // Control: with C = f7 (not the destination), both the pre-fix and
        // post-fix analyzer must demand f7 -- isolates self-reference as
        // the trigger.
        var fn = Fn(
            PsqL("f5_0", "r3_0"),
            new IrCall("f6_1", "PPC_PsSum0", new[] { R("f5_0"), R("f5_0"), R("f7_0") }),
            PsqStW1("r5_0", "f6_1"),
            new IrReturn(null));
        var rbw = GuestStateLaneLivenessAnalyzer.RequiredFprInputs(fn, NoCallees, 0x40u);
        Assert.Equal(0x80u, rbw);
    }

    [Fact]
    public void PsMul_FullOverwrite_StillKillsDemand()
    {
        // Control in the other direction: a non-self-referential full
        // overwrite must still kill the exit demand (the fix must not
        // simply stop killing).
        var fn = Fn(
            PsqL("f5_0", "r3_0"),
            new IrCall("f6_1", "PPC_PsMul", new[] { R("f5_0"), R("f5_0") }),
            PsqStW1("r5_0", "f6_1"),
            new IrReturn(null));
        var rbw = GuestStateLaneLivenessAnalyzer.RequiredFprInputs(fn, NoCallees, 0x40u);
        Assert.Equal(0u, rbw);
    }

    [Fact]
    public void PsMadd_SelfAccumulator_RequiresEntryF5()
    {
        var fn = Fn(
            PsqL("f3_0", "r3_0"),
            PsqL("f4_0", "r4_0"),
            new IrCall("f5_1", "PPC_PsMadd", new[] { R("f5_0"), R("f3_0"), R("f4_0") }),
            new IrCall("", "PPC_PsqSt", new[] { R("r5_0"), R("f5_1"), Imm(0), Imm(0) }),
            new IrReturn(null));
        var rbw = GuestStateLaneLivenessAnalyzer.RequiredFprInputs(fn, NoCallees, 0x20u);
        Assert.Equal(0x20u, rbw);
    }

    [Fact]
    public void PsSum0Chain_SeedZero_RequiresNothing()
    {
        var rbw = GuestStateLaneLivenessAnalyzer.RequiredFprInputs(
            MultVecF6Chain(), NoCallees, demandedFprOutputs: 0u);
        Assert.Equal(0u, rbw);
    }

    [Fact]
    public void PsSum0Chain_EntryLaneAttribution_DemandsPs1Only()
    {
        // Lane attribution per the analyzer's own PsSum0 map
        // (result ps1 <- C.ps1): the entry demand lands on ps1 of f6.
        GuestStateLaneLivenessAnalyzer.RequiredFprInputs(
            MultVecF6Chain(), NoCallees, demandedFprOutputs: 0x40u);
        var lanes = GuestStateLaneLivenessAnalyzer.ProbeEntryDemand;
        Assert.Equal(0u, lanes.Ps0);
        Assert.Equal(0x40u, lanes.Ps1);
    }
}
