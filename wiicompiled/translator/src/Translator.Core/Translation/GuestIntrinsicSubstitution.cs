using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.Translation;

/// <summary>
/// Call-site intrinsic substitution: a direct call to a known guest leaf becomes the equivalent
/// pure runtime helper in the caller, so the call, the ABI bridge and the context sync around it
/// disappear. Runs before leaf inlining so a tiny wrapper is replaced rather than spliced in.
/// Only substitutions that are bit-exact with the guest routine belong here.
/// </summary>
public static class GuestIntrinsicSubstitution
{
    /// <summary>Intrinsic kinds the table may name, mapped to the IR that replaces the call.</summary>
    public const string Sqrt = "sqrt";

    public static readonly IReadOnlySet<string> KnownKinds = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { Sqrt };

    /// <summary>
    /// Reads a table of "ADDR kind" lines (hex entry point, intrinsic kind; '#' starts a comment).
    /// Unknown kinds are rejected, so a typo cannot silently drop a call.
    /// </summary>
    public static IReadOnlyDictionary<uint, string> LoadTable(string path)
    {
        var table = new Dictionary<uint, string>();
        foreach (var raw in File.ReadAllLines(path))
        {
            var line = raw.Split('#')[0].Trim();
            if (line.Length == 0)
                continue;
            var parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != 2)
                throw new FormatException($"{path}: expected \"ADDR kind\", got \"{raw}\"");
            var address = parts[0].StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? parts[0][2..] : parts[0];
            if (!KnownKinds.Contains(parts[1]))
                throw new FormatException($"{path}: unknown intrinsic kind \"{parts[1]}\"");
            table[uint.Parse(address, NumberStyles.HexNumber, CultureInfo.InvariantCulture)] = parts[1].ToLowerInvariant();
        }
        return table;
    }

    /// <summary>Replaces direct calls to table entries; returns the function unchanged when nothing matches.</summary>
    public static IrFunction Apply(IrFunction function, IReadOnlyDictionary<uint, string> table, out int substituted)
    {
        substituted = 0;
        if (table.Count == 0)
            return function;
        var blocks = new List<IrBasicBlock>(function.Blocks.Count);
        var changed = false;
        foreach (var block in function.Blocks)
        {
            List<IrInstruction>? rewritten = null;
            for (var i = 0; i < block.Instructions.Count; i++)
            {
                var instruction = block.Instructions[i];
                if (instruction is IrCall call && TryReplace(call, table, out var replacement))
                {
                    rewritten ??= block.Instructions.Take(i).ToList();
                    rewritten.Add(replacement);
                    substituted++;
                    continue;
                }
                rewritten?.Add(instruction);
            }
            if (rewritten is null)
            {
                blocks.Add(block);
                continue;
            }
            changed = true;
            blocks.Add(block with { Instructions = rewritten });
        }
        return changed ? function with { Blocks = blocks } : function;
    }

    private static bool TryReplace(IrCall call, IReadOnlyDictionary<uint, string> table, out IrInstruction replacement)
    {
        replacement = call;
        // Only a plain direct guest call ("bl ADDR"): no captured destination, target is an address.
        if (!string.IsNullOrEmpty(call.Destination) ||
            !call.Target.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ||
            !uint.TryParse(call.Target.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var target) ||
            !table.TryGetValue(target, out var kind))
            return false;
        switch (kind)
        {
            case Sqrt:
                // fdlibm sqrt (correctly rounded, no errno write) == IEEE sqrt: f1 -> f1.
                replacement = new IrCall("f1", "PPC_Fsqrt", new[] { IrValue.Register("f1") });
                return true;
            default:
                return false;
        }
    }
}
