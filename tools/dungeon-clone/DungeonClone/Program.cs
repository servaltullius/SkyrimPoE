// 지도 팩 복제 도구(6단계 재작업 계획 R1). 사용법은 tools/dungeon-clone/README.md.
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Skyrim;

namespace SkyrimPoE.Tools.DungeonClone;

internal static class Program
{
    private const string Usage =
        "사용:\n" +
        "  DungeonClone anchors    --data <Data 폴더> --list <dungeons.json>\n" +
        "  DungeonClone build      --data <Data 폴더> --list <dungeons.json> --ledger <formid-ledger.json> --out <지도 팩 .esp> [--update-ledger] [--create-ledger]\n" +
        "  DungeonClone verify     --data <Data 폴더> --list <dungeons.json> --ledger <formid-ledger.json> --esp <지도 팩 .esp>\n" +
        "  DungeonClone ledger-diff --old <이전 장부> --new <새 장부>\n";

    private static readonly Dictionary<string, string[]> Allowed = new()
    {
        ["anchors"] = new[] { "--data", "--list" },
        ["build"] = new[] { "--data", "--list", "--ledger", "--out", "--update-ledger", "--create-ledger" },
        ["verify"] = new[] { "--data", "--list", "--ledger", "--esp" },
        ["ledger-diff"] = new[] { "--old", "--new" },
    };

    private static readonly HashSet<string> Flags = new() { "--update-ledger", "--create-ledger" };

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 0) throw new ArgumentException("명령이 없습니다.");
            var cmd = args[0];
            if (!Allowed.TryGetValue(cmd, out var allowed)) throw new ArgumentException($"알 수 없는 명령: {cmd}");
            var opt = ParseOptions(args.Skip(1).ToArray(), allowed);

            if (cmd == "ledger-diff") return LedgerDiff(Required(opt, "--old"), Required(opt, "--new"));

            var data = Required(opt, "--data");
            var pack = PackSpec.Load(Required(opt, "--list"));
            var esm = Path.Combine(data, Policy.SourceMaster.FileName);
            if (!File.Exists(esm))
            {
                Console.Error.WriteLine($"{Policy.SourceMaster}을 찾지 못했습니다: {esm}");
                return 3;
            }
            using var mod = SkyrimMod.CreateFromBinaryOverlay(esm, SkyrimRelease.SkyrimSE);
            var src = SourceIndex.Create(mod);
            return cmd switch
            {
                "anchors" => Anchors(pack, src),
                "build" => BuildCmd(pack, src, Required(opt, "--list"), Required(opt, "--ledger"), Required(opt, "--out"),
                    opt.ContainsKey("--update-ledger"), opt.ContainsKey("--create-ledger")),
                _ => Verify.Run(pack, src, Ledger.Load(Required(opt, "--ledger"), createIfMissing: false), Required(opt, "--esp")),
            };
        }
        catch (ArgumentException ex)
        {
            Console.Error.WriteLine($"인자 오류: {ex.Message}");
            Console.Error.WriteLine(Usage);
            return 2;
        }
        catch (FileNotFoundException ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 3;
        }
        catch (LedgerChangeRequiredException ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 8;
        }
        catch (Exception ex) when (ex is InvalidDataException or JsonException or FormatException or OverflowException)
        {
            Console.Error.WriteLine(ex.Message);
            return 7;
        }
        catch (IOException ex)
        {
            Console.Error.WriteLine($"파일 입출력 오류: {ex.Message}");
            return 9;
        }
    }

    private static int Anchors(PackSpec pack, SourceIndex src)
    {
        foreach (var d in pack.Dungeons)
        {
            var cell = src.GetCell(d.SourceCell);
            var marker = Builder.EntryMarker(cell);
            var entry = marker.Placement!.Position;
            var navs = cell.NavigationMeshes.ToList();
            Console.WriteLine($"== {d.Key} ({cell.FormKey.ID:X6} {cell.EditorID}), 입구 {marker.FormKey.ID:X6}. 거리는 내비메쉬로 옮긴 뒤 입구까지(3차원)");
            foreach (var c in Builder.AnchorCandidates(cell, src, entry))
            {
                var (pos, moved) = Builder.SnapToNavmesh(c.Npc.Placement!.Position, navs);
                var dist = Builder.Distance(pos, entry);
                var flags = string.Join(" ", new[]
                {
                    c.BossTagged ? "BOSS" : "", c.StartsDead ? "시체" : "",
                    c.Npc.MajorFlags.HasFlag(PlacedNpc.MajorFlag.InitiallyDisabled) ? "처음비활성" : "",
                    dist < pack.AnchorMinDistanceFromEntry ? "입구근처" : "",
                    moved > pack.AnchorMaxSnapDistance ? $"내비메쉬에서멂({moved:F0})" : moved > 0 ? $"옮김({moved:F0})" : "",
                }.Where(s => s.Length > 0));
                Console.WriteLine($"  {c.Npc.FormKey.ID:X6} {c.BaseEditorId,-45} {dist,7:F0} {flags}");
            }
        }
        return 0;
    }

    private static int BuildCmd(PackSpec pack, SourceIndex src, string listPath, string ledgerPath, string outPath, bool updateLedger, bool createLedger)
    {
        if (!string.Equals(Path.GetFileName(outPath), pack.Plugin, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException($"--out의 파일 이름은 목록의 plugin({pack.Plugin})과 같아야 합니다.");
        var manifestPath = Path.ChangeExtension(outPath, ".json");
        var inputs = new[] { Path.GetFullPath(listPath), Path.GetFullPath(ledgerPath) };
        if (inputs.Any(i => string.Equals(i, Path.GetFullPath(outPath), StringComparison.OrdinalIgnoreCase)
                            || string.Equals(i, Path.GetFullPath(manifestPath), StringComparison.OrdinalIgnoreCase)))
            throw new ArgumentException("출력 ESP·목록 파일이 입력(던전 목록·장부)과 같은 경로입니다.");
        if (createLedger && File.Exists(ledgerPath)) throw new ArgumentException("--create-ledger는 장부가 없을 때만 씁니다.");

        // 같은 장부로 두 빌드가 동시에 번호를 주지 않게 잠근다.
        using var lockFile = new FileStream(ledgerPath + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose);
        var ledger = Ledger.Load(ledgerPath, createIfMissing: createLedger);
        var result = Builder.Build(pack, src, ledger, updateLedger || createLedger);

        // 임시 파일에 다 쓴 뒤 장부를 먼저 저장하고 산출물을 바꿔 넣는다.
        // 장부 저장 뒤에 실패하면 장부에만 쓰이지 않은 번호가 남고(무해), 산출물은 이전 판 그대로다.
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPath))!);
        var tmpDir = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(outPath))!, $".dungeonclone-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tmpDir);
        try
        {
            var tmpEsp = Path.Combine(tmpDir, pack.Plugin);
            result.Mod.WriteToBinary(tmpEsp, Encodings.WriteParameters);
            result.Manifest.EspSha256 = Verify.Sha256(tmpEsp);
            var tmpManifest = Path.ChangeExtension(tmpEsp, ".json");
            File.WriteAllText(tmpManifest, JsonSerializer.Serialize(result.Manifest, Json.Options).Replace("\r\n", "\n") + "\n", new System.Text.UTF8Encoding(false));
            if (ledger.NewAssignments > 0 || createLedger) ledger.Save(ledgerPath);
            File.Move(tmpEsp, outPath, overwrite: true);
            File.Move(tmpManifest, manifestPath, overwrite: true);
        }
        finally
        {
            Directory.Delete(tmpDir, recursive: true);
        }

        Console.WriteLine(JsonSerializer.Serialize(new { result.Reports, LedgerNewAssignments = ledger.NewAssignments, Esp = outPath, Manifest = manifestPath, result.Manifest.EspSha256 }, Json.Options));
        return 0;
    }

    private static int LedgerDiff(string oldPath, string newPath)
    {
        var problems = Ledger.Diff(Ledger.Load(oldPath, createIfMissing: false), Ledger.Load(newPath, createIfMissing: false));
        foreach (var p in problems) Console.WriteLine(p);
        Console.WriteLine(problems.Count == 0 ? "LEDGER=ok" : "LEDGER=fail");
        return problems.Count == 0 ? 0 : 6;
    }

    private static Dictionary<string, string> ParseOptions(string[] args, string[] allowed)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var i = 0; i < args.Length; i++)
        {
            var a = args[i];
            if (!allowed.Contains(a)) throw new ArgumentException($"이 명령에 없는 인자: {a}");
            if (d.ContainsKey(a)) throw new ArgumentException($"인자가 두 번 있습니다: {a}");
            if (Flags.Contains(a)) { d[a] = "1"; continue; }
            if (i + 1 >= args.Length || args[i + 1].StartsWith("--", StringComparison.Ordinal)) throw new ArgumentException($"{a} 뒤에 값이 없습니다.");
            d[a] = args[++i];
        }
        return d;
    }

    private static string Required(Dictionary<string, string> opt, string name)
        => opt.TryGetValue(name, out var v) ? v : throw new ArgumentException($"{name}이 필요합니다.");
}
