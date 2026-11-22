#include "PCH.h"
#include "runtime/MapPackRuntime.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace SkyrimPoE::Runtime::MapPackRuntime
{
    namespace
    {
        // 결정 169, R4: 데이터 팩과 같은 게임 Data 경로에서 목록 및 ESP를 읽는다.
        constexpr auto kManifestPath = "Data/SKSE/Plugins/SkyrimPoE/maps/SkyrimPoE_Maps.json";
        Core::MapPack g_manifest;
        std::string g_error = "지도 팩을 아직 확인하지 못했습니다.";
        using FormsByDungeon = std::unordered_map<const Core::MapDungeonDef*, DungeonForms>;
        FormsByDungeon g_forms;
        const RE::TESFile* g_plugin = nullptr;

        std::string HashMapsPlugin()
        {
            std::ifstream file(std::filesystem::path("Data") / Core::kMapsPluginName, std::ios::binary);
            if (!file) {
                throw std::runtime_error("Data 폴더에서 SkyrimPoE_Maps.esp를 읽을 수 없습니다.");
            }
            BCRYPT_ALG_HANDLE provider = nullptr;
            if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
                throw std::runtime_error("지도 팩 SHA-256 공급자를 열 수 없습니다.");
            }
            const auto closeProvider = [](void* value) { BCryptCloseAlgorithmProvider(value, 0); };
            const std::unique_ptr<void, decltype(closeProvider)> algorithm(provider, closeProvider);
            ULONG objectSize = 0;
            ULONG written = 0;
            if (BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                    sizeof(objectSize), &written, 0) != 0 || written != sizeof(objectSize) || objectSize == 0) {
                throw std::runtime_error("지도 팩 SHA-256 상태 크기를 확인할 수 없습니다.");
            }
            std::vector<UCHAR> object(objectSize);
            BCRYPT_HASH_HANDLE handle = nullptr;
            if (BCryptCreateHash(algorithm.get(), &handle, object.data(), objectSize, nullptr, 0, 0) != 0) {
                throw std::runtime_error("지도 팩 SHA-256 계산을 시작할 수 없습니다.");
            }
            const std::unique_ptr<void, decltype(&BCryptDestroyHash)> hash(handle, &BCryptDestroyHash);
            std::array<char, 64 * 1024> buffer{};
            while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
                if (BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer.data()),
                        static_cast<ULONG>(file.gcount()), 0) != 0) {
                    throw std::runtime_error("지도 팩 SHA-256 계산에 실패했습니다.");
                }
            }
            std::array<UCHAR, 32> digest{};
            if (file.bad() || !file.eof() ||
                BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
                throw std::runtime_error("지도 팩 SHA-256 계산을 마치지 못했습니다.");
            }
            std::string hex;
            hex.reserve(digest.size() * 2);
            for (const auto byte : digest) {
                std::format_to(std::back_inserter(hex), "{:02x}", byte);
            }
            return hex;
        }

        DungeonForms ResolveDungeon(RE::TESDataHandler& data, const Core::MapDungeonDef& dungeon)
        {
            DungeonForms forms;
            forms.cell = data.LookupForm<RE::TESObjectCELL>(dungeon.cell, Core::kMapsPluginName);
            if (!forms.cell || !forms.cell->IsInteriorCell()) {
                throw std::runtime_error(std::format("{}: 0x{:06X} 실내 셀을 찾지 못했습니다.", dungeon.key, dungeon.cell));
            }
            forms.cellId = forms.cell->GetFormID();
            const auto resolveMarker = [&](std::uint32_t id) {
                auto* marker = data.LookupForm<RE::TESObjectREFR>(id, Core::kMapsPluginName);
                if (!marker || marker->GetFormType() != RE::FormType::Reference || marker->GetParentCell() != forms.cell) {
                    throw std::runtime_error(std::format("{}: 마커 0x{:06X}의 형식이나 부모 셀이 다릅니다.", dungeon.key, id));
                }
                return marker;
            };
            const auto resolveNpc = [&](std::uint32_t id) {
                auto* npc = data.LookupForm<RE::TESNPC>(id, Core::kMapsSourceMaster);
                if (!npc) {
                    throw std::runtime_error(std::format("{}: Skyrim.esm 원형 0x{:06X}를 NPC_로 찾지 못했습니다.", dungeon.key, id));
                }
                return npc;
            };
            forms.entryMarker = resolveMarker(dungeon.entryMarker);
            forms.bossAnchor = resolveMarker(dungeon.bossAnchor);
            for (const auto anchor : dungeon.packAnchors) {
                forms.packAnchors.push_back(resolveMarker(anchor));
            }
            forms.bossNpc = resolveNpc(dungeon.bossNpc);
            for (const auto npc : dungeon.packNpcs) {
                forms.packNpcs.push_back(resolveNpc(npc));
            }
            for (const auto npc : dungeon.addNpcs) {
                forms.addNpcs.push_back(resolveNpc(npc));
            }
            for (const auto id : dungeon.returnDoors) {
                auto* door = data.LookupForm<RE::TESObjectREFR>(id, Core::kMapsPluginName);
                auto* base = door ? door->GetBaseObject() : nullptr;
                if (!door || door->GetFormType() != RE::FormType::Reference || door->IsDeleted() ||
                    door->GetParentCell() != forms.cell || !base || !base->As<RE::TESObjectDOOR>()) {
                    throw std::runtime_error(std::format("{}: 귀환 문 0x{:06X}의 형식이나 부모 셀이 다릅니다.", dungeon.key, id));
                }
                forms.returnDoors.push_back(door->GetFormID());
            }
            return forms;
        }
    }

    void Initialize()
    {
        try {
            auto* data = RE::TESDataHandler::GetSingleton();
            g_plugin = data && data->LookupModByName(Core::kMapsPluginName)
                ? data->LookupLoadedModByName(Core::kMapsPluginName)
                : nullptr;
            std::ifstream input(kManifestPath);
            if (!input) {
                throw std::runtime_error("SkyrimPoE_Maps.json을 찾거나 읽을 수 없습니다.");
            }
            g_manifest = Core::ParseMapManifest(input);
            if (!g_plugin) {
                throw std::runtime_error("로드 순서에 SkyrimPoE_Maps.esp가 없습니다.");
            }
            if (HashMapsPlugin() != g_manifest.espSha256) {
                throw std::runtime_error("지도 팩 ESP와 목록 파일의 SHA-256이 다릅니다.");
            }

            FormsByDungeon validated;
            for (const auto& dungeon : g_manifest.maps) {
                validated.emplace(&dungeon, ResolveDungeon(*data, dungeon));
            }
            validated.emplace(&g_manifest.arena, ResolveDungeon(*data, g_manifest.arena));
            g_forms = std::move(validated);
            g_error.clear();
            SKSE::log::info("Map pack: verified version {}, {} maps and one arena.",
                g_manifest.version, g_manifest.maps.size());
        } catch (const std::exception& e) {
            g_error = std::format("지도 팩 사용 불가 · {}", e.what());
            SKSE::log::error("{}", g_error);
        }
    }

    const Core::MapPack& Manifest() noexcept { return g_manifest; }
    const std::string& UnavailableReason() noexcept { return g_error; }

    const DungeonForms* FindForms(const Core::MapDungeonDef* dungeon)
    {
        const auto found = g_forms.find(dungeon);
        return found == g_forms.end() ? nullptr : &found->second;
    }

    RE::FormID ReturnDoorCell(RE::FormID doorId) noexcept
    {
        for (const auto& [dungeon, forms] : g_forms) {
            if (std::ranges::find(forms.returnDoors, doorId) != forms.returnDoors.end()) {
                return forms.cellId;
            }
        }
        return 0;
    }

    RE::TESObjectCELL* ResolveCell(const Core::MapDungeonDef& dungeon)
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        return g_plugin && data ? data->LookupForm<RE::TESObjectCELL>(dungeon.cell, Core::kMapsPluginName) : nullptr;
    }

    bool OwnsCell(const RE::TESObjectCELL* cell) noexcept
    {
        return cell && g_plugin && cell->GetFile(0) == g_plugin;
    }
}
