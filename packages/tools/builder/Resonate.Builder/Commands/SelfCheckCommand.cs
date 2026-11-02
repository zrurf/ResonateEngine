using System;
using System.Collections.Generic;
using System.Text;
using Resonate.Builder.Assets;
using Resonate.Builder.Hashing;

namespace Resonate.Builder.Commands;

/* The builder's own checks: the pieces a C++ test cannot reach. They run in
   CI and on demand (`xmake builder-check`), and a failure is a defect in the
   builder, not in a schema or the content. */
internal static class SelfCheckCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate selfcheck: unknown argument '{args[0]}'");
            return 2;
        }
        int failures = 0;
        void Check(bool condition, string what)
        {
            if (!condition)
            {
                context.Error.WriteLine($"selfcheck: failed: {what}");
                ++failures;
            }
        }

        Check(Hex(Blake3.Hash(ReadOnlySpan<byte>.Empty)) ==
              "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
              "blake3 empty");
        Check(Hex(Blake3.Hash([0, 1, 2])) ==
              "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f",
              "blake3 [0,1,2]");

        byte[] key = Encoding.ASCII.GetBytes("whats the Elvish word for friend");
        Check(Hex(Blake3.HashKeyed(ReadOnlySpan<byte>.Empty, key)) ==
              "92b2b75604ed3c761f9d6f62392c8a9227ad0ea3f09573e783f1498a4ed60d26",
              "blake3 keyed empty");
        Check(Hex(Blake3.HashKeyed([0, 1, 2], key)) ==
              "39e67b76b5a007d4921969779fe666da67b5213b096084ab674742f0d5ec62b9",
              "blake3 keyed [0,1,2]");

        /* 1024 bytes of the repeating 0..250 sequence, the official vector that
           exercises a full chunk. */
        Check(Hex(Blake3.Hash(Sequence(1024))) ==
              "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7",
              "blake3 1024-byte chunk");
        Check(Hex(Blake3.Hash(Sequence(2048))) ==
              "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a",
              "blake3 two-chunk tree");

        /* Identity: the engine's GUID text and the compact form both parse, and
           the sidecar's three keys are the ones the pipeline writes. */
        const string kGuid = "15cc75a0-14ca-4c8e-b752-bf5ac010bc9a";
        Check(AssetMeta.ParseGuid(kGuid, out ulong high, out ulong low), "guid parse");
        Check(AssetMeta.ParseGuid("15cc75a014ca4c8eb752bf5ac010bc9a", out ulong compactHigh,
                                  out ulong compactLow), "compact guid parse");
        Check(high == compactHigh && low == compactLow, "compact guid agrees");
        Check(AssetMeta.FormatGuid(high, low) == kGuid, "guid formats canonically");
        Check(!AssetMeta.ParseGuid("15cc75a0-14ca-4c8e-b752", out _, out _), "short guid refused");

        AssetMeta meta = new();
        string sidecar = "guid = \"" + kGuid + "\"\n"
                         + "importer = \"texture\"\n"
                         + "source_hash = \"" + new string('a', 64) + "\"\n";
        Check(AssetMeta.Parse(sidecar, meta, out string metaError),
              $"sidecar parses: {metaError}");
        Check(meta.ValidGuid && meta.Importer == "texture", "sidecar carries identity");
        Check(!AssetMeta.Parse("guid = \"00000000-0000-0000-0000-000000000000\"", new AssetMeta(),
                               out _), "null guid refused");

        /* The engine's canonical [settings] table: values carry, escapes
           decode, an unknown table still refuses. */
        AssetMeta with_settings = new();
        string settings_sidecar = "guid = \"" + kGuid + "\"\n"
                                  + "[settings]\n"
                                  + "quality = \"a \\\"quoted\\\" value\"\n"
                                  + "notes = \"line one\\nline two\"\n";
        Check(AssetMeta.Parse(settings_sidecar, with_settings, out string settingsError),
              $"settings sidecar parses: {settingsError}");
        Check(with_settings.Settings.Count == 2 &&
              with_settings.Settings["quality"] == "a \"quoted\" value" &&
              with_settings.Settings["notes"] == "line one\nline two",
              "settings carry and decode");
        Check(!AssetMeta.Parse("guid = \"" + kGuid + "\"\n[future]\nx = \"1\"\n",
                               new AssetMeta(), out _), "unknown table refused");

        if (failures != 0)
        {
            context.Error.WriteLine($"selfcheck: {failures} check(s) failed");
            return 1;
        }
        context.Output.WriteLine("selfcheck: ok");
        return 0;
    }

    private static byte[] Sequence(int length)
    {
        byte[] data = new byte[length];
        for (int index = 0; index < length; ++index)
        {
            data[index] = (byte)(index % 251);
        }
        return data;
    }

    private static string Hex(byte[] bytes)
    {
        StringBuilder text = new(bytes.Length * 2);
        foreach (byte value in bytes)
        {
            text.Append(value.ToString("x2"));
        }
        return text.ToString();
    }
}
