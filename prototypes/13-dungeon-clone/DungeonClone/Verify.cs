// 복제 결과 ESP를 다시 읽어 구조를 확인한다(게임 실행 없이 확인할 수 있는 부분만).
// 확인하는 것: 마스터 목록, ESL 플래그와 FormID 범위, ESP 안 레코드 구성(셀과 그 자식만), 모든 연결(FormLink)이 가리키는 곳,
// 참조마다 원본과의 연결 대상 비교(필드별 비교는 아님), 셀 자체 필드의 연결, 문 순간이동,
// 내비메쉬의 부모 셀·꼭짓점·격자·문 삼각형·가장자리 연결, 입구 마커의 영구 플래그.
// 확인하지 않는 것: 위치·회전, 스크립트의 정수·문자열 속성, 같은 참조 안에서 연결 필드끼리 바뀐 것.
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

namespace SkyrimPoE.Prototypes.DungeonClone;

internal static class Verify
{
    private static readonly FormKey CocMarkerHeading = FormKey.Factory("000032:Skyrim.esm");
    private const int PersistentFlag = 0x400;

    public static int Run(string dataFolder, string espPath, string sourceCellEditorId)
    {
        var esmPath = Path.Combine(dataFolder, "Skyrim.esm");
        if (!File.Exists(esmPath) || !File.Exists(espPath))
        {
            Console.Error.WriteLine($"파일이 없습니다: {(File.Exists(esmPath) ? espPath : esmPath)}");
            return 3;
        }

        using var source = SkyrimMod.CreateFromBinaryOverlay(esmPath, SkyrimRelease.SkyrimSE);
        using var output = SkyrimMod.CreateFromBinaryOverlay(espPath, SkyrimRelease.SkyrimSE);

        var srcCell = FindCell(source, c => string.Equals(c.EditorID, sourceCellEditorId, StringComparison.OrdinalIgnoreCase));
        var cells = AllCells(output).ToList();
        if (srcCell is null || cells.Count != 1)
        {
            Console.Error.WriteLine($"원본 셀을 찾지 못했거나 출력 셀 수가 1이 아닙니다(출력 셀 {cells.Count}개).");
            return 4;
        }
        var cell = cells[0];

        var r = new Result
        {
            Masters = output.ModHeader.MasterReferences.Select(m => m.Master.FileName.String).ToList(),
            SmallFlag = output.ModHeader.Flags.HasFlag(SkyrimModHeader.HeaderFlag.Small),
            OutputCell = cell.FormKey.ToString(),
        };

        // 원본 셀과 그 자식의 키(이것을 가리키는 연결이 남아 있으면 복제가 원본 세계에 묶인 것이다)
        var srcKeys = new HashSet<FormKey> { srcCell.FormKey };
        foreach (var c in srcCell.Persistent) srcKeys.Add(c.FormKey);
        foreach (var c in srcCell.Temporary) srcKeys.Add(c.FormKey);
        foreach (var c in srcCell.NavigationMeshes) srcKeys.Add(c.FormKey);

        var records = new List<IMajorRecordGetter> { cell };
        records.AddRange(cell.Persistent);
        records.AddRange(cell.Temporary);
        records.AddRange(cell.NavigationMeshes);
        var ownKeys = records.Select(x => x.FormKey).ToHashSet();

        r.RecordCount = records.Count;
        r.MinId = $"0x{records.Min(x => x.FormKey.ID):X}";
        r.MaxId = $"0x{records.Max(x => x.FormKey.ID):X}";
        r.ForeignRecordKeys = records.Count(x => x.FormKey.ModKey != output.ModKey);
        r.IdsOutsideEslRange = records.Count(x => x.FormKey.ID < 0x800 || x.FormKey.ID > 0xFFF);
        r.NpcsInCopy = records.Count(x => x is IPlacedNpcGetter);
        // ESP 전체가 셀과 그 자식만으로 이루어졌는지(다른 최상위 레코드·오버라이드가 섞이지 않았는지)
        r.ExtraRecordsOutsideCell = output.EnumerateMajorRecords().Count() - records.Count;
        r.DuplicateKeys = records.GroupBy(x => x.FormKey).Where(g => g.Count() > 1).Select(g => g.Key.ToString()).ToList();
        // 복제 도구는 셀에 첫 번호(0x800)를 준다
        r.CellAtFirstId = cell.FormKey.ID == 0x800;

        // 복제본 연결 분류. 바깥(Skyrim.esm) 대상은 선언형이 아니라 실제로 찾은 레코드의 형식으로 나눈다.
        // 스크립트 속성처럼 선언형이 일반 레코드인 연결도 대상이 참조·퀘스트·위치면 잡히게 하려는 것이다.
        // 셀의 연결 목록은 자식 참조의 연결까지 담으므로, 셀 자체 필드의 연결은 (셀 목록 - 자식 목록 합)으로 구한다.
        var cache = source.ToImmutableLinkCache();
        var childLinks = records.Skip(1).SelectMany(x => x.EnumerateFormLinks()).Where(l => !l.IsNull).Select(l => l.FormKey).ToList();
        var cellOwn = cell.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => l.FormKey).ToList();
        foreach (var k in childLinks)
        {
            cellOwn.Remove(k);
        }
        var linkSets = records.Skip(1)
            .Select(x => (Rec: x, Links: x.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => l.FormKey).Where(k => k != x.FormKey).ToList()))
            .Append((Rec: (IMajorRecordGetter)cell, Links: cellOwn));
        foreach (var (rec, links) in linkSets)
        {
            foreach (var k in links)
            {
                if (k.ModKey == output.ModKey)
                {
                    if (ownKeys.Contains(k)) r.LinksInsideCopy++;
                    else r.DanglingOwnLinks.Add($"{Describe(rec)} -> {k}");
                    continue;
                }
                if (srcKeys.Contains(k))
                {
                    r.LinksToOriginalCell.Add($"{Describe(rec)} -> {k}");
                    continue;
                }
                if (!cache.TryResolve(k, out IMajorRecordGetter? target))
                {
                    r.UnresolvedOutsideLinks.Add($"{Describe(rec)} -> {k}");
                    continue;
                }
                var typeName = target.GetType().Name.Replace("BinaryOverlay", "");
                r.OutsideTargetsByType[typeName] = r.OutsideTargetsByType.GetValueOrDefault(typeName) + 1;
                if (WorldState.Is(target))
                {
                    r.LinksToWorldState.Add($"{Describe(rec)} -> {k} {target.EditorID} ({typeName})");
                }
            }
        }

        // 스크립트가 붙은 참조(스크립트 이름과 속성 수). 지도 안에서 바닐라 상태를 건드릴 수 있는 것을 사람이 보게 한다.
        foreach (var o in records.Skip(1))
        {
            if (WorldState.GetVm(o) is not { Scripts.Count: > 0 } vm) continue;
            var baseKey = o is IPlacedObjectGetter po ? po.Base.FormKey.ToString() : "";
            r.ScriptedRefs.Add($"{Describe(o)} base={baseKey}: {string.Join(", ", vm.Scripts.Select(sc => $"{sc.Name}({sc.Properties.Count})"))}");
        }

        // 참조마다 1:1 대조. 복제 도구는 셀 → 영구 → 임시 → 내비메쉬 순서로 0x800부터 번호를 매긴다.
        // NPC는 번호를 받지 않고 빠지며, NPC를 가리키던 연결은 빈 연결이 된다.
        // 그 번호표를 복원해, 원본 자식의 연결을 번호표로 옮긴 것과 복제본 자식의 연결이 같은지 본다.
        var srcChildren = srcCell.Persistent.Cast<IMajorRecordGetter>().Concat(srcCell.Temporary).Concat(srcCell.NavigationMeshes).ToList();
        var map = new Dictionary<FormKey, FormKey> { [srcCell.FormKey] = cell.FormKey };
        var nextId = cell.FormKey.ID;
        var npcKeys = srcChildren.OfType<IPlacedNpcGetter>().Select(n => n.FormKey).ToHashSet();
        foreach (var c in srcChildren)
        {
            map[c.FormKey] = npcKeys.Contains(c.FormKey) ? FormKey.Null : new FormKey(output.ModKey, ++nextId);
        }
        var outByKey = records.GroupBy(x => x.FormKey).ToDictionary(g => g.Key, g => g.First());
        foreach (var c in srcChildren)
        {
            var mappedKey = map[c.FormKey];
            if (c is IPlacedNpcGetter)
            {
                r.LinksDroppedWithNpcs += c.EnumerateFormLinks().Count(l => !l.IsNull && srcKeys.Contains(l.FormKey) && l.FormKey != c.FormKey);
                continue;
            }
            if (!outByKey.TryGetValue(mappedKey, out var copy) || copy.GetType() != c.GetType())
            {
                r.PerRefMismatches.Add($"{c.FormKey} -> {mappedKey}: 복제본이 없거나 형식이 다름");
                continue;
            }
            r.PerRefCompared++;
            // 영구 여부가 원본과 같은지(입구 마커는 일부러 영구로 바꿈)
            var srcPersistent = (c.MajorRecordFlagsRaw & PersistentFlag) != 0;
            var copyPersistent = (copy.MajorRecordFlagsRaw & PersistentFlag) != 0;
            var isMarker = c is IPlacedObjectGetter mk && mk.Base.FormKey == CocMarkerHeading;
            if (srcPersistent != copyPersistent && !(isMarker && copyPersistent))
            {
                r.PerRefMismatches.Add($"{Describe(c)} -> {mappedKey}: 영구 플래그가 원본과 다름({srcPersistent}->{copyPersistent})");
            }
            static string Key(FormKey k) => k.ToString();
            var npcLinks = c.EnumerateFormLinks().Count(l => npcKeys.Contains(l.FormKey));
            r.ClearedLinksToNpcs += npcLinks;
            var expected = c.EnumerateFormLinks().Where(l => !l.IsNull && !npcKeys.Contains(l.FormKey))
                .Select(l => Key(map.TryGetValue(l.FormKey, out var m) ? m : l.FormKey)).OrderBy(x => x).ToList();
            var actual = copy.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => Key(l.FormKey)).OrderBy(x => x).ToList();
            // 의도된 차이: 바닐라 월드 상태를 가리키던 참조 스크립트를 뗀 것. 뗀 스크립트의 연결을 기대값에서 빼고,
            // 뗀 스크립트가 정말 월드 상태(퀘스트·위치·조우 구역·바깥 참조/셀)를 가리켰는지 확인한다.
            if (WorldState.GetVm(c) is { } svm)
            {
                var kept = WorldState.GetVm(copy)?.Scripts.Select(x => x.Name).ToHashSet() ?? new HashSet<string>();
                foreach (var gone in svm.Scripts.Where(x => !kept.Contains(x.Name)))
                {
                    var goneLinks = gone.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => l.FormKey).ToList();
                    // 복제 도구와 같은 기준: 찾지 못한 대상도 위험으로 본다
                    var hitsWorld = goneLinks.Any(k => !srcKeys.Contains(k)
                        && (!cache.TryResolve(k, out IMajorRecordGetter? t) || WorldState.IsScriptHazard(t)));
                    if (!hitsWorld)
                    {
                        r.PerRefMismatches.Add($"{Describe(c)} -> {mappedKey}: 스크립트 {gone.Name}가 빠졌는데 월드 상태를 가리키지 않음");
                    }
                    foreach (var k in goneLinks.Where(k => !npcKeys.Contains(k)))
                    {
                        expected.Remove(Key(map.TryGetValue(k, out var m) ? m : k));
                    }
                    r.IntendedScriptStrips.Add($"{Describe(c)} -> {mappedKey}: {gone.Name}");
                }
            }
            if (!expected.SequenceEqual(actual))
            {
                var missing = MultisetMinus(expected, actual);
                var extra = MultisetMinus(actual, expected);
                // 의도된 차이: 셀 밖으로 가는 문 순간이동을 끊은 것(빠진 것이 그 목적지 문 하나뿐)
                if (c is IPlacedObjectGetter so && so.TeleportDestination is { } st && !srcKeys.Contains(st.Door.FormKey)
                    && copy is IPlacedObjectGetter co && co.TeleportDestination is null
                    && extra.Count == 0 && missing.SequenceEqual(new[] { Key(st.Door.FormKey) }))
                {
                    r.IntendedTeleportCuts.Add($"{Describe(c)} -> {mappedKey}: 바깥 목적지 {st.Door.FormKey} 끊음");
                    continue;
                }
                r.PerRefMismatches.Add($"{Describe(c)} -> {mappedKey}: 빠짐[{string.Join(", ", missing)}] 더해짐[{string.Join(", ", extra)}] (개수 {expected.Count}->{actual.Count})");
            }
        }

        // 셀 자체 필드의 연결(조명 템플릿·음향·음악·물 등)이 원본과 같은지. 위치·조우 구역·소유자는 일부러 비웠다.
        var srcCellOwn = srcCell.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => l.FormKey).ToList();
        foreach (var k in srcChildren.SelectMany(x => x.EnumerateFormLinks()).Where(l => !l.IsNull).Select(l => l.FormKey))
        {
            srcCellOwn.Remove(k);
        }
        foreach (var cleared in new[] { srcCell.Location.FormKeyNullable, srcCell.EncounterZone.FormKeyNullable, srcCell.Owner.FormKeyNullable })
        {
            if (cleared is { } ck && !ck.IsNull) srcCellOwn.Remove(ck);
        }
        var expectedCellOwn = srcCellOwn.Select(k => (map.TryGetValue(k, out var m) ? m : k).ToString()).ToList();
        var actualCellOwn = cellOwn.Select(k => k.ToString()).ToList();
        var cellMissing = MultisetMinus(expectedCellOwn, actualCellOwn);
        var cellExtra = MultisetMinus(actualCellOwn, expectedCellOwn);
        if (cellMissing.Count > 0 || cellExtra.Count > 0)
        {
            r.PerRefMismatches.Add($"셀 자체 필드: 빠짐[{string.Join(", ", cellMissing)}] 더해짐[{string.Join(", ", cellExtra)}]");
        }
        r.CellOwnLinks = actualCellOwn.Count;
        if (!cell.Location.IsNull || !cell.EncounterZone.IsNull || !cell.Owner.IsNull)
        {
            r.PerRefMismatches.Add("셀의 위치·조우 구역·소유자가 비어 있지 않음");
        }

        // 문
        foreach (var o in cell.Persistent.Concat(cell.Temporary).OfType<IPlacedObjectGetter>())
        {
            if (o.TeleportDestination is { } t)
            {
                r.Teleports.Add($"{Describe(o)} -> {t.Door.FormKey}{(ownKeys.Contains(t.Door.FormKey) ? "" : " (복제본 밖!)")}");
            }
            if (o.Portals is { Count: > 0 }) r.PortalRefs++;
            if (o.LinkedRooms is { Count: > 0 }) r.RoomRefsWithLinkedRooms++;
            if (o.Base.FormKey == CocMarkerHeading)
            {
                r.EntryMarkers.Add($"{Describe(o)} persistent={(o.MajorRecordFlagsRaw & PersistentFlag) != 0} inPersistentGroup={cell.Persistent.Any(p => p.FormKey == o.FormKey)}");
            }
        }

        // 내비메쉬
        var srcNav = srcCell.NavigationMeshes.ToList();
        foreach (var nav in cell.NavigationMeshes)
        {
            var d = nav.Data;
            if (d is null)
            {
                r.Navmeshes.Add($"{nav.FormKey}: 데이터 없음");
                continue;
            }
            var parent = d.Parent is ICellNavmeshParentGetter cp ? cp.Parent.FormKey.ToString() : d.Parent.GetType().Name;
            // 번호표로 짝지은 원본 내비메쉬와 꼭짓점 좌표·격자 자료·삼각형 수·문 삼각형 수·가장자리 수가 같은지
            var srcMatch = srcNav.FirstOrDefault(s => map.TryGetValue(s.FormKey, out var mk2) && mk2 == nav.FormKey)?.Data;
            var same = srcMatch is not null
                && srcMatch.Vertices.SequenceEqual(d.Vertices)
                && srcMatch.NavmeshGrid.Span.SequenceEqual(d.NavmeshGrid.Span)
                && srcMatch.Triangles.Count == d.Triangles.Count
                && srcMatch.DoorTriangles.Count == d.DoorTriangles.Count
                && srcMatch.EdgeLinks.Count == d.EdgeLinks.Count;
            r.Navmeshes.Add(
                $"{nav.FormKey}: parent={parent} (셀 일치 {parent == cell.FormKey.ToString()}), " +
                $"verts={d.Vertices.Count} tris={d.Triangles.Count} (원본과 같음 {same}), " +
                $"doors={d.DoorTriangles.Count} [{string.Join(", ", d.DoorTriangles.Select(x => x.Door.FormKey + (ownKeys.Contains(x.Door.FormKey) ? "" : "!")))}], " +
                $"edgeLinks={d.EdgeLinks.Count} [{string.Join(", ", d.EdgeLinks.Select(x => x.Mesh.FormKey + (ownKeys.Contains(x.Mesh.FormKey) ? "" : "!")).Distinct())}], " +
                $"NNAM={nav.NNAM?.Length ?? -1} ONAM={nav.ONAM?.Length ?? -1} PNAM={nav.PNAM?.Length ?? -1}");
        }

        // 게임 안 경로 찾기 시험용 지점: 입구 마커에서 가장 먼 내비메쉬 꼭짓점(걸을 수 있는 바닥 위가 보장됨)
        var entry = cell.Persistent.Concat(cell.Temporary).OfType<IPlacedObjectGetter>()
            .FirstOrDefault(o => o.Base.FormKey == CocMarkerHeading)?.Placement?.Position;
        if (entry is { } e)
        {
            var far = cell.NavigationMeshes.Where(n => n.Data is not null).SelectMany(n => n.Data!.Vertices)
                .Select(v => (V: v, D: Math.Sqrt(Math.Pow(v.X - e.X, 2) + Math.Pow(v.Y - e.Y, 2) + Math.Pow(v.Z - e.Z, 2))))
                .OrderByDescending(x => x.D).FirstOrDefault();
            r.FarthestNavmeshVertexFromEntry = $"x={far.V.X:F0} y={far.V.Y:F0} z={far.V.Z:F0} (직선 거리 {far.D:F0})";
        }

        // NAVI(내비메쉬 정보표): 원본 내비메쉬가 등재돼 있는지, 복제본은 당연히 없다.
        var navi = source.NavigationMeshInfoMaps.FirstOrDefault();
        if (navi is not null)
        {
            var srcNavKeys = srcNav.Select(n => n.FormKey).ToHashSet();
            r.NaviEntriesForSourceNavmesh = navi.MapInfos.Count(m => srcNavKeys.Contains(m.NavigationMesh.FormKey));
            r.NaviTotalEntries = navi.MapInfos.Count;
        }

        Console.WriteLine(JsonSerializer.Serialize(r, new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));

        var ok = r.Masters.SequenceEqual(new[] { "Skyrim.esm" })
                 && r.ExtraRecordsOutsideCell == 0 && r.DuplicateKeys.Count == 0 && r.CellAtFirstId
                 && r.ForeignRecordKeys == 0 && r.NpcsInCopy == 0 && (!r.SmallFlag || r.IdsOutsideEslRange == 0)
                 && r.UnresolvedOutsideLinks.Count == 0 && r.LinksToWorldState.Count == 0
                 && r.DanglingOwnLinks.Count == 0 && r.LinksToOriginalCell.Count == 0
                 && r.PerRefMismatches.Count == 0 && r.PerRefCompared > 0
                 && r.Teleports.All(t => !t.Contains("!"))
                 && srcNav.Count > 0 && r.Navmeshes.Count == srcNav.Count
                 && r.Navmeshes.All(n => n.Contains("셀 일치 True") && n.Contains("원본과 같음 True") && !n.Contains("!"))
                 && r.EntryMarkers.Count > 0 && r.EntryMarkers.All(m => m.Contains("persistent=True inPersistentGroup=True"));
        Console.WriteLine(ok ? "VERIFY=ok" : "VERIFY=fail");
        return ok ? 0 : 6;
    }

    // 중복을 살린 차집합(a에서 b에 있는 만큼 하나씩 뺌)
    private static List<string> MultisetMinus(List<string> a, List<string> b)
    {
        var rest = new List<string>(a);
        foreach (var x in b)
        {
            rest.Remove(x);
        }
        return rest;
    }

    private static string Describe(IMajorRecordGetter rec)
        => $"{rec.FormKey}{(string.IsNullOrEmpty(rec.EditorID) ? "" : " " + rec.EditorID)}";

    private static IEnumerable<ICellGetter> AllCells(ISkyrimModGetter mod)
        => mod.Cells.Records.SelectMany(b => b.SubBlocks).SelectMany(s => s.Cells);

    private static ICellGetter? FindCell(ISkyrimModGetter mod, Func<ICellGetter, bool> pred)
        => AllCells(mod).FirstOrDefault(pred);

    private sealed class Result
    {
        public List<string> Masters { get; set; } = new();
        public bool SmallFlag { get; set; }
        public string OutputCell { get; set; } = "";
        public int RecordCount { get; set; }
        public string MinId { get; set; } = "";
        public string MaxId { get; set; } = "";
        public int ForeignRecordKeys { get; set; }
        public int IdsOutsideEslRange { get; set; }
        public int NpcsInCopy { get; set; }
        public int ExtraRecordsOutsideCell { get; set; }
        public List<string> DuplicateKeys { get; set; } = new();
        public bool CellAtFirstId { get; set; }
        public int CellOwnLinks { get; set; }
        public int LinksInsideCopy { get; set; }
        public int PerRefCompared { get; set; }
        public int LinksDroppedWithNpcs { get; set; }
        // 남은 참조가 빠진 NPC를 가리키던 연결(복제본에서는 빈 연결)
        public int ClearedLinksToNpcs { get; set; }
        public List<string> PerRefMismatches { get; set; } = new();
        public List<string> IntendedTeleportCuts { get; set; } = new();
        public List<string> IntendedScriptStrips { get; set; } = new();
        // 바깥 대상의 실제 형식별 개수
        public SortedDictionary<string, int> OutsideTargetsByType { get; set; } = new();
        public List<string> UnresolvedOutsideLinks { get; set; } = new();
        public List<string> ScriptedRefs { get; set; } = new();
        public List<string> DanglingOwnLinks { get; set; } = new();
        public List<string> LinksToOriginalCell { get; set; } = new();
        // 월드 상태(다른 셀의 참조, 셀, 월드스페이스, 내비메쉬, 위치, 조우 구역, 퀘스트)를 가리키는 연결
        public List<string> LinksToWorldState { get; set; } = new();
        public List<string> Teleports { get; set; } = new();
        public int PortalRefs { get; set; }
        public int RoomRefsWithLinkedRooms { get; set; }
        public List<string> EntryMarkers { get; set; } = new();
        public List<string> Navmeshes { get; set; } = new();
        public string FarthestNavmeshVertexFromEntry { get; set; } = "";
        public int NaviEntriesForSourceNavmesh { get; set; }
        public int NaviTotalEntries { get; set; }
    }
}
