#pragma once

#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/Types.h"

#include <random>
#include <string>
#include <unordered_set>

namespace SkyrimPoE::Core
{
    enum class CraftResultCode : std::uint8_t
    {
        Ok = 0,
        InvalidRarity = 1,
        AffixLimitReached = 2,
        NoEligibleAffix = 3,
        NoOp = 4,
        UnsupportedOrb = 5,
        UnsupportedTarget = 6,
        AlreadyMaxSockets = 7
    };

    struct CraftOutcome
    {
        CraftResultCode code = CraftResultCode::Ok;
        std::string message;

        [[nodiscard]] bool IsOk() const noexcept { return code == CraftResultCode::Ok; }
    };

    // 결정 157, 158, 검수 M1/M5:
    // 순수 상태 머신(주입 가능 RNG)으로 5티어 봉인 토큰 감정, 12종 화폐 오브 제작, 젬 소켓 확장을 수행한다.
    class ItemCrafting
    {
    public:
        // 봉인 토큰(무상태 5티어 × 부위 × 매직/희귀)을 감정해 새 PoEItemRecord를 생성한다.
        // iLvl은 해당 티어 구간 [tierMin, tierMax] 내부로 고정되어 저레벨 토큰 고레벨 세탁을 차단한다(결정 157).
        [[nodiscard]] static PoEItemRecord IdentifyToken(
            std::uint8_t tier,
            ItemRarity rarity,
            TokenSlotGroup slotGroup,
            DefenseSubtype defenseSubtype,
            std::string_view weaponTag,
            bool isAmulet,
            std::uint32_t baseFormId,
            std::uint32_t exileLevel,
            const ItemData& data,
            std::mt19937_64& rng,
            std::string_view implicitAffixId = {},
            std::uint8_t jewelColor = 0);

        // 5단계(결정 164): PoE1 고유 아이템 16종 중 1종을 스펙대로 생성한다(임플리싯·명시 어픽스 롤 포함).
        [[nodiscard]] static PoEItemRecord RollUniqueItem(
            const UniqueDefinition& def,
            std::uint32_t resolvedBaseFormId,
            std::uint32_t itemLevel,
            const ItemData& data,
            std::mt19937_64& rng);

        // 5단계(결정 162): 벨트 베이스의 고정 임플리싯 어픽스를 1회 추첨한다.
        [[nodiscard]] static std::vector<RolledAffix> RollBeltImplicits(
            const BeltBaseDef& belt,
            const ItemData& data,
            std::mt19937_64& rng);

        // 5단계(결정 162): 장착 중인 벨트의 티어/희귀도/Stygian/고유 여부에 따라 개방 참 슬롯 수(0~3)를 계산한다.
        [[nodiscard]] static std::uint8_t ComputeBeltCharmSlots(
            bool beltEquipped,
            const PoEItemRecord* beltRec,
            bool isStygianVise) noexcept;

        // 장비 대상 제작 오브 9종 적용(Transmutation, Augmentation, Alteration, Regal, Alchemy, Chaos, Exalted, Scouring, Divine).
        // 실패(code != Ok) 시 item은 수정되지 않으며 호출자는 오브를 차감하지 않는다.
        [[nodiscard]] static CraftOutcome ApplyOrb(
            CurrencyKind orb,
            PoEItemRecord& item,
            const std::unordered_set<std::string>& itemTags,
            const ItemData& data,
            std::mt19937_64& rng);

        // 현재 item에 해당 orb를 적용 가능한지 사전 판정(UI 버튼 활성/비활성 표시용).
        [[nodiscard]] static CraftOutcome CanApplyOrb(
            CurrencyKind orb,
            const PoEItemRecord& item,
            const std::unordered_set<std::string>& itemTags,
            const ItemData& data);

        // 쥬얼러의 오브: 스킬·발동 젬 및 발동 젬 내부 주문 젬의 보조 소켓을 +1 확정 확장(최대 5).
        // 보조 젬(Support)은 거부하고, 5소켓 도달 젬은 AlreadyMaxSockets로 거부해 낭비를 방지한다(검수 M5).
        [[nodiscard]] static CraftOutcome ApplyJewellerToGem(
            GemInstance& gem,
            GemKind gemKind);
    };
}
