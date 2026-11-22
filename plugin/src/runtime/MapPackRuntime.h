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
    };

    void Initialize();
    [[nodiscard]] const Core::MapPack& Manifest() noexcept;
    [[nodiscard]] const std::string& UnavailableReason() noexcept;
    [[nodiscard]] const DungeonForms* FindForms(const Core::MapDungeonDef* dungeon);
    [[nodiscard]] RE::FormID ReturnDoorCell(RE::FormID doorId) noexcept;

    // 비호환 세이브의 귀환 판정에는 검증 실패 후에도 원본 셀을 확인할 수 있다.
    [[nodiscard]] RE::TESObjectCELL* ResolveCell(const Core::MapDungeonDef& dungeon);
    [[nodiscard]] bool OwnsCell(const RE::TESObjectCELL* cell) noexcept;
}
