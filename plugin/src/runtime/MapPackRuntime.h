#pragma once

#include "core/MapPack.h"

namespace SkyrimPoE::Runtime::MapPackRuntime
{
    struct DungeonForms
    {
        RE::TESObjectCELL* cell = nullptr;
        RE::FormID cellId = 0;
        RE::TESObjectREFR* entryMarker = nullptr;
        RE::TESObjectREFR* bossAnchor = nullptr;
        std::vector<RE::TESObjectREFR*> packAnchors;
        RE::TESNPC* bossNpc = nullptr;
        std::vector<RE::TESNPC*> packNpcs;
        std::vector<RE::TESNPC*> addNpcs;
        std::vector<RE::FormID> returnDoors;
        // 복제 던전의 출구 자동 통로 문(목적지 없음, ESP에서 처음부터 꺼짐). 결정 177 ③ 재보완: 출구는 문이 아니라 런타임
        // 출구 구역으로 판정하고, 이 문은 꺼진 채인지만 지킨다.
        std::vector<RE::FormID> exitDoors;
    };

    void Initialize();
    [[nodiscard]] const Core::MapPack& Manifest() noexcept;
    [[nodiscard]] const std::string& UnavailableReason() noexcept;
    [[nodiscard]] const DungeonForms* FindForms(const Core::MapDungeonDef* dungeon);
    [[nodiscard]] const Core::MapDungeonDef* FindDungeonByCell(RE::FormID cellId) noexcept;
    // 검증된 팩의 모든 던전 셀(팩을 쓸 수 없으면 빈 목록)
    [[nodiscard]] std::vector<RE::FormID> DungeonCellIds();
    // 직접 여는 귀환 문이면 그 던전 셀, 아니면 0. 활성화되면 우리 귀환을 건다.
    [[nodiscard]] RE::FormID ReturnDoorCell(RE::FormID doorId) noexcept;
    // 그 던전 셀의 출구 자동 통로 문 가운데 켜져 있는 것을 끄고 끈 수를 돌려준다(셀이 붙어 있을 때 주 스레드에서).
    // 출구 문은 ESP에서 처음부터 꺼져 있으므로 보통 0이다. 켜져 있으면 엔진이 구역 경계에서 화면을 어둡게 하므로 되돌린다.
    [[nodiscard]] std::size_t DisableEnabledExitDoors(RE::FormID cellId);

    // 비호환 세이브의 귀환 판정에는 검증 실패 후에도 원본 셀을 확인할 수 있다.
    [[nodiscard]] RE::TESObjectCELL* ResolveCell(const Core::MapDungeonDef& dungeon);
    [[nodiscard]] bool OwnsCell(const RE::TESObjectCELL* cell) noexcept;
}
