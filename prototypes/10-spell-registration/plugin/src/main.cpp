// PoE Proto 10: 칸마다 고정된 대리 주문을 바꿔 끼우고 플레이어의 시전 경로를 기록한다.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "AutoTest.h"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/sinks/basic_file_sink.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Seconds = std::chrono::duration<double>;
    using CastReason = RE::MagicSystem::CannotCastReason;

    constexpr auto kEspName = "PoEProtoSpells.esp";
    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoSpells.ini";
    constexpr auto kNoticePrefix = "[PoE Proto 10]";
    constexpr auto kTick = std::chrono::milliseconds(100);
    constexpr auto kAutoTick = std::chrono::milliseconds(25);
    constexpr auto kNoticeInterval = std::chrono::seconds(1);
    constexpr auto kCooldownNoticeInterval = std::chrono::seconds(1);
    constexpr auto kDefaultCooldownReason = CastReason::kPowerUsed;
    constexpr std::uint32_t kPresetKey = 0x40;
    constexpr std::uint32_t kStateKey = 0x41;

    struct SlotSpec
    {
        std::string_view label;
        std::string_view name;
        RE::FormID spellId;
        RE::FormID effectId;
    };
    constexpr std::array kSlotSpecs{
        SlotSpec{ "Slot1", "slot1", 0x804, 0x800 },
        SlotSpec{ "Slot2", "slot2", 0x805, 0x801 },
        SlotSpec{ "Slot3", "slot3", 0x806, 0x802 },
        SlotSpec{ "Power1", "power1", 0x807, 0x803 },
    };
    constexpr std::array kPresets{ "PresetA"sv, "PresetB"sv };

    struct Gem
    {
        std::string name;
        std::int32_t cost = 0;
        float chargeTime = 0.0f;
        Seconds cooldown{};
        std::string projectile;
        bool valid = false;
    };

    struct Settings
    {
        std::map<std::string, Gem, std::less<>> gems;
        std::array<std::array<std::string, kSlotSpecs.size()>, kPresets.size()> presets;
        std::size_t startPreset = 0;
        std::uint32_t presetKey = kPresetKey;
        std::uint32_t stateKey = kStateKey;
        CastReason cooldownReason = kDefaultCooldownReason;
        bool logAllPlayerSpells = false;
        bool notify = true;
        AutoTest::Settings autoTest;
    };

    enum class CastRoute { Check, SpellCast, Immediate, Count };
    using CastCounts = std::array<std::uint64_t, static_cast<std::size_t>(CastRoute::Count)>;

    struct Slot
    {
        const SlotSpec* spec = nullptr;
        RE::SpellItem* spell = nullptr;
        RE::EffectSetting* effect = nullptr;
        std::string_view gem = "-";
        Seconds cooldown{};
        std::chrono::time_point<Clock, Seconds> until{};
        Clock::time_point nextNotice{};
        std::array<std::atomic<std::uint64_t>, std::tuple_size_v<CastCounts>> calls{};
    };

    Settings g_settings;
    std::array<Slot, kSlotSpecs.size()> g_slots;
    std::mutex g_slotMutex;
    std::size_t g_preset = 0;
    std::once_flag g_installOnce;
    bool g_recordsReady = false;
    bool g_hooksReady = false;
    std::atomic<bool> g_gameReady = false;
    std::atomic<bool> g_loadPending = false;
    std::atomic<bool> g_tickQueued = false;
    std::filesystem::path g_autoMarkerPath;

    std::mutex g_noticeMutex;
    std::deque<std::string> g_notices;
    Clock::time_point g_nextNotice;
    const auto g_epoch = Clock::now();

    unsigned long Tid() { return ::GetCurrentThreadId(); }

    auto Micros()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_epoch).count();
    }

    char PresetName(std::size_t index) { return kPresets[index].back(); }

    bool InitializeLogging()
    {
        const auto directory = SKSE::log::log_directory();
        if (!directory) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(*directory, error);
        const auto path = *directory / "PoEProtoSpells.log";
        g_autoMarkerPath = *directory / "PoEProtoSpells.autotest";
        if (std::filesystem::exists(path, error)) {
            std::filesystem::copy_file(path, *directory / "PoEProtoSpells.prev.log",
                std::filesystem::copy_options::overwrite_existing, error);
        }
        try {
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto logger = std::make_shared<spdlog::logger>("poe_proto_spells", std::move(sink));
            logger->set_pattern("%Y-%m-%d %H:%M:%S.%e\t%l\t%v");
            spdlog::set_default_logger(std::move(logger));
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::info);
        } catch (const std::exception&) {
            return false;
        }
        if (error) {
            SKSE::log::error("이전 PoEProtoSpells.log 보존 실패: {} ({})", error.message(), error.value());
        }
        return true;
    }

    // 엔진에 돌려줄 값과 원래 함수 호출은 이 예외 처리 밖에 둔다.
    template <class Action>
    bool TryRun(std::string_view label, Action action) noexcept
    {
        try {
            action();
            return true;
        } catch (...) {
            try {
                SKSE::log::error("{} 처리 중 예외", label);
            } catch (...) {
            }
            return false;
        }
    }

    using AutoTest::ParseUnsigned;
    using AutoTest::Trim;

    template <class Float>
    bool ParseSeconds(std::string_view value, Float& out)
    {
        if (value.empty()) {
            return false;
        }
        const auto end = value.data() + value.size();
        const auto result = std::from_chars(value.data(), end, out);
        return result.ec == std::errc{} && result.ptr == end && std::isfinite(out) && out >= 0;
    }

    using AutoTest::IniSection;

    std::string_view Setting(const IniSection& section, std::string_view key)
    {
        const auto entry = section.find(key);
        return entry == section.end() ? ""sv : entry->second;
    }

    bool ReadBool(const IniSection& section, std::string_view key, bool fallback)
    {
        const auto value = Setting(section, key);
        if (value == "true" || value == "1") {
            return true;
        }
        if (value == "false" || value == "0") {
            return false;
        }
        if (section.contains(key)) {
            SKSE::log::warn("설정 {} '{}'의 기본값 {} 유지", key, value, fallback);
        }
        return fallback;
    }

    std::uint32_t ReadKey(const IniSection& section, std::string_view key, std::uint32_t fallback)
    {
        const auto value = Setting(section, key);
        std::uint32_t code = 0;
        if (ParseUnsigned(value, code) && code <= 255) {
            return code;
        }
        if (section.contains(key)) {
            SKSE::log::warn("설정 {} '{}'의 기본 키 0x{:02X} 유지", key, value, fallback);
        }
        return fallback;
    }

    Settings LoadSettings()
    {
        Settings settings;
        std::ifstream file(kConfigPath);
        if (!file) {
            SKSE::log::error("설정 파일을 열지 못함: {}", kConfigPath);
            return settings;
        }
        std::map<std::string, IniSection, std::less<>> sections;
        auto* section = &sections["General"];
        std::string line;
        while (std::getline(file, line)) {
            std::string_view text = line;
            if (text.starts_with("\xEF\xBB\xBF")) {
                text.remove_prefix(3);
            }
            text = Trim(text.substr(0, text.find(';')));
            if (text.starts_with('[') && text.ends_with(']')) {
                section = &sections[std::string(Trim(text.substr(1, text.size() - 2)))];
            } else if (const auto equals = text.find('='); equals != std::string_view::npos) {
                (*section)[std::string(Trim(text.substr(0, equals)))] = Trim(text.substr(equals + 1));
            } else if (!text.empty() && sections.contains("AutoTest") && section == &sections.at("AutoTest")) {
                SKSE::log::warn("설정 [AutoTest] 줄 형식 오류. 키=값 형식 필요");
            }
        }

        const auto& general = sections["General"];
        settings.logAllPlayerSpells = ReadBool(general, "LogAllPlayerSpells", settings.logAllPlayerSpells);
        settings.notify = ReadBool(general, "Notify", settings.notify);
        settings.presetKey = ReadKey(general, "PresetKey", settings.presetKey);
        settings.stateKey = ReadKey(general, "StateKey", settings.stateKey);
        settings.autoTest = AutoTest::ReadSettings(sections["AutoTest"], [](std::string_view key) {
            SKSE::log::warn("설정 [AutoTest] {} 값이 잘못됨. 기본값 유지", key);
        });
        const auto preset = Setting(general, "StartPreset");
        if (preset == "B") {
            settings.startPreset = 1;
        } else if (preset != "A" && general.contains("StartPreset")) {
            SKSE::log::warn("설정 StartPreset '{}'의 기본 배정 A 유지", preset);
        }
        const auto reasonText = Setting(general, "CooldownReason");
        std::uint32_t reason = 0;
        if (ParseUnsigned(reasonText, reason) &&
            (reason <= static_cast<std::uint32_t>(CastReason::kShoutWhileRecovering) ||
                reason == static_cast<std::uint32_t>(CastReason::kCustomReasonNoStart))) {
            settings.cooldownReason = static_cast<CastReason>(reason);
        } else if (general.contains("CooldownReason")) {
            SKSE::log::warn("설정 CooldownReason '{}'의 기본값 2 유지", reasonText);
        }

        for (std::size_t i = 0; i < kPresets.size(); ++i) {
            const auto& entries = sections[std::string(kPresets[i])];
            for (std::size_t j = 0; j < kSlotSpecs.size(); ++j) {
                settings.presets[i][j] = Setting(entries, kSlotSpecs[j].label);
            }
        }
        for (const auto& [name, entries] : sections) {
            if (!name.starts_with("Gem.")) {
                continue;
            }
            auto& gem = settings.gems[name.substr(4)];
            gem.name = Setting(entries, "Name");
            gem.projectile = Setting(entries, "Projectile");
            std::uint32_t cost = 0;
            double cooldown = 0;
            gem.valid = !gem.name.empty() && ParseUnsigned(Setting(entries, "Cost"), cost) &&
                        cost <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
                        ParseSeconds(Setting(entries, "ChargeTime"), gem.chargeTime) &&
                        ParseSeconds(Setting(entries, "Cooldown"), cooldown);
            gem.cost = gem.valid ? static_cast<std::int32_t>(cost) : 0;
            gem.cooldown = Seconds(cooldown);
        }
        return settings;
    }

    std::string FormId(const RE::TESForm* form)
    {
        return form ? std::format("{:08X}", form->GetFormID()) : "-";
    }

    std::string TsvText(std::string_view value)
    {
        std::string text(value);
        for (auto& ch : text) {
            if (ch == '\t' || ch == '\r' || ch == '\n') {
                ch = ' ';
            }
        }
        return text;
    }

    // 게임 스레드에서 HUD가 준비된 뒤 알림을 하나씩 보낸다.
    void ShowNotice(const std::string& message)
    {
        SKSE::log::info("{}", message);
        RE::SendHUDMessage::ShowHUDMessage(message.c_str());
    }

    void QueueNotice(std::string text, bool always)
    {
        if (always || g_settings.notify) {
            const std::scoped_lock lock(g_noticeMutex);
            g_notices.push_back(std::move(text));
        }
    }

    double Remaining(const Slot& slot, Clock::time_point now)
    {
        return std::max(0.0, Seconds(slot.until - now).count());
    }

    void StartCooldown(Slot* slot)
    {
        if (slot) {
            const std::scoped_lock lock(g_slotMutex);
            if (slot->cooldown > Seconds::zero()) {
                slot->until = Clock::now() + slot->cooldown;
            }
        }
    }

    struct CooldownRead
    {
        double remaining = 0.0;
        bool notice = false;
    };

    CooldownRead ReadCooldown(Slot* slot, bool mayNotify = false)
    {
        if (!slot) {
            return {};
        }
        const auto now = Clock::now();
        const std::scoped_lock lock(g_slotMutex);
        const auto remaining = Remaining(*slot, now);
        const bool notice = mayNotify && remaining > 0 && now >= slot->nextNotice;
        if (notice) {
            slot->nextNotice = now + kCooldownNoticeInterval;
        }
        return { remaining, notice };
    }

    void NotifyCooldown(const Slot* slot, const CooldownRead& cooldown)
    {
        if (cooldown.notice) {
            QueueNotice(std::format("{} {} 쿨다운 {:.1f}초", kNoticePrefix, slot->spec->label, cooldown.remaining), true);
        }
    }

    // ---------------------------------------------------------------- 시전 가로채기

    struct CastContext
    {
        const RE::PlayerCharacter* player = nullptr;
        Slot* slot = nullptr;
    };

    void CountCast(const CastContext& context, CastRoute route)
    {
        if (context.slot) {
            context.slot->calls[static_cast<std::size_t>(route)].fetch_add(1, std::memory_order_relaxed);
        }
    }

    CastContext ReadContext(RE::MagicCaster* caster, const RE::MagicItem* spell)
    {
        const auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || caster->GetCasterAsActor() != player) {
            return {};
        }
        for (auto& slot : g_slots) {
            if (spell && spell == slot.spell) {
                return { player, &slot };
            }
        }
        return { g_settings.logAllPlayerSpells ? player : nullptr, nullptr };
    }

    float Magicka(const RE::PlayerCharacter* player)
    {
        return player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
    }

    struct CastRead
    {
        std::int64_t time = Micros();
        int source = 0;
        bool dualCast = false;
        std::string formId;
        std::string costOverride;
        float currentCost = 0.0f;
        std::string magickaBefore = "-";
    };

    CastRead ReadCast(RE::MagicCaster* caster, RE::MagicItem* spell, const CastContext& context,
        std::optional<bool> checkDual = std::nullopt)
    {
        CastRead read;
        read.source = std::to_underlying(caster->GetCastingSource());
        read.dualCast = checkDual ? *checkDual : caster->GetIsDualCasting();
        read.formId = FormId(spell);
        const auto* spellItem = spell ? spell->As<RE::SpellItem>() : nullptr;
        if (spellItem) {
            const std::scoped_lock lock(g_slotMutex);
            read.costOverride = std::to_string(spellItem->data.costOverride);
        } else {
            read.costOverride = "-";
        }
        read.currentCost = caster->currentSpellCost;
        if (!checkDual.has_value()) {
            read.magickaBefore = std::format("{:.3f}", Magicka(context.player));
        }
        return read;
    }

    void LogCast(std::string_view route, const CastContext& context, const CastRead& read, std::string_view result,
        std::string_view magickaAfter, double remaining)
    {
        SKSE::log::info("CAST\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.3f}\t{}\t{}",
            read.time, Tid(), route, read.source, read.dualCast, read.formId, context.slot ? context.slot->spec->name : "-"sv,
            result, read.magickaBefore, magickaAfter, read.currentCost, read.costOverride,
            context.slot ? std::format("{:.3f}", remaining) : "-");
    }

    struct CheckCastHook
    {
        static constexpr std::size_t index = 0x0A;

        static bool Thunk(RE::MagicCaster* caster, RE::MagicItem* spell, bool dualCast, float* effectStrength,
            CastReason* reason, bool useBaseValueForCost)
        {
            const bool originalResult = original(caster, spell, dualCast, effectStrength, reason, useBaseValueForCost);
            bool result = originalResult;
            TryRun("CheckCast", [&] {
                const auto context = ReadContext(caster, spell);
                CountCast(context, CastRoute::Check);
                if (!context.player) {
                    return;
                }
                const auto cooldown = ReadCooldown(context.slot, true);
                const auto originalReason = reason ? std::optional(*reason) : std::nullopt;
                if (originalResult && cooldown.remaining > 0) {
                    result = false;
                    if (reason) {
                        *reason = g_settings.cooldownReason;
                    }
                }
                TryRun("CheckCast 알림", [&] { NotifyCooldown(context.slot, cooldown); });
                const auto reasonText = [](std::optional<CastReason> value) {
                    return value ? std::to_string(std::to_underlying(*value)) : "-";
                };
                const auto finalReason = originalResult && !result ? std::optional(g_settings.cooldownReason) : originalReason;
                LogCast("check", context, ReadCast(caster, spell, context, dualCast),
                    std::format("{}\t{}\t{}\t{}\t-\t-", originalResult, result, reasonText(originalReason), reasonText(finalReason)),
                    "-", cooldown.remaining);
            });
            return result;
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    struct SpellCastHook
    {
        static constexpr std::size_t index = 0x09;

        static void Thunk(RE::MagicCaster* caster, bool doCast, std::uint32_t arg2, RE::MagicItem* spell)
        {
            CastContext context;
            std::optional<CastRead> read;
            TryRun("SpellCast 이전", [&] {
                context = ReadContext(caster, spell);
                CountCast(context, CastRoute::SpellCast);
                if (context.player) {
                    read = ReadCast(caster, spell, context);
                }
            });
            original(caster, doCast, arg2, spell);
            if (doCast && context.slot) {
                TryRun("SpellCast 쿨다운", [&] { StartCooldown(context.slot); });
            }
            if (read) {
                TryRun("SpellCast 기록", [&] {
                    LogCast("spellcast", context, *read, std::format("-\t-\t-\t-\t{}\t-", doCast),
                        std::format("{:.3f}", Magicka(context.player)), ReadCooldown(context.slot).remaining);
                });
            }
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    struct CastSpellImmediateHook
    {
        static constexpr std::size_t index = 0x01;

        static void Thunk(RE::MagicCaster* caster, RE::MagicItem* spell, bool noHitEffectArt, RE::TESObjectREFR* target,
            float effectiveness, bool hostileEffectivenessOnly, float magnitudeOverride, RE::Actor* blameActor)
        {
            CastContext context;
            CooldownRead cooldown;
            TryRun("CastSpellImmediate 쿨다운", [&] {
                context = ReadContext(caster, spell);
                CountCast(context, CastRoute::Immediate);
                cooldown = ReadCooldown(context.slot, true);
            });
            const bool blocked = cooldown.remaining > 0;
            std::optional<CastRead> read;
            if (context.player) {
                TryRun("CastSpellImmediate 이전", [&] { read = ReadCast(caster, spell, context); });
            }
            if (blocked) {
                TryRun("CastSpellImmediate 알림", [&] { NotifyCooldown(context.slot, cooldown); });
            } else {
                original(caster, spell, noHitEffectArt, target, effectiveness, hostileEffectivenessOnly, magnitudeOverride, blameActor);
                if (context.slot) {
                    TryRun("CastSpellImmediate 쿨다운 시작", [&] {
                        StartCooldown(context.slot);
                        cooldown = ReadCooldown(context.slot);
                    });
                }
            }
            if (read) {
                TryRun("CastSpellImmediate 기록", [&] {
                    LogCast("immediate", context, *read, std::format("-\t-\t-\t-\t-\t{}", blocked),
                        std::format("{:.3f}", Magicka(context.player)), cooldown.remaining);
                });
            }
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    template <class Base, class Result, class... Args>
    auto ThunkType(Result (Base::*)(Args...)) -> Result (*)(Base*, Args...);

    static_assert(std::is_same_v<decltype(&CheckCastHook::Thunk), decltype(ThunkType(&RE::MagicCaster::CheckCast))>);
    static_assert(std::is_same_v<decltype(&SpellCastHook::Thunk), decltype(ThunkType(&RE::MagicCaster::SpellCast))>);
    static_assert(std::is_same_v<decltype(&CastSpellImmediateHook::Thunk), decltype(ThunkType(&RE::MagicCaster::CastSpellImmediate))>);

    HMODULE ModuleOf(std::uintptr_t address)
    {
        HMODULE module = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module);
        return module;
    }

    std::string FileNameOf(HMODULE module)
    {
        std::wstring buffer(MAX_PATH, L'\0');
        buffer.resize(::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size())));
        const auto name = std::filesystem::path(buffer).filename().u8string();
        return { reinterpret_cast<const char*>(name.data()), name.size() };
    }

    template <class Hook>
    bool InstallHook(REL::Relocation<std::uintptr_t> table, std::string_view name)
    {
        const auto& entry = reinterpret_cast<const volatile std::uintptr_t*>(table.address())[Hook::index];
        const auto target = entry;
        const auto replacement = reinterpret_cast<std::uintptr_t>(&Hook::Thunk);
        if (!target || target == replacement) {
            SKSE::log::error("{} vtable 0x{:02X} 설치 실패, 슬롯 값 0x{:X}", name, Hook::index, target);
            return false;
        }
        const auto module = ModuleOf(target);
        if (module && module != ::GetModuleHandleW(nullptr)) {
            SKSE::log::warn("{} vtable 0x{:02X}는 이미 {}가 가로챘음. 그 위에 겹쳐 설치함",
                name, Hook::index, FileNameOf(module));
        } else if (!module) {
            SKSE::log::warn("{} vtable 0x{:02X}의 기존 주소 0x{:X}는 모듈을 찾지 못함. 기존 주소에 연결함",
                name, Hook::index, target);
        }
        Hook::original = table.write_vfunc(Hook::index, Hook::Thunk);
        if (entry != replacement) {
            SKSE::log::error("{} vtable 0x{:02X} 설치 실패, 변경 내용을 읽지 못함", name, Hook::index);
            return false;
        }
        SKSE::log::info("{} vtable 0x{:02X}를 가로챔", name, Hook::index);
        return true;
    }

    void InstallHooks()
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        std::size_t found = 0;
        std::size_t installed = 0;
        if (data) {
            for (std::size_t i = 0; i < kSlotSpecs.size(); ++i) {
                auto& slot = g_slots[i];
                slot.spec = &kSlotSpecs[i];
                slot.spell = data->LookupForm<RE::SpellItem>(slot.spec->spellId, kEspName);
                slot.effect = data->LookupForm<RE::EffectSetting>(slot.spec->effectId, kEspName);
                found += static_cast<std::size_t>(slot.spell != nullptr) + static_cast<std::size_t>(slot.effect != nullptr);
                SKSE::log::info("{}\tSPEL 0x{:03X}={}\tMGEF 0x{:03X}={}", slot.spec->name,
                    slot.spec->spellId, FormId(slot.spell), slot.spec->effectId, FormId(slot.effect));
            }
        }
        g_recordsReady = found == 2 * kSlotSpecs.size();
        if (g_recordsReady) {
            const REL::Relocation<std::uintptr_t> table{ RE::VTABLE_ActorMagicCaster[0] };
            TryRun("CastSpellImmediate 설치", [&] { installed += InstallHook<CastSpellImmediateHook>(table, "CastSpellImmediate"); });
            TryRun("SpellCast 설치", [&] { installed += InstallHook<SpellCastHook>(table, "SpellCast"); });
            TryRun("CheckCast 설치", [&] { installed += InstallHook<CheckCastHook>(table, "CheckCast"); });
        } else {
            SKSE::log::error("{}의 대리 레코드를 모두 찾지 못해 가로채기를 설치하지 않음", kEspName);
        }
        g_hooksReady = installed == 3;
        SKSE::log::info("설치 결과: 성공 {}/3 (가로챈 vtable 슬롯)", installed);
        SKSE::log::info("설치 결과: 성공 {}/{} (대리 SPEL·MGEF 레코드)", found, 2 * kSlotSpecs.size());
    }

    // ---------------------------------------------------------------- 배정과 상태

    struct SpellView
    {
        std::string name;
        std::int32_t cost;
        bool costOverride;
        float chargeTime;
        std::string projectile;
    };

    SpellView ReadSpell(const Slot& slot)
    {
        const auto& data = slot.spell->data;
        return { TsvText(slot.spell->fullName.c_str()), data.costOverride, data.flags.all(RE::SpellItem::SpellFlag::kCostOverride),
            data.chargeTime, FormId(slot.effect->data.projectileBase) };
    }

    std::string FormatSpell(const SpellView& view)
    {
        return std::format("{}\t{}\t{}\t{:.3f}\t{}", view.name, view.cost, view.costOverride, view.chargeTime, view.projectile);
    }

    RE::BGSProjectile* LookupProjectile(std::string_view value)
    {
        const auto separator = value.find('|');
        if (separator == std::string_view::npos) {
            return nullptr;
        }
        const auto plugin = Trim(value.substr(0, separator));
        RE::FormID id = 0;
        if (plugin.empty() || !ParseUnsigned(Trim(value.substr(separator + 1)), id) || id == 0 || id > 0xFFFFFF) {
            return nullptr;
        }
        auto* data = RE::TESDataHandler::GetSingleton();
        return data ? data->LookupForm<RE::BGSProjectile>(id, plugin) : nullptr;
    }

    void ApplyError(const Slot& slot, std::string_view key, std::string_view error)
    {
        SKSE::log::error("배정 {} {} 젬 '{}' 적용 실패: {}", PresetName(g_preset), slot.spec->name, key, error);
        QueueNotice(std::format("{} {} 젬 오류. PoEProtoSpells.log 확인", kNoticePrefix, slot.spec->label), true);
    }

    void ApplySlot(Slot& slot, std::string_view key)
    {
        const auto entry = g_settings.gems.find(key);
        if (key.empty() || entry == g_settings.gems.end()) {
            ApplyError(slot, key, "젬 정의가 없음");
            return;
        }
        const auto& gem = entry->second;
        if (!gem.valid) {
            ApplyError(slot, key, "Name, Cost, ChargeTime 또는 Cooldown 값이 잘못됨");
            return;
        }
        RE::BGSProjectile* projectile = nullptr;
        if (!gem.projectile.empty()) {
            projectile = LookupProjectile(gem.projectile);
            if (!projectile) {
                ApplyError(slot, key, std::format("투사체 '{}'를 찾지 못함", gem.projectile));
                return;
            }
        }

        const std::scoped_lock lock(g_slotMutex);
        const auto before = ReadSpell(slot);
        slot.spell->fullName = gem.name.c_str();
        slot.spell->data.costOverride = gem.cost;
        slot.spell->data.flags.set(RE::SpellItem::SpellFlag::kCostOverride);
        slot.spell->data.chargeTime = gem.chargeTime;
        if (projectile) {
            slot.effect->data.projectileBase = projectile;
        }
        slot.gem = entry->first;
        slot.cooldown = gem.cooldown;
        SKSE::log::info("APPLY\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.3f}",
            Micros(), Tid(), PresetName(g_preset), slot.spec->name, FormId(slot.spell), TsvText(key),
            FormatSpell(before), FormatSpell(ReadSpell(slot)), gem.cooldown.count());
    }

    void ApplyPreset()
    {
        for (std::size_t i = 0; i < g_slots.size(); ++i) {
            TryRun(kSlotSpecs[i].label, [&] { ApplySlot(g_slots[i], g_settings.presets[g_preset][i]); });
        }
    }

    void ChangePreset(std::size_t next)
    {
        const auto previous = g_preset;
        g_preset = next;
        SKSE::log::info("MODE\t{}\t{}\t{}\t{}", Micros(), Tid(), PresetName(previous), PresetName(g_preset));
        ApplyPreset();
        QueueNotice(std::format("{} 배정 {}", kNoticePrefix, PresetName(g_preset)), false);
    }

    void DisplayState()
    {
        for (const auto& slot : g_slots) {
            const auto view = ReadSpell(slot);
            const auto id = FormId(slot.spell);
            double remaining = 0.0;
            {
                const std::scoped_lock lock(g_slotMutex);
                remaining = Remaining(slot, Clock::now());
            }
            SKSE::log::info("STATE\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.3f}",
                Micros(), Tid(), PresetName(g_preset), slot.spec->name, id, TsvText(slot.gem), FormatSpell(view), remaining);
            QueueNotice(std::format("{} {} {} {} {} {}m {:.2f}s P{} CD{:.1f}s", kNoticePrefix, slot.spec->label, id,
                slot.gem, view.name, view.cost, view.chargeTime, view.projectile, remaining), true);
        }
    }

    std::string KeyHint(std::uint32_t key)
    {
        if (key == kPresetKey) {
            return "F6";
        }
        if (key == kStateKey) {
            return "F7";
        }
        return key ? std::format("0x{:02X}", key) : "끔";
    }

    void AuditAutoLoad(RE::PlayerCharacter& player, std::string_view label);

    bool LoadGame()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }
        {
            const std::scoped_lock lock(g_noticeMutex);
            g_notices.clear();
        }
        if (!g_recordsReady) {
            QueueNotice(std::format("{} 대리 주문을 찾지 못함. PoEProtoSpells.log 확인", kNoticePrefix), true);
            return true;
        }
        std::array<bool, kSlotSpecs.size()> known{};
        {
            const std::scoped_lock lock(g_slotMutex);
            for (auto& slot : g_slots) {
                slot.until = {};
                slot.nextNotice = {};
            }
        }
        g_preset = g_settings.startPreset;
        AuditAutoLoad(*player, "favstate-before");
        for (std::size_t i = 0; i < g_slots.size(); ++i) {
            const auto& slot = g_slots[i];
            known[i] = player->HasSpell(slot.spell);
            SKSE::log::info("LOADSTATE\t{}\t{}\t{}\t{}\t{}\t{}", Micros(), Tid(), slot.spec->name, FormId(slot.spell),
                known[i], FormatSpell(ReadSpell(slot)));
        }
        for (std::size_t i = 0; i < g_slots.size(); ++i) {
            if (!known[i] && !player->AddSpell(g_slots[i].spell)) {
                SKSE::log::error("{}을 플레이어에게 가르치지 못함", g_slots[i].spec->label);
            }
        }
        ApplyPreset();
        AuditAutoLoad(*player, "favstate-after");
        g_gameReady = true;
        QueueNotice(std::format("{} 배정 {}. {} 바꾸기, {} 상태", kNoticePrefix, PresetName(g_preset),
            KeyHint(g_settings.presetKey), KeyHint(g_settings.stateKey)), false);
        return true;
    }

    bool MenuIsOpen()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME)) {
            return true;
        }
        // HUD와 상시 표시용 메뉴는 게임 중에도 스택에 있다.
        const auto hud = ui->GetMenu<RE::HUDMenu>();
        const auto cursor = ui->GetMenu<RE::CursorMenu>();
        const auto fader = ui->GetMenu<RE::FaderMenu>();
        for (const auto& menu : ui->menuStack) {
            if (menu && !menu->AlwaysOpen() && menu.get() != hud.get() && menu.get() != cursor.get() && menu.get() != fader.get()) {
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------- 자동 시험 (게임 스레드)

    enum class AutoBoot { Idle, MenuDelay, AwaitLoad, StartDelay, Running, QuitDelay };
    enum class AutoLoadResult { None, Success, Failed };
    enum class AutoHand { Right, Both, Console };
    enum class MenuCheck { Idle, Opening, Holding, Closing };

    struct ButtonDelete
    {
        void operator()(RE::ButtonEvent* event) const
        {
            // Create는 게임 힙에서 할당한다. 문자열 소멸 후 같은 힙으로 돌려준다.
            event->~ButtonEvent();
            RE::free(event);
        }
    };
    using Buttons = std::array<std::unique_ptr<RE::ButtonEvent, ButtonDelete>, 2>;

    struct AttemptSpec
    {
        std::string_view label;
        std::size_t slot;
        AutoHand hand;
        bool waitCooldown = false;
        bool restoreMagicka = false;
        bool chargingCheckpoint = false;
        double consoleWait = 1;
    };

    struct Attempt
    {
        enum class State { Idle, Held, Released };
        State state = State::Idle;
        CastCounts startCounts{};
        Clock::time_point pressedAt{};
        double hold = 0;
        float before = 0;
        float minimum = 0;
        float released = 0;
        double cooldownAtRelease = 0;
        bool checkpoint = false;
        Buttons releaseButtons;
    };

    constexpr auto kMainMenu = "Main Menu"sv;
    constexpr auto kAutoNotice = "[PoE Proto 10 자동]"sv;
    constexpr double kCooldownPadding = 0.3;
    constexpr double kMenuTimeout = 3;
    constexpr double kLoadTimeout = 60;

    struct AutoSession
    {
        AutoTest::Plan plan;
        AutoBoot boot = AutoBoot::Idle;
        int phase = 0;
        std::string saveToLoad;
        std::string_view stage;
        std::chrono::time_point<Clock, Seconds> deadline{};
        Attempt attempt;
        std::string_view ownMenu;
        MenuCheck menuCheck = MenuCheck::Idle;
        bool markerWritten = false;
    };
    AutoSession g_auto;
    std::atomic<bool> g_autoTick = false;
    std::atomic<bool> g_mainMenuSeen = false;
    std::atomic<bool> g_mainMenuPending = false;
    std::atomic<bool> g_autoRequestedLoad = false;
    std::atomic<bool> g_autoInterrupted = false;
    std::atomic<AutoLoadResult> g_autoLoadResult = AutoLoadResult::None;

    template <class... Args>
    void AutoLogAt(std::string_view stage, std::string_view event, std::format_string<Args...> detail, Args&&... args)
    {
        SKSE::log::info("AUTO\t{}\t{}\t{}\t{}\t{}\t{}", Micros(), Tid(), g_auto.phase,
            stage.empty() ? "-"sv : stage, event, std::format(detail, std::forward<Args>(args)...));
    }

    template <class... Args>
    void AutoLog(std::string_view event, std::format_string<Args...> detail, Args&&... args)
    {
        AutoLogAt(g_auto.stage, event, detail, std::forward<Args>(args)...);
    }

    void AutoWait(double seconds)
    {
        g_auto.deadline = Clock::now() + Seconds(seconds);
    }

    void AutoCheckpoint(std::string_view name, double hold)
    {
        AutoLog("CHECKPOINT", "{}\t{:.3f}", name, hold);
        ShowNotice(std::format("{} {}", kAutoNotice, name));
    }

    Buttons MakeButtons(const RE::UserEvents& events, AutoHand hand, float value, float held)
    {
        Buttons buttons;
        buttons[0].reset(RE::ButtonEvent::Create(RE::INPUT_DEVICE::kMouse, events.rightAttack, 0, value, held));
        if (hand == AutoHand::Both) {
            buttons[1].reset(RE::ButtonEvent::Create(RE::INPUT_DEVICE::kMouse, events.leftAttack, 1, value, held));
        }
        if (!buttons[0] || (hand == AutoHand::Both && !buttons[1])) {
            throw std::runtime_error("ButtonEvent::Create");
        }
        return buttons;
    }

    void SendButtons(RE::BSInputDeviceManager& input, const Buttons& buttons)
    {
        RE::InputEvent* head = buttons[0].get();
        head->next = buttons[1].get();
        input.SendEvent(&head);
    }

    bool ReleaseButtons()
    {
        auto& buttons = g_auto.attempt.releaseButtons;
        if (!buttons[0]) {
            return true;
        }
        auto* input = RE::BSInputDeviceManager::GetSingleton();
        if (!input) {
            AutoLog("SKIP", "release\tinput-manager-missing");
            return false;
        }
        const auto held = static_cast<float>(std::max(0.001, Seconds(Clock::now() - g_auto.attempt.pressedAt).count()));
        for (const auto& button : buttons) {
            if (button) {
                button->GetRuntimeData().heldDownSecs = held;
            }
        }
        SendButtons(*input, buttons);
        buttons = {};
        return true;
    }

    bool SendMenu(std::string_view name, RE::UI_MESSAGE_TYPE message)
    {
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (!queue) {
            AutoLog("SKIP", "{}\tui-message-queue-missing", name);
            return false;
        }
        queue->AddMessage(RE::BSFixedString(name), message, nullptr);
        return true;
    }

    void EndAutoStage(std::string_view status)
    {
        if (!g_auto.stage.empty()) {
            AutoLog("END", "{}", status);
            g_auto.stage = {};
        }
    }

    void ResetAuto()
    {
        g_autoTick = false;
        g_autoRequestedLoad = false;
        g_autoInterrupted = false;
        g_autoLoadResult = AutoLoadResult::None;
        g_auto = {};
    }

    void AbortAuto(std::string_view reason)
    {
        g_auto.plan.Stop();
        TryRun("자동 입력 놓기", [] { ReleaseButtons(); });
        TryRun("자동 메뉴 닫기", [] {
            if (!g_auto.ownMenu.empty()) {
                SendMenu(g_auto.ownMenu, RE::UI_MESSAGE_TYPE::kHide);
            }
        });
        TryRun("자동 시험 중단 기록", [&] {
            if (g_auto.markerWritten) {
                std::error_code error;
                std::filesystem::remove(g_autoMarkerPath, error);
                if (error) {
                    SKSE::log::error("자동 시험 표지 삭제 실패: {}", error.message());
                }
            }
            AutoLog("ABORT", "{}", reason);
            EndAutoStage("aborted");
            ShowNotice(std::format("{} 중단 ({})", kAutoNotice, reason));
        });
        ResetAuto();
    }

    template <class Action>
    void RunAutoSafely(Action action)
    {
        if (!TryRun("자동 시험", action)) {
            AbortAuto("exception");
        }
    }

    std::string_view AutoAbortReason(const RE::PlayerCharacter* player, bool injecting)
    {
        return AutoTest::AbortReason(player != nullptr, player && player->IsDead(), injecting, injecting && MenuIsOpen());
    }

    bool IsFavorite(const RE::MagicFavorites& favorites, const Slot& slot)
    {
        return std::find(favorites.spells.begin(), favorites.spells.end(), slot.spell) != favorites.spells.end();
    }

    void LogFavoriteState(const RE::PlayerCharacter& player, std::string_view label, std::string_view stage)
    {
        const auto* favorites = RE::MagicFavorites::GetSingleton();
        if (!favorites) {
            AutoLogAt(stage, "SKIP", "{}\tmagic-favorites-missing", label);
        }
        std::string fields;
        for (const auto& slot : g_slots) {
            fields += std::format("\t{}\t{}\t{}", slot.spec->name,
                favorites ? (IsFavorite(*favorites, slot) ? "true"sv : "false"sv) : "-"sv, player.HasSpell(slot.spell));
        }
        AutoLogAt(stage, "INFO", "{}{}", label, fields);
    }

    void AuditAutoLoad(RE::PlayerCharacter& player, std::string_view label)
    {
        if (g_auto.boot == AutoBoot::AwaitLoad && g_autoLoadResult == AutoLoadResult::Success) {
            RunAutoSafely([&] { LogFavoriteState(player, label, g_auto.phase == 2 ? "P2-state"sv : ""sv); });
        }
    }

    void RestoreMagicka(RE::PlayerCharacter& player)
    {
        constexpr auto magicka = RE::ActorValue::kMagicka;
        const auto damage = player.GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, magicka);
        if (damage < 0) {
            player.AsActorValueOwner()->RestoreActorValue(magicka, -damage);
        }
    }

    bool EnsureAutoPreset(std::size_t preset)
    {
        if (g_preset != preset) {
            ChangePreset(preset);
        }
        for (std::size_t i = 0; i < g_slots.size(); ++i) {
            if (g_slots[i].gem != g_settings.presets[preset][i]) {
                AbortAuto("preset-apply-failed");
                return false;
            }
        }
        AutoLog("INFO", "preset\t{}", PresetName(preset));
        return true;
    }

    void AutoSetup(RE::PlayerCharacter& player)
    {
        if (!EnsureAutoPreset(0)) {
            return;
        }
        auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(0x0153CF);
        if (perk) {
            const bool hadPerk = player.HasPerk(perk);
            if (!hadPerk) {
                player.AddPerk(perk, 1);
            }
            AutoLog("INFO", "dual-perk\t{}\t{}", hadPerk, player.HasPerk(perk));
        } else {
            AutoLog("SKIP", "destruction-dual-casting-perk-missing");
        }
        RestoreMagicka(player);
        LogFavoriteState(player, "favstate-setup-before", g_auto.stage);
        if (auto* favorites = RE::MagicFavorites::GetSingleton()) {
            for (const auto& slot : g_slots) {
                if (!IsFavorite(*favorites, slot)) {
                    favorites->SetFavorite(slot.spell);
                }
            }
            LogFavoriteState(player, "favstate-setup-after", g_auto.stage);
        }
        auto* equip = RE::ActorEquipManager::GetSingleton();
        const auto* voice = RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x025BEE);
        if (equip && voice) {
            equip->EquipSpell(&player, g_slots[3].spell, voice);
        } else {
            AutoLog("SKIP", "equip-power\t{}", !equip ? "equip-manager-missing" : "voice-slot-missing");
        }
        AutoWait(g_settings.autoTest.equipSettle);
    }

    void AutoEquip(RE::PlayerCharacter& player, bool both)
    {
        auto* equip = RE::ActorEquipManager::GetSingleton();
        const auto* right = RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x013F42);
        const auto* left = RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x013F43);
        if (!equip || !right || (both && !left)) {
            AbortAuto(!equip ? "equip-manager-missing" : !right ? "right-slot-missing" : "left-slot-missing");
            return;
        }
        if (both) {
            equip->EquipSpell(&player, g_slots[1].spell, left);
        } else if (auto* equipped = player.GetEquippedObject(true)) {
            auto* object = equipped->As<RE::TESBoundObject>();
            if (!left || !object || !equip->UnequipObject(&player, object, nullptr, 1, left, false, false, false, true)) {
                AutoLog("SKIP", "unequip-left\t{}", FormId(equipped));
            }
        }
        equip->EquipSpell(&player, g_slots[1].spell, right);
        player.DrawWeaponMagicHands(true);
        AutoWait(g_settings.autoTest.equipSettle);
    }

    CastCounts ReadCounts(const Slot& slot)
    {
        CastCounts counts{};
        for (std::size_t i = 0; i < counts.size(); ++i) {
            counts[i] = slot.calls[i].load(std::memory_order_relaxed);
        }
        return counts;
    }

    bool AutoConsole(RE::PlayerCharacter& player, const Slot& slot)
    {
        RE::FormID target = player.GetFormID();
        if (auto* processes = RE::ProcessLists::GetSingleton()) {
            auto distance = std::numeric_limits<float>::infinity();
            processes->ForEachHighActor([&](RE::Actor* actor) {
                if (actor && actor != &player && !actor->IsDead()) {
                    const auto square = player.GetPosition().GetSquaredDistance(actor->GetPosition());
                    if (square < distance) {
                        distance = square;
                        target = actor->GetFormID();
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        } else {
            AutoLog("SKIP", "process-lists-missing");
        }
        if (target == player.GetFormID()) {
            AutoLog("INFO", "console-target-self\t{:08X}", target);
        }
        const std::unique_ptr<RE::Script> script(RE::IFormFactory::Create<RE::Script>());
        if (!script) {
            AutoLog("SKIP", "console-script-factory-missing");
            return false;
        }
        const auto command = std::format("player.cast {} {:08X}", FormId(slot.spell), target);
        AutoLog("INFO", "console-command\t{}", command);
        script->SetCommand(command);
        script->CompileAndRun(&player);
        return true;
    }

    bool AutoAttempt(RE::PlayerCharacter& player, const AttemptSpec& spec)
    {
        auto& attempt = g_auto.attempt;
        auto& slot = g_slots[spec.slot];
        const auto& config = g_settings.autoTest;
        const auto hand = spec.hand == AutoHand::Right ? "right" : spec.hand == AutoHand::Both ? "both" : "console";
        const auto* events = RE::UserEvents::GetSingleton();
        auto* input = RE::BSInputDeviceManager::GetSingleton();
        if (attempt.state == Attempt::State::Idle) {
            if (spec.waitCooldown) {
                const auto remaining = ReadCooldown(&slot).remaining;
                if (remaining > 0) {
                    AutoWait(remaining + kCooldownPadding);
                    return false;
                }
            }
            if (spec.hand != AutoHand::Console && (!input || !events || MenuIsOpen())) {
                AbortAuto(!input ? "input-manager-missing" : !events ? "user-events-missing" : "external-menu");
                return false;
            }
            if (spec.restoreMagicka) {
                RestoreMagicka(player);
            }
            attempt.startCounts = ReadCounts(slot);
            attempt.minimum = attempt.before = Magicka(&player);
            if (spec.hand == AutoHand::Console) {
                if (!AutoConsole(player, slot)) {
                    return true;
                }
            } else {
                attempt.hold = spec.chargingCheckpoint ? config.barHold : ReadSpell(slot).chargeTime + config.holdExtra;
                auto press = MakeButtons(*events, spec.hand, 1, 0);
                attempt.releaseButtons = MakeButtons(*events, spec.hand, 0, 0);
                attempt.pressedAt = Clock::now();
                attempt.state = Attempt::State::Held;
                SendButtons(*input, press);
                attempt.minimum = std::min(attempt.minimum, Magicka(&player));
                return false;
            }
        } else if (attempt.state == Attempt::State::Held) {
            attempt.minimum = std::min(attempt.minimum, Magicka(&player));
            const auto held = Seconds(Clock::now() - attempt.pressedAt).count();
            if (spec.chargingCheckpoint && !attempt.checkpoint && held >= 1) {
                AutoCheckpoint("charging-B", std::max(0.0, attempt.hold - held));
                attempt.checkpoint = true;
            }
            if (held < attempt.hold) {
                if (!input || !events) {
                    AbortAuto(!input ? "input-manager-missing" : "user-events-missing");
                    return false;
                }
                SendButtons(*input, MakeButtons(*events, spec.hand, 1, static_cast<float>(held)));
                attempt.minimum = std::min(attempt.minimum, Magicka(&player));
                return false;
            }
            if (!ReleaseButtons()) {
                AbortAuto("input-release-failed");
                return false;
            }
        } else {
            const auto endCounts = ReadCounts(slot);
            CastCounts delta{};
            for (std::size_t i = 0; i < delta.size(); ++i) {
                delta[i] = endCounts[i] - attempt.startCounts[i];
            }
            AutoLog("ATTEMPT", "{}\t{}\t{}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{}\t{}\t{}\t{:.3f}",
                spec.label, slot.spec->name, hand, attempt.before, attempt.minimum, attempt.released, Magicka(&player),
                delta[0], delta[1], delta[2], attempt.cooldownAtRelease);
            if (spec.label == "cd-first") {
                g_auto.plan.ObserveFirstHandAttempt(delta[0]);
                if (delta[0] == 0) {
                    AutoLog("INFO", "input-injection-no-effect");
                }
            }
            attempt = {};
            return true;
        }
        attempt.released = Magicka(&player);
        attempt.minimum = std::min(attempt.minimum, attempt.released);
        attempt.cooldownAtRelease = ReadCooldown(&slot).remaining;
        attempt.state = Attempt::State::Released;
        AutoWait(spec.hand == AutoHand::Console ? spec.consoleWait : config.afterRelease);
        return false;
    }

    bool AutoMenu(std::string_view menu, std::string_view checkpoint)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            AutoLog("SKIP", "{}\tui-missing", checkpoint);
            g_auto.ownMenu = {};
            g_auto.menuCheck = MenuCheck::Idle;
            return true;
        }
        const bool open = ui->IsMenuOpen(menu);
        switch (g_auto.menuCheck) {
        case MenuCheck::Idle:
            if (open || MenuIsOpen()) {
                AutoLog("SKIP", "{}\tmenu-already-open", checkpoint);
                return true;
            }
            if (!SendMenu(menu, RE::UI_MESSAGE_TYPE::kShow)) {
                return true;
            }
            g_auto.ownMenu = menu;
            g_auto.menuCheck = MenuCheck::Opening;
            AutoWait(kMenuTimeout);
            return false;
        case MenuCheck::Opening:
            if (open) {
                AutoCheckpoint(checkpoint, g_settings.autoTest.menuHold);
                g_auto.menuCheck = MenuCheck::Holding;
                AutoWait(g_settings.autoTest.menuHold);
            } else if (Clock::now() >= g_auto.deadline) {
                AutoLog("SKIP", "{}\tmenu-did-not-open", checkpoint);
                SendMenu(menu, RE::UI_MESSAGE_TYPE::kHide);
                g_auto.ownMenu = {};
                g_auto.menuCheck = MenuCheck::Idle;
                return true;
            }
            return false;
        case MenuCheck::Holding:
            if (Clock::now() < g_auto.deadline) {
                return false;
            }
            if (!open) {
                AutoLog("SKIP", "{}\tmenu-closed-before-hide", checkpoint);
            }
            if (open) {
                if (!SendMenu(menu, RE::UI_MESSAGE_TYPE::kHide)) {
                    AbortAuto("menu-hide-failed");
                    return false;
                }
                g_auto.menuCheck = MenuCheck::Closing;
                AutoWait(kMenuTimeout);
                return false;
            }
            break;
        case MenuCheck::Closing:
            if (open) {
                if (Clock::now() >= g_auto.deadline) {
                    AbortAuto("menu-did-not-close");
                }
                return false;
            }
            break;
        }
        g_auto.ownMenu = {};
        g_auto.menuCheck = MenuCheck::Idle;
        return true;
    }

    bool WriteAutoMarker()
    {
        auto temporary = g_autoMarkerPath;
        temporary += ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << "phase=2\nsave=" << g_settings.autoTest.saveName << '\n';
        file.close();
        if (file && ::MoveFileExW(temporary.c_str(), g_autoMarkerPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            g_auto.markerWritten = true;
            return true;
        }
        const auto code = file ? ::GetLastError() : 0;
        SKSE::log::error("자동 시험 phase 2 표지 쓰기 실패 ({})", code);
        std::error_code error;
        std::filesystem::remove(temporary, error);
        AbortAuto("marker-write-failed");
        return false;
    }

    bool RunAutoStep(AutoTest::Action action, RE::PlayerCharacter& player)
    {
        using enum AutoTest::Action;
        const auto& config = g_settings.autoTest;
        switch (action) {
        case Setup: AutoSetup(player); break;
        case EquipRight: AutoEquip(player, false); break;
        case EquipBoth: AutoEquip(player, true); break;
        case CoolFirst: return AutoAttempt(player, { AutoTest::HandAttemptLabel(action), 1, AutoHand::Right, true, true });
        case CoolBlocked: return AutoAttempt(player, { AutoTest::HandAttemptLabel(action), 1, AutoHand::Right });
        case CoolAfter: return AutoAttempt(player, { AutoTest::HandAttemptLabel(action), 1, AutoHand::Right, true });
        case Dual: return AutoAttempt(player, { AutoTest::HandAttemptLabel(action), 1, AutoHand::Both, true, true });
        case Single: return AutoAttempt(player, { AutoTest::HandAttemptLabel(action), 1, AutoHand::Right, true, true });
        case Console1: return AutoAttempt(player, { "console-slot1", 0, AutoHand::Console, true });
        case Console2a: return AutoAttempt(player, { "console-slot2-a", 1, AutoHand::Console, true, false, false, 0.3 });
        case Console2b: return AutoAttempt(player, { "console-slot2-b", 1, AutoHand::Console });
        case HudA: AutoCheckpoint("hud-A", config.checkpoint); AutoWait(config.checkpoint); break;
        case Swap: return EnsureAutoPreset(1);
        case HudB: AutoCheckpoint("hud-B-after-swap", config.checkpoint); AutoWait(config.checkpoint); break;
        case FavoritesB: return AutoMenu(RE::FavoritesMenu::MENU_NAME, "favorites-B");
        case MagicB: return AutoMenu(RE::MagicMenu::MENU_NAME, "magic-B");
        case AfterSwap: return AutoAttempt(player, { AutoTest::HandAttemptLabel(action), 1, AutoHand::Right, true, true, true });
        case Save:
            if (MenuIsOpen()) {
                AbortAuto("external-menu-at-save");
                return false;
            }
            if (auto* saves = RE::BGSSaveLoadManager::GetSingleton()) {
                LogFavoriteState(player, "favstate-save", g_auto.stage);
                AutoLog("INFO", "save-request\t{}", config.saveName);
                saves->Save(config.saveName.c_str());
                AutoWait(config.saveWait);
            } else {
                AbortAuto("save-load-manager-missing");
            }
            break;
        case WriteMarker: return WriteAutoMarker();
        case State: AutoLog("INFO", "applied-preset\t{}", PresetName(g_preset)); break;
        case MagicLoaded: return AutoMenu(RE::MagicMenu::MENU_NAME, "magic-after-load");
        case FavoritesLoaded: return AutoMenu(RE::FavoritesMenu::MENU_NAME, "favorites-after-load");
        }
        return true;
    }

    void BeginAuto(int phase)
    {
        g_auto.phase = phase;
        g_auto.boot = AutoBoot::Running;
        g_auto.stage = {};
        g_autoTick = true;
        if (!g_recordsReady || !g_gameReady || (phase == 1 && !g_hooksReady)) {
            AbortAuto(!g_recordsReady ? "proxy-records-missing" : !g_gameReady ? "game-not-ready" : "cast-hooks-missing");
            return;
        }
        g_auto.plan.Start(phase);
    }

    void RunAutoPlan(RE::PlayerCharacter& player)
    {
        // 한 작업 안에서 즉시 끝나는 동작만 이어 간다. 대기는 다음 ticker 작업에서 처리한다.
        while (g_auto.boot == AutoBoot::Running) {
            if (g_auto.menuCheck == MenuCheck::Idle && Clock::now() < g_auto.deadline) {
                return;
            }
            const auto* step = g_auto.plan.Current();
            if (!step) {
                EndAutoStage("ok");
                AutoLog("DONE", "phase{}", g_auto.phase);
                if (g_settings.autoTest.quitWhenDone) {
                    g_auto.boot = AutoBoot::QuitDelay;
                    AutoWait(2);
                } else {
                    ResetAuto();
                }
                return;
            }
            if (step->stage != g_auto.stage) {
                EndAutoStage("ok");
                g_auto.stage = step->stage;
                AutoLog("BEGIN", "-");
            }
            if (g_auto.plan.SkipCurrent()) {
                auto label = AutoTest::HandAttemptLabel(step->action);
                if (label.empty()) {
                    label = step->action == AutoTest::Action::EquipBoth ? "equip-both" : "equip-right";
                }
                AutoLog("SKIP", "{}\tinput-injection-no-effect", label);
            } else if (!RunAutoStep(step->action, player) || g_auto.boot != AutoBoot::Running) {
                return;
            }
            g_auto.plan.Advance();
        }
    }

    void ReadAutoMarker()
    {
        std::error_code error;
        const bool exists = std::filesystem::exists(g_autoMarkerPath, error);
        if (error) {
            SKSE::log::error("자동 시험 표지 확인 실패: {}", error.message());
        }
        if (!exists) {
            return;
        }
        std::array<char, 1024> text{};
        std::ifstream file(g_autoMarkerPath, std::ios::binary);
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        const auto size = static_cast<std::size_t>(file.gcount());
        const bool read = file.eof() && !file.bad();
        file.close();
        if (!std::filesystem::remove(g_autoMarkerPath, error)) {
            SKSE::log::error("자동 시험 표지 삭제 실패: {}", error ? error.message() : "표지 파일이 사라짐");
            return;
        }
        auto marker = read ? AutoTest::ParseMarker({ text.data(), size }) : std::nullopt;
        if (!marker) {
            SKSE::log::error("자동 시험 표지 형식 오류 또는 읽기 실패");
            return;
        }
        g_auto.boot = AutoBoot::MenuDelay;
        g_auto.phase = marker->phase;
        g_auto.saveToLoad = std::move(marker->save);
        g_autoTick = true;
        AutoLog("INFO", "marker\t{}", g_auto.saveToLoad);
        AutoWait(g_settings.autoTest.mainMenuDelay);
    }

    void AutoLoaded()
    {
        if (g_auto.boot != AutoBoot::AwaitLoad || g_autoLoadResult != AutoLoadResult::Success) {
            return;
        }
        g_autoRequestedLoad = false;
        g_auto.boot = AutoBoot::StartDelay;
        AutoWait(g_settings.autoTest.startDelay);
    }

    void TickAuto()
    {
        if (g_mainMenuPending.exchange(false)) {
            ReadAutoMarker();
        }
        if (g_autoInterrupted.exchange(false) && g_auto.boot != AutoBoot::Idle) {
            AbortAuto("unexpected-game-load");
            return;
        }
        const auto now = Clock::now();
        switch (g_auto.boot) {
        case AutoBoot::Idle: return;
        case AutoBoot::MenuDelay:
            if (now >= g_auto.deadline) {
                auto* ui = RE::UI::GetSingleton();
                auto* saves = RE::BGSSaveLoadManager::GetSingleton();
                if (!ui || !ui->IsMenuOpen(kMainMenu) || !saves) {
                    AbortAuto(!ui ? "ui-missing" : !saves ? "save-load-manager-missing" : "main-menu-closed");
                    return;
                }
                g_auto.boot = AutoBoot::AwaitLoad;
                g_autoRequestedLoad = true;
                AutoWait(kLoadTimeout);
                AutoLog("INFO", "load-request\t{}", g_auto.saveToLoad);
                saves->Load(g_auto.saveToLoad.c_str());
            }
            return;
        case AutoBoot::AwaitLoad:
            if (g_autoLoadResult == AutoLoadResult::Failed || now >= g_auto.deadline) {
                AbortAuto(g_autoLoadResult == AutoLoadResult::Failed ? "load-failed" : "load-timeout");
            }
            return;
        case AutoBoot::StartDelay:
            if (now < g_auto.deadline) {
                return;
            }
            BeginAuto(g_auto.phase);
            if (g_auto.boot != AutoBoot::Running) {
                return;
            }
            break;
        case AutoBoot::Running: break;
        case AutoBoot::QuitDelay:
            if (const auto reason = AutoAbortReason(RE::PlayerCharacter::GetSingleton(), false); !reason.empty()) {
                AbortAuto(reason);
                return;
            }
            if (now >= g_auto.deadline) {
                if (auto* main = RE::Main::GetSingleton()) {
                    main->GetRuntimeData().quitGame = true;
                    ResetAuto();
                } else {
                    AbortAuto("main-missing");
                }
            }
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto reason = AutoAbortReason(player, g_auto.attempt.state == Attempt::State::Held);
        if (!reason.empty()) {
            AbortAuto(reason);
            return;
        }
        RunAutoPlan(*player);
    }

    class AutoMenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (event && event->opening && event->menuName == kMainMenu && !g_mainMenuSeen.exchange(true)) {
                g_mainMenuPending = true;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    AutoMenuSink g_autoMenuSink;

    class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputSink& Get()
        {
            static InputSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            TryRun("입력", [&] {
                const auto* tasks = SKSE::GetTaskInterface();
                if (!events || !tasks) {
                    return;
                }
                for (auto* event = *events; event; event = event->next) {
                    const auto* button = event->AsButtonEvent();
                    if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) {
                        continue;
                    }
                    const auto key = button->GetIDCode();
                    if (g_settings.autoTest.startKey && key == g_settings.autoTest.startKey) {
                        tasks->AddTask([] {
                            RunAutoSafely([] {
                                if (g_auto.boot != AutoBoot::Idle) {
                                    AbortAuto("start-key");
                                } else if (!MenuIsOpen()) {
                                    BeginAuto(1);
                                }
                            });
                        });
                        continue;
                    }
                    const bool change = g_settings.presetKey && key == g_settings.presetKey;
                    const bool state = g_settings.stateKey && key == g_settings.stateKey;
                    if ((!change && !state) || !g_gameReady || MenuIsOpen()) {
                        continue;
                    }
                    tasks->AddTask([change, state] {
                        TryRun("배정 키", [&] {
                            if (!g_gameReady || g_auto.boot != AutoBoot::Idle || MenuIsOpen()) {
                                return;
                            }
                            if (change) {
                                ChangePreset((g_preset + 1) % kPresets.size());
                            }
                            if (state) {
                                DisplayState();
                            }
                        });
                    });
                }
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void DeliverNotice()
    {
        const auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        const auto now = Clock::now();
        if (!player || !player->Is3DLoaded() || !ui || ui->GameIsPaused() || now < g_nextNotice) {
            return;
        }
        std::string notice;
        {
            const std::scoped_lock lock(g_noticeMutex);
            if (g_notices.empty()) {
                return;
            }
            notice = std::move(g_notices.front());
            g_notices.pop_front();
        }
        g_nextNotice = now + kNoticeInterval;
        ShowNotice(notice);
    }

    // 작업 안에서 다음 작업을 맡기면 SKSE가 같은 틱에 다시 실행한다. 다음 틱은 이 스레드에서만 맡긴다.
    void TickerMain()
    {
        for (;;) {
            std::this_thread::sleep_for(g_autoTick ? kAutoTick : kTick);
            TryRun("틱", [] {
                const auto* tasks = SKSE::GetTaskInterface();
                bool pending = g_loadPending || g_autoTick || g_mainMenuPending;
                {
                    const std::scoped_lock lock(g_noticeMutex);
                    pending = pending || !g_notices.empty();
                }
                if (!tasks || !pending || g_tickQueued.exchange(true)) {
                    return;
                }
                try {
                    tasks->AddTask([] {
                        if (g_loadPending.exchange(false)) {
                            if (!TryRun("불러오기", [] {
                                if (LoadGame()) {
                                    RunAutoSafely(AutoLoaded);
                                } else {
                                    g_loadPending = true;
                                }
                            }) && g_auto.boot == AutoBoot::AwaitLoad) {
                                AbortAuto("load-initialization-exception");
                            }
                        }
                        if (g_autoTick || g_mainMenuPending) {
                            RunAutoSafely(TickAuto);
                        }
                        TryRun("알림", DeliverNotice);
                        g_tickQueued = false;
                    });
                } catch (...) {
                    g_tickQueued = false;
                    throw;
                }
            });
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }
        TryRun("SKSE 메시지", [&] {
            switch (message->type) {
            case SKSE::MessagingInterface::kInputLoaded:
                if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
                    input->AddEventSink(&InputSink::Get());
                } else {
                    SKSE::log::error("입력 장치 관리자를 찾지 못함. 배정 키를 쓸 수 없습니다.");
                }
                break;
            case SKSE::MessagingInterface::kDataLoaded:
                std::call_once(g_installOnce, [] {
                    InstallHooks();
                    if (auto* ui = RE::UI::GetSingleton()) {
                        ui->AddEventSink(&g_autoMenuSink);
                        if (ui->IsMenuOpen(kMainMenu) && !g_mainMenuSeen.exchange(true)) {
                            g_mainMenuPending = true;
                        }
                    } else {
                        SKSE::log::error("UI를 찾지 못함. 자동 시험 표지로 시작할 수 없습니다.");
                    }
                    std::thread(TickerMain).detach();
                });
                break;
            case SKSE::MessagingInterface::kNewGame:
            case SKSE::MessagingInterface::kPostLoadGame:
                if (message->type == SKSE::MessagingInterface::kPostLoadGame && g_autoRequestedLoad) {
                    g_autoLoadResult = message->data ? AutoLoadResult::Success : AutoLoadResult::Failed;
                }
                if (message->type == SKSE::MessagingInterface::kNewGame || message->data) {
                    if (message->type == SKSE::MessagingInterface::kNewGame && g_autoTick) {
                        g_autoInterrupted = true;
                    }
                    g_gameReady = false;
                    g_loadPending = true;
                }
                break;
            case SKSE::MessagingInterface::kPreLoadGame:
                if (g_autoTick && !g_autoRequestedLoad) {
                    g_autoInterrupted = true;
                }
                break;
            default:
                break;
            }
        });
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    if (!InitializeLogging()) {
        return false;
    }
    SKSE::Init(skse, { .log = false });
    g_settings = LoadSettings();
    SKSE::log::info("런타임 {}", skse->RuntimeVersion().string("."));
    const auto& config = g_settings.autoTest;
    SKSE::log::info("설정 StartPreset={} PresetKey=0x{:02X} StateKey=0x{:02X} LogAllPlayerSpells={} CooldownReason={} Notify={}"
        " [AutoTest] StartKey=0x{:02X} MainMenuDelaySeconds={} StartDelaySeconds={} CheckpointSeconds={} MenuHoldSeconds={}"
        " EquipSettleSeconds={} HoldExtraSeconds={} AfterReleaseSeconds={} BarHoldSeconds={} SaveWaitSeconds={} SaveName={} QuitWhenDone={}",
        PresetName(g_settings.startPreset), g_settings.presetKey, g_settings.stateKey, g_settings.logAllPlayerSpells,
        std::to_underlying(g_settings.cooldownReason), g_settings.notify, config.startKey, config.mainMenuDelay,
        config.startDelay, config.checkpoint, config.menuHold, config.equipSettle, config.holdExtra, config.afterRelease,
        config.barHold, config.saveWait, config.saveName, config.quitWhenDone);
    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함");
        return false;
    }
    SKSE::log::info("PoEProtoSpells 준비 완료. 데이터가 로드되면 설치함");
    return true;
}
