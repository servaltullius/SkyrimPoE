#include "Core/EventNames.h"

#include <algorithm>
#include <cctype>

namespace poe
{
    namespace
    {
        char LowerAscii(char c)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool ContainsNoCase(std::string_view text, std::string_view needle)
        {
            const auto found = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                [](char a, char b) { return LowerAscii(a) == LowerAscii(b); });
            return found != text.end();
        }
    }

    bool IsAttackRelatedEvent(std::string_view name)
    {
        return ContainsNoCase(name, "attack") || ContainsNoCase(name, "bash");
    }

    bool IsAttackStartEvent(std::string_view name)
    {
        if (!ContainsNoCase(name, "attack")) {
            return false;
        }
        if (ContainsNoCase(name, "stop") || ContainsNoCase(name, "_end") || ContainsNoCase(name, "win")) {
            return false;
        }
        return ContainsNoCase(name, "start") || ContainsNoCase(name, "initiate");
    }

    bool IsPowerAttackStartEvent(std::string_view name)
    {
        return IsAttackStartEvent(name) && ContainsNoCase(name, "power");
    }
}
