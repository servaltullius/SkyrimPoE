// 프로토타입 13: Skyrim.esm의 실내 던전 셀 하나를 새 플러그인으로 복제한다(결정 169, README 참고).
// 원본(Skyrim.esm)만 읽으므로 다른 모드의 셀 수정과 무관한 결과가 나온다.
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

namespace SkyrimPoE.Prototypes.DungeonClone;

internal static class Program
{
    // Skyrim.esm의 COCMarkerHeading 기본 객체(STAT 000032)
    private static readonly FormKey CocMarkerHeading = FormKey.Factory("000032:Skyrim.esm");
    private const int PersistentFlag = 0x400;

    private static int Main(string[] args)
    {
        try
        {
            var opt = Options.Parse(args);
            return opt.VerifyOnly ? Verify.Run(opt.DataFolder, opt.OutputPath, opt.CellEditorId) : Run(opt);
        }
        catch (ArgumentException ex)
        {
            Console.Error.WriteLine($"인자 오류: {ex.Message}");
            Console.Error.WriteLine(Options.Usage);
            return 2;
        }
    }

    private static int Run(Options opt)
    {
        var esmPath = Path.Combine(opt.DataFolder, "Skyrim.esm");
        if (!File.Exists(esmPath))
        {
            Console.Error.WriteLine($"Skyrim.esm을 찾지 못했습니다: {esmPath}");
            return 3;
        }

        using var source = SkyrimMod.CreateFromBinaryOverlay(esmPath, SkyrimRelease.SkyrimSE);

        ICellGetter? srcCell = null;
        foreach (var block in source.Cells.Records)
        {
            foreach (var sub in block.SubBlocks)
            {
                foreach (var candidate in sub.Cells)
                {
                    if (string.Equals(candidate.EditorID, opt.CellEditorId, StringComparison.OrdinalIgnoreCase))
                    {
                        srcCell = candidate;
                    }
                }
            }
        }
        if (srcCell is null)
        {
            Console.Error.WriteLine($"Skyrim.esm에서 실내 셀 '{opt.CellEditorId}'을 찾지 못했습니다.");
            return 4;
        }

        var outKey = ModKey.FromFileName(Path.GetFileName(opt.OutputPath));
        var outMod = new SkyrimMod(outKey, SkyrimRelease.SkyrimSE);
        // 결정 20(확정): 지도 팩은 정규 ESP 하나다. ESL 플래그는 --esl을 줄 때만 붙인다.
        if (opt.Esl)
        {
            outMod.ModHeader.Flags |= SkyrimModHeader.HeaderFlag.Small;
        }

        var report = new Report { SourceCell = srcCell.FormKey.ToString(), SourceEditorId = srcCell.EditorID ?? "" };

        // 1) 새 FormID 배정표: 셀과 그 안의 모든 자식(배치 참조·내비메쉬)
        var mapping = new Dictionary<FormKey, FormKey>();
        var newCellKey = outMod.GetNextFormKey();
        mapping[srcCell.FormKey] = newCellKey;

        var children = new List<IMajorRecordGetter>();
        children.AddRange(srcCell.Persistent);
        children.AddRange(srcCell.Temporary);
        children.AddRange(srcCell.NavigationMeshes);
        // 배치된 NPC는 복제하지 않는다. 번호를 주지 않고 빈 번호(Null)로 매핑해, 다른 참조가 NPC를 가리키던 연결은
        // 없는 레코드가 아니라 빈 연결이 되게 한다(활성화 부모·연결 참조·스크립트 속성 등).
        var npcKeys = children.OfType<IPlacedNpcGetter>().Select(n => n.FormKey).ToHashSet();
        foreach (var child in children)
        {
            mapping[child.FormKey] = npcKeys.Contains(child.FormKey) ? FormKey.Null : outMod.GetNextFormKey();
        }
        report.ClearedLinksToNpcs = children.Where(c => !npcKeys.Contains(c.FormKey))
            .Sum(c => c.EnumerateFormLinks().Count(l => npcKeys.Contains(l.FormKey)));

        // 2) 셀 통째 복제 뒤, 자식을 새 FormID 복제본으로 바꾼다.
        var cell = srcCell.Duplicate(newCellKey);

        var persistent = new List<IPlaced>();
        var temporary = new List<IPlaced>();
        foreach (var p in srcCell.Persistent)
        {
            if (p is IPlacedNpcGetter)
            {
                report.StrippedNpcs++;
                continue;
            }
            persistent.Add((IPlaced)p.Duplicate(mapping[p.FormKey]));
        }
        foreach (var t in srcCell.Temporary)
        {
            if (t is IPlacedNpcGetter)
            {
                report.StrippedNpcs++;
                continue;
            }
            temporary.Add((IPlaced)t.Duplicate(mapping[t.FormKey]));
        }
        var navmeshes = srcCell.NavigationMeshes
            .Select(n => (NavigationMesh)n.Duplicate(mapping[n.FormKey]))
            .ToList();

        cell.Persistent.Clear();
        cell.Persistent.AddRange(persistent);
        cell.Temporary.Clear();
        cell.Temporary.AddRange(temporary);
        cell.NavigationMeshes.Clear();
        cell.NavigationMeshes.AddRange(navmeshes);

        // 3) 셀 안의 연결(문 짝, 활성화 부모, 연결 참조, 방·포털 표지, 내비메쉬의 문 삼각형 등)을 새 번호로 다시 잇는다.
        cell.RemapLinks(mapping);

        // 4) 셀 밖으로 가는 문 순간이동을 끊는다(지도에서 바닐라 세계로 걸어 나가지 못하게).
        var newKeys = mapping.Values.Where(k => !k.IsNull).ToHashSet();
        foreach (var placed in cell.Persistent.Concat(cell.Temporary).OfType<PlacedObject>())
        {
            if (placed.TeleportDestination is { } tele && !newKeys.Contains(tele.Door.FormKey))
            {
                placed.TeleportDestination = null;
                report.RemovedExteriorTeleports++;
            }
        }

        // 4-2) 바닐라 월드 상태(WorldState.IsScriptHazard: 퀘스트·위치·조우 구역·전역 변수·팩션·키워드, 이 셀 밖의 참조·셀)를 속성으로 가리키는 참조 스크립트를 뗀다.
        //      예: Embershard의 defaultsetStageTrigSCRIPT 셋은 지나가면 바닐라 던전 퀘스트의 단계를 올린다.
        //      기본 객체(Activator 등)에 붙은 스크립트는 복제 대상이 아니라 여기서 다루지 않는다(README 위험 항목).
        var cache = source.ToImmutableLinkCache();
        bool IsWorldState(FormKey k)
        {
            if (k.IsNull || newKeys.Contains(k)) return false;
            if (!cache.TryResolve(k, out IMajorRecordGetter? target)) return true; // 찾지 못하면 안전하게 바깥으로 본다
            return WorldState.IsScriptHazard(target);
        }
        foreach (var placed in cell.Persistent.Concat(cell.Temporary))
        {
            // 스크립트를 붙일 수 있는 배치 형식: 일반 배치 객체와 함정·위험물류(NPC는 이미 빠짐)
            var vm = placed switch
            {
                PlacedObject o => o.VirtualMachineAdapter,
                IPlacedTrap t => t.VirtualMachineAdapter,
                _ => null,
            };
            if (vm is null) continue;
            var removed = vm.Scripts.Where(sc => sc.EnumerateFormLinks().Any(l => IsWorldState(l.FormKey))).ToList();
            foreach (var sc in removed)
            {
                vm.Scripts.Remove(sc);
                report.StrippedScripts.Add($"{placed.FormKey} {sc.Name}");
            }
            if (removed.Count > 0 && vm.Scripts.Count == 0)
            {
                switch (placed)
                {
                    case PlacedObject o: o.VirtualMachineAdapter = null; break;
                    case IPlacedTrap t: t.VirtualMachineAdapter = null; break;
                }
            }
        }

        // 5) 입구 마커를 영구 참조로 옮긴다(셀이 로드되기 전에도 DLL이 찾을 수 있게).
        var markers = cell.Temporary.OfType<PlacedObject>().Where(o => o.Base.FormKey == CocMarkerHeading).ToList();
        foreach (var marker in markers)
        {
            cell.Temporary.Remove(marker);
            marker.MajorRecordFlagsRaw |= PersistentFlag;
            marker.EditorID = $"{opt.Prefix}{srcCell.EditorID}_Entry{(markers.Count > 1 ? report.EntryMarkers.Count.ToString() : "")}";
            cell.Persistent.Add(marker);
            report.EntryMarkers.Add(marker.FormKey.ToString());
        }
        report.EntryMarkers.AddRange(cell.Persistent.OfType<PlacedObject>()
            .Where(o => o.Base.FormKey == CocMarkerHeading && !report.EntryMarkers.Contains(o.FormKey.ToString()))
            .Select(o =>
            {
                o.EditorID ??= $"{opt.Prefix}{srcCell.EditorID}_Entry";
                return o.FormKey.ToString();
            }));

        if (report.EntryMarkers.Count == 0)
        {
            Console.Error.WriteLine($"입구 마커(COCMarkerHeading)가 없는 셀입니다: {srcCell.EditorID}. 지도 입장 지점이 없어 복제하지 않습니다.");
            return 7;
        }

        // 6) 월드 상태와 얽히는 값을 비운다(위치 정리·소유·조우 구역).
        cell.Location.Clear();
        cell.EncounterZone.Clear();
        cell.Owner.Clear();
        cell.EditorID = $"{opt.Prefix}{srcCell.EditorID}";
        if (!string.IsNullOrEmpty(opt.DisplayName))
        {
            cell.Name = opt.DisplayName;
        }

        // 7) 다른 에디터 ID와 겹치지 않게 자식의 에디터 ID에도 접두사를 붙인다.
        foreach (var rec in cell.Persistent.Concat<IMajorRecord>(cell.Temporary).Concat(cell.NavigationMeshes))
        {
            if (!string.IsNullOrEmpty(rec.EditorID) && !rec.EditorID.StartsWith(opt.Prefix, StringComparison.Ordinal))
            {
                rec.EditorID = opt.Prefix + rec.EditorID;
            }
        }

        // 8) 실내 셀 그룹 배치(블록 = FormID 끝자리, 하위 블록 = 그다음 자리)
        var id = (int)newCellKey.ID;
        outMod.Cells.Records.Add(new CellBlock
        {
            BlockNumber = id % 10,
            GroupType = GroupTypeEnum.InteriorCellBlock,
            SubBlocks =
            {
                new CellSubBlock
                {
                    BlockNumber = (id / 10) % 10,
                    GroupType = GroupTypeEnum.InteriorCellSubBlock,
                    Cells = { cell },
                },
            },
        });

        report.OutputCell = newCellKey.ToString();
        report.OutputEditorId = cell.EditorID ?? "";
        report.Persistent = cell.Persistent.Count;
        report.Temporary = cell.Temporary.Count;
        report.NavigationMeshes = cell.NavigationMeshes.Count;
        report.RemappedRecords = newKeys.Count;

        // ESL 범위(0x800~0xFFF) 확인(--esl일 때만)
        var maxId = mapping.Values.Max(k => k.ID);
        if (opt.Esl && maxId > 0xFFF)
        {
            Console.Error.WriteLine($"새 FormID가 ESL 범위를 넘습니다(최대 0x{maxId:X}). 셀이 너무 큽니다.");
            return 5;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(opt.OutputPath))!);
        outMod.WriteToBinary(opt.OutputPath);

        Console.WriteLine(JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
        return 0;
    }

    private sealed class Report
    {
        public string SourceCell { get; set; } = "";
        public string SourceEditorId { get; set; } = "";
        public string OutputCell { get; set; } = "";
        public string OutputEditorId { get; set; } = "";
        public int Persistent { get; set; }
        public int Temporary { get; set; }
        public int NavigationMeshes { get; set; }
        public int StrippedNpcs { get; set; }
        public int ClearedLinksToNpcs { get; set; }
        public int RemovedExteriorTeleports { get; set; }
        public int RemappedRecords { get; set; }
        public List<string> EntryMarkers { get; set; } = new();
        public List<string> StrippedScripts { get; set; } = new();
    }

    private sealed record Options(string DataFolder, string CellEditorId, string OutputPath, string Prefix, string? DisplayName, bool VerifyOnly, bool Esl)
    {
        public const string Usage =
            "사용: DungeonClone --data <게임 Data 폴더> --cell <실내 셀 EditorID> --out <출력 .esp 경로> [--prefix SPOE_MapProbe_] [--name <표시 이름>] [--esl] [--verify]\n" +
            "  --esl: ESL 플래그를 붙인다(기본은 정규 ESP, 결정 20).\n" +
            "  --verify: 복제하지 않고, --out의 ESP를 --cell 원본과 대조해 검사만 한다.";

        public static Options Parse(string[] args)
        {
            string? data = null, cell = null, output = null, name = null;
            var prefix = "SPOE_MapProbe_";
            var verify = false;
            var esl = false;
            for (var i = 0; i < args.Length; i++)
            {
                string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} 뒤에 값이 없습니다.");
                switch (args[i])
                {
                    case "--data": data = Next(); break;
                    case "--cell": cell = Next(); break;
                    case "--out": output = Next(); break;
                    case "--prefix": prefix = Next(); break;
                    case "--name": name = Next(); break;
                    case "--verify": verify = true; break;
                    case "--esl": esl = true; break;
                    default: throw new ArgumentException($"알 수 없는 인자: {args[i]}");
                }
            }
            if (data is null || cell is null || output is null)
            {
                throw new ArgumentException("--data, --cell, --out은 꼭 줘야 합니다.");
            }
            if (!output.EndsWith(".esp", StringComparison.OrdinalIgnoreCase))
            {
                throw new ArgumentException("--out은 .esp 경로여야 합니다.");
            }
            return new Options(data, cell, output, prefix, name, verify, esl);
        }
    }
}
