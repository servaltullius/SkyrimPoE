// 복제 던전이 건드리면 안 되는 바닐라 "월드 상태" 레코드 형식. 복제(Program)와 검증(Verify)이 같은 기준을 쓴다.
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

namespace SkyrimPoE.Prototypes.DungeonClone;

internal static class WorldState
{
    // 퀘스트·위치·조우 구역: 던전 정리, 퀘스트 단계, 위치 기반 이벤트
    // 배치 참조·셀·월드스페이스·내비메쉬: 복제본 밖의 세계
    // 전역 변수·팩션: 스크립트가 값을 바꾸면 바닐라 세계에 그대로 남는다
    public static bool Is(IMajorRecordGetter target)
        => target is IQuestGetter or ILocationGetter or IEncounterZoneGetter
            or IPlacedGetter or ICellGetter or IWorldspaceGetter or INavigationMeshGetter
            or IGlobalGetter or IFactionGetter;

    // 참조 스크립트를 뗄 기준: 월드 상태에 더해 키워드를 속성으로 가진 스크립트도 뗀다.
    // 키워드 속성은 스토리 이벤트(SendStoryEvent)에 쓰인다. 예: WICommentTriggerScript는 동료가 들어오면
    // CommentKeyword로 스토리 이벤트를 보낸다(CK 소스 WICommentTriggerScript.psc). 지도 안에서는 필요 없다.
    public static bool IsScriptHazard(IMajorRecordGetter target)
        => Is(target) || target is IKeywordGetter;

    // 스크립트를 붙일 수 있는 배치 형식(NPC는 복제에서 빠지므로 제외)
    public static IVirtualMachineAdapterGetter? GetVm(IMajorRecordGetter rec) => rec switch
    {
        IPlacedObjectGetter o => o.VirtualMachineAdapter,
        IPlacedTrapGetter t => t.VirtualMachineAdapter,
        _ => null,
    };
}
