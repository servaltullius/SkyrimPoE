#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/ItemData.h"
#include "core/KillTracker.h"
#include "core/PassiveTreeData.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RE { class Main; }

// Keep the marker parser testable without loading the game or including CommonLib.
extern "C" __declspec(dllimport) int __stdcall CompareStringOrdinal(
    const wchar_t* left, int leftLength, const wchar_t* right, int rightLength, int ignoreCase);

namespace SkyrimPoE::Runtime
{
    namespace AutoTestMarker
    {
        inline constexpr auto kHCheckpointSuffix = "_h_restore";
        inline constexpr auto kCleanupFinalSuffix = "_final";
        inline constexpr auto kPreserveOutSuffix = "_out";
        inline constexpr std::size_t kMaxPreserveSaves = 16;

        // A~G의 저장 왕복, H의 이동·셀 초기화, 새 포털/경로 시험을 포함한 벽시계 예산.
        // 2026-11-23 실측: H5~H7이 끝까지 돌면 H23 정리 20회까지 약 25~28분이 걸리고, 그 뒤 H26·H27 착지 탐침(17곳)과 H8이 남는다.
        inline constexpr double kDefaultAutoTestTimeoutSec = 2700.0;
        inline constexpr double kMaxAutoTestTimeoutSec = 7200.0;

        struct AutoTestConfig
        {
            std::string baselineSave;
            std::string tempSave = "SkyrimPoE_autotest";
            std::string outSave;
            bool quitWhenDone = true;
            bool consumeMarker = true;
            double globalTimeoutSec = kDefaultAutoTestTimeoutSec;
            // 조사용 단축 실행. "h27"이면 기준 세이브를 불러온 뒤 곧바로 경로 탐침(H26·H27)만 돌고 끝낸다(앞 시나리오 상태 없이).
            bool routeOnly = false;
            // 결정 185 확인용 단축 실행. "es"이면 기준 세이브를 불러온 뒤 적 ES 표시 시험(ES1~ES5)만 돌고 끝낸다.
            bool enemyEsOnly = false;
            // 결정 236~238 확인용 단축 실행("react"): 스킬 타격 반응(적중 이벤트·전투·움찔·강한 기절·폭행 경보)만 본다.
            bool reactOnly = false;
            bool saveOnly = false;
            bool cleanupOnly = false;
            bool gemsOnly = false;
            // "firstinstall": 모드 기록 없는 세이브로 세대 1 패키지에 처음 들어가는 경로.
            bool firstInstall = false;
            // "newgame": 메인 메뉴 StartNewGame 콜백으로 세대 1 새 게임 진입, 패널을 처음 열 때의 시작 지급(결정 247·263, C6).
            bool newGameOnly = false;
            // "preserve": 보존 음성·세대 불일치 세이브 목록을 차례로 불러와 보존·무정리·무지급·<stem>_out 저장 확인(결정 256).
            bool preserveOnly = false;
            // "gen1gate": 세대 1 게이트(C5·결정 248·249·259)와 세대 0 양성 대조.
            bool gen1GateOnly = false;
            std::vector<std::string> preserveSaves;

            [[nodiscard]] std::string EffectiveOutSave() const
            {
                if (!outSave.empty()) {
                    return outSave;
                }
                if (cleanupOnly || gemsOnly || firstInstall) {
                    return tempSave + kCleanupFinalSuffix;
                }
                return tempSave;
            }

            [[nodiscard]] bool UsesTestSession() const noexcept
            {
                return cleanupOnly || firstInstall || saveOnly || gemsOnly || enemyEsOnly || reactOnly ||
                    newGameOnly || preserveOnly || gen1GateOnly;
            }
        };

        inline std::string Trim(std::string_view sv)
        {
            std::size_t b = 0;
            while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) {
                ++b;
            }
            std::size_t e = sv.size();
            while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) {
                --e;
            }
            return std::string(sv.substr(b, e - b));
        }

        inline std::string ToLowerAscii(std::string_view sv)
        {
            std::string out(sv);
            std::transform(out.begin(), out.end(), out.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return out;
        }

        // 세이브 이름 안의 점(.)을 보존하면서 경로 접두사와 끝의 ".ess"만 대소문자 무관하게 떼어낸다.
        inline std::string StripSaveStem(std::string_view raw)
        {
            while (!raw.empty() && raw.back() == '\0') {
                raw.remove_suffix(1);
            }
            std::string s = Trim(raw);
            if (const auto slash = s.find_last_of("\\/"); slash != std::string::npos) {
                s = s.substr(slash + 1);
            }
            if (s.size() >= 4 && ToLowerAscii(std::string_view(s).substr(s.size() - 4)) == ".ess") {
                s.resize(s.size() - 4);
            }
            return s;
        }

        inline bool ParseBool(std::string_view sv, bool fallback)
        {
            const auto low = ToLowerAscii(Trim(sv));
            if (low == "true" || low == "1" || low == "yes" || low == "on") return true;
            if (low == "false" || low == "0" || low == "no" || low == "off") return false;
            return fallback;
        }

        inline bool SameSaveStem(std::string_view left, std::string_view right)
        {
            if (left.empty() || right.empty()) {
                return left == right;
            }
            const std::filesystem::path leftName(std::u8string(left.begin(), left.end()));
            const std::filesystem::path rightName(std::u8string(right.begin(), right.end()));
            constexpr int ordinalEqual = 2;
            return CompareStringOrdinal(leftName.c_str(), -1, rightName.c_str(), -1, 1) == ordinalEqual;
        }

        enum class AutoSaveCollection : std::uint8_t { GameSetting, IniPref, IniSetting };

        struct AutoSaveEntry
        {
            AutoSaveCollection collection = AutoSaveCollection::IniSetting;
            std::string name;
            bool* pointer = nullptr;
            bool original = false;
        };

        class AutoSaveSettings
        {
        public:
            void Set(AutoSaveCollection collection, std::string_view name, bool& value, bool temporary)
            {
                const bool sameKey = std::ranges::any_of(entries_, [&](const AutoSaveEntry& e) {
                    return e.collection == collection && e.name == name && e.pointer == &value;
                });
                if (!sameKey) {
                    bool original = value;
                    if (const auto it = std::ranges::find(entries_, &value, &AutoSaveEntry::pointer); it != entries_.end()) {
                        original = it->original;
                    }
                    entries_.push_back({ collection, std::string(name), &value, original });
                }
                value = temporary;
            }
            template <class Reader>
            [[nodiscard]] bool RestoreAndVerify(Reader&& readFromCollection, std::size_t& countOut,
                std::string* mismatchDetail = nullptr)
            {
                countOut = entries_.size();
                for (const auto& entry : entries_) {
                    if (entry.pointer) {
                        *entry.pointer = entry.original;
                    }
                }
                bool ok = true;
                for (const auto& entry : entries_) {
                    const std::optional<bool> observed = readFromCollection(entry.collection, std::string_view(entry.name));
                    if (!observed || *observed != entry.original) {
                        ok = false;
                        if (mismatchDetail) {
                            if (!mismatchDetail->empty()) {
                                *mismatchDetail += ", ";
                            }
                            *mismatchDetail += std::format("{}[{}](orig={}, read={})",
                                entry.name, static_cast<unsigned>(entry.collection), entry.original,
                                observed ? (*observed ? "true" : "false") : "missing");
                        }
                    }
                }
                entries_.clear();
                return ok;
            }
            [[nodiscard]] std::size_t CapturedCount() const noexcept
            {
                return entries_.size();
            }

        private:
            std::vector<AutoSaveEntry> entries_;
        };

        class GemsRetry
        {
        public:
            enum class Action { Wait, Request, Exhausted };

            explicit GemsRetry(unsigned limit) noexcept : limit_(limit) {}

            [[nodiscard]] Action Poll(double elapsed, double interval = 2.0) noexcept
            {
                if (elapsed < next_) return Action::Wait;
                if (attempts_ == limit_) return Action::Exhausted;
                ++attempts_;
                next_ = elapsed + interval;
                return Action::Request;
            }

            [[nodiscard]] unsigned Attempts() const noexcept { return attempts_; }
            [[nodiscard]] std::string ExhaustionMessage(std::string_view action) const
            {
                return std::format("{} did not complete after {} requests.", action, attempts_);
            }

        private:
            unsigned limit_;
            unsigned attempts_ = 0;
            double next_ = 0.0;
        };

        // The process slot may be empty while the game stores a spell in its
        // selected-spell slot. A different right-hand object still blocks the cast.
        [[nodiscard]] constexpr bool IsGemsProxyInHand(bool hasRightObject, bool rightObjectIsProxy,
            bool rightSelectedIsProxy) noexcept
        {
            return hasRightObject ? rightObjectIsProxy : rightSelectedIsProxy;
        }

        inline bool IsCheckpointStem(std::string_view stem, std::string_view temporary)
        {
            const auto prefix = std::string(temporary) + kHCheckpointSuffix;
            if (stem.size() < prefix.size() || !SameSaveStem(stem.substr(0, prefix.size()), prefix)) {
                return false;
            }
            const auto suffix = stem.substr(prefix.size());
            if (suffix.empty()) return true;
            if (suffix.front() != '_') return false;
            const auto split = suffix.find('_', 1);
            if (split == std::string_view::npos) return false;
            const auto digits = [](std::string_view value) {
                return !value.empty() && value.find_first_not_of("0123456789") == std::string_view::npos;
            };
            return digits(suffix.substr(1, split - 1)) && digits(suffix.substr(split + 1));
        }

        inline bool HasInvalidSaveStemChars(std::string_view sv) noexcept
        {
            for (const unsigned char ch : sv) {
                if (ch < 0x20u || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
                    ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
                    return true;
                }
            }
            return sv.find("..") != std::string_view::npos;
        }

        inline std::optional<AutoTestConfig> ParseMarker(std::istream& input)
        {
            AutoTestConfig out;
            std::string line;
            bool firstLine = true;
            while (std::getline(input, line)) {
                if (firstLine) {
                    firstLine = false;
                    // UTF-8 BOM 제거
                    if (line.size() >= 3 &&
                        static_cast<unsigned char>(line[0]) == 0xEF &&
                        static_cast<unsigned char>(line[1]) == 0xBB &&
                        static_cast<unsigned char>(line[2]) == 0xBF) {
                        line.erase(0, 3);
                    }
                }
                const auto trimmed = Trim(line);
                if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
                    continue;
                }
                const auto eq = trimmed.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const auto key = ToLowerAscii(Trim(std::string_view(trimmed).substr(0, eq)));
                const auto val = Trim(std::string_view(trimmed).substr(eq + 1));
                if (key == "save") {
                    if (HasInvalidSaveStemChars(val)) {
                        return std::nullopt;
                    }
                    out.baselineSave = StripSaveStem(val);
                } else if (key == "temp_save") {
                    if (HasInvalidSaveStemChars(val)) {
                        return std::nullopt;
                    }
                    const auto stripped = StripSaveStem(val);
                    if (!stripped.empty()) {
                        out.tempSave = stripped;
                    }
                } else if (key == "out_save") {
                    if (HasInvalidSaveStemChars(val)) {
                        return std::nullopt;
                    }
                    const auto stripped = StripSaveStem(val);
                    if (!stripped.empty()) {
                        out.outSave = stripped;
                    }
                } else if (key == "preserve_saves") {
                    std::size_t start = 0;
                    while (start <= val.size()) {
                        const auto comma = val.find(',', start);
                        const auto rawItem = comma == std::string_view::npos
                            ? val.substr(start) : val.substr(start, comma - start);
                        const auto trimmedItem = Trim(rawItem);
                        if (trimmedItem.empty() || HasInvalidSaveStemChars(trimmedItem)) {
                            return std::nullopt;
                        }
                        const auto stripped = StripSaveStem(trimmedItem);
                        if (stripped.empty()) {
                            return std::nullopt;
                        }
                        out.preserveSaves.push_back(stripped);
                        if (comma == std::string_view::npos) {
                            break;
                        }
                        start = comma + 1;
                    }
                } else if (key == "quit") {
                    out.quitWhenDone = ParseBool(val, out.quitWhenDone);
                } else if (key == "consume_marker") {
                    out.consumeMarker = ParseBool(val, out.consumeMarker);
                } else if (key == "only") {
                    const auto mode = ToLowerAscii(val);
                    if (mode.empty()) {
                        continue;
                    }
                    if (mode != "h27" && mode != "es" && mode != "save" && mode != "cleanup" && mode != "gems" &&
                        mode != "firstinstall" && mode != "react" && mode != "newgame" && mode != "preserve" &&
                        mode != "gen1gate") {
                        return std::nullopt;
                    }
                    const bool anyOther =
                        (out.routeOnly && mode != "h27") ||
                        (out.enemyEsOnly && mode != "es") ||
                        (out.reactOnly && mode != "react") ||
                        (out.saveOnly && mode != "save") ||
                        (out.cleanupOnly && mode != "cleanup") ||
                        (out.gemsOnly && mode != "gems") ||
                        (out.firstInstall && mode != "firstinstall") ||
                        (out.newGameOnly && mode != "newgame") ||
                        (out.preserveOnly && mode != "preserve") ||
                        (out.gen1GateOnly && mode != "gen1gate");
                    if (anyOther) {
                        return std::nullopt;
                    }
                    out.routeOnly = mode == "h27";
                    out.enemyEsOnly = mode == "es";
                    out.reactOnly = mode == "react";
                    out.saveOnly = mode == "save";
                    out.cleanupOnly = mode == "cleanup";
                    out.gemsOnly = mode == "gems";
                    out.firstInstall = mode == "firstinstall";
                    out.newGameOnly = mode == "newgame";
                    out.preserveOnly = mode == "preserve";
                    out.gen1GateOnly = mode == "gen1gate";
                } else if (key == "timeout_sec") {
                    try {
                        const double sec = std::stod(val);
                        // 기본값(kDefaultAutoTestTimeoutSec)보다 넉넉히 늘릴 수 있게 상한을 둔다. tools/autotest.ps1과 같은 범위.
                        if (std::isfinite(sec) && sec >= 30.0 && sec <= kMaxAutoTestTimeoutSec) {
                            out.globalTimeoutSec = sec;
                        }
                    } catch (...) {
                    }
                }
            }
            if (out.UsesTestSession() && !out.quitWhenDone) {
                return std::nullopt;
            }
            // A4: 입력 없음 허용은 newgame에서만. preserve는 preserve_saves, 그 외 시험 세션 모드는 save= 필수.
            if (out.UsesTestSession() && !out.newGameOnly && !out.preserveOnly && out.baselineSave.empty()) {
                return std::nullopt;
            }
            if (out.newGameOnly) {
                if (!out.baselineSave.empty() || out.outSave.empty() || !out.preserveSaves.empty()) {
                    return std::nullopt;
                }
            } else if (out.preserveOnly) {
                if (!out.baselineSave.empty() || !out.outSave.empty() ||
                    out.preserveSaves.empty() || out.preserveSaves.size() > kMaxPreserveSaves) {
                    return std::nullopt;
                }
            } else if (!out.preserveSaves.empty()) {
                return std::nullopt;
            }
            try {
                if (!out.baselineSave.empty()) {
                    (void)SameSaveStem(out.baselineSave, out.baselineSave);
                }
                if (!out.tempSave.empty()) {
                    (void)SameSaveStem(out.tempSave, out.tempSave);
                }
                if (!out.outSave.empty()) {
                    (void)SameSaveStem(out.outSave, out.outSave);
                }
                if (out.preserveOnly) {
                    for (std::size_t i = 0; i < out.preserveSaves.size(); ++i) {
                        const auto& a = out.preserveSaves[i];
                        (void)SameSaveStem(a, a);
                        const auto aOut = a + kPreserveOutSuffix;
                        (void)SameSaveStem(aOut, aOut);
                        for (std::size_t j = 0; j < out.preserveSaves.size(); ++j) {
                            const auto& b = out.preserveSaves[j];
                            if (i < j && SameSaveStem(a, b)) {
                                return std::nullopt;
                            }
                            if (SameSaveStem(b, aOut)) {
                                return std::nullopt;
                            }
                        }
                    }
                }
                if (!out.baselineSave.empty()) {
                    if (!out.tempSave.empty() && SameSaveStem(out.baselineSave, out.tempSave)) {
                        return std::nullopt;
                    }
                    if (!out.outSave.empty() && SameSaveStem(out.baselineSave, out.outSave)) {
                        return std::nullopt;
                    }
                    if ((out.cleanupOnly || out.gemsOnly || out.firstInstall) &&
                        SameSaveStem(out.baselineSave, out.EffectiveOutSave())) {
                        return std::nullopt;
                    }
                    if (!out.tempSave.empty() && IsCheckpointStem(out.baselineSave, out.tempSave)) {
                        return std::nullopt;
                    }
                }
            } catch (const std::exception&) {
                // Save names in a marker must be representable as Windows filenames.
                return std::nullopt;
            }
            return out;
        }
    }

    // 마커 파일(<SKSE 로그 폴더>/SkyrimPoE.autotest)이 있을 때만 활성화되는
    // 2~5단계 시나리오 A·B·C·D·E·F 자동 통합 시험 하네스(총 45건).
    // 마커 파일이 없으면 Install에서 비활성화되어 일반 플레이에 영향과 비용이 0이다.
    class AutoTest
    {
    public:
        static void Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            Core::EnergyShieldPool* esPool,
            Core::KillTracker* killTracker,
            Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::ItemData* itemData,
            Core::Config* config,
            Core::SkillSystem* skillSystem = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        [[nodiscard]] static bool IsEnabled() noexcept;
        [[nodiscard]] static bool UsesTestSession() noexcept;

        // 주 갱신 스레드 프레임 콜백에서 호출(비활성 시 즉시 반환)
        static void Tick(RE::Main* main, double deltaSec);
        // 모드가 멈춘 프레임에서도 세이브 시험의 결과 보고와 종료를 처리한다.
        static void TickSaveOnlyWhileSuspended(RE::Main* main);

        // SKSE 메시징 콜백에서 호출(비활성 시 즉시 반환, 원자/뮤텍스 플래그만 갱신)
        template <class Message>
        static void OnPreLoadGame(const Message& msg)
        {
            OnPreLoadGame(msg.data ? std::string_view(static_cast<const char*>(msg.data), msg.dataLen) : std::string_view{});
        }
        static void OnPostLoadGame(bool succeeded);
        static void OnNewGame();
        template <class Message>
        static void OnSaveGame(const Message& msg)
        {
            OnSaveGame(msg.data ? std::string_view(static_cast<const char*>(msg.data), msg.dataLen) : std::string_view{});
        }

    private:
        static void OnPreLoadGame(std::string_view saveName);
        static void OnSaveGame(std::string_view saveName);
    };
}
