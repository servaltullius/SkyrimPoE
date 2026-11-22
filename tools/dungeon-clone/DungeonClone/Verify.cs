// 디스크의 지도 팩 ESP와 목록 파일을 검사한다(게임 실행 없이 확인할 수 있는 부분).
// 1) 재현: 같은 목록·장부·Skyrim.esm으로 메모리에서 다시 만든 결과와 레코드마다 깊은 비교. 디스크 파일이 만든 대로인지 본다.
// 2) 원본 대조: 장부로 출력 레코드를 원본 레코드에 짝지어, 만드는 코드와 따로 원본 보존을 본다
//    (위치·회전·기본 객체·연결 필드·내비메쉬 삼각형과 이웃·문 삼각형, 뺀 참조의 사유, 뗀 스크립트의 근거, 앵커, 문 목록).
// 3) 불변식: 디스크만 보고 확인한다(마스터, 레코드 구성, 바깥 연결, 블록 배치, 장부 범위, EditorID).
// 확인하지 않는 것: 게임 안 동작(경로 찾기, 룸바운드 가림, 스크립트 실행, 자동 출구 너머). 실험 단계 E와 게임 시험의 몫이다.
using System.Security.Cryptography;
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Noggog;

namespace SkyrimPoE.Tools.DungeonClone;

internal static class Verify
{
    public const float ExpectedHeaderVersion = 1.71f;

    public static int Run(PackSpec pack, SourceIndex src, Ledger ledger, string espPath)
    {
        var fails = new List<string>();
        var notes = new List<string>();
        var manifestPath = Path.ChangeExtension(espPath, ".json");
        if (!File.Exists(espPath) || !File.Exists(manifestPath))
        {
            Console.Error.WriteLine($"파일이 없습니다: {(File.Exists(espPath) ? manifestPath : espPath)}");
            return 3;
        }
        if (!string.Equals(Path.GetFileName(espPath), pack.Plugin, StringComparison.OrdinalIgnoreCase))
        {
            fails.Add($"ESP 파일 이름이 목록의 plugin({pack.Plugin})과 다릅니다.");
        }

        using var disk = SkyrimMod.CreateFromBinaryOverlay(espPath, SkyrimRelease.SkyrimSE, Encodings.ReadParameters);
        PackManifest? diskManifest = null;
        try
        {
            diskManifest = JsonSerializer.Deserialize<PackManifest>(File.ReadAllText(manifestPath), Json.Strict);
        }
        catch (JsonException ex)
        {
            fails.Add($"목록 파일을 읽지 못했습니다(필수 필드·모르는 필드·형식): {ex.Message}");
        }
        if (diskManifest is null && fails.Count == 0)
            fails.Add("목록 파일이 비어 있습니다(JSON null).");
        if (diskManifest is not null && diskManifest.ManifestFormat != PackManifest.CurrentFormat)
            fails.Add($"목록 파일 형식 {diskManifest.ManifestFormat}이 도구의 {PackManifest.CurrentFormat}과 다릅니다.");
        if (diskManifest is not null && diskManifest.EspSha256 != Sha256(espPath))
            fails.Add("목록 파일의 espSha256이 이 ESP의 해시와 다릅니다.");

        // 1) 재현 비교
        try
        {
            var mem = Builder.Build(pack, src, ledger, allowLedgerChanges: false);
            CompareRecords(mem.Mod, disk, fails);
            if (diskManifest is not null)
            {
                mem.Manifest.EspSha256 = diskManifest.EspSha256;
                if (JsonSerializer.Serialize(mem.Manifest, Json.Options) != JsonSerializer.Serialize(diskManifest, Json.Options))
                    fails.Add("목록 파일이 다시 만든 목록과 다릅니다.");
            }
        }
        catch (Exception ex) when (ex is LedgerChangeRequiredException or InvalidDataException)
        {
            fails.Add($"재현 불가: {ex.Message}");
        }

        // 2) 원본 대조와 3) 불변식
        if (diskManifest is not null)
        {
            CheckInvariants(pack, src, ledger, disk, diskManifest, fails, notes);
            CheckAgainstSource(pack, src, ledger, disk, diskManifest, fails, notes);
        }

        var summary = new
        {
            Esp = Path.GetFileName(espPath),
            Records = disk.EnumerateMajorRecords().Count(),
            Dungeons = diskManifest?.Dungeons.Select(d => new { d.Key, d.Cell, d.EntryMarker, d.BossAnchor, PackAnchors = d.PackAnchors.Count, d.ReturnDoors, AutoExits = d.AutoExits.Select(x => $"{x.Id}({x.Leads})"), d.SealedDoors }),
            Notes = notes,
            Failures = fails,
        };
        Console.WriteLine(JsonSerializer.Serialize(summary, Json.Options));
        Console.WriteLine(fails.Count == 0 ? "VERIFY=ok" : "VERIFY=fail");
        return fails.Count == 0 ? 0 : 6;
    }

    public static string Sha256(string path)
    {
        using var s = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(s)).ToLowerInvariant();
    }

    // ---------- 1) 재현 비교 ----------

    private static void CompareRecords(ISkyrimModGetter mem, ISkyrimModGetter disk, List<string> fails)
    {
        // 마스터 목록은 쓸 때 채워지고 레코드 수·다음 번호는 쓸 때 셈하므로 비교에서 빼고 불변식에서 따로 본다.
        if (!mem.ModHeader.Equals(disk.ModHeader, new SkyrimModHeader.TranslationMask(defaultOn: true) { Stats = false, MasterReferences = false }))
        {
            fails.Add($"헤더가 다시 만든 것과 다릅니다: {mem.ModHeader.GetEqualsMask(disk.ModHeader, Loqui.EqualsMaskHelper.Include.OnlyFailures)}");
        }
        var memCells = AllCells(mem).ToDictionary(c => c.FormKey);
        var diskCells = AllCells(disk).ToList();
        if (diskCells.Count != memCells.Count) fails.Add($"셀 수가 다릅니다: 디스크 {diskCells.Count}, 다시 만든 것 {memCells.Count}");
        var cellMask = new Cell.TranslationMask(defaultOn: true) { Persistent = false, Temporary = false, NavigationMeshes = false };
        foreach (var dc in diskCells)
        {
            if (!memCells.TryGetValue(dc.FormKey, out var mc))
            {
                fails.Add($"디스크에만 있는 셀: {dc.FormKey}");
                continue;
            }
            if (!mc.Equals(dc, cellMask))
            {
                // 진단: 자식을 비운 사본끼리 비교해 셀 자체 필드의 차이만 보인다.
                var a = mc.DeepCopy();
                var b = dc.DeepCopy();
                foreach (var x in new[] { a, b })
                {
                    x.Persistent.Clear();
                    x.Temporary.Clear();
                    x.NavigationMeshes.Clear();
                }
                fails.Add($"셀 {dc.FormKey} 자체 필드가 다릅니다: {a.GetEqualsMask(b, Loqui.EqualsMaskHelper.Include.OnlyFailures)}");
            }
            CompareChildren("영구", mc.Persistent, dc.Persistent, fails);
            CompareChildren("임시", mc.Temporary, dc.Temporary, fails);
            CompareChildren("내비메쉬", mc.NavigationMeshes, dc.NavigationMeshes, fails);
        }
    }

    private static void CompareChildren<T>(string group, IReadOnlyList<T> mem, IReadOnlyList<T> disk, List<string> fails)
        where T : IMajorRecordGetter
    {
        if (mem.Count != disk.Count) fails.Add($"{group} 자식 수가 다릅니다: 디스크 {disk.Count}, 다시 만든 것 {mem.Count}");
        if (!mem.Select(x => x.FormKey).SequenceEqual(disk.Select(x => x.FormKey))) fails.Add($"{group} 자식 순서가 다시 만든 것과 다릅니다.");
        var memByKey = mem.ToDictionary(x => x.FormKey);
        foreach (var d in disk)
        {
            if (!memByKey.TryGetValue(d.FormKey, out var m))
            {
                fails.Add($"{group}: 디스크에만 있는 레코드 {d.FormKey}");
                continue;
            }
            if (m.Equals(d)) continue;
            var detail = (m, d) switch
            {
                (IPlacedObjectGetter mo, IPlacedObjectGetter dobj) => mo.GetEqualsMask(dobj, Loqui.EqualsMaskHelper.Include.OnlyFailures).ToString(),
                (INavigationMeshGetter mn, INavigationMeshGetter dn) => mn.GetEqualsMask(dn, Loqui.EqualsMaskHelper.Include.OnlyFailures).ToString(),
                _ => m.GetType().Name,
            };
            fails.Add($"{group}: {d.FormKey} 가 다시 만든 것과 다릅니다: {detail}");
        }
    }

    // ---------- 2) 원본 대조 ----------

    internal static void CheckAgainstSource(PackSpec pack, SourceIndex src, Ledger ledger, ISkyrimModGetter disk,
        PackManifest manifest, List<string> fails, List<string> notes)
    {
        var cells = AllCells(disk).ToDictionary(c => c.FormKey);
        foreach (var dm in manifest.Dungeons)
        {
            var spec = pack.Dungeons.FirstOrDefault(d => d.Key == dm.Key);
            var slot = ledger.Slots.FirstOrDefault(s => s.Dungeon == dm.Key);
            var outCellKey = new FormKey(disk.ModKey, Ledger.ParseId(dm.Cell));
            if (spec is null || slot is null || !cells.TryGetValue(outCellKey, out var cell))
            {
                fails.Add($"{dm.Key}: 목록·장부·셀 가운데 빠진 것이 있어 원본 대조를 못 했습니다.");
                continue;
            }
            ICellGetter srcCell;
            try { srcCell = src.GetCell(spec.SourceCell); }
            catch (InvalidDataException ex) { fails.Add($"{dm.Key}: {ex.Message}"); continue; }
            if (!string.Equals(slot.Source, Ledger.SourceKey(srcCell.FormKey), StringComparison.OrdinalIgnoreCase))
                fails.Add($"{dm.Key}: 장부 구간의 원본 {slot.Source}가 목록의 원본 셀 {Ledger.SourceKey(srcCell.FormKey)}과 다릅니다.");

            // 장부로 원본 ↔ 출력 짝짓기
            var outOf = new Dictionary<(FormKey, string), FormKey>();
            var sourceOf = new Dictionary<FormKey, (FormKey Src, string Role)>();
            foreach (var (k, v) in slot.Ids)
            {
                var role = k[(k.LastIndexOf(':') + 1)..];
                var srcPart = k[..k.LastIndexOf(':')];
                var sep = srcPart.LastIndexOf(':');
                var srcKey = new FormKey(ModKey.FromFileName(srcPart[..sep]), Convert.ToUInt32(srcPart[(sep + 1)..], 16));
                var outKey = new FormKey(disk.ModKey, Ledger.ParseId(v));
                outOf[(srcKey, role)] = outKey;
                sourceOf[outKey] = (srcKey, role);
            }
            if (!outOf.TryGetValue((srcCell.FormKey, "cell"), out var mappedCell) || mappedCell != outCellKey)
                fails.Add($"{dm.Key}: 장부의 셀 번호가 목록의 셀과 다릅니다.");

            var outRecs = cell.Persistent.Concat<IMajorRecordGetter>(cell.Temporary).Concat(cell.NavigationMeshes).ToDictionary(r => r.FormKey);
            var srcChildren = srcCell.Persistent.Concat<IMajorRecordGetter>(srcCell.Temporary).Concat(srcCell.NavigationMeshes).ToList();
            var srcChildKeys = srcChildren.Select(c => c.FormKey).ToHashSet();
            var keptSrc = srcChildren.Where(c => outOf.TryGetValue((c.FormKey, "ref"), out var o) && outRecs.ContainsKey(o)).Select(c => c.FormKey).ToHashSet();
            var removedSrc = srcChildKeys.Where(k => !keptSrc.Contains(k)).ToHashSet();
            // 원본 연결을 출력 연결로 옮기는 규칙: 원본 셀의 남긴 자식 → 장부 번호, 뺀 자식 → 빈 연결, 셀 자신 → 출력 셀, 그 밖 → 그대로
            FormKey Map(FormKey k)
            {
                if (k.IsNull) return FormKey.Null;
                if (k == srcCell.FormKey) return outCellKey;
                if (srcChildKeys.Contains(k)) return keptSrc.Contains(k) ? outOf[(k, "ref")] : FormKey.Null;
                return k;
            }

            // (가) 뺀 원본 자식마다 사유가 있어야 한다. 형식 사유는 여기서 따로 판정하고, 스크립트 사유는 복제 계획과 대조한다.
            var plan = Builder.Plan(srcCell, src);
            foreach (var k in removedSrc)
            {
                var child = srcChildren.First(c => c.FormKey == k);
                if (child is INavigationMeshGetter) { fails.Add($"{dm.Key}: 원본 내비메쉬 {k}가 빠졌습니다."); continue; }
                if (outOf.ContainsKey((k, "ref"))) notes.Add($"{dm.Key}: 원본 {k.ID:X6}은 장부에 번호가 있지만 이번 판에서 뺐습니다(번호는 보존).");
                var typeReason = IndependentTypeReason(child, src);
                if (typeReason is not null) continue;
                if (plan.Removed.TryGetValue(k, out var r) && r is RemovalReason.HazardBaseScript or RemovalReason.QuestTrigger) continue;
                fails.Add($"{dm.Key}: 원본 {k.ID:X6}을 뺐는데 사유가 없습니다.");
            }
            foreach (var k in keptSrc)
            {
                var child = srcChildren.First(c => c.FormKey == k);
                if (IndependentTypeReason(child, src) is { } tr) fails.Add($"{dm.Key}: 원본 {k.ID:X6}은 {tr}라 뺐어야 하는데 남았습니다.");
            }

            // (나) 출력 레코드마다 원본과 대조
            var cutDoorsExpected = new Dictionary<FormKey, string>();
            foreach (var (outKey, outRec) in outRecs)
            {
                if (!sourceOf.TryGetValue(outKey, out var so))
                {
                    fails.Add($"{dm.Key}: 출력 {outKey}가 장부에 없습니다.");
                    continue;
                }
                if (so.Role == "anchor")
                {
                    CheckAnchor(pack, dm, srcCell, so.Src, outRec, src, fails);
                    continue;
                }
                if (so.Role != "ref" || !srcChildKeys.Contains(so.Src))
                {
                    fails.Add($"{dm.Key}: 출력 {outKey}의 장부 원본 {so.Src}({so.Role})가 원본 셀의 자식이 아닙니다.");
                    continue;
                }
                var srcRec = srcChildren.First(c => c.FormKey == so.Src);
                if (srcRec.GetType().Name.Replace("BinaryOverlay", "") != outRec.GetType().Name.Replace("BinaryOverlay", ""))
                {
                    fails.Add($"{dm.Key}: {outKey}의 형식이 원본과 다릅니다.");
                    continue;
                }
                switch (srcRec, outRec)
                {
                    case (IPlacedObjectGetter s, IPlacedObjectGetter o):
                        ComparePlacedObject(dm, s, o, Map, srcCell, src, plan, removedSrc, cutDoorsExpected, fails);
                        break;
                    case (INavigationMeshGetter s, INavigationMeshGetter o):
                        CompareNavmesh(dm, s, o, Map, outCellKey, fails);
                        break;
                    case (IPlacedGetter s, IPlacedGetter o):
                        if (!Equals(s.Placement, o.Placement) && !(s.Placement?.Equals(o.Placement) ?? false))
                            fails.Add($"{dm.Key}: {outKey}의 위치가 원본과 다릅니다.");
                        break;
                }
            }

            // (다) 문 목록: 원본에서 끊어야 할 문과 목록이 정확히 같아야 한다.
            var listed = dm.ReturnDoors.Select(id => (id, "return")).Concat(dm.SealedDoors.Select(id => (id, "sealed")))
                .Concat(dm.AutoExits.Select(x => (x.Id, x.Leads == "interior" ? "auto-interior" : x.Leads == "exterior" ? "auto-exterior" : "auto-?")))
                .ToList();
            foreach (var g in listed.GroupBy(x => x.Item1).Where(g => g.Count() > 1)) fails.Add($"{dm.Key}: 문 {g.Key}가 목록에 두 번 있습니다.");
            var listedMap = listed.GroupBy(x => x.Item1).ToDictionary(g => g.Key, g => g.First().Item2);
            foreach (var (k, kind) in cutDoorsExpected)
            {
                var id = Ledger.Format(k.ID);
                if (!listedMap.TryGetValue(id, out var got)) fails.Add($"{dm.Key}: 끊은 문 {id}({kind})가 목록에 없습니다.");
                else if (got != kind) fails.Add($"{dm.Key}: 문 {id}의 분류가 {got}인데 원본으로는 {kind}입니다.");
            }
            foreach (var id in listedMap.Keys.Where(id => !cutDoorsExpected.ContainsKey(new FormKey(disk.ModKey, Ledger.ParseId(id)))))
                fails.Add($"{dm.Key}: 목록의 문 {id}는 원본 기준으로 끊은 문이 아닙니다.");
        }
    }

    // 형식만으로 정해지는 제거 사유(복제 코드의 Policy와 따로 적음)
    private static string? IndependentTypeReason(IMajorRecordGetter child, SourceIndex src)
    {
        if (child is IPlacedNpcGetter) return "NPC";
        if (child is not IPlacedObjectGetter po || po.Base.IsNull || !src.Cache.TryResolve(po.Base.FormKey, out var b)) return null;
        return b switch
        {
            IKeyGetter => "열쇠",
            IWeaponGetter or IArmorGetter or IIngestibleGetter or IIngredientGetter or IMiscItemGetter
                or IBookGetter or IAmmunitionGetter or ISoulGemGetter or IScrollGetter or ILeveledItemGetter => "바닥 물건",
            IContainerGetter => "상자",
            IFloraGetter f when !f.Ingredient.IsNull => "채집 식물",
            ITreeGetter t when !t.Ingredient.IsNull => "채집 나무",
            ILightGetter l when l.Flags.HasFlag(Light.Flag.CanBeCarried) => "들고 가는 광원",
            IActivatorGetter or IFurnitureGetter when Policy.GetVm(b)?.Scripts.Any(s => s.Name.Equals("MineOreScript", StringComparison.OrdinalIgnoreCase)
                || s.Name.Equals("MineOreFurnitureScript", StringComparison.OrdinalIgnoreCase)) == true => "광맥",
            _ => null,
        };
    }

    private static void ComparePlacedObject(DungeonManifest dm, IPlacedObjectGetter s, IPlacedObjectGetter o, Func<FormKey, FormKey> map,
        ICellGetter srcCell, SourceIndex src, (Dictionary<FormKey, RemovalReason> Removed, Dictionary<FormKey, HashSet<string>> Strip, List<string> LostTargets) plan,
        HashSet<FormKey> removedSrc, Dictionary<FormKey, string> cutDoorsExpected, List<string> fails)
    {
        var id = $"{dm.Key}: {o.FormKey}(원본 {s.FormKey.ID:X6})";
        if (s.Base.FormKey != o.Base.FormKey) fails.Add($"{id} 기본 객체가 원본과 다릅니다.");
        if (!(s.Placement?.Equals(o.Placement) ?? o.Placement is null)) fails.Add($"{id} 위치·회전이 원본과 다릅니다.");
        if (s.Scale != o.Scale) fails.Add($"{id} 크기가 원본과 다릅니다.");
        var isEntry = s.Base.FormKey == Policy.CocMarkerHeading;
        if (((s.MajorRecordFlagsRaw ^ o.MajorRecordFlagsRaw) & ~Policy.PersistentFlag) != 0) fails.Add($"{id} 레코드 플래그가 원본과 다릅니다.");
        if (!isEntry && ((s.MajorRecordFlagsRaw ^ o.MajorRecordFlagsRaw) & Policy.PersistentFlag) != 0) fails.Add($"{id} 영구 플래그가 원본과 다릅니다.");

        // 연결 필드는 필드별로(필드끼리 바뀐 연결을 잡기 위해)
        var sp = s.EnableParent is { } se ? (map(se.Reference.FormKey), se.Flags) : ((FormKey, EnableParent.Flag)?)null;
        var op = o.EnableParent is { } oe ? (oe.Reference.FormKey, oe.Flags) : ((FormKey, EnableParent.Flag)?)null;
        if (!Equals(sp, op)) fails.Add($"{id} 활성화 부모가 원본을 옮긴 것과 다릅니다.");
        var sl = s.LinkedReferences?.Select(l => (map(l.KeywordOrReference.FormKey), map(l.Reference.FormKey))).ToList() ?? new();
        var ol = o.LinkedReferences?.Select(l => (l.KeywordOrReference.FormKey, l.Reference.FormKey)).ToList() ?? new();
        if (!sl.SequenceEqual(ol)) fails.Add($"{id} 연결 참조가 원본을 옮긴 것과 다릅니다.");
        var sa = s.ActivateParents?.Parents.Select(p => (map(p.Reference.FormKey), p.Delay)).ToList() ?? new();
        var oa = o.ActivateParents?.Parents.Select(p => (p.Reference.FormKey, p.Delay)).ToList() ?? new();
        if (!sa.SequenceEqual(oa)) fails.Add($"{id} 활성화 부모 목록이 원본을 옮긴 것과 다릅니다.");

        // 문 순간이동: 원본 목적지가 이 셀의 남긴 문이면 그대로 이어져야 하고, 아니면 끊고 목록에 있어야 한다.
        var isDoor = src.Cache.TryResolve(s.Base.FormKey, out var baseRec) && baseRec is IDoorGetter;
        var cut = false;
        if (s.TeleportDestination is { } st)
        {
            var mapped = map(st.Door.FormKey);
            if (!mapped.IsNull && mapped.ModKey == o.FormKey.ModKey)
            {
                if (o.TeleportDestination?.Door.FormKey != mapped) fails.Add($"{id} 셀 안 순간이동이 원본을 옮긴 것과 다릅니다.");
            }
            else
            {
                cut = true;
                if (o.TeleportDestination is not null) fails.Add($"{id} 셀 밖 순간이동이 남았습니다.");
                var auto = baseRec is IDoorGetter d && d.Flags.HasFlag(Door.Flag.Automatic);
                var interior = src.InteriorRefCell.ContainsKey(st.Door.FormKey);
                cutDoorsExpected[o.FormKey] = auto ? (interior ? "auto-interior" : "auto-exterior") : interior ? "sealed" : "return";
            }
        }
        else if (o.TeleportDestination is not null) fails.Add($"{id} 원본에 없던 순간이동이 생겼습니다.");

        // 잠금: 안쪽 문은 풀고, 그 밖은 원본 그대로
        if (isDoor && !cut) { if (o.Lock is not null) fails.Add($"{id} 안쪽 문이 잠겨 있습니다."); }
        else if (!(s.Lock?.Equals(o.Lock) ?? o.Lock is null)) fails.Add($"{id} 잠금이 원본과 다릅니다.");

        // 스크립트: 출력은 원본의 부분집합이고, 뗀 것마다 근거가 있어야 한다.
        var sNames = s.VirtualMachineAdapter?.Scripts.Select(x => x.Name).ToList() ?? new();
        var oNames = o.VirtualMachineAdapter?.Scripts.Select(x => x.Name).ToList() ?? new();
        foreach (var n in oNames.Where(n => !sNames.Contains(n, StringComparer.OrdinalIgnoreCase))) fails.Add($"{id} 원본에 없던 스크립트 {n}가 생겼습니다.");
        foreach (var gone in s.VirtualMachineAdapter?.Scripts.Where(x => !oNames.Contains(x.Name, StringComparer.OrdinalIgnoreCase)) ?? Enumerable.Empty<IScriptEntryGetter>())
        {
            var inside = (Func<FormKey, bool>)(k => k == srcCell.FormKey || srcCell.Persistent.Concat<IMajorRecordGetter>(srcCell.Temporary).Any(c => c.FormKey == k));
            var hazards = Policy.HazardsOf(gone, inside, src.Cache);
            var allowed = Policy.AllowedDeviceScripts.TryGetValue(gone.Name, out var a) ? a : HazardKind.None;
            var hitsRemoved = gone.EnumerateFormLinks().Any(l => removedSrc.Contains(l.FormKey));
            if ((hazards & ~allowed) == HazardKind.None && !(hitsRemoved && allowed == HazardKind.None))
                fails.Add($"{id} 스크립트 {gone.Name}를 뗐는데 근거(허용 밖 대상, 뺀 참조)가 없습니다.");
        }
        // 남은 스크립트의 값 속성은 원본과 같아야 한다(연결은 위 규칙으로 옮긴 값).
        foreach (var os in o.VirtualMachineAdapter?.Scripts ?? Enumerable.Empty<IScriptEntryGetter>())
        {
            var ss = s.VirtualMachineAdapter!.Scripts.First(x => x.Name.Equals(os.Name, StringComparison.OrdinalIgnoreCase));
            var sLinks = ss.EnumerateFormLinks().Select(l => map(l.FormKey)).ToList();
            var oLinks = os.EnumerateFormLinks().Select(l => l.FormKey).ToList();
            if (!sLinks.SequenceEqual(oLinks)) fails.Add($"{id} 스크립트 {os.Name}의 연결 속성이 원본을 옮긴 것과 다릅니다.");
            if (ss.Properties.Count != os.Properties.Count) fails.Add($"{id} 스크립트 {os.Name}의 속성 수가 원본과 다릅니다.");
        }
    }

    private static void CompareNavmesh(DungeonManifest dm, INavigationMeshGetter s, INavigationMeshGetter o, Func<FormKey, FormKey> map,
        FormKey outCellKey, List<string> fails)
    {
        var id = $"{dm.Key}: 내비메쉬 {o.FormKey}(원본 {s.FormKey.ID:X6})";
        if (s.Data is not { } sd || o.Data is not { } od) { fails.Add($"{id} 데이터가 없습니다."); return; }
        if (od.Parent is not ICellNavmeshParentGetter cp || cp.Parent.FormKey != outCellKey) fails.Add($"{id} 부모 셀이 출력 셀이 아닙니다.");
        if (!sd.Vertices.SequenceEqual(od.Vertices)) fails.Add($"{id} 꼭짓점이 원본과 다릅니다.");
        if (!sd.NavmeshGrid.Span.SequenceEqual(od.NavmeshGrid.Span)) fails.Add($"{id} 격자 자료가 원본과 다릅니다.");
        if (sd.Triangles.Count != od.Triangles.Count) fails.Add($"{id} 삼각형 수가 원본과 다릅니다.");
        else
        {
            for (var i = 0; i < sd.Triangles.Count; i++)
            {
                var (a, b) = (sd.Triangles[i], od.Triangles[i]);
                if (!a.Vertices.Equals(b.Vertices) || a.EdgeLink_0_1 != b.EdgeLink_0_1 || a.EdgeLink_1_2 != b.EdgeLink_1_2
                    || a.EdgeLink_2_0 != b.EdgeLink_2_0 || a.Flags != b.Flags || a.CoverFlags != b.CoverFlags)
                {
                    fails.Add($"{id} 삼각형 {i}이 원본과 다릅니다.");
                    break;
                }
            }
        }
        var sDoors = sd.DoorTriangles.Select(x => (x.TriangleBeforeDoor, x.Unknown, map(x.Door.FormKey))).ToList();
        var oDoors = od.DoorTriangles.Select(x => (x.TriangleBeforeDoor, x.Unknown, x.Door.FormKey)).ToList();
        if (!sDoors.SequenceEqual(oDoors)) fails.Add($"{id} 문 삼각형이 원본을 옮긴 것과 다릅니다.");
        var sEdges = sd.EdgeLinks.Select(x => (x.Unknown, map(x.Mesh.FormKey), x.TriangleIndex)).ToList();
        var oEdges = od.EdgeLinks.Select(x => (x.Unknown, x.Mesh.FormKey, x.TriangleIndex)).ToList();
        if (!sEdges.SequenceEqual(oEdges)) fails.Add($"{id} 가장자리 연결이 원본을 옮긴 것과 다릅니다.");
    }

    private static void CheckAnchor(PackSpec pack, DungeonManifest dm, ICellGetter srcCell, FormKey srcNpc, IMajorRecordGetter outRec,
        SourceIndex src, List<string> fails)
    {
        var npc = srcCell.Persistent.Concat(srcCell.Temporary).OfType<IPlacedNpcGetter>().FirstOrDefault(n => n.FormKey == srcNpc);
        if (npc is null) { fails.Add($"{dm.Key}: 앵커 {outRec.FormKey}의 원본 {srcNpc}가 원본 셀의 NPC가 아닙니다."); return; }
        if (npc.MajorFlags.HasFlag(PlacedNpc.MajorFlag.StartsDead)) fails.Add($"{dm.Key}: 앵커 {outRec.FormKey}의 원본이 시체 NPC입니다.");
        if (outRec is not IPlacedObjectGetter a || a.Base.FormKey != Policy.XMarkerHeading) { fails.Add($"{dm.Key}: 앵커 {outRec.FormKey}가 XMarkerHeading이 아닙니다."); return; }
        if (npc.Placement is { } np && a.Placement is { } ap)
        {
            var moved = Builder.Distance(np.Position, ap.Position);
            if (moved > pack.AnchorMaxSnapDistance + 0.01) fails.Add($"{dm.Key}: 앵커 {outRec.FormKey}가 원본 NPC 자리에서 {moved:F0}유닛 떨어졌습니다(상한 {pack.AnchorMaxSnapDistance}).");
            if (!np.Rotation.Equals(ap.Rotation)) fails.Add($"{dm.Key}: 앵커 {outRec.FormKey}의 회전이 원본 NPC와 다릅니다.");
        }
        else fails.Add($"{dm.Key}: 앵커 {outRec.FormKey}나 원본 NPC에 위치가 없습니다.");
    }

    // ---------- 3) 불변식 ----------

    private static void CheckInvariants(PackSpec pack, SourceIndex src, Ledger ledger, ISkyrimModGetter disk,
        PackManifest manifest, List<string> fails, List<string> notes)
    {
        var masters = disk.ModHeader.MasterReferences.Select(m => m.Master.FileName.String).ToList();
        if (!masters.SequenceEqual(new[] { Policy.SourceMaster.FileName.String })) fails.Add($"마스터가 {Policy.SourceMaster} 하나가 아닙니다: {string.Join(", ", masters)}");
        if (disk.ModHeader.Flags.HasFlag(SkyrimModHeader.HeaderFlag.Small)) fails.Add("ESL 플래그가 붙었습니다(결정 20: 정규 ESP).");
        if (Math.Abs(disk.ModHeader.Stats.Version - ExpectedHeaderVersion) > 0.001f) fails.Add($"헤더 판(HEDR)이 {disk.ModHeader.Stats.Version}입니다({ExpectedHeaderVersion}이어야 함).");
        if (manifest.Plugin != pack.Plugin || manifest.PackVersion != pack.PackVersion || manifest.SourceMaster != Policy.SourceMaster.FileName.String)
            fails.Add("목록 파일의 plugin·packVersion·sourceMaster가 던전 목록과 다릅니다.");

        var cells = AllCells(disk).ToList();
        var all = new List<IMajorRecordGetter>();
        foreach (var c in cells)
        {
            all.Add(c);
            all.AddRange(c.Persistent);
            all.AddRange(c.Temporary);
            all.AddRange(c.NavigationMeshes);
        }
        if (disk.EnumerateMajorRecords().Count() != all.Count) fails.Add("셀과 그 자식이 아닌 레코드가 섞였습니다.");
        foreach (var g in all.GroupBy(r => r.FormKey).Where(g => g.Count() > 1)) fails.Add($"번호가 겹칩니다: {g.Key}");
        foreach (var r in all.Where(r => r.FormKey.ModKey != disk.ModKey)) fails.Add($"다른 플러그인 번호의 레코드: {r.FormKey}");
        foreach (var r in all.OfType<IPlacedNpcGetter>()) fails.Add($"NPC 참조가 남았습니다: {r.FormKey}");
        foreach (var g in all.Where(r => !string.IsNullOrEmpty(r.EditorID)).GroupBy(r => r.EditorID!, StringComparer.OrdinalIgnoreCase).Where(g => g.Count() > 1))
            fails.Add($"EditorID가 겹칩니다: {g.Key}");

        // 블록·하위 블록 배치(실내 블록 그룹, 빈 블록·겹친 블록 없음)
        var blockNos = new HashSet<int>();
        foreach (var block in disk.Cells.Records)
        {
            if (!blockNos.Add(block.BlockNumber)) fails.Add($"블록 {block.BlockNumber}이 두 번 있습니다.");
            if (block.GroupType != GroupTypeEnum.InteriorCellBlock) fails.Add($"블록 {block.BlockNumber}의 그룹 형식이 실내 블록이 아닙니다.");
            var subNos = new HashSet<int>();
            if (block.SubBlocks.Count == 0) fails.Add($"블록 {block.BlockNumber}이 비었습니다.");
            foreach (var sub in block.SubBlocks)
            {
                if (!subNos.Add(sub.BlockNumber)) fails.Add($"블록 {block.BlockNumber}의 하위 블록 {sub.BlockNumber}이 두 번 있습니다.");
                if (sub.GroupType != GroupTypeEnum.InteriorCellSubBlock) fails.Add($"하위 블록 {block.BlockNumber}/{sub.BlockNumber}의 그룹 형식이 틀렸습니다.");
                if (sub.Cells.Count == 0) fails.Add($"하위 블록 {block.BlockNumber}/{sub.BlockNumber}이 비었습니다.");
                foreach (var c in sub.Cells)
                {
                    var id = (int)c.FormKey.ID;
                    if (block.BlockNumber != id % 10 || sub.BlockNumber != (id / 10) % 10)
                        fails.Add($"셀 {c.FormKey}의 블록 배치가 틀렸습니다({block.BlockNumber}/{sub.BlockNumber}).");
                }
            }
        }

        if (manifest.Dungeons.Select(d => d.Key).SequenceEqual(pack.Dungeons.Select(d => d.Key)) == false)
            fails.Add("목록 파일의 던전 키가 던전 목록과 다릅니다.");
        var ownerOf = new Dictionary<FormKey, string>();
        foreach (var dm in manifest.Dungeons)
        {
            var cellKey = new FormKey(disk.ModKey, Ledger.ParseId(dm.Cell));
            var cell = cells.FirstOrDefault(c => c.FormKey == cellKey);
            if (cell is null)
            {
                fails.Add($"{dm.Key}: 셀 {dm.Cell}이 ESP에 없습니다.");
                continue;
            }
            if (!cell.Flags.HasFlag(Cell.Flag.IsInteriorCell)) fails.Add($"{dm.Key}: 셀이 실내 셀이 아닙니다.");
            var cellName = cell.Name?.String;
            if (cellName != dm.Name) fails.Add($"{dm.Key}: 셀 이름이 '{cellName}'로 목록의 '{dm.Name}'과 다릅니다.");
            notes.Add($"{dm.Key}: 셀 이름 '{cellName}', 압축 {((cell.MajorRecordFlagsRaw & 0x40000) != 0 ? "예" : "아니오")}, EditorID {cell.EditorID}");
            if (!cell.Location.IsNull || !cell.EncounterZone.IsNull || !cell.Owner.IsNull) fails.Add($"{dm.Key}: 셀의 위치·조우 구역·소유자가 비어 있지 않습니다.");
            if (cell.NavigationMeshes.Count == 0) fails.Add($"{dm.Key}: 내비메쉬가 없습니다.");
            var recs = cell.Persistent.Concat<IMajorRecordGetter>(cell.Temporary).Concat(cell.NavigationMeshes).Append(cell).ToList();
            foreach (var r in recs) ownerOf[r.FormKey] = dm.Key;

            var slot = ledger.Slots.FirstOrDefault(s => s.Dungeon == dm.Key);
            if (slot is null) fails.Add($"{dm.Key}: 장부 구간이 없습니다.");
            else
            {
                var ids = slot.Ids.Values.Select(Ledger.ParseId).ToHashSet();
                foreach (var r in recs.Where(r => !ids.Contains(r.FormKey.ID))) fails.Add($"{dm.Key}: {r.FormKey}가 장부 구간에 없습니다.");
            }

            var placed = cell.Persistent.Concat(cell.Temporary).OfType<IPlacedObjectGetter>().ToDictionary(o => o.FormKey);
            IPlacedObjectGetter? Get(string id, string what)
            {
                var k = new FormKey(disk.ModKey, Ledger.ParseId(id));
                if (placed.TryGetValue(k, out var o)) return o;
                fails.Add($"{dm.Key}: {what} {id}가 이 셀의 배치 참조가 아닙니다.");
                return null;
            }
            bool InPersistent(IPlacedObjectGetter o) => (o.MajorRecordFlagsRaw & Policy.PersistentFlag) != 0 && cell.Persistent.Any(p => p.FormKey == o.FormKey);

            var entry = Get(dm.EntryMarker, "입구 마커");
            P3Float? entryPos = null;
            if (entry is not null)
            {
                if (entry.Base.FormKey != Policy.CocMarkerHeading) fails.Add($"{dm.Key}: 입구 마커의 기본 객체가 COCMarkerHeading이 아닙니다.");
                if (!InPersistent(entry)) fails.Add($"{dm.Key}: 입구 마커가 영구 참조가 아닙니다.");
                if (entry.Placement is null) fails.Add($"{dm.Key}: 입구 마커에 위치가 없습니다.");
                entryPos = entry.Placement?.Position;
            }
            var cocCount = placed.Values.Count(o => o.Base.FormKey == Policy.CocMarkerHeading);
            if (cocCount != 1) fails.Add($"{dm.Key}: 입구 마커가 {cocCount}개입니다.");

            // 앵커: 보스는 _Boss, 무리는 _Pack01부터 차례로, 번호·자리가 모두 다르다.
            var anchorIds = new List<string> { dm.BossAnchor };
            anchorIds.AddRange(dm.PackAnchors);
            foreach (var g in anchorIds.GroupBy(x => x).Where(g => g.Count() > 1)) fails.Add($"{dm.Key}: 앵커 번호 {g.Key}가 목록에 두 번 있습니다.");
            var spec = pack.Dungeons.FirstOrDefault(d => d.Key == dm.Key);
            if (spec is not null && dm.PackAnchors.Count < spec.MinPackAnchors) fails.Add($"{dm.Key}: 무리 앵커가 {dm.PackAnchors.Count}개로 최소보다 적습니다.");
            var anchorObjs = new List<IPlacedObjectGetter>();
            for (var i = 0; i < anchorIds.Count; i++)
            {
                var what = i == 0 ? "보스 앵커" : "무리 앵커";
                var o = Get(anchorIds[i], what);
                if (o is null) continue;
                anchorObjs.Add(o);
                var wantName = i == 0 ? $"{pack.EditorIdPrefix}{dm.Key}_Boss" : $"{pack.EditorIdPrefix}{dm.Key}_Pack{i:00}";
                if (o.EditorID != wantName) fails.Add($"{dm.Key}: {what} {o.FormKey}의 EditorID가 {o.EditorID}로 {wantName}이 아닙니다.");
                if (o.Base.FormKey != Policy.XMarkerHeading) fails.Add($"{dm.Key}: {what} {o.FormKey}의 기본 객체가 XMarkerHeading이 아닙니다.");
                if (!InPersistent(o)) fails.Add($"{dm.Key}: {what} {o.FormKey}가 영구 참조가 아닙니다.");
                if (o.Placement is not { } ap) { fails.Add($"{dm.Key}: {what} {o.FormKey}에 위치가 없습니다."); continue; }
                if (entryPos is { } ep && Builder.Distance(ap.Position, ep) < pack.AnchorMinDistanceFromEntry)
                    fails.Add($"{dm.Key}: {what} {o.FormKey}가 입구에서 {Builder.Distance(ap.Position, ep):F0}유닛입니다.");
                if (!Builder.IsOnNavmesh(ap.Position, cell.NavigationMeshes))
                    fails.Add($"{dm.Key}: {what} {o.FormKey}가 걸을 수 있는 내비메쉬 위에 있지 않습니다.");
            }
            for (var i = 0; i < anchorObjs.Count; i++)
                for (var j = i + 1; j < anchorObjs.Count; j++)
                    if (anchorObjs[i].Placement is { } pi && anchorObjs[j].Placement is { } pj && Builder.Distance(pi.Position, pj.Position) < pack.AnchorMinSeparation)
                        fails.Add($"{dm.Key}: 앵커 {anchorObjs[i].FormKey}와 {anchorObjs[j].FormKey}가 {Builder.Distance(pi.Position, pj.Position):F0}유닛으로 너무 가깝습니다.");
            var namedAnchors = placed.Values.Count(o => o.Base.FormKey == Policy.XMarkerHeading && o.EditorID is { } e
                && e.StartsWith($"{pack.EditorIdPrefix}{dm.Key}_", StringComparison.Ordinal) && (e.EndsWith("_Boss", StringComparison.Ordinal) || e.Contains("_Pack", StringComparison.Ordinal)));
            if (namedAnchors != anchorIds.Count) fails.Add($"{dm.Key}: 앵커 이름을 가진 마커가 {namedAnchors}개인데 목록에는 {anchorIds.Count}개입니다.");

            foreach (var id in dm.ReturnDoors.Concat(dm.SealedDoors).Concat(dm.AutoExits.Select(x => x.Id)))
            {
                var d = Get(id, "문");
                if (d is null) continue;
                if (!src.Cache.TryResolve(d.Base.FormKey, out var db) || db is not IDoorGetter) fails.Add($"{dm.Key}: {id}의 기본 객체가 문이 아닙니다.");
                if (d.TeleportDestination is not null) fails.Add($"{dm.Key}: {id}에 순간이동이 남았습니다.");
            }
            foreach (var x in dm.AutoExits.Where(x => x.Leads is not ("interior" or "exterior") || x.Position.Length != 3 || x.Rotation.Length != 3))
                fails.Add($"{dm.Key}: 자동 출구 {x.Id}의 leads·위치·회전 형식이 틀렸습니다.");
            foreach (var o in placed.Values.Where(o => o.TeleportDestination is not null))
            {
                var target = o.TeleportDestination!.Door.FormKey;
                if (!placed.TryGetValue(target, out var t)) fails.Add($"{dm.Key}: {o.FormKey}의 순간이동이 이 셀 밖({target})으로 갑니다.");
                else if (!src.Cache.TryResolve(t.Base.FormKey, out var tb) || tb is not IDoorGetter) fails.Add($"{dm.Key}: {o.FormKey}의 순간이동 목적지 {target}가 문이 아닙니다.");
            }
            if (dm.ReturnDoors.Count == 0) notes.Add($"{dm.Key}: 직접 여는 귀환 문이 없습니다(바깥문이 자동 출구뿐). 귀환·무허가 입장 탈출은 런타임이 따로 다뤄야 합니다.");

            // 남은 스크립트는 허용 범위 안이어야 한다.
            var own = recs.Select(r => r.FormKey).ToHashSet();
            foreach (var p in cell.Persistent.Concat(cell.Temporary))
            {
                if (Builder.GetPlacedVm(p) is not { } vm) continue;
                foreach (var sc in vm.Scripts.Where(sc => !Policy.IsScriptAllowed(sc, own.Contains, src.Cache)))
                    fails.Add($"{dm.Key}: {p.FormKey}에 허용 범위를 넘는 스크립트 {sc.Name}가 남았습니다.");
            }
            foreach (var o in placed.Values)
            {
                if (o.Lock is not null && src.Cache.TryResolve(o.Base.FormKey, out var lb) && lb is IDoorGetter
                    && !dm.ReturnDoors.Concat(dm.SealedDoors).Concat(dm.AutoExits.Select(x => x.Id)).Contains(Ledger.Format(o.FormKey.ID)))
                    fails.Add($"{dm.Key}: 안쪽 문 {o.FormKey}가 잠겨 있습니다.");
            }

            if (entryPos is { } e2)
            {
                var far = cell.NavigationMeshes.Where(n => n.Data is not null).SelectMany(n => n.Data!.Vertices)
                    .Select(v => (V: v, D: Builder.Distance(v, e2))).OrderByDescending(x => x.D).FirstOrDefault();
                notes.Add($"{dm.Key}: 입구에서 가장 먼 내비메쉬 꼭짓점 x={far.V.X:F0} y={far.V.Y:F0} z={far.V.Z:F0} (직선 {far.D:F0})");
            }
        }

        // 바깥 연결: 같은 던전 안이거나 원본의 월드 상태가 아닌 레코드만 허용. 다른 던전으로 가는 연결은 실패.
        // 허용 목록 스크립트도 그 스크립트에 허용한 형식(키워드·전역·팩션)만 뺀다.
        foreach (var c in cells)
        {
            if (!ownerOf.TryGetValue(c.FormKey, out var dungeon)) continue;
            var recs = c.Persistent.Concat<IMajorRecordGetter>(c.Temporary).Concat(c.NavigationMeshes).ToList();
            foreach (var r in recs.Append(c))
            {
                var links = new List<(FormKey Key, HazardKind Allowed)>();
                if (r is ICellGetter)
                {
                    var childLinks = recs.SelectMany(x => x.EnumerateFormLinks()).Where(l => !l.IsNull).Select(l => l.FormKey).ToList();
                    var own = c.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => l.FormKey).ToList();
                    foreach (var k in childLinks) own.Remove(k);
                    links.AddRange(own.Select(k => (k, HazardKind.None)));
                }
                else
                {
                    var vmLinks = new List<(FormKey, HazardKind)>();
                    if (Builder.GetPlacedVm(r) is { } vm)
                    {
                        foreach (var sc in vm.Scripts)
                        {
                            var allowed = Policy.AllowedDeviceScripts.TryGetValue(sc.Name, out var a) ? a : HazardKind.None;
                            vmLinks.AddRange(sc.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => (l.FormKey, allowed)));
                        }
                    }
                    var rest = r.EnumerateFormLinks().Where(l => !l.IsNull).Select(l => l.FormKey).ToList();
                    foreach (var (k, _) in vmLinks) rest.Remove(k);
                    links.AddRange(rest.Select(k => (k, HazardKind.None)));
                    links.AddRange(vmLinks);
                }
                foreach (var (k, allowed) in links)
                {
                    if (k.ModKey == disk.ModKey)
                    {
                        if (!ownerOf.TryGetValue(k, out var other)) fails.Add($"{dungeon}: {r.FormKey} -> {k} 없는 레코드를 가리킵니다.");
                        else if (other != dungeon) fails.Add($"{dungeon}: {r.FormKey} -> {k} 다른 던전({other})을 가리킵니다.");
                        continue;
                    }
                    if (!src.Cache.TryResolve(k, out var t)) { fails.Add($"{dungeon}: {r.FormKey} -> {k} 원본에서 찾지 못했습니다."); continue; }
                    var kind = Policy.KindOf(t);
                    if (kind == HazardKind.Forbidden || (Policy.IsWorldState(t) && (kind & allowed) == HazardKind.None))
                        fails.Add($"{dungeon}: {r.FormKey} -> {k} {t.EditorID} 바닐라 월드 상태를 가리킵니다.");
                }
            }
        }
    }

    private static IEnumerable<ICellGetter> AllCells(ISkyrimModGetter mod)
        => mod.Cells.Records.SelectMany(b => b.SubBlocks).SelectMany(s => s.Cells);
}
