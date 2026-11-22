// 복제 도구 통합 시험: 메모리에 만든 작은 원본(이름 Skyrim.esm)으로 Builder와 원본 대조 검증을 돌린다(게임 데이터 불필요).
// - 두 판의 번호 보존(6단계 재작업 계획 R1 2번): 원본에서 참조를 빼고 더해도 남은 참조·앵커의 번호가 그대로인지
// - 원본 대조 검증이 만드는 코드의 결함을 잡는지: 위치 변경, 연결 필드 바뀜, 뺐어야 할 참조가 남음
using Mutagen.Bethesda;
using System.Text.Json;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Noggog;
using SkyrimPoE.Tools.DungeonClone;
using Xunit;

namespace DungeonClone.Tests;

public class BuilderTests
{
    private static readonly ModKey Esm = ModKey.FromFileName("Skyrim.esm");
    private static FormKey K(uint id) => new(Esm, id);
    private const SkyrimRelease Rel = SkyrimRelease.SkyrimSE;

    private const uint CellA = 0x000200, CellB = 0x000300;
    private const uint Coc = 0x000210, RockA = 0x000211, RockB = 0x000212, Chest = 0x000213, DoorA = 0x000214, RockC = 0x000216;
    private const uint NpcBoss = 0x000220, NpcP1 = 0x000221, NpcP2 = 0x000222, NpcNear = 0x000223, Nav = 0x000230, DoorB = 0x000310;
    private const uint RockNew = 0x000240;

    // removeRockB: 2판에서 RockB를 빼고 RockNew를 더한다.
    private static SkyrimMod MakeSource(bool secondVersion)
    {
        var mod = new SkyrimMod(Esm, Rel);
        mod.LocationReferenceTypes.Add(new LocationReferenceType(K(0x0130F7), Rel) { EditorID = "Boss" });
        mod.Statics.Add(new Static(K(0x000032), Rel) { EditorID = "COCMarkerHeading" });
        mod.Statics.Add(new Static(K(0x000034), Rel) { EditorID = "XMarkerHeading" });
        mod.Statics.Add(new Static(K(0x000100), Rel) { EditorID = "Rock" });
        mod.Containers.Add(new Container(K(0x000101), Rel) { EditorID = "Chest" });
        mod.Doors.Add(new Door(K(0x000102), Rel) { EditorID = "DoorBase" });
        mod.Npcs.Add(new Npc(K(0x000103), Rel) { EditorID = "Bandit" });
        mod.Keywords.Add(new Keyword(K(0x000104), Rel) { EditorID = "LinkKw" });

        PlacedObject Obj(uint id, uint baseId, float x, float y) => new(K(id), Rel)
        {
            Base = new FormLinkNullable<IPlaceableObjectGetter>(K(baseId)),
            Placement = new Placement { Position = new P3Float(x, y, 0), Rotation = new P3Float(0, 0, 0) },
        };
        PlacedNpc Npc(uint id, float x, float y, bool boss)
        {
            var n = new PlacedNpc(K(id), Rel)
            {
                Base = new FormLinkNullable<INpcGetter>(K(0x000103)),
                Placement = new Placement { Position = new P3Float(x, y, 0), Rotation = new P3Float(0, 0, 1) },
            };
            if (boss) n.LocationRefTypes = new ExtendedList<IFormLinkGetter<ILocationReferenceTypeGetter>> { new FormLink<ILocationReferenceTypeGetter>(K(0x0130F7)) };
            return n;
        }

        var cellA = new Cell(K(CellA), Rel) { EditorID = "TestCellA", Flags = Cell.Flag.IsInteriorCell };
        var rockA = Obj(RockA, 0x000100, 100, 100);
        // RockA → RockC 연결 참조(필드 바뀜 시험용)
        rockA.LinkedReferences.Add(new LinkedReferences
        {
            KeywordOrReference = new FormLink<IKeywordLinkedReferenceGetter>(K(0x000104)),
            Reference = new FormLink<IPlacedGetter>(K(RockC)),
        });
        cellA.Temporary.Add(Obj(Coc, 0x000032, 0, 0));
        cellA.Temporary.Add(rockA);
        if (!secondVersion) cellA.Temporary.Add(Obj(RockB, 0x000100, 200, 200));
        cellA.Temporary.Add(Obj(RockC, 0x000100, 300, 300));
        if (secondVersion) cellA.Temporary.Add(Obj(RockNew, 0x000100, 400, 400));
        cellA.Temporary.Add(Obj(Chest, 0x000101, 500, 0));
        var doorA = Obj(DoorA, 0x000102, -50, 0);
        doorA.TeleportDestination = new TeleportDestination { Door = new FormLink<IPlacedObjectGetter>(K(DoorB)) };
        cellA.Temporary.Add(doorA);
        cellA.Temporary.Add(Npc(NpcBoss, 2500, 0, boss: true));
        cellA.Temporary.Add(Npc(NpcP1, 1500, 600, boss: false));
        cellA.Temporary.Add(Npc(NpcP2, 1500, -600, boss: false));
        cellA.Temporary.Add(Npc(NpcNear, 100, -100, boss: false));
        cellA.NavigationMeshes.Add(new NavigationMesh(K(Nav), Rel)
        {
            Data = new NavigationMeshData
            {
                Parent = new CellNavmeshParent { Parent = new FormLink<ICellGetter>(K(CellA)) },
                Vertices = new ExtendedList<P3Float> { new(-200, -1000, 0), new(3000, -1000, 0), new(3000, 1000, 0), new(-200, 1000, 0) },
                Triangles = new ExtendedList<NavmeshTriangle>
                {
                    new() { Vertices = new P3Int16(0, 1, 2) },
                    new() { Vertices = new P3Int16(0, 2, 3) },
                },
            },
        });

        var cellB = new Cell(K(CellB), Rel) { EditorID = "TestCellB", Flags = Cell.Flag.IsInteriorCell };
        var doorB = Obj(DoorB, 0x000102, 0, 0);
        doorB.TeleportDestination = new TeleportDestination { Door = new FormLink<IPlacedObjectGetter>(K(DoorA)) };
        cellB.Temporary.Add(doorB);

        mod.Cells.Records.Add(new CellBlock
        {
            BlockNumber = 0,
            GroupType = GroupTypeEnum.InteriorCellBlock,
            SubBlocks = { new CellSubBlock { BlockNumber = 0, GroupType = GroupTypeEnum.InteriorCellSubBlock, Cells = { cellA, cellB } } },
        });
        return mod;
    }

    private static PackSpec Pack(string sourceCell = "TestCellA") => new()
    {
        Plugin = "SkyrimPoE_Maps.esp",
        PackVersion = 1,
        EditorIdPrefix = "SPOE_Map_",
        AnchorMinDistanceFromEntry = 600,
        AnchorMaxSnapDistance = 256,
        AnchorMinSeparation = 128,
        Dungeons = { new DungeonSpec
        {
            Key = "test", Type = "cave", Role = "map", Name = "시험", SourceCell = sourceCell, MinPackAnchors = 2,
            BossNpc = "000103", PackNpcs = ["000103"],
        } },
    };

    [Theory]
    [InlineData("map", "", "000103", null)]
    [InlineData("map", "000103", null, null)]
    [InlineData("arena", "000103", null, null)]
    [InlineData("map", "000100", "000103", null)]
    [InlineData("map", "000103", "000220", null)]
    [InlineData("arena", "000103", null, "FFFFFF")]
    public void MissingOrInvalidSpawnTemplatesFail(string role, string boss, string? packNpc, string? addNpc)
    {
        var pack = Pack();
        var dungeon = pack.Dungeons[0];
        dungeon.Role = role;
        dungeon.BossNpc = boss;
        dungeon.PackNpcs = packNpc is null ? null : [packNpc];
        dungeon.AddNpcs = addNpc is null ? null : [addNpc];
        var source = SourceIndex.Create(MakeSource(false));
        Assert.Throws<InvalidDataException>(() => Builder.Build(pack, source, new Ledger(), allowLedgerChanges: true));
    }

    [Theory]
    [InlineData("map", "packNpcs", "addNpcs")]
    [InlineData("arena", "addNpcs", "packNpcs")]
    public void FormatThreeWritesRoleSpecificSpawnTemplatesAndNavmeshHeight(string role, string presentList, string absentList)
    {
        var pack = Pack();
        var dungeon = pack.Dungeons[0];
        dungeon.Role = role;
        if (role == "arena")
        {
            dungeon.AddNpcs = dungeon.PackNpcs;
            dungeon.PackNpcs = null;
        }
        var source = SourceIndex.Create(MakeSource(false));
        var result = Builder.Build(pack, source, new Ledger(), allowLedgerChanges: true);
        using var output = JsonDocument.Parse(JsonSerializer.Serialize(result.Manifest, Json.Options));
        Assert.Equal(3, output.RootElement.GetProperty("manifestFormat").GetInt32());
        var entry = output.RootElement.GetProperty("dungeons")[0];
        Assert.Equal(0f, entry.GetProperty("navmeshMinZ").GetSingle());
        Assert.Equal("000103", entry.GetProperty("bossNpc").GetString());
        Assert.Equal("000103", entry.GetProperty(presentList)[0].GetString());
        Assert.False(entry.TryGetProperty(absentList, out _));
    }

    [Fact]
    public void NavmeshHeightUsesOnlyWalkableTriangleVerticesAcrossMeshes()
    {
        var mod = MakeSource(false);
        var cell = mod.Cells.Records.Single().SubBlocks.Single().Cells.First();
        var nav = cell.NavigationMeshes.Single().Data!;
        nav.Vertices.Add(new P3Float(1400, 200, -500));
        nav.Vertices.Add(new P3Float(1400, 300, -600));
        nav.Vertices.Add(new P3Float(1400, 400, -700)); // 삼각형에서 참조하지 않는 꼭짓점
        nav.Triangles.Add(new NavmeshTriangle { Vertices = new P3Int16(0, 1, 4), Flags = NavmeshTriangle.Flag.Deleted });
        nav.Triangles.Add(new NavmeshTriangle { Vertices = new P3Int16(0, 1, 5), Flags = NavmeshTriangle.Flag.Water });
        cell.NavigationMeshes.Add(new NavigationMesh(K(Nav + 1), Rel)
        {
            Data = new NavigationMeshData
            {
                Parent = new CellNavmeshParent { Parent = new FormLink<ICellGetter>(K(CellA)) },
                Vertices = new ExtendedList<P3Float> { new(100, 100, -10), new(200, 100, -17.5f), new(100, 200, -12) },
                Triangles = new ExtendedList<NavmeshTriangle> { new() { Vertices = new P3Int16(0, 1, 2) } },
            },
        });

        var result = Builder.Build(Pack(), SourceIndex.Create(mod), new Ledger(), allowLedgerChanges: true);
        Assert.Equal(-17.5f, result.Manifest.Dungeons.Single().NavmeshMinZ);
    }

    [Fact]
    public void NoWalkableTrianglesFailTheBuild()
    {
        var mod = MakeSource(false);
        var nav = mod.Cells.Records.Single().SubBlocks.Single().Cells.First().NavigationMeshes.Single().Data!;
        nav.Triangles[0].Flags = NavmeshTriangle.Flag.Deleted;
        nav.Triangles[1].Flags = NavmeshTriangle.Flag.Water;

        var error = Assert.Throws<InvalidDataException>(() =>
            Builder.Build(Pack(), SourceIndex.Create(mod), new Ledger(), allowLedgerChanges: true));
        Assert.Contains("걸을 수 있는 내비메쉬 삼각형", error.Message);
    }

    private static Dictionary<string, uint> IdsByKey(Ledger l) => l.Slots[0].Ids.ToDictionary(kv => kv.Key, kv => Ledger.ParseId(kv.Value));

    [Fact]
    public void SecondVersionKeepsSurvivingIdsAndDoesNotReuse()
    {
        var ledger = new Ledger();
        var v1 = Builder.Build(Pack(), SourceIndex.Create(MakeSource(false)), ledger, allowLedgerChanges: true);
        var ids1 = IdsByKey(ledger);
        var out1 = v1.Mod.EnumerateMajorRecords().Select(r => r.FormKey.ID).ToHashSet();

        var v2 = Builder.Build(Pack(), SourceIndex.Create(MakeSource(true)), ledger, allowLedgerChanges: true);
        var ids2 = IdsByKey(ledger);
        var out2 = v2.Mod.EnumerateMajorRecords().Select(r => r.FormKey.ID).ToHashSet();

        // 이전 판의 키는 모두 같은 번호
        foreach (var (k, v) in ids1) Assert.Equal(v, ids2[k]);
        // 빠진 RockB의 번호는 2판 출력에 없고, 새 RockNew는 새 번호를 받음
        var rockB = ids1[$"Skyrim.esm:{RockB:X6}:ref"];
        Assert.Contains(rockB, out1);
        Assert.DoesNotContain(rockB, out2);
        var rockNew = ids2[$"Skyrim.esm:{RockNew:X6}:ref"];
        Assert.True(rockNew > ids1.Values.Max());
        // 앵커 번호도 그대로
        Assert.Equal(ids1[$"Skyrim.esm:{NpcBoss:X6}:anchor"], ids2[$"Skyrim.esm:{NpcBoss:X6}:anchor"]);
        // 같은 입력으로 다시 만들면 장부가 늘지 않음
        var before = ledger.Slots[0].Next;
        Builder.Build(Pack(), SourceIndex.Create(MakeSource(true)), ledger, allowLedgerChanges: false);
        Assert.Equal(before, ledger.Slots[0].Next);
    }

    [Fact]
    public void BuildAppliesPolicy()
    {
        var ledger = new Ledger();
        var r = Builder.Build(Pack(), SourceIndex.Create(MakeSource(false)), ledger, true);
        var dm = r.Manifest.Dungeons.Single();
        var cell = r.Mod.Cells.Records.SelectMany(b => b.SubBlocks).SelectMany(s => s.Cells).Single();
        var placed = cell.Persistent.Concat(cell.Temporary).OfType<IPlacedObjectGetter>().ToList();
        Assert.DoesNotContain(placed, p => p.Base.FormKey == K(0x000101));               // 상자 뺌
        Assert.Empty(cell.Persistent.Concat(cell.Temporary).OfType<IPlacedNpcGetter>()); // NPC 뺌
        Assert.Single(dm.SealedDoors);                                                    // 다른 실내 칸으로 가던 문
        Assert.Equal(2, dm.PackAnchors.Count);                                            // 입구 근처 NPC는 앵커에서 뺌
        Assert.Contains(r.Reports[0].SkippedAnchorsNearEntry, s => s.StartsWith($"{NpcNear:X6}"));
    }

    private static (List<string> Fails, SkyrimMod Mod, PackManifest Manifest, Ledger Ledger, SourceIndex Src) BuildAndCheck(Action<SkyrimMod, Ledger>? mutate = null)
    {
        var src = SourceIndex.Create(MakeSource(false));
        var ledger = new Ledger();
        var r = Builder.Build(Pack(), src, ledger, true);
        mutate?.Invoke(r.Mod, ledger);
        var fails = new List<string>();
        Verify.CheckAgainstSource(Pack(), src, ledger, r.Mod, r.Manifest, fails, new List<string>());
        return (fails, r.Mod, r.Manifest, ledger, src);
    }

    private static PlacedObject OutRef(SkyrimMod mod, Ledger l, uint srcId)
    {
        var id = Ledger.ParseId(l.Slots[0].Ids[$"Skyrim.esm:{srcId:X6}:ref"]);
        var cell = mod.Cells.Records.SelectMany(b => b.SubBlocks).SelectMany(s => s.Cells).Single();
        return cell.Persistent.Concat(cell.Temporary).OfType<PlacedObject>().Single(p => p.FormKey.ID == id);
    }

    [Fact]
    public void SourceCheckPassesOnCleanBuild()
    {
        var (fails, _, _, _, _) = BuildAndCheck();
        Assert.Empty(fails);
    }

    [Fact]
    public void SourceCheckCatchesMovedReference()
    {
        var (fails, _, _, _, _) = BuildAndCheck((mod, l) =>
        {
            var p = OutRef(mod, l, RockA);
            p.Placement = new Placement { Position = new P3Float(101, 100, 0), Rotation = p.Placement!.Rotation };
        });
        Assert.Contains(fails, f => f.Contains("위치·회전이 원본과 다릅니다"));
    }

    [Fact]
    public void SourceCheckCatchesSwappedLinkField()
    {
        var (fails, _, _, _, _) = BuildAndCheck((mod, l) =>
        {
            var p = OutRef(mod, l, RockA);
            var rockB = OutRef(mod, l, RockB);
            p.LinkedReferences[0].Reference = new FormLink<IPlacedGetter>(rockB.FormKey);
        });
        Assert.Contains(fails, f => f.Contains("연결 참조가 원본을 옮긴 것과 다릅니다"));
    }

    [Fact]
    public void SourceCheckCatchesKeptLoot()
    {
        var (fails, _, _, _, _) = BuildAndCheck((mod, l) =>
        {
            // 만드는 코드가 상자를 빼지 않은 경우를 흉내: 장부 번호를 주고 복제본을 넣는다.
            var slot = l.Slots[0];
            var id = l.Assign(slot, $"Skyrim.esm:{Chest:X6}:ref", true);
            var cell = mod.Cells.Records.SelectMany(b => b.SubBlocks).SelectMany(s => s.Cells).Single();
            cell.Temporary.Add(new PlacedObject(new FormKey(mod.ModKey, id), Rel)
            {
                Base = new FormLinkNullable<IPlaceableObjectGetter>(K(0x000101)),
                Placement = new Placement { Position = new P3Float(500, 0, 0), Rotation = new P3Float(0, 0, 0) },
            });
        });
        Assert.Contains(fails, f => f.Contains("뺐어야 하는데 남았습니다"));
    }

    [Fact]
    public void SourceCheckCatchesAnchorFarFromNpc()
    {
        var (fails, _, _, _, _) = BuildAndCheck((mod, l) =>
        {
            var id = Ledger.ParseId(l.Slots[0].Ids[$"Skyrim.esm:{NpcP1:X6}:anchor"]);
            var cell = mod.Cells.Records.SelectMany(b => b.SubBlocks).SelectMany(s => s.Cells).Single();
            var a = cell.Persistent.OfType<PlacedObject>().Single(p => p.FormKey.ID == id);
            a.Placement = new Placement { Position = new P3Float(1500, 0, 0), Rotation = a.Placement!.Rotation };
        });
        Assert.Contains(fails, f => f.Contains("원본 NPC 자리에서"));
    }

    [Fact]
    public void ChangingSourceCellForKeyFails()
    {
        var ledger = new Ledger();
        Builder.Build(Pack(), SourceIndex.Create(MakeSource(false)), ledger, true);
        Assert.Throws<InvalidDataException>(() => Builder.Build(Pack("TestCellB"), SourceIndex.Create(MakeSource(false)), ledger, true));
    }

    // 생성기가 받은 목록은 런타임 목록 파서(plugin/src/core/MapPack.cpp)도 받아야 한다:
    // 플러그인 이름 고정, 지도 하나 이상, 결투장 정확히 하나.
    [Theory]
    [InlineData("SkyrimPoE_Maps.esp", true, true, true)]
    [InlineData("SkyrimPoE_Maps.esp", true, false, false)]
    [InlineData("SkyrimPoE_Maps.esp", false, true, false)]
    [InlineData("Other.esp", true, true, false)]
    [InlineData("skyrimpoe_maps.esp", true, true, false)]
    public void PackSpecRolesAndPluginMatchRuntime(string plugin, bool withMap, bool withArena, bool valid)
    {
        static object Dungeon(string key, string role, string cell) => new
        {
            key, type = "cave", role, name = key, sourceCell = cell, minPackAnchors = 1, bossNpc = "000103",
            packNpcs = role == "map" ? new[] { "000103" } : null,
            addNpcs = role == "arena" ? new[] { "000103" } : null,
        };
        var dungeons = new List<object>();
        if (withMap) dungeons.Add(Dungeon("map_a", "map", "CellA"));
        if (withArena) dungeons.Add(Dungeon("arena_a", "arena", "CellB"));
        var spec = new
        {
            plugin, packVersion = 1, editorIdPrefix = "SPOE_Map_", anchorMinDistanceFromEntry = 600,
            anchorMaxSnapDistance = 256, anchorMinSeparation = 128, dungeons,
        };
        var path = Path.Combine(Path.GetTempPath(), $"dungeonclone-spec-{Guid.NewGuid():N}.json");
        File.WriteAllText(path, JsonSerializer.Serialize(spec, new JsonSerializerOptions
        {
            DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,
        }));
        try
        {
            if (valid) Assert.Equal(plugin, PackSpec.Load(path).Plugin);
            else Assert.Throws<InvalidDataException>(() => PackSpec.Load(path));
        }
        finally
        {
            File.Delete(path);
        }
    }

    // 빌드 산출물은 저장소 밖에 있으므로 이 소스 파일의 위치에서 tools/dungeon-clone/dungeons.json을 찾는다.
    private static string ShippedDungeonList([System.Runtime.CompilerServices.CallerFilePath] string thisFile = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(thisFile)!, "..", "dungeons.json"));

    [Fact]
    public void ShippedDungeonListLoads()
    {
        var spec = PackSpec.Load(ShippedDungeonList());
        Assert.Single(spec.Dungeons, d => d.Role == "arena");
    }

    [Fact]
    public void NavmeshMath()
    {
        var nav = new NavigationMesh(K(Nav), Rel)
        {
            Data = new NavigationMeshData
            {
                Vertices = new ExtendedList<P3Float> { new(0, 0, 0), new(100, 0, 0), new(0, 100, 0), new(0, 0, 0) },
                Triangles = new ExtendedList<NavmeshTriangle>
                {
                    new() { Vertices = new P3Int16(0, 1, 2) },
                    new() { Vertices = new P3Int16(0, 3, 1) }, // 넓이 0(퇴화) — 후보에서 빠져야 함
                },
            },
        };
        var navs = new List<INavigationMeshGetter> { nav };
        Assert.True(Builder.IsOnNavmesh(new P3Float(10, 10, 40), navs));
        Assert.False(Builder.IsOnNavmesh(new P3Float(10, 10, 60), navs));
        var (pos, moved) = Builder.SnapToNavmesh(new P3Float(200, 50, 0), navs);
        Assert.Equal(new P3Float(100, 0, 0), pos);
        Assert.True(Math.Abs(moved - Math.Sqrt(100 * 100 + 50 * 50)) < 0.01);
        var (inside, m0) = Builder.SnapToNavmesh(new P3Float(10, 10, 0), navs);
        Assert.Equal(0, m0);
        Assert.Equal(new P3Float(10, 10, 0), inside);
    }
}
