// 복제 규칙: 무엇을 빼고, 어떤 스크립트를 떼고, 무엇을 남기는지. 복제(Builder)와 검증(Verify)이 쓴다.
// 근거: 결정 169(전용 복제 던전), 결정 170 U-5(바닐라 전리품·열쇠·상자·광맥 제거, 안쪽 문 잠금 해제, 보상은 우리 드랍만),
//       6단계 재작업 계획 R1의 4·5번.
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

namespace SkyrimPoE.Tools.DungeonClone;

internal enum RemovalReason
{
    None,
    Npc,
    Loot,
    Key,
    Container,
    Ore,
    // 채집해 재료·물건을 주는 식물·나무(Flora, 수확물이 있는 Tree)
    Harvestable,
    // 들고 갈 수 있는 광원(바닥 물건과 같음)
    CarryableLight,
    // 기본 객체의 스크립트가 허용 범위를 넘는 대상을 가리킴
    HazardBaseScript,
    // 참조에 붙은 위험 스크립트를 떼면 같은 이름의 기본 스크립트가 빈 속성으로 남는 트리거(예: 퀘스트 단계 트리거)
    QuestTrigger,
}

[Flags]
internal enum HazardKind
{
    None = 0,
    Keyword = 1,
    Global = 2,
    Faction = 4,
    // 퀘스트·위치·조우 구역·복제본 밖의 참조·셀·월드스페이스·내비메쉬, 찾을 수 없는 대상. 어떤 스크립트에도 허용하지 않는다.
    Forbidden = 8,
}

internal static class Policy
{
    public static readonly ModKey SourceMaster = ModKey.FromFileName("Skyrim.esm");
    public static readonly FormKey CocMarkerHeading = FormKey.Factory("000032:Skyrim.esm");
    public static readonly FormKey XMarkerHeading = FormKey.Factory("000034:Skyrim.esm");
    // 위치 참조 유형 "Boss"(LCRT). SourceIndex가 실행할 때 EditorID가 Boss인지 다시 확인한다.
    public static readonly FormKey BossLocRefType = FormKey.Factory("0130F7:Skyrim.esm");
    public const int PersistentFlag = 0x400;
    // 배치 참조의 "처음부터 꺼짐"(Initially Disabled). 결정 177 ③ 재보완: 자동 출구 문은 엔진이 쓰지 않게 꺼 둔다.
    public const int InitiallyDisabledFlag = 0x800;
    // 엔진 자동 문 발동 거리(GMST). 런타임 출구 구역의 반경으로 목록 파일에 내보낸다(결정 177 ③ 재보완).
    public const string AutoDoorActivateDistanceSetting = "fAutoDoorActivateDistance";
    // 런타임(plugin/src/core/MapPack.h kMaxExitZoneRadius)이 받는 출구 구역 반경 상한(유닛)
    public const float MaxExitZoneRadius = 10000f;

    // 바닐라 월드 상태: 복제본이 가리키면 안 되는 레코드 형식(검증의 바깥 연결 검사에도 쓴다).
    public static bool IsWorldState(IMajorRecordGetter target)
        => target is IQuestGetter or ILocationGetter or IEncounterZoneGetter
            or IPlacedGetter or ICellGetter or IWorldspaceGetter or INavigationMeshGetter
            or IGlobalGetter or IFactionGetter;

    public static HazardKind KindOf(IMajorRecordGetter target) => target switch
    {
        IKeywordGetter => HazardKind.Keyword,
        IGlobalGetter => HazardKind.Global,
        IFactionGetter => HazardKind.Faction,
        _ when IsWorldState(target) => HazardKind.Forbidden,
        _ => HazardKind.None,
    };

    // 일반 장치 스크립트 허용 목록: 스크립트마다 "읽기만 하는" 대상 형식만 허용한다. 근거는 CK Scripts.zip의 원본(.psc).
    // - TrapTriggerBase 계열: owningFaction은 IsInFaction으로, LightFoot·LightHands 전역은 getValue로 읽기만 함(TrapTriggerBase.psc 293–294, 331–352).
    //   PressurePlate·TrapFirePlate·TrapLever·TrapTriggerTimeInterval은 TrapTriggerBase를 잇고, TrapOilPool은 폭발물을 PlaceAtMe로 만든다.
    // - doorBar: 키워드(BarredDoorNavCutKeyword)와 메시지만 가짐.
    // - default2StateActivator: 키워드(TwoStateCollisionKeyword)만 가짐.
    // - defaultActivateSelf: PlayerFaction·CurrentFollowerFaction을 GetFactionReaction·IsInFaction으로 읽기만 함(defaultActivateSelf.psc 50–57).
    // 무기 거치대(WeaponRackActivateSCRIPT)는 바닐라 전역 WRackGlobal에 SetValue(1)을 하므로 넣지 않는다(WeaponRackActivateSCRIPT.psc 178).
    public static readonly IReadOnlyDictionary<string, HazardKind> AllowedDeviceScripts =
        new Dictionary<string, HazardKind>(StringComparer.OrdinalIgnoreCase)
        {
            ["TrapTriggerBase"] = HazardKind.Keyword | HazardKind.Global | HazardKind.Faction,
            ["PressurePlate"] = HazardKind.Keyword | HazardKind.Global | HazardKind.Faction,
            ["TrapFirePlate"] = HazardKind.Keyword | HazardKind.Global | HazardKind.Faction,
            ["TrapLever"] = HazardKind.Keyword | HazardKind.Global | HazardKind.Faction,
            ["TrapTriggerTimeInterval"] = HazardKind.Keyword | HazardKind.Global | HazardKind.Faction,
            ["TrapOilPool"] = HazardKind.Keyword | HazardKind.Global | HazardKind.Faction,
            ["doorBar"] = HazardKind.Keyword,
            ["default2StateActivator"] = HazardKind.Keyword,
            ["defaultActivateSelf"] = HazardKind.Faction,
        };

    // 물건을 주는 기본 스크립트 가운데 일부러 남기는 것. 결정 171(결정 170 U-5의 예외, 2026-11-22 사용자 확정).
    // RemovableTorchSconce01SCRIPT: 촛대가 횃불(RemovableTorch01)을 놓고, 활성화하면 Torch01을 준다(.psc 32, 68).
    // 횃불 촛대는 던전 조명의 상당 부분이라, 빼면 셀이 어두워진다.
    public static readonly IReadOnlySet<string> KeptItemGivers = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
    {
        "RemovableTorchSconce01SCRIPT",
    };

    // 광맥과 그 곡괭이 자리(결정 170 U-5)
    private static readonly IReadOnlySet<string> OreScripts = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
    {
        "MineOreScript",
        "MineOreFurnitureScript",
    };

    public static IVirtualMachineAdapterGetter? GetVm(object? rec) => rec switch
    {
        IPlacedObjectGetter p => p.VirtualMachineAdapter,
        IPlacedTrapGetter t => t.VirtualMachineAdapter,
        IActivatorGetter a => a.VirtualMachineAdapter,
        ITalkingActivatorGetter ta => ta.VirtualMachineAdapter,
        IContainerGetter c => c.VirtualMachineAdapter,
        IDoorGetter d => d.VirtualMachineAdapter,
        IFurnitureGetter f => f.VirtualMachineAdapter,
        IFloraGetter fl => fl.VirtualMachineAdapter,
        ITreeGetter tr => tr.VirtualMachineAdapter,
        IMiscItemGetter m => m.VirtualMachineAdapter,
        ILightGetter l => l.VirtualMachineAdapter,
        _ => null,
    };

    // 스크립트 속성이 가리키는 대상의 위험 종류 합(복제본 안의 대상과 빈 연결은 뺀다)
    public static HazardKind HazardsOf(IScriptEntryGetter script, Func<FormKey, bool> isInsideCopy, ILinkCacheLike cache)
    {
        var kinds = HazardKind.None;
        foreach (var l in script.EnumerateFormLinks())
        {
            if (l.FormKey.IsNull || isInsideCopy(l.FormKey)) continue;
            kinds |= cache.TryResolve(l.FormKey, out var t) ? KindOf(t) : HazardKind.Forbidden;
        }
        return kinds;
    }

    // 스크립트를 남겨도 되는지: 허용 목록 스크립트는 그 스크립트에 허용한 대상만, 나머지는 위험 대상이 하나도 없어야 한다.
    public static bool IsScriptAllowed(IScriptEntryGetter script, Func<FormKey, bool> isInsideCopy, ILinkCacheLike cache)
    {
        var hazards = HazardsOf(script, isInsideCopy, cache);
        if (hazards == HazardKind.None) return true;
        return AllowedDeviceScripts.TryGetValue(script.Name, out var allowed) && (hazards & ~allowed) == HazardKind.None;
    }

    // 원본 셀의 자식 하나를 기본 객체 기준으로 빼야 하는지(참조 스크립트로 정하는 QuestTrigger는 Builder가 정한다).
    public static RemovalReason Classify(IMajorRecordGetter child, Func<FormKey, bool> isInsideSource, ILinkCacheLike cache)
    {
        if (child is IPlacedNpcGetter) return RemovalReason.Npc;
        if (child is not IPlacedObjectGetter po) return RemovalReason.None;
        if (po.Base.IsNull || !cache.TryResolve(po.Base.FormKey, out var baseRec)) return RemovalReason.None;
        switch (baseRec)
        {
            case IKeyGetter:
                return RemovalReason.Key;
            case IWeaponGetter or IArmorGetter or IIngestibleGetter or IIngredientGetter or IMiscItemGetter
                or IBookGetter or IAmmunitionGetter or ISoulGemGetter or IScrollGetter or ILeveledItemGetter:
                return RemovalReason.Loot;
            case IContainerGetter:
                return RemovalReason.Container;
            case IFloraGetter fl when !fl.Ingredient.IsNull:
                return RemovalReason.Harvestable;
            case ITreeGetter tr when !tr.Ingredient.IsNull:
                return RemovalReason.Harvestable;
            case ILightGetter li when li.Flags.HasFlag(Light.Flag.CanBeCarried):
                return RemovalReason.CarryableLight;
        }
        if (GetVm(baseRec) is { } bvm)
        {
            if (bvm.Scripts.Any(s => OreScripts.Contains(s.Name))) return RemovalReason.Ore;
            if (bvm.Scripts.Any(s => !KeptItemGivers.Contains(s.Name) && !IsScriptAllowed(s, isInsideSource, cache)))
            {
                return RemovalReason.HazardBaseScript;
            }
        }
        return RemovalReason.None;
    }
}

// 링크 캐시에서 쓰는 조회만 추린 것
internal interface ILinkCacheLike
{
    bool TryResolve(FormKey key, out IMajorRecordGetter target);
}

internal sealed class LinkCacheAdapter : ILinkCacheLike
{
    private readonly Mutagen.Bethesda.Plugins.Cache.ILinkCache _cache;
    public LinkCacheAdapter(Mutagen.Bethesda.Plugins.Cache.ILinkCache cache) => _cache = cache;

    public bool TryResolve(FormKey key, out IMajorRecordGetter target)
    {
        if (_cache.TryResolve<IMajorRecordGetter>(key, out var r))
        {
            target = r;
            return true;
        }
        target = null!;
        return false;
    }
}
