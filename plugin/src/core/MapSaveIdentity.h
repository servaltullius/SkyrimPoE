#pragma once

#include "core/MapTypes.h"

#include <utility>

namespace SkyrimPoE::Core
{
    // 결정 169, R4: MAPS v2는 기존 배열 순번 자리에 길이, 키 바이트, 팩 판 번호를 쓴다.
    template <class Write>
    bool WriteMapSaveIdentity(Write&& write, const ActiveMapState& state)
    {
        if (state.dungeonKey.size() > kMaxMapDungeonKeyBytes) {
            return false;
        }
        const auto length = static_cast<std::uint32_t>(state.dungeonKey.size());
        return write(&length, sizeof(length)) &&
               (length == 0 || write(state.dungeonKey.data(), length)) &&
               write(&state.packVersion, sizeof(state.packVersion));
    }

    template <class Read>
    bool ReadMapSaveIdentity(Read&& read, ActiveMapState& state)
    {
        std::uint32_t length = 0;
        if (!read(&length, sizeof(length)) || length > kMaxMapDungeonKeyBytes) {
            return false;
        }
        std::string key(length, '\0');
        std::uint32_t version = 0;
        if ((length != 0 && !read(key.data(), length)) || !read(&version, sizeof(version))) {
            return false;
        }
        state.dungeonKey = std::move(key);
        state.packVersion = version;
        return true;
    }
}
