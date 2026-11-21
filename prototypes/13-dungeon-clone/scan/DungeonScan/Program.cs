// R0 조사: Skyrim.esm의 실내 셀마다 던전 후보 판단에 쓸 값을 JSON 줄로 낸다(읽기 전용).
using System.Text.Json;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

var data = args.Length > 0 ? args[0] : @"G:\TAKEALOOK\Stock Game\Data";
var outPath = args.Length > 1 ? args[1] : "scan.jsonl";
using var mod = SkyrimMod.CreateFromBinaryOverlay(Path.Combine(data, "Skyrim.esm"), SkyrimRelease.SkyrimSE);
var cache = mod.ToImmutableLinkCache();
var coc = FormKey.Factory("000032:Skyrim.esm");
var bossLcrt = FormKey.Factory("0130F7:Skyrim.esm");

var cells = new List<ICellGetter>();
foreach (var b in mod.Cells.Records)
    foreach (var sb in b.SubBlocks)
        foreach (var c in sb.Cells)
            cells.Add(c);

// 참조 → 실내 셀
var refCell = new Dictionary<FormKey, ICellGetter>();
foreach (var c in cells)
    foreach (var r in c.Persistent.Concat(c.Temporary))
        refCell[r.FormKey] = c;

var cellsPerLocation = cells.Where(c => !c.Location.IsNull)
    .GroupBy(c => c.Location.FormKey).ToDictionary(g => g.Key, g => g.Count());

string Edid(FormKey k) => cache.TryResolve(k, out IMajorRecordGetter? r) ? (r.EditorID ?? k.ToString()) : k.ToString();

bool IsHazard(FormKey k, HashSet<FormKey> local)
{
    if (k.IsNull || local.Contains(k)) return false;
    if (!cache.TryResolve(k, out IMajorRecordGetter? t)) return true;
    return t is IQuestGetter or ILocationGetter or IEncounterZoneGetter or IPlacedGetter or ICellGetter
        or IWorldspaceGetter or INavigationMeshGetter or IGlobalGetter or IFactionGetter or IKeywordGetter;
}

IVirtualMachineAdapterGetter? Vm(object? o) => o switch
{
    IPlacedObjectGetter p => p.VirtualMachineAdapter,
    IPlacedTrapGetter t => t.VirtualMachineAdapter,
    IActivatorGetter a => a.VirtualMachineAdapter,
    IContainerGetter ct => ct.VirtualMachineAdapter,
    IDoorGetter d => d.VirtualMachineAdapter,
    IFurnitureGetter f => f.VirtualMachineAdapter,
    IFloraGetter fl => fl.VirtualMachineAdapter,
    IMiscItemGetter m => m.VirtualMachineAdapter,
    ILightGetter l => l.VirtualMachineAdapter,
    IStaticGetter => null,
    _ => null,
};

using var w = new StreamWriter(outPath);
foreach (var c in cells)
{
    if (!c.Flags.HasFlag(Cell.Flag.IsInteriorCell)) continue;
    var all = c.Persistent.Concat(c.Temporary).ToList();
    var local = all.Select(r => r.FormKey).Append(c.FormKey).Concat(c.NavigationMeshes.Select(n => n.FormKey)).ToHashSet();
    var objs = all.OfType<IPlacedObjectGetter>().ToList();
    var npcs = all.OfType<IPlacedNpcGetter>().ToList();

    var exteriorDoors = new List<string>();
    var otherCellDoors = new HashSet<string>();
    foreach (var o in objs)
    {
        if (o.TeleportDestination is not { } tele) continue;
        if (refCell.TryGetValue(tele.Door.FormKey, out var dc))
        {
            if (dc.FormKey != c.FormKey) otherCellDoors.Add(dc.EditorID ?? dc.FormKey.ToString());
        }
        else exteriorDoors.Add(o.FormKey.ID.ToString("X6"));
    }

    var refHazards = new List<string>();
    var baseHazards = new SortedSet<string>();
    int containers = 0, ore = 0, loose = 0, keys = 0, lockedDoors = 0, keyDoors = 0;
    foreach (var r in all)
    {
        var vm = Vm(r);
        if (vm is not null)
            foreach (var sc in vm.Scripts)
                if (sc.EnumerateFormLinks().Any(l => IsHazard(l.FormKey, local)))
                    refHazards.Add($"{r.FormKey.ID:X6}:{sc.Name}");
        if (r is not IPlacedObjectGetter po) continue;
        if (po.Lock is { } lk && po.Base.TryResolve(cache, out var lb) && lb is IDoorGetter)
        {
            lockedDoors++;
            if (lk.Level == LockLevel.RequiresKey) keyDoors++;
        }
        if (!po.Base.TryResolve(cache, out var bo)) continue;
        switch (bo)
        {
            case IContainerGetter: containers++; break;
            case IKeyGetter: keys++; break;
            case IWeaponGetter or IArmorGetter or IIngestibleGetter or IIngredientGetter or IMiscItemGetter
                or IBookGetter or IAmmunitionGetter or ISoulGemGetter or IScrollGetter: loose++; break;
        }
        var bvm = Vm(bo);
        if (bvm is null) continue;
        if (bvm.Scripts.Any(s => s.Name.Equals("MineOreScript", StringComparison.OrdinalIgnoreCase))) ore++;
        foreach (var sc in bvm.Scripts)
            if (sc.EnumerateFormLinks().Any(l => IsHazard(l.FormKey, local)))
                baseHazards.Add($"{bo.FormKey.ID:X6}:{bo.EditorID}:{sc.Name}");
    }

    var bossRefs = all.Where(r => r.EnumerateFormLinks().Any(l => l.FormKey == bossLcrt)).Select(r => r.FormKey.ID.ToString("X6")).ToList();
    var locKw = new List<string>();
    string? locEdid = null;
    if (c.Location.TryResolve(cache, out var loc))
    {
        locEdid = loc.EditorID;
        if (loc.Keywords is { } kws) locKw.AddRange(kws.Select(k => Edid(k.FormKey)));
    }
    float minX = float.MaxValue, minY = float.MaxValue, maxX = float.MinValue, maxY = float.MinValue, minZ = float.MaxValue, maxZ = float.MinValue;
    int tris = 0, verts = 0;
    foreach (var nm in c.NavigationMeshes)
    {
        if (nm.Data is not { } nd) continue;
        tris += nd.Triangles.Count;
        verts += nd.Vertices.Count;
        foreach (var v in nd.Vertices)
        {
            minX = Math.Min(minX, v.X); maxX = Math.Max(maxX, v.X);
            minY = Math.Min(minY, v.Y); maxY = Math.Max(maxY, v.Y);
            minZ = Math.Min(minZ, v.Z); maxZ = Math.Max(maxZ, v.Z);
        }
    }
    var bossNpcs = npcs.Where(n => n.EnumerateFormLinks().Any(l => l.FormKey == bossLcrt))
        .Select(n => $"{n.FormKey.ID:X6}:{Edid(n.Base.FormKey)}").ToList();
    var npcBases = npcs.Select(n => Edid(n.Base.FormKey)).GroupBy(x => x).Select(g => $"{g.Key}x{g.Count()}").ToList();
    var row = new
    {
        cell = c.FormKey.ID.ToString("X6"),
        edid = c.EditorID,
        loc = locEdid,
        locKw,
        cellsInLoc = c.Location.IsNull ? 0 : cellsPerLocation.GetValueOrDefault(c.Location.FormKey),
        navmeshes = c.NavigationMeshes.Count,
        refs = all.Count,
        npcs = npcs.Count,
        coc = objs.Count(o => o.Base.FormKey == coc),
        bossRefs,
        bossNpcs,
        npcBases,
        tris, verts,
        extentX = tris > 0 ? (int)(maxX - minX) : 0,
        extentY = tris > 0 ? (int)(maxY - minY) : 0,
        extentZ = tris > 0 ? (int)(maxZ - minZ) : 0,
        exteriorDoors,
        otherCellDoors = otherCellDoors.ToList(),
        refHazards,
        baseHazards = baseHazards.ToList(),
        containers, ore, loose, keys, lockedDoors, keyDoors,
        encounterZone = c.EncounterZone.IsNull ? null : Edid(c.EncounterZone.FormKey),
    };
    w.WriteLine(JsonSerializer.Serialize(row));
}
Console.WriteLine($"interior cells written to {outPath}");
