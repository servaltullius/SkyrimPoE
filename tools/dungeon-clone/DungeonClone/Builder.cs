// 던전 목록과 장부로 지도 팩 플러그인을 메모리에 만든다. build는 이것을 쓰고, verify는 같은 것을 다시 만들어 디스크와 비교한다.
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Noggog;

namespace SkyrimPoE.Tools.DungeonClone;

internal sealed class SourceIndex
{
    public required ISkyrimModGetter Mod { get; init; }
    public required ILinkCacheLike Cache { get; init; }
    // 원본 마스터의 실내 셀에 놓인 참조 → 그 셀(문 목적지가 실내인지 가르는 데 씀)
    public required Dictionary<FormKey, ICellGetter> InteriorRefCell { get; init; }
    public required Dictionary<string, ICellGetter> InteriorCellsByEditorId { get; init; }

    public static SourceIndex Create(ISkyrimModGetter mod)
    {
        if (mod.ModKey != Policy.SourceMaster) throw new InvalidDataException($"원본은 {Policy.SourceMaster}여야 합니다: {mod.ModKey}");
        var refCell = new Dictionary<FormKey, ICellGetter>();
        var byEdid = new Dictionary<string, ICellGetter>(StringComparer.OrdinalIgnoreCase);
        foreach (var c in mod.Cells.Records.SelectMany(b => b.SubBlocks).SelectMany(s => s.Cells))
        {
            if (!c.Flags.HasFlag(Cell.Flag.IsInteriorCell)) continue;
            if (c.EditorID is { } e) byEdid[e] = c;
            foreach (var r in c.Persistent.Concat(c.Temporary)) refCell[r.FormKey] = c;
        }
        var cache = new LinkCacheAdapter(mod.ToImmutableLinkCache());
        if (!cache.TryResolve(Policy.BossLocRefType, out var lcrt) || lcrt is not ILocationReferenceTypeGetter || lcrt.EditorID != "Boss")
            throw new InvalidDataException($"{Policy.BossLocRefType}가 위치 참조 유형 Boss가 아닙니다(도구 상수 확인 필요).");
        return new SourceIndex { Mod = mod, Cache = cache, InteriorRefCell = refCell, InteriorCellsByEditorId = byEdid };
    }

    public ICellGetter GetCell(string editorId)
        => InteriorCellsByEditorId.TryGetValue(editorId, out var c) ? c
            : throw new InvalidDataException($"{Policy.SourceMaster}에서 실내 셀 '{editorId}'을 찾지 못했습니다.");

    public string EditorIdOf(FormKey k) => Cache.TryResolve(k, out var r) ? r.EditorID ?? k.ToString() : k.ToString();
}

internal sealed record AnchorCandidate(IPlacedNpcGetter Npc, string BaseEditorId, bool BossTagged, bool StartsDead, double DistanceFromEntry);

internal sealed class DungeonReport
{
    public string Key { get; set; } = "";
    public string SourceCell { get; set; } = "";
    public string OutputCell { get; set; } = "";
    public int Kept { get; set; }
    public SortedDictionary<string, int> Removed { get; set; } = new();
    public List<string> RemovedHazardBaseScripts { get; set; } = new();
    public List<string> RemovedQuestTriggers { get; set; } = new();
    // 남은 참조가 뺀 참조(NPC 포함)를 가리키던 연결. 복제본에서는 빈 연결이 된다.
    public List<string> LinksToRemovedRefs { get; set; } = new();
    public List<string> StrippedRefScripts { get; set; } = new();
    // 허용 목록 스크립트인데 속성이 가리키던 참조를 뺀 것(빈 속성으로 남음)
    public List<string> AllowedScriptsLostTarget { get; set; } = new();
    // 결정 171(결정 170 U-5의 예외)로 남긴, 물건을 주는 장치
    public List<string> KeptItemGivers { get; set; } = new();
    public int UnlockedDoors { get; set; }
    public string Boss { get; set; } = "";
    public List<string> PackAnchors { get; set; } = new();
    public List<string> SkippedAnchorsNearEntry { get; set; } = new();
    public List<string> SkippedAnchorsFarFromNavmesh { get; set; } = new();
    public List<string> SkippedAnchorsTooClose { get; set; } = new();
    public List<string> RemainingScriptedRefs { get; set; } = new();
}

internal sealed class BuildResult
{
    public required SkyrimMod Mod { get; init; }
    public required PackManifest Manifest { get; init; }
    public required List<DungeonReport> Reports { get; init; }
}

internal static class Builder
{
    // 내비메쉬 위로 볼 높이 차(유닛). NPC 발 위치와 내비메쉬 면은 조금 어긋난다.
    public const float NavmeshHeightTolerance = 48f;

    public static IVirtualMachineAdapterGetter? GetPlacedVm(IMajorRecordGetter rec) => rec switch
    {
        IPlacedObjectGetter o => o.VirtualMachineAdapter,
        IPlacedTrapGetter t => t.VirtualMachineAdapter,
        _ => null,
    };

    public static List<AnchorCandidate> AnchorCandidates(ICellGetter srcCell, SourceIndex src, P3Float entry)
        => srcCell.Persistent.Concat(srcCell.Temporary).OfType<IPlacedNpcGetter>()
            .Select(n => new AnchorCandidate(
                n,
                src.EditorIdOf(n.Base.FormKey),
                n.LocationRefTypes?.Any(l => l.FormKey == Policy.BossLocRefType) == true,
                n.MajorFlags.HasFlag(PlacedNpc.MajorFlag.StartsDead),
                Distance(n.Placement?.Position ?? entry, entry)))
            .ToList();

    public static double Distance(P3Float a, P3Float b)
        => Math.Sqrt(Math.Pow(a.X - b.X, 2) + Math.Pow(a.Y - b.Y, 2) + Math.Pow(a.Z - b.Z, 2));

    // 걸을 수 있는 삼각형만(삭제·물 표시와 넓이가 없는 삼각형은 뺌)
    public static IEnumerable<(P3Float A, P3Float B, P3Float C)> WalkableTriangles(IEnumerable<INavigationMeshGetter> navs)
    {
        foreach (var nav in navs)
        {
            if (nav.Data is not { } d) continue;
            var v = d.Vertices;
            foreach (var t in d.Triangles)
            {
                if (t.Flags.HasFlag(NavmeshTriangle.Flag.Deleted) || t.Flags.HasFlag(NavmeshTriangle.Flag.Water)) continue;
                var i = t.Vertices;
                if (i.X < 0 || i.Y < 0 || i.Z < 0 || i.X >= v.Count || i.Y >= v.Count || i.Z >= v.Count) continue;
                var (a, b, c) = (v[i.X], v[i.Y], v[i.Z]);
                var area2d = Math.Abs((b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y));
                if (area2d < 1f) continue;
                yield return (a, b, c);
            }
        }
    }

    // 점이 걸을 수 있는 삼각형 위(2D 안쪽, 면 높이와의 차가 허용 안)에 있는지
    public static bool IsOnNavmesh(P3Float p, IEnumerable<INavigationMeshGetter> navs)
        => WalkableTriangles(navs).Any(t => TryInterpolateZ(p, t.A, t.B, t.C, out var z) && Math.Abs(p.Z - z) <= NavmeshHeightTolerance);

    // 내비메쉬 위가 아니면 걸을 수 있는 삼각형 위의 가장 가까운 점으로 옮긴다. 옮긴 거리(0이면 그대로)를 함께 준다.
    // 관·파이프 같은 매복 가구 안의 NPC 자리에 그대로 스폰하면 갇힐 수 있어서다(6단계 계획 R1 3번).
    public static (P3Float Position, double Moved) SnapToNavmesh(P3Float p, IReadOnlyList<INavigationMeshGetter> navs)
    {
        if (IsOnNavmesh(p, navs)) return (p, 0);
        var best = p;
        var bestD = double.MaxValue;
        foreach (var (a, b, c) in WalkableTriangles(navs))
        {
            var q = ClosestPointOnTriangle(p, a, b, c);
            var d = Distance(p, q);
            if (d < bestD)
            {
                bestD = d;
                best = q;
            }
        }
        if (bestD == double.MaxValue) throw new InvalidDataException("걸을 수 있는 내비메쉬 삼각형이 없어 앵커를 붙일 수 없습니다.");
        return (best, bestD);
    }

    // 삼각형 위의 가장 가까운 점(Ericson, Real-Time Collision Detection 5.1.5)
    public static P3Float ClosestPointOnTriangle(P3Float p, P3Float a, P3Float b, P3Float c)
    {
        static (double X, double Y, double Z) Sub(P3Float u, P3Float v) => (u.X - v.X, u.Y - v.Y, u.Z - v.Z);
        static double Dot((double X, double Y, double Z) u, (double X, double Y, double Z) v) => u.X * v.X + u.Y * v.Y + u.Z * v.Z;
        static P3Float At(P3Float o, (double X, double Y, double Z) dir, double t)
            => new((float)(o.X + dir.X * t), (float)(o.Y + dir.Y * t), (float)(o.Z + dir.Z * t));

        var ab = Sub(b, a);
        var ac = Sub(c, a);
        var ap = Sub(p, a);
        var d1 = Dot(ab, ap);
        var d2 = Dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) return a;
        var bp = Sub(p, b);
        var d3 = Dot(ab, bp);
        var d4 = Dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) return b;
        var vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) return At(a, ab, d1 / (d1 - d3));
        var cp = Sub(p, c);
        var d5 = Dot(ab, cp);
        var d6 = Dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) return c;
        var vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) return At(a, ac, d2 / (d2 - d6));
        var va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        {
            var bc = Sub(c, b);
            return At(b, bc, (d4 - d3) / ((d4 - d3) + (d5 - d6)));
        }
        var denom = 1.0 / (va + vb + vc);
        var v = vb * denom;
        var w = vc * denom;
        return new P3Float(
            (float)(a.X + ab.X * v + ac.X * w),
            (float)(a.Y + ab.Y * v + ac.Y * w),
            (float)(a.Z + ab.Z * v + ac.Z * w));
    }

    private static bool TryInterpolateZ(P3Float p, P3Float a, P3Float b, P3Float c, out float z)
    {
        z = 0;
        var det = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
        if (Math.Abs(det) < 1e-6f) return false;
        var l1 = ((b.Y - c.Y) * (p.X - c.X) + (c.X - b.X) * (p.Y - c.Y)) / det;
        var l2 = ((c.Y - a.Y) * (p.X - c.X) + (a.X - c.X) * (p.Y - c.Y)) / det;
        var l3 = 1 - l1 - l2;
        const float eps = -1e-4f;
        if (l1 < eps || l2 < eps || l3 < eps) return false;
        z = l1 * a.Z + l2 * b.Z + l3 * c.Z;
        return true;
    }

    public static IPlacedObjectGetter EntryMarker(ICellGetter srcCell)
    {
        var markers = srcCell.Persistent.Concat(srcCell.Temporary).OfType<IPlacedObjectGetter>()
            .Where(o => o.Base.FormKey == Policy.CocMarkerHeading).ToList();
        return markers.Count == 1 ? markers[0]
            : throw new InvalidDataException($"{srcCell.EditorID}: 입구 마커(COCMarkerHeading)가 {markers.Count}개입니다. 정확히 하나여야 합니다.");
    }

    public static BuildResult Build(PackSpec pack, SourceIndex src, Ledger ledger, bool allowLedgerChanges)
    {
        var outKey = ModKey.FromFileName(pack.Plugin);
        var outMod = new SkyrimMod(outKey, SkyrimRelease.SkyrimSE);
        var manifest = new PackManifest { Plugin = pack.Plugin, PackVersion = pack.PackVersion, SourceMaster = Policy.SourceMaster.FileName };
        var reports = new List<DungeonReport>();
        var blocks = new SortedDictionary<int, CellBlock>();

        foreach (var spec in pack.Dungeons)
        {
            var (cell, dm, report) = BuildDungeon(pack, spec, src, ledger, allowLedgerChanges, outKey);
            manifest.Dungeons.Add(dm);
            reports.Add(report);

            // 실내 셀 그룹 배치(블록 = FormID 끝자리, 하위 블록 = 그다음 자리)
            var id = (int)cell.FormKey.ID;
            var blockNo = id % 10;
            var subNo = (id / 10) % 10;
            if (!blocks.TryGetValue(blockNo, out var block))
            {
                block = new CellBlock { BlockNumber = blockNo, GroupType = GroupTypeEnum.InteriorCellBlock };
                blocks[blockNo] = block;
            }
            var sub = block.SubBlocks.FirstOrDefault(s => s.BlockNumber == subNo);
            if (sub is null)
            {
                sub = new CellSubBlock { BlockNumber = subNo, GroupType = GroupTypeEnum.InteriorCellSubBlock };
                block.SubBlocks.Add(sub);
            }
            sub.Cells.Add(cell);
        }
        foreach (var b in blocks.Values)
        {
            var ordered = b.SubBlocks.OrderBy(s => s.BlockNumber).ToList();
            b.SubBlocks.Clear();
            b.SubBlocks.AddRange(ordered);
            outMod.Cells.Records.Add(b);
        }
        return new BuildResult { Mod = outMod, Manifest = manifest, Reports = reports };
    }

    // 원본 자식마다 뺄지와, 남길 참조에서 뗄 스크립트 이름을 정한다(원본 기준, 번호를 주기 전).
    // 1) 기본 객체 기준 분류(Policy.Classify)
    // 2) 참조 스크립트: 허용 범위를 넘는 대상을 가리키면 떼고, 뺀 참조를 가리키면(허용 목록이 아니면) 뗀다.
    //    뗄 스크립트와 같은 이름의 스크립트가 기본 객체에도 있으면, 떼어도 빈 속성으로 다시 돌므로 참조째 뺀다(QuestTrigger).
    //    참조를 빼면 다른 참조의 판정이 바뀔 수 있어 바뀌지 않을 때까지 되풀이한다.
    internal static (Dictionary<FormKey, RemovalReason> Removed, Dictionary<FormKey, HashSet<string>> Strip, List<string> LostTargets)
        Plan(ICellGetter srcCell, SourceIndex src)
    {
        var children = srcCell.Persistent.Cast<IMajorRecordGetter>().Concat(srcCell.Temporary).Concat(srcCell.NavigationMeshes).ToList();
        var srcKeys = children.Select(c => c.FormKey).Append(srcCell.FormKey).ToHashSet();
        var removed = new Dictionary<FormKey, RemovalReason>();
        foreach (var child in children)
        {
            var reason = Policy.Classify(child, srcKeys.Contains, src.Cache);
            if (reason != RemovalReason.None) removed[child.FormKey] = reason;
        }

        var strip = new Dictionary<FormKey, HashSet<string>>();
        var lost = new List<string>();
        bool changed;
        do
        {
            changed = false;
            strip.Clear();
            lost.Clear();
            foreach (var child in children.Where(c => !removed.ContainsKey(c.FormKey)))
            {
                if (GetPlacedVm(child) is not { } vm) continue;
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var sc in vm.Scripts)
                {
                    var hitsRemoved = sc.EnumerateFormLinks().Any(l => removed.ContainsKey(l.FormKey));
                    var allowedName = Policy.AllowedDeviceScripts.ContainsKey(sc.Name);
                    if (!Policy.IsScriptAllowed(sc, srcKeys.Contains, src.Cache) || (hitsRemoved && !allowedName))
                    {
                        names.Add(sc.Name);
                    }
                    else if (hitsRemoved)
                    {
                        lost.Add($"{child.FormKey.ID:X6} {sc.Name}");
                    }
                }
                if (names.Count == 0) continue;
                var baseVm = child is IPlacedObjectGetter po && !po.Base.IsNull && src.Cache.TryResolve(po.Base.FormKey, out var b)
                    ? Policy.GetVm(b) : null;
                if (baseVm is not null && baseVm.Scripts.Any(s => names.Contains(s.Name)))
                {
                    removed[child.FormKey] = RemovalReason.QuestTrigger;
                    changed = true;
                    continue;
                }
                strip[child.FormKey] = names;
            }
        } while (changed);
        return (removed, strip, lost);
    }

    private static (Cell, DungeonManifest, DungeonReport) BuildDungeon(
        PackSpec pack, DungeonSpec spec, SourceIndex src, Ledger ledger, bool allowNew, ModKey outKey)
    {
        var srcCell = src.GetCell(spec.SourceCell);
        var report = new DungeonReport { Key = spec.Key, SourceCell = $"{srcCell.FormKey.ID:X6} {srcCell.EditorID}" };
        var slot = ledger.GetSlot(spec.Key, Ledger.SourceKey(srcCell.FormKey), allowNew);
        FormKey Assign(FormKey source, string role) => new(outKey, ledger.Assign(slot, Ledger.EntryKey(source, role), allowNew));

        var srcEntry = EntryMarker(srcCell);
        var entryPos = srcEntry.Placement?.Position ?? throw new InvalidDataException($"{spec.Key}: 입구 마커에 위치가 없습니다.");

        // 1) 뺄 것과 뗄 스크립트를 원본 기준으로 정한다.
        var (removed, strip, lost) = Plan(srcCell, src);
        report.AllowedScriptsLostTarget.AddRange(lost);
        var srcChildren = srcCell.Persistent.Cast<IMajorRecordGetter>()
            .Concat(srcCell.Temporary).Concat(srcCell.NavigationMeshes).ToList();
        foreach (var (k, reason) in removed.OrderBy(x => x.Key.ID))
        {
            report.Removed[reason.ToString()] = report.Removed.GetValueOrDefault(reason.ToString()) + 1;
            var child = srcChildren.First(c => c.FormKey == k);
            if (child is IPlacedObjectGetter hp && src.Cache.TryResolve(hp.Base.FormKey, out var hb))
            {
                if (reason == RemovalReason.HazardBaseScript && Policy.GetVm(hb) is { } hvm)
                    report.RemovedHazardBaseScripts.Add($"{k.ID:X6} base={hb.EditorID}: {string.Join(", ", hvm.Scripts.Select(s => s.Name))}");
                if (reason == RemovalReason.QuestTrigger)
                    report.RemovedQuestTriggers.Add($"{k.ID:X6} base={hb.EditorID}: {string.Join(", ", GetPlacedVm(child)!.Scripts.Select(s => s.Name))}");
            }
        }
        foreach (var child in srcChildren.Where(c => !removed.ContainsKey(c.FormKey)))
        {
            foreach (var l in child.EnumerateFormLinks().Where(l => removed.ContainsKey(l.FormKey)))
            {
                report.LinksToRemovedRefs.Add($"{child.FormKey.ID:X6} -> {l.FormKey.ID:X6} ({removed[l.FormKey]})");
            }
            if (child is IPlacedObjectGetter kp && src.Cache.TryResolve(kp.Base.FormKey, out var kb) && Policy.GetVm(kb) is { } kvm
                && kvm.Scripts.Any(s => Policy.KeptItemGivers.Contains(s.Name)))
            {
                report.KeptItemGivers.Add($"{child.FormKey.ID:X6} base={kb.EditorID}");
            }
        }

        // 2) 새 번호. 남기는 것만 번호를 받고, 빼는 것은 빈 번호(Null)로 매핑한다.
        var mapping = new Dictionary<FormKey, FormKey> { [srcCell.FormKey] = Assign(srcCell.FormKey, "cell") };
        foreach (var child in srcChildren)
        {
            mapping[child.FormKey] = removed.ContainsKey(child.FormKey) ? FormKey.Null : Assign(child.FormKey, "ref");
        }

        // 3) 셀 통째 복제 뒤, 자식을 남길 것의 복제본으로 바꾸고 연결을 새 번호로 다시 잇는다.
        var cell = srcCell.Duplicate(mapping[srcCell.FormKey]);
        var persistent = srcCell.Persistent.Where(p => !removed.ContainsKey(p.FormKey))
            .Select(p => (IPlaced)p.Duplicate(mapping[p.FormKey])).ToList();
        var temporary = srcCell.Temporary.Where(t => !removed.ContainsKey(t.FormKey))
            .Select(t => (IPlaced)t.Duplicate(mapping[t.FormKey])).ToList();
        var navmeshes = srcCell.NavigationMeshes.Select(n => (NavigationMesh)n.Duplicate(mapping[n.FormKey])).ToList();
        cell.Persistent.Clear();
        cell.Persistent.AddRange(persistent);
        cell.Temporary.Clear();
        cell.Temporary.AddRange(temporary);
        cell.NavigationMeshes.Clear();
        cell.NavigationMeshes.AddRange(navmeshes);
        report.Kept = persistent.Count + temporary.Count + navmeshes.Count;
        cell.RemapLinks(mapping);
        var newKeys = mapping.Values.Where(k => !k.IsNull).ToHashSet();
        var srcByOut = srcChildren.Where(c => !removed.ContainsKey(c.FormKey)).ToDictionary(c => mapping[c.FormKey], c => c);

        // 4) 정해 둔 참조 스크립트를 뗀다.
        foreach (var placed in cell.Persistent.Concat(cell.Temporary))
        {
            if (!strip.TryGetValue(srcByOut[placed.FormKey].FormKey, out var names)) continue;
            var vm = placed switch
            {
                PlacedObject o => o.VirtualMachineAdapter,
                IPlacedTrap t => t.VirtualMachineAdapter,
                _ => null,
            } ?? throw new InvalidDataException($"{spec.Key}: {placed.FormKey}의 스크립트가 사라졌습니다.");
            foreach (var sc in vm.Scripts.Where(s => names.Contains(s.Name)).ToList())
            {
                vm.Scripts.Remove(sc);
                report.StrippedRefScripts.Add($"{Ledger.Format(placed.FormKey.ID)} {sc.Name}");
            }
            if (vm.Scripts.Count == 0)
            {
                switch (placed)
                {
                    case PlacedObject o: o.VirtualMachineAdapter = null; break;
                    case IPlacedTrap t: t.VirtualMachineAdapter = null; break;
                }
            }
        }

        // 5) 셀 밖으로 가는 문 순간이동을 끊고, 문의 종류를 기록한다.
        var dm = new DungeonManifest
        {
            Key = spec.Key, Type = spec.Type, Role = spec.Role, Name = spec.Name,
            SourceCell = $"{srcCell.FormKey.ID:X6}", Cell = Ledger.Format(mapping[srcCell.FormKey].ID),
        };
        var cutDoors = new HashSet<FormKey>();
        foreach (var placed in cell.Persistent.Concat(cell.Temporary).OfType<PlacedObject>())
        {
            if (placed.TeleportDestination is not { } tele || newKeys.Contains(tele.Door.FormKey)) continue;
            var srcDoor = (IPlacedObjectGetter)srcByOut[placed.FormKey];
            var srcTarget = srcDoor.TeleportDestination!.Door.FormKey;
            if (srcTarget.ModKey != Policy.SourceMaster || !src.Cache.TryResolve(srcTarget, out var targetRec) || targetRec is not IPlacedObjectGetter)
                throw new InvalidDataException($"{spec.Key}: 문 {srcDoor.FormKey}의 목적지 {srcTarget}를 {Policy.SourceMaster}에서 배치 참조로 찾지 못했습니다(문 분류 불가).");
            if (!src.Cache.TryResolve(placed.Base.FormKey, out var db) || db is not IDoorGetter door)
                throw new InvalidDataException($"{spec.Key}: 순간이동을 가진 {srcDoor.FormKey}의 기본 객체가 문이 아닙니다.");
            var leadsInterior = src.InteriorRefCell.ContainsKey(srcTarget);
            placed.TeleportDestination = null;
            cutDoors.Add(placed.FormKey);
            var id = Ledger.Format(placed.FormKey.ID);
            if (door.Flags.HasFlag(Door.Flag.Automatic))
            {
                var p = placed.Placement ?? throw new InvalidDataException($"{spec.Key}: 자동 문 {id}에 위치가 없습니다.");
                dm.AutoExits.Add(new AutoExit
                {
                    Id = id,
                    Leads = leadsInterior ? "interior" : "exterior",
                    Position = new[] { p.Position.X, p.Position.Y, p.Position.Z },
                    Rotation = new[] { p.Rotation.X, p.Rotation.Y, p.Rotation.Z },
                });
            }
            else if (leadsInterior) dm.SealedDoors.Add(id);
            else dm.ReturnDoors.Add(id);
        }

        // 6) 안쪽 문(순간이동을 끊지 않은 문) 잠금을 푼다(결정 170 U-5: 열쇠를 가진 NPC와 열쇠를 뺐으므로).
        foreach (var placed in cell.Persistent.Concat(cell.Temporary).OfType<PlacedObject>())
        {
            if (placed.Lock is null || cutDoors.Contains(placed.FormKey)) continue;
            if (!src.Cache.TryResolve(placed.Base.FormKey, out var lb) || lb is not IDoorGetter) continue;
            placed.Lock = null;
            report.UnlockedDoors++;
        }

        // 7) 입구 마커를 영구 참조로 옮긴다(셀이 로드되기 전에도 DLL이 찾게).
        var entryKey = mapping[srcEntry.FormKey];
        var entry = cell.Persistent.Concat(cell.Temporary).OfType<PlacedObject>().First(o => o.FormKey == entryKey);
        if (cell.Temporary.Remove(entry)) cell.Persistent.Add(entry);
        entry.MajorRecordFlagsRaw |= Policy.PersistentFlag;
        entry.EditorID = $"{pack.EditorIdPrefix}{spec.Key}_Entry";
        dm.EntryMarker = Ledger.Format(entry.FormKey.ID);

        // 8) 스폰 앵커: 뺀 NPC 자리(시체 제외)에 영구 X마커를 둔다.
        //    내비메쉬 위가 아니면 걸을 수 있는 삼각형 위 가장 가까운 점으로 옮기되, anchorMaxSnapDistance를 넘으면 버린다.
        //    입구에 가깝거나, 이미 고른 앵커와 anchorMinSeparation보다 가까우면 버린다.
        var navList = srcCell.NavigationMeshes.ToList();
        var candidates = AnchorCandidates(srcCell, src, entryPos).Where(c => !c.StartsDead)
            .Select(c =>
            {
                var p = c.Npc.Placement ?? throw new InvalidDataException($"{spec.Key}: NPC {c.Npc.FormKey}에 위치가 없습니다.");
                var (pos, moved) = SnapToNavmesh(p.Position, navList);
                return new Snapped(c, pos, p.Rotation, moved, Distance(pos, entryPos));
            })
            .ToList();
        string Describe(Snapped a)
            => $"{a.C.Npc.FormKey.ID:X6} {a.C.BaseEditorId} (입구 {a.Dist:F0}{(a.Moved > 0 ? $", 내비메쉬로 {a.Moved:F0} 옮김" : "")})";

        Snapped boss;
        if (spec.BossAnchor is { } bossId)
        {
            var want = Convert.ToUInt32(bossId, 16);
            boss = candidates.FirstOrDefault(a => a.C.Npc.FormKey.ID == want && a.C.Npc.FormKey.ModKey == srcCell.FormKey.ModKey)
                   ?? throw new InvalidDataException($"{spec.Key}: bossAnchor {bossId}가 이 셀의 (시체가 아닌) NPC 참조가 아닙니다.");
        }
        else
        {
            var tagged = candidates.Where(x => x.C.BossTagged).ToList();
            if (tagged.Count != 1)
                throw new InvalidDataException($"{spec.Key}: Boss 표시 NPC가 {tagged.Count}개입니다. 목록 파일의 bossAnchor로 하나를 정해야 합니다(anchors 명령으로 후보 확인).");
            boss = tagged[0];
        }
        if (boss.Moved > pack.AnchorMaxSnapDistance)
            throw new InvalidDataException($"{spec.Key}: 보스 앵커를 내비메쉬로 {boss.Moved:F0}유닛 옮겨야 해서 {pack.AnchorMaxSnapDistance}를 넘습니다.");
        if (boss.Dist < pack.AnchorMinDistanceFromEntry)
            throw new InvalidDataException($"{spec.Key}: 보스 앵커가 입구에서 {boss.Dist:F0}유닛으로 {pack.AnchorMinDistanceFromEntry}보다 가깝습니다.");
        var chosen = new List<Snapped> { boss };
        foreach (var c in candidates.Where(c => c != boss))
        {
            if (c.Moved > pack.AnchorMaxSnapDistance) { report.SkippedAnchorsFarFromNavmesh.Add(Describe(c)); continue; }
            if (c.Dist < pack.AnchorMinDistanceFromEntry) { report.SkippedAnchorsNearEntry.Add(Describe(c)); continue; }
            if (chosen.Any(x => Distance(x.Pos, c.Pos) < pack.AnchorMinSeparation)) { report.SkippedAnchorsTooClose.Add(Describe(c)); continue; }
            chosen.Add(c);
        }
        var packs = chosen.Skip(1).ToList();
        if (packs.Count < spec.MinPackAnchors)
            throw new InvalidDataException($"{spec.Key}: 무리 앵커가 {packs.Count}개로 최소 {spec.MinPackAnchors}개보다 적습니다.");

        PlacedObject MakeAnchor(Snapped c, string editorId)
        {
            var a = new PlacedObject(Assign(c.C.Npc.FormKey, "anchor"), SkyrimRelease.SkyrimSE)
            {
                Base = new FormLinkNullable<IPlaceableObjectGetter>(Policy.XMarkerHeading),
                Placement = new Placement { Position = c.Pos, Rotation = c.Rot },
                EditorID = editorId,
            };
            a.MajorRecordFlagsRaw |= Policy.PersistentFlag;
            cell.Persistent.Add(a);
            return a;
        }
        var bossAnchor = MakeAnchor(boss, $"{pack.EditorIdPrefix}{spec.Key}_Boss");
        dm.BossAnchor = Ledger.Format(bossAnchor.FormKey.ID);
        report.Boss = $"{dm.BossAnchor} <- {Describe(boss)}";
        for (var i = 0; i < packs.Count; i++)
        {
            var a = MakeAnchor(packs[i], $"{pack.EditorIdPrefix}{spec.Key}_Pack{i + 1:00}");
            dm.PackAnchors.Add(Ledger.Format(a.FormKey.ID));
            report.PackAnchors.Add($"{Ledger.Format(a.FormKey.ID)} <- {Describe(packs[i])}");
        }

        // 9) 월드 상태와 얽히는 셀 값을 비우고, 이름을 붙인다.
        cell.Location.Clear();
        cell.EncounterZone.Clear();
        cell.Owner.Clear();
        cell.EditorID = $"{pack.EditorIdPrefix}{spec.Key}";
        cell.Name = spec.Name;

        // 10) 자식 EditorID에 접두사(원본 EditorID는 원본 안에서 유일하므로 접두사를 붙여도 유일).
        var childPrefix = $"{pack.EditorIdPrefix}{spec.Key}_";
        foreach (var rec in cell.Persistent.Concat<IMajorRecord>(cell.Temporary).Concat(cell.NavigationMeshes))
        {
            if (!string.IsNullOrEmpty(rec.EditorID) && !rec.EditorID.StartsWith(pack.EditorIdPrefix, StringComparison.Ordinal))
            {
                rec.EditorID = childPrefix + rec.EditorID;
            }
        }

        foreach (var placed in cell.Persistent.Concat(cell.Temporary))
        {
            if (GetPlacedVm(placed) is { Scripts.Count: > 0 } rvm)
            {
                report.RemainingScriptedRefs.Add($"{Ledger.Format(placed.FormKey.ID)}: {string.Join(", ", rvm.Scripts.Select(s => s.Name))}");
            }
        }
        report.OutputCell = dm.Cell;
        return (cell, dm, report);
    }

    private sealed record Snapped(AnchorCandidate C, P3Float Pos, P3Float Rot, double Moved, double Dist);
}
