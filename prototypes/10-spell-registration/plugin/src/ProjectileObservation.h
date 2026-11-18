#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace ProjectileObservation
{
    using CallBytes = std::array<std::uint8_t, 5>;

    std::uintptr_t RelativeTarget(std::uintptr_t nextInstruction, const std::uint8_t* displacement) noexcept;
    std::optional<std::uintptr_t> DirectCallTarget(std::uintptr_t address, const CallBytes& bytes) noexcept;

    using Clock = std::chrono::steady_clock;

    struct Key
    {
        std::uint32_t handle;
        std::uint32_t formId;
        bool operator==(const Key&) const = default;
    };

    struct Match
    {
        bool authorized;
        std::chrono::duration<double> elapsed;
    };

    // 호출자가 잠금 안에서 시간을 읽고 이 등록부에 접근한다.
    class Registry
    {
    public:
        static constexpr auto lifetime = std::chrono::seconds(30);
        static constexpr std::size_t capacity = 1024;

        void Insert(Key key, bool authorized, Clock::time_point now);
        std::optional<Match> Find(Key key, Clock::time_point now);

    private:
        struct Launch
        {
            Key key;
            bool authorized;
            Clock::time_point launchedAt;
        };

        void Expire(Clock::time_point now);
        std::deque<Launch> launches_;
    };
}
