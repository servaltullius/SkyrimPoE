#include "test_harness.h"

#include "core/EspFormCheck.h"

#include <array>

using namespace SkyrimPoE::Core;

POE_TEST("ESP form check: accepts forms that pass every check")
{
    constexpr std::array forms{
        EspFormObservation{ "SPOE_ProxySpellEffect", 0x800, true, true, true, false },
        EspFormObservation{ "SPOE_ProxySpell", 0x801, true, true, true, false },
    };
    const auto result = CheckEspForms(forms);
    POE_CHECK(result.reason.empty());
    POE_CHECK(result.found == 2);
    POE_CHECK(result.missing == 0);
    POE_CHECK(result.wrongType == 0);
    POE_CHECK(result.wrongOwner == 0);
    POE_CHECK(result.deleted == 0);
    POE_CHECK(result.failedForms == 0);
}

POE_TEST("ESP form check: each kind of failure rejects a form independently")
{
    constexpr std::array forms{
        EspFormObservation{ "Missing", 0x800, false, false, false, false },
        EspFormObservation{ "WrongType", 0x801, true, false, true, false },
        EspFormObservation{ "WrongOwner", 0x802, true, true, false, false },
        EspFormObservation{ "Deleted", 0x803, true, true, true, true },
    };
    constexpr std::array expectedReasons{
        "없음 1, 타입 0, 소유 0, 삭제 0). Missing (0x000800)",
        "없음 0, 타입 1, 소유 0, 삭제 0). WrongType (0x000801)",
        "없음 0, 타입 0, 소유 1, 삭제 0). WrongOwner (0x000802)",
        "없음 0, 타입 0, 소유 0, 삭제 1). Deleted (0x000803)",
    };
    for (std::size_t i = 0; i < forms.size(); ++i) {
        const auto result = CheckEspForms(std::span{ &forms[i], 1 });
        POE_CHECK(result.failedForms == 1);
        POE_CHECK(result.reason.find(expectedReasons[i]) != std::string::npos);
    }
}

POE_TEST("ESP form check: lists the first three failures and counts all failures")
{
    constexpr std::array forms{
        EspFormObservation{ "FirstMissing", 0x800, false, false, false, false },
        EspFormObservation{ "ValidForm", 0x801, true, true, true, false },
        EspFormObservation{ "MultipleFailures", 0x802, true, false, false, true },
        EspFormObservation{ "ThirdFailure", 0x803, true, true, false, false },
        EspFormObservation{ "FourthFailure", 0x804, true, true, true, true },
    };
    const auto result = CheckEspForms(forms);
    POE_CHECK(result.found == 4);
    POE_CHECK(result.missing == 1);
    POE_CHECK(result.wrongType == 1);
    POE_CHECK(result.wrongOwner == 2);
    POE_CHECK(result.deleted == 2);
    POE_CHECK(result.failedForms == 4);
    POE_CHECK(result.reason.find("5개 중 4개 실패(없음 1, 타입 1, 소유 2, 삭제 2)") != std::string::npos);
    POE_CHECK(result.reason.find("FirstMissing (0x000800), MultipleFailures (0x000802), ThirdFailure (0x000803)") !=
        std::string::npos);
    POE_CHECK(result.reason.find("ValidForm") == std::string::npos);
    POE_CHECK(result.reason.find("FourthFailure") == std::string::npos);
}
