using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Translator.Core.Ir;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public class GuestIntrinsicSubstitutionTests
{
    private static readonly IReadOnlyDictionary<uint, string> SqrtTable = new Dictionary<uint, string>
    {
        [0x80261E08u] = GuestIntrinsicSubstitution.Sqrt
    };

    private static IrFunction CallerOf(string target, string destination = "") =>
        new("caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(destination, target, Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

    [Fact]
    public void ADirectCallToATableEntryBecomesThePureHelper()
    {
        var result = GuestIntrinsicSubstitution.Apply(CallerOf("0x80261E08"), SqrtTable, out var n);

        Assert.Equal(1, n);
        var call = Assert.IsType<IrCall>(result.Blocks[0].Instructions[0]);
        Assert.Equal("f1", call.Destination);
        Assert.Equal("PPC_Fsqrt", call.Target);
        Assert.Equal("f1", Assert.Single(call.Arguments).RegisterName);
    }

    [Theory]
    [InlineData("0x80261E0C", "")]      // a different address
    [InlineData("PPC_Fres", "f1")]      // a runtime helper, not a guest call
    [InlineData("0x80261E08", "r3")]    // a call that captures a destination is not a plain bl
    public void OtherCallsAreLeftAlone(string target, string destination)
    {
        var function = CallerOf(target, destination);
        var result = GuestIntrinsicSubstitution.Apply(function, SqrtTable, out var n);

        Assert.Equal(0, n);
        Assert.Same(function, result);
    }

    [Fact]
    public void AnEmptyTableChangesNothing()
    {
        var function = CallerOf("0x80261E08");
        Assert.Same(function, GuestIntrinsicSubstitution.Apply(function, new Dictionary<uint, string>(), out var n));
        Assert.Equal(0, n);
    }

    [Fact]
    public void TheTableRejectsUnknownKinds()
    {
        var path = Path.GetTempFileName();
        try
        {
            File.WriteAllText(path, "80261E08 sqrt  # ok\n80000000 frobnicate\n");
            Assert.Throws<FormatException>(() => GuestIntrinsicSubstitution.LoadTable(path));
            File.WriteAllText(path, "# comment only\n0x80261E08 SQRT\n");
            var table = GuestIntrinsicSubstitution.LoadTable(path);
            Assert.Equal("sqrt", table[0x80261E08u]);
        }
        finally
        {
            File.Delete(path);
        }
    }
}
