#include "core/EspFormCheck.h"

#include <format>

namespace SkyrimPoE::Core
{
    EspFormCheckResult CheckEspForms(std::span<const EspFormObservation> forms)
    {
        constexpr std::size_t kMaxExamples = 3;
        EspFormCheckResult result;
        std::string examples;
        for (const auto& form : forms) {
            if (form.found) {
                ++result.found;
                result.wrongType += !form.typeMatches;
                result.wrongOwner += !form.ownerMatches;
                result.deleted += form.deleted;
                if (form.typeMatches && form.ownerMatches && !form.deleted) {
                    continue;
                }
            } else {
                ++result.missing;
            }

            if (++result.failedForms <= kMaxExamples) {
                if (!examples.empty()) {
                    examples += ", ";
                }
                examples += std::format("{} (0x{:06X})", form.name, form.localId);
            }
        }
        if (result.failedForms != 0) {
            result.reason = std::format("ESP 폼 검사 실패. {}개 중 {}개 실패(없음 {}, 타입 {}, 소유 {}, 삭제 {}). {}.",
                forms.size(), result.failedForms, result.missing, result.wrongType, result.wrongOwner, result.deleted, examples);
        }
        return result;
    }
}
