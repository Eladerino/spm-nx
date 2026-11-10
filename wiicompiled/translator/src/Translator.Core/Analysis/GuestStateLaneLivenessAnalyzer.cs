using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// Lane-granular (paired-single ps0/ps1) backward demand analysis over one
/// function's canonical IR. The register-granular liveness in
/// <see cref="GuestStateLivenessAnalyzer"/> cannot see that an incoming FPR
/// value may be consumed only in a lane that no demanded output and no store
/// ever observes (the PSMTXMultVec shape: incoming f6/f12 reach only the ps1
/// lanes of results that are stored with w=1 psq stores, which read ps0
/// only). This analyzer tracks demand per lane so the state-free planner can
/// drop an FPR read-before-write bit when neither entry lane is demanded.
///
/// Soundness rests on the defaults, not on the table: any instruction or
/// helper without an exact lane entry below demands BOTH lanes of every
/// architectural FPR it reads, and an FPR-defining instruction without an
/// exact entry also keeps the destination's own incoming lanes demanded
/// (nothing is killed). A lane is killed only by a form listed here whose
/// runtime semantics were verified in the runtime headers (see the per-form
/// comments). Demand is reported per architectural register: a register is a
/// required input when either of its entry lanes is demanded, because the
/// native state-free parameter is one packed PPC_FPR slot either way.
/// </summary>
public static class GuestStateLaneLivenessAnalyzer
{
    /// <summary>Live-lane bitsets: bit N set means register fN's lane is demanded.</summary>
    private readonly record struct LaneDemand(uint Ps0, uint Ps1)
    {
        public static LaneDemand Empty => default;
        public LaneDemand Union(LaneDemand other) => new(Ps0 | other.Ps0, Ps1 | other.Ps1);
    }

    /// <summary>
    /// Computes the mask of FPRs whose incoming value is demanded (in either
    /// lane) at function entry, given the FPR registers demanded at exit.
    /// Demanded exit registers demand both lanes: callers consume the packed
    /// pair and no caller-side lane liveness exists. Stores, guest calls and
    /// comparisons inside the body root their operands regardless of the exit
    /// seed, mirroring the register-granular transfer.
    /// </summary>
    public static uint RequiredFprInputs(
        IrFunction function,
        IReadOnlyDictionary<uint, GuestAbiContract> calleeContracts,
        uint demandedFprOutputs)
    {
        var blockList = function.Blocks;
        var blockCount = blockList.Count;
        var blocks = blockList.ToDictionary(block => block.Label, StringComparer.Ordinal);
        var successors = new string[blockCount][];
        for (var index = 0; index < blockCount; ++index)
        {
            var successorCount = Successors(
                blockList[index],
                index + 1 < blockCount ? blockList[index + 1].Label : null,
                out var first,
                out var second);
            var keepFirst = successorCount >= 1 && blocks.ContainsKey(first);
            var keepSecond = successorCount == 2 && blocks.ContainsKey(second);
            successors[index] =
                keepFirst && keepSecond ? new[] { first, second }
                : keepFirst ? new[] { first }
                : keepSecond ? new[] { second }
                : Array.Empty<string>();
        }

        var exitLive = new LaneDemand(demandedFprOutputs, demandedFprOutputs);
        var liveIn = blockList.ToDictionary(block => block.Label, _ => LaneDemand.Empty, StringComparer.Ordinal);
        var liveOut = blockList.ToDictionary(block => block.Label, _ => LaneDemand.Empty, StringComparer.Ordinal);

        bool changed;
        do
        {
            changed = false;
            for (var blockIndex = blockCount - 1; blockIndex >= 0; --blockIndex)
            {
                var block = blockList[blockIndex];
                var blockSuccessors = successors[blockIndex];
                var outgoing = exitLive;
                if (blockSuccessors.Length != 0)
                {
                    outgoing = LaneDemand.Empty;
                    foreach (var label in blockSuccessors) outgoing = outgoing.Union(liveIn[label]);
                }

                var incoming = TransferBlock(block, outgoing, calleeContracts);
                if (outgoing != liveOut[block.Label] || incoming != liveIn[block.Label])
                {
                    liveOut[block.Label] = outgoing;
                    liveIn[block.Label] = incoming;
                    changed = true;
                }
            }
        } while (changed);

        var entry = liveIn[function.EntryLabel];
        return entry.Ps0 | entry.Ps1;
    }

    private static LaneDemand TransferBlock(
        IrBasicBlock block,
        LaneDemand live,
        IReadOnlyDictionary<uint, GuestAbiContract> calleeContracts)
    {
        for (var index = block.Instructions.Count - 1; index >= 0; --index)
            live = Transfer(block.Instructions[index], live, calleeContracts);
        return live;
    }

    private static LaneDemand Transfer(
        IrInstruction instruction,
        LaneDemand after,
        IReadOnlyDictionary<uint, GuestAbiContract> calleeContracts)
    {
        switch (instruction)
        {
            // Memory-visible effects are roots regardless of demanded outputs.
            case IrStore store:
                return RootStoreValue(after, store.Source, store.SizeBytes);
            case IrResolvedStore store:
                return RootStoreValue(after, store.Source, store.SizeBytes);
            case IrResolvedPsqStore store:
                // w=1 stores read ps0 only; w=0 stores read the pair
                // (ppc_isa_quantized.h: w selects single vs paired access).
                return store.W == 1
                    ? RootLanes(after, store.Source, ps0: true, ps1: false)
                    : RootLanes(after, store.Source, ps0: true, ps1: true);
            case IrResolvedStorePair store:
            {
                var live = RootLanes(after, store.FirstSource, ps0: true, ps1: true);
                return RootLanes(live, store.SecondSource, ps0: true, ps1: true);
            }
            case IrResolvedPsqLoad load:
            {
                // W=0 loads the pair from memory. W=1 loads ps0 from memory
                // and defines ps1 as the constant 1.0f: every W=1 path in
                // ppc_isa_quantized.h (float and quantized, fast / resolved /
                // stack, inline and the PPC_PsqLStateFallback bodies in
                // ppc_quantized.cpp) funnels to a PpcLoadSinglePsq* helper
                // returning PpcMakePairedResultInline(value, 1.0f). Both
                // lanes are therefore fully defined by the load.
                var live = after;
                if (FprIndex(load.Destination) is int dest)
                    live = Kill(live, dest, ps0: true, ps1: true);
                return live;
            }
            case IrResolvedLoad load:
                return TransferScalarLoad(after, load.Destination, load.SizeBytes);
            case IrLoad load:
                return TransferScalarLoad(after, load.Destination, load.SizeBytes);
            case IrResolvedLoadPair pair:
            {
                // Values arrive from guest memory, never from an incoming FPR.
                var live = after;
                if (FprIndex(pair.FirstDestination) is int first) live = Kill(live, first, ps0: true, ps1: true);
                if (FprIndex(pair.SecondDestination) is int second) live = Kill(live, second, ps0: true, ps1: true);
                return live;
            }
            case IrCall call:
                return TransferCall(call, after, calleeContracts);
            case IrIndirectCall:
                // An unresolvable callee may consume the whole register file.
                return new LaneDemand(uint.MaxValue, uint.MaxValue);
            case IrUndefined:
                return new LaneDemand(uint.MaxValue, uint.MaxValue);
            case IrAssign assign:
                return TransferAssign(assign, after);
            case IrBinary binary:
                return TransferBinary(binary, after);
            case IrPhi phi:
            {
                // A phi merges predecessor versions of one architectural
                // register lane-for-lane; demand passes through unchanged.
                // (If the destination were not an FPR the phi cannot carry
                // FPR state; its sources are rooted by the universal rule.)
                if (FprIndex(phi.Destination) is not null) return after;
                return RootFprUses(after, instruction);
            }
            default:
                // Branches, returns, CR compares (IrSetCrField), jumps and
                // every unmodelled form: root both lanes of every FPR read.
                return RootFprUses(after, instruction);
        }
    }

    private static LaneDemand TransferScalarLoad(LaneDemand after, string destination, int sizeBytes)
    {
        if (FprIndex(destination) is not int dest) return after;
        // An 8-byte load replaces the whole PPC_FPR double. A 4-byte (lfs)
        // load defines ps0; the destination's own incoming ps1 stays
        // demanded (scalar writes preserve PS1 in this architecture).
        return sizeBytes >= 8
            ? Kill(after, dest, ps0: true, ps1: true)
            : Kill(after, dest, ps0: true, ps1: false);
    }

    private static LaneDemand TransferAssign(IrAssign assign, LaneDemand after)
    {
        if (FprIndex(assign.Destination) is not int dest)
            return RootFprUses(after, assign);
        if (assign.Value.Kind == "register" && FprIndex(assign.Value.RegisterName) is int source)
        {
            // fmr: scalar move. ps0 comes from the source's ps0; ps1 is the
            // destination's own incoming ps1 (scalar writes preserve PS1,
            // unlike ps_mr, which the lifter emits as PPC_PsMr).
            var live = after;
            if (Demanded(live, dest, lane: 0)) live = RootLanes(live, source, ps0: true, ps1: false);
            return Kill(live, dest, ps0: true, ps1: false);
        }
        // Constant, GPR or temporary source: the value does not come from an
        // incoming FPR (temporaries carrying FPR values are rooted at their
        // own defining instruction, which is unmodelled or non-FPR-destined
        // and therefore roots its FPR reads unconditionally).
        return Kill(after, dest, ps0: true, ps1: true);
    }

    private static LaneDemand TransferBinary(IrBinary binary, LaneDemand after)
    {
        if (FprIndex(binary.Destination) is not int dest)
            return RootFprUses(after, binary);
        // Every FPR-destined IrBinary the lifter emits is a scalar FP op
        // (fneg / fabs / frsp / fctiw / fctiwz / scalar mul): ps0 is computed
        // from the operands' ps0 lanes and ps1 is the destination's own
        // incoming ps1 (scalar writes preserve PS1).
        var live = after;
        if (Demanded(live, dest, lane: 0))
        {
            live = RootLanes(live, binary.Left, ps0: true, ps1: false);
            live = RootLanes(live, binary.Right, ps0: true, ps1: false);
        }
        return Kill(live, dest, ps0: true, ps1: false);
    }

    private static LaneDemand TransferCall(
        IrCall call,
        LaneDemand after,
        IReadOnlyDictionary<uint, GuestAbiContract> calleeContracts)
    {
        if (GuestTargetParser.TryParseAddress(call.Target, out var targetAddress) &&
            calleeContracts.TryGetValue(targetAddress, out var contract))
        {
            // Guest call: register-level contracts are the only
            // interprocedural lane information that exists, so both lanes of
            // every callee read-before-write register are demanded, and no
            // lane is killed across the call (a register-level definite
            // write may be a scalar write, whose ps1 is the incoming ps1).
            return after.Union(new LaneDemand(
                contract.FprReadBeforeWriteMask, contract.FprReadBeforeWriteMask));
        }

        var dest = FprIndex(call.Destination);
        var target = NormalizeHelperName(call.Target);

        if (target.Equals("PPC_PsqL", StringComparison.OrdinalIgnoreCase))
        {
            // Same verified lane behaviour as IrResolvedPsqLoad: W=0 loads
            // the pair; W=1 defines ps1 as the constant 1.0f. With an
            // unknown W nothing is killed (the destination lanes stay
            // demanded as incoming values, the sound default for a load).
            if (dest is int loadDest && call.Arguments.Count > 1 &&
                call.Arguments[1].Constant is long w && w is 0 or 1)
                return Kill(after, loadDest, ps0: true, ps1: true);
            return after;
        }
        if (target.Equals("PPC_PsqSt", StringComparison.OrdinalIgnoreCase))
        {
            // Arguments are (address, source, w, gqr index).
            if (call.Arguments.Count > 2 && call.Arguments[2].Constant is long w)
                return w == 1
                    ? RootLanes(after, call.Arguments[1], ps0: true, ps1: false)
                    : RootLanes(after, call.Arguments[1], ps0: true, ps1: true);
            return call.Arguments.Count > 1
                ? RootLanes(after, call.Arguments[1], ps0: true, ps1: true)
                : after;
        }
        if (target.StartsWith("PPC_PsqLKnown", StringComparison.OrdinalIgnoreCase) ||
            target.StartsWith("PPC_PsqStKnown", StringComparison.OrdinalIgnoreCase))
        {
            // Known-GQR psq forms encode W in the target name, which this
            // analysis does not parse: loads kill nothing, stores root both
            // lanes of every FPR argument.
            if (target.StartsWith("PPC_PsqSt", StringComparison.OrdinalIgnoreCase))
                return RootFprUses(after, call);
            return after;
        }

        if (dest is not int destFpr)
        {
            // Comparisons and every other helper whose result is not an FPR
            // (CR fields, GPRs, nothing): root both lanes of FPR arguments.
            return RootFprUses(after, call);
        }

        // Helper lane table. Argument lane maps are (source lane per dest
        // lane) and were verified against runtime/include/isa/ppc_isa_float.h
        // for every entry. Anything absent falls to the default at the end:
        // both lanes of every argument are demanded and the destination's
        // incoming lanes stay demanded (no kill).
        if (LaneWiseHelpers.Contains(target))
        {
            var live = after;
            foreach (var lane in DemandedLanes(live, destFpr))
                foreach (var argument in call.Arguments)
                    live = RootLanes(live, argument, ps0: lane == 0, ps1: lane == 1);
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (target.Equals("PPC_PsMr", StringComparison.OrdinalIgnoreCase))
        {
            // ps_mr copies both lanes (paired move).
            var live = after;
            foreach (var lane in DemandedLanes(live, destFpr))
                foreach (var argument in call.Arguments)
                    live = RootLanes(live, argument, ps0: lane == 0, ps1: lane == 1);
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (target.Equals("PPC_PsMuls0", StringComparison.OrdinalIgnoreCase) ||
            target.Equals("PPC_PsMadds0", StringComparison.OrdinalIgnoreCase))
        {
            // Both dest lanes: arg0 lane-wise, the middle argument's ps0 is
            // broadcast, arg2 (madds only) lane-wise (ppc_isa_float.h:
            // PPC_PsMuls0Inline / PPC_PsMadds0Inline use BroadcastPs0).
            var live = after;
            var lanes = DemandedLanes(live, destFpr).ToArray();
            if (lanes.Length != 0)
            {
                foreach (var lane in lanes)
                {
                    if (call.Arguments.Count > 0) live = RootLanes(live, call.Arguments[0], ps0: lane == 0, ps1: lane == 1);
                    if (call.Arguments.Count > 2) live = RootLanes(live, call.Arguments[2], ps0: lane == 0, ps1: lane == 1);
                }
                if (call.Arguments.Count > 1) live = RootLanes(live, call.Arguments[1], ps0: true, ps1: false);
            }
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (target.Equals("PPC_PsMuls1", StringComparison.OrdinalIgnoreCase) ||
            target.Equals("PPC_PsMadds1", StringComparison.OrdinalIgnoreCase))
        {
            var live = after;
            var lanes = DemandedLanes(live, destFpr).ToArray();
            if (lanes.Length != 0)
            {
                foreach (var lane in lanes)
                {
                    if (call.Arguments.Count > 0) live = RootLanes(live, call.Arguments[0], ps0: lane == 0, ps1: lane == 1);
                    if (call.Arguments.Count > 2) live = RootLanes(live, call.Arguments[2], ps0: lane == 0, ps1: lane == 1);
                }
                if (call.Arguments.Count > 1) live = RootLanes(live, call.Arguments[1], ps0: false, ps1: true);
            }
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (target.Equals("PPC_PsSum0", StringComparison.OrdinalIgnoreCase))
        {
            // PPC_PsSum0Inline: ps0 = Ps0(a) + Ps1(b); ps1 = Ps1(c).
            var live = after;
            if (Demanded(live, destFpr, lane: 0))
            {
                if (call.Arguments.Count > 0) live = RootLanes(live, call.Arguments[0], ps0: true, ps1: false);
                if (call.Arguments.Count > 1) live = RootLanes(live, call.Arguments[1], ps0: false, ps1: true);
            }
            if (Demanded(live, destFpr, lane: 1) && call.Arguments.Count > 2)
                live = RootLanes(live, call.Arguments[2], ps0: false, ps1: true);
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (target.Equals("PPC_PsSum1", StringComparison.OrdinalIgnoreCase))
        {
            // PPC_PsSum1Inline: ps0 = Ps0(c); ps1 = Ps0(a) + Ps1(b).
            var live = after;
            if (Demanded(live, destFpr, lane: 0) && call.Arguments.Count > 2)
                live = RootLanes(live, call.Arguments[2], ps0: true, ps1: false);
            if (Demanded(live, destFpr, lane: 1))
            {
                if (call.Arguments.Count > 0) live = RootLanes(live, call.Arguments[0], ps0: true, ps1: false);
                if (call.Arguments.Count > 1) live = RootLanes(live, call.Arguments[1], ps0: false, ps1: true);
            }
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (MergeHelpers.TryGetValue(target, out var merge))
        {
            // PPC_PsMergeXYInline: dest ps0 = a's lane X, dest ps1 = b's
            // lane Y (ppc_isa_float.h merge block).
            var live = after;
            if (Demanded(live, destFpr, lane: 0) && call.Arguments.Count > 0)
                live = RootLanes(live, call.Arguments[0], ps0: merge.Ps0FromA == 0, ps1: merge.Ps0FromA == 1);
            if (Demanded(live, destFpr, lane: 1) && call.Arguments.Count > 1)
                live = RootLanes(live, call.Arguments[1], ps0: merge.Ps1FromB == 0, ps1: merge.Ps1FromB == 1);
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (target.Equals("PPC_PsFromScalar", StringComparison.OrdinalIgnoreCase))
        {
            // PPC_PsFromScalarInline: both dest lanes = source ps0.
            var live = after;
            if ((Demanded(live, destFpr, lane: 0) || Demanded(live, destFpr, lane: 1)) &&
                call.Arguments.Count > 0)
                live = RootLanes(live, call.Arguments[0], ps0: true, ps1: false);
            return Kill(live, destFpr, ps0: true, ps1: true);
        }
        if (target.Equals("PPC_PsToScalar", StringComparison.OrdinalIgnoreCase))
        {
            // Produces a scalar: dest ps0 = source ps0; dest ps1 is the
            // destination's own incoming ps1 (scalar result convention).
            var live = after;
            if (Demanded(live, destFpr, lane: 0) && call.Arguments.Count > 0)
                live = RootLanes(live, call.Arguments[0], ps0: true, ps1: false);
            return Kill(live, destFpr, ps0: true, ps1: false);
        }
        if (ScalarSingleHelpers.Contains(target))
        {
            // Single-precision scalar ops: dest ps0 from the arguments' ps0
            // lanes; dest ps1 is the destination's own incoming ps1.
            var live = after;
            if (Demanded(live, destFpr, lane: 0))
                foreach (var argument in call.Arguments)
                    live = RootLanes(live, argument, ps0: true, ps1: false);
            return Kill(live, destFpr, ps0: true, ps1: false);
        }
        if (ScalarDoubleHelpers.Contains(target))
        {
            // Double-precision scalar ops consume and produce the whole
            // 64-bit double, i.e. both packed lanes of every argument.
            var live = after;
            if (Demanded(live, destFpr, lane: 0) || Demanded(live, destFpr, lane: 1))
                foreach (var argument in call.Arguments)
                    live = RootLanes(live, argument, ps0: true, ps1: true);
            return Kill(live, destFpr, ps0: true, ps1: true);
        }

        // DEFAULT (the soundness anchor): an unmodelled FPR-writing helper
        // demands both lanes of every FPR argument, and the destination's
        // incoming lanes stay demanded. No lane is ever killed here.
        return RootFprUses(after, call);
    }

    private static readonly HashSet<string> LaneWiseHelpers = new(StringComparer.OrdinalIgnoreCase)
    {
        // dest lane i computed from argument lane i of every argument
        // (ppc_isa_float.h pair arithmetic and PPC_PsSelInline, which
        // selects per lane from lhs / control / rhs lane i).
        "PPC_PsAdd", "PPC_PsSub", "PPC_PsMul", "PPC_PsDiv",
        "PPC_PsMadd", "PPC_PsMsub", "PPC_PsNmadd", "PPC_PsNmsub",
        "PPC_PsNeg", "PPC_PsAbs", "PPC_PsNabs",
        "PPC_PsRes", "PPC_PsRsqrte", "PPC_PsSel"
    };

    private static readonly Dictionary<string, (int Ps0FromA, int Ps1FromB)> MergeHelpers =
        new(StringComparer.OrdinalIgnoreCase)
        {
            ["PPC_PsMerge00"] = (0, 0),
            ["PPC_PsMerge01"] = (0, 1),
            ["PPC_PsMerge10"] = (1, 0),
            ["PPC_PsMerge11"] = (1, 1)
        };

    private static readonly HashSet<string> ScalarSingleHelpers = new(StringComparer.OrdinalIgnoreCase)
    {
        "PPC_Fadds", "PPC_Fsubs", "PPC_Fmuls", "PPC_Fdivs",
        "PPC_Fmadds", "PPC_Fmsubs", "PPC_Fnmadds", "PPC_Fnmsubs", "PPC_Fres"
    };

    private static readonly HashSet<string> ScalarDoubleHelpers = new(StringComparer.OrdinalIgnoreCase)
    {
        "PPC_Fadd", "PPC_Fsub", "PPC_Fmul", "PPC_Fdiv",
        "PPC_Fmadd", "PPC_Fmsub", "PPC_Fnmadd", "PPC_Fnmsub",
        "PPC_Fsqrt", "PPC_Frsqrte", "PPC_Fsel"
    };

    private static string NormalizeHelperName(string target) =>
        target.EndsWith("NoNi", StringComparison.OrdinalIgnoreCase) ? target[..^4] : target;

    private static LaneDemand RootStoreValue(LaneDemand after, IrValue source, int sizeBytes) =>
        // A 4-byte (scalar) store reads ps0 only; an 8-byte store reads the
        // whole packed pair. Other sizes cannot carry an FPR in this IR.
        RootLanes(after, source, ps0: true, ps1: sizeBytes >= 8);

    private static LaneDemand RootLanes(LaneDemand live, IrValue value, bool ps0, bool ps1)
    {
        if (value.Kind == "register" && FprIndex(value.RegisterName) is int index)
            return RootLanes(live, index, ps0, ps1);
        return live;
    }

    private static LaneDemand RootLanes(LaneDemand live, string? name, bool ps0, bool ps1)
    {
        if (FprIndex(name) is int index) return RootLanes(live, index, ps0, ps1);
        return live;
    }

    private static LaneDemand RootLanes(LaneDemand live, int index, bool ps0, bool ps1) =>
        new(ps0 ? live.Ps0 | (1u << index) : live.Ps0,
            ps1 ? live.Ps1 | (1u << index) : live.Ps1);

    private static LaneDemand Kill(LaneDemand live, int index, bool ps0, bool ps1) =>
        new(ps0 ? live.Ps0 & ~(1u << index) : live.Ps0,
            ps1 ? live.Ps1 & ~(1u << index) : live.Ps1);

    private static bool Demanded(LaneDemand live, int index, int lane) =>
        ((lane == 0 ? live.Ps0 : live.Ps1) & (1u << index)) != 0;

    private static IEnumerable<int> DemandedLanes(LaneDemand live, int index)
    {
        if (Demanded(live, index, lane: 0)) yield return 0;
        if (Demanded(live, index, lane: 1)) yield return 1;
    }

    /// <summary>Adds both lanes of every architectural FPR the instruction reads.</summary>
    private static LaneDemand RootFprUses(LaneDemand live, IrInstruction instruction)
    {
        switch (instruction)
        {
            case IrAssign value: live = RootLanes(live, value.Value, ps0: true, ps1: true); break;
            case IrBinary value:
                live = RootLanes(live, value.Left, ps0: true, ps1: true);
                live = RootLanes(live, value.Right, ps0: true, ps1: true);
                break;
            case IrStore value: live = RootLanes(live, value.Source, ps0: true, ps1: true); break;
            case IrResolvedStore value: live = RootLanes(live, value.Source, ps0: true, ps1: true); break;
            case IrResolvedPsqStore value: live = RootLanes(live, value.Source, ps0: true, ps1: true); break;
            case IrResolvedStorePair value:
                live = RootLanes(live, value.FirstSource, ps0: true, ps1: true);
                live = RootLanes(live, value.SecondSource, ps0: true, ps1: true);
                break;
            case IrCall value:
                foreach (var argument in value.Arguments)
                    live = RootLanes(live, argument, ps0: true, ps1: true);
                break;
            case IrIndirectCall value:
                live = RootLanes(live, value.Target, ps0: true, ps1: true);
                foreach (var argument in value.Arguments)
                    live = RootLanes(live, argument, ps0: true, ps1: true);
                break;
            case IrSetCrField value:
                live = RootLanes(live, value.Left, ps0: true, ps1: true);
                live = RootLanes(live, value.Right, ps0: true, ps1: true);
                break;
            case IrPhi value:
                foreach (var source in value.Sources.Values)
                    live = RootLanes(live, source, ps0: true, ps1: true);
                break;
            case IrReturn { Value: { } value }: live = RootLanes(live, value, ps0: true, ps1: true); break;
        }
        return live;
    }

    /// <summary>
    /// Architectural FPR index of an SSA register name ("f6", "f6_12"), or
    /// null for GPRs, temporaries ("r3_psq_tmp_0", "f5_psq_ea") and anything
    /// else. Mirrors the base-name rule of the contract analyzer: only a
    /// purely numeric suffix is an SSA version.
    /// </summary>
    private static int? FprIndex(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return null;
        var baseName = RegisterNameUtils.StripNumericSuffix(name);
        if (baseName.Length >= 2 && baseName[0] == 'f' &&
            int.TryParse(baseName.AsSpan(1), out var fpr) && fpr is >= 0 and < 32)
            return fpr;
        return null;
    }

    /// <summary>
    /// Successor computation identical to
    /// <see cref="GuestStateLivenessAnalyzer"/>: branches have two targets,
    /// jumps one, and ordinary sequential flow falls through to the next
    /// block because canonical IR leaves it implicit.
    /// </summary>
    private static int Successors(
        IrBasicBlock block,
        string? fallthroughLabel,
        out string first,
        out string second)
    {
        var instructions = block.Instructions;
        var terminator = instructions.Count == 0 ? null : instructions[instructions.Count - 1];
        if (terminator is IrBranch branch)
        {
            first = branch.TrueLabel;
            second = branch.FalseLabel;
            return 2;
        }

        if (terminator is IrJump jump)
        {
            first = jump.TargetLabel;
            second = string.Empty;
            return 1;
        }

        if (terminator is not (IrReturn or IrUndefined or IrIndirectJump or IrJumpTable) &&
            fallthroughLabel is not null)
        {
            first = fallthroughLabel;
            second = string.Empty;
            return 1;
        }

        first = string.Empty;
        second = string.Empty;
        return 0;
    }
}
