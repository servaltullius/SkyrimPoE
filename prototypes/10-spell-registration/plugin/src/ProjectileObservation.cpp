#include "ProjectileObservation.h"

#include <algorithm>
#include <cstring>

namespace ProjectileObservation
{
    std::uintptr_t RelativeTarget(std::uintptr_t nextInstruction, const std::uint8_t* displacement) noexcept
    {
        std::int32_t relative = 0;
        std::memcpy(&relative, displacement, sizeof(relative));
        return nextInstruction + static_cast<std::uintptr_t>(relative);
    }

    std::optional<std::uintptr_t> DirectCallTarget(std::uintptr_t address, const CallBytes& bytes) noexcept
    {
        if (bytes.front() != 0xE8) {
            return std::nullopt;
        }
        return RelativeTarget(address + bytes.size(), bytes.data() + 1);
    }

    void Registry::Expire(Clock::time_point now)
    {
        while (!launches_.empty() && now - launches_.front().launchedAt >= lifetime) {
            launches_.pop_front();
        }
    }

    void Registry::Insert(Key key, bool authorized, Clock::time_point now)
    {
        Expire(now);
        std::erase_if(launches_, [key](const Launch& launch) { return launch.key == key; });
        launches_.push_back({ key, authorized, now });
        if (launches_.size() > capacity) {
            launches_.pop_front();
        }
    }

    std::optional<Match> Registry::Find(Key key, Clock::time_point now)
    {
        Expire(now);
        const auto launch = std::ranges::find(launches_, key, &Launch::key);
        if (launch == launches_.end()) {
            return std::nullopt;
        }
        return Match{ launch->authorized, now - launch->launchedAt };
    }
}
