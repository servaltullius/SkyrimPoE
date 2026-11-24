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
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

        // A~G의 저장 왕복, H의 이동·셀 초기화, 새 포털/경로 시험을 포함한 벽시계 예산.
        // 2026-11-23 실측: H5~H7이 끝까지 돌면 H23 정리 20회까지 약 25~28분이 걸리고, 그 뒤 H26·H27 착지 탐침(17곳)과 H8이 남는다.
        inline constexpr double kDefaultAutoTestTimeoutSec = 2700.0;
        inline constexpr double kMaxAutoTestTimeoutSec = 7200.0;

        struct AutoTestConfig
        {
            std::string baselineSave;
            std::string tempSave = "SkyrimPoE_autotest";
            bool quitWhenDone = true;
            bool consumeMarker = true;
            double globalTimeoutSec = kDefaultAutoTestTimeoutSec;
            // 조사용 단축 실행. "h27"이면 기준 세이브를 불러온 뒤 곧바로 경로 탐침(H26·H27)만 돌고 끝낸다(앞 시나리오 상태 없이).
            bool routeOnly = false;
            // 결정 185 확인용 단축 실행. "es"이면 기준 세이브를 불러온 뒤 적 ES 표시 시험(ES1~ES5)만 돌고 끝낸다.
            bool enemyEsOnly = false;
            bool saveOnly = false;
            bool cleanupOnly = false;
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

        class CleanupLoadRequest
        {
        public:
            void Expect(std::string_view stem) { expected_ = stem; }
            void Clear() noexcept { expected_.clear(); }

            [[nodiscard]] bool Match(std::string_view name) noexcept
            {
                const auto expected = std::exchange(expected_, {});
                try {
                    return !expected.empty() && SameSaveStem(expected, StripSaveStem(name));
                } catch (const std::exception&) {
                    return false;
                }
            }

        private:
            std::string expected_;
        };

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
                    out.baselineSave = StripSaveStem(val);
                } else if (key == "temp_save") {
                    const auto stripped = StripSaveStem(val);
                    if (!stripped.empty()) {
                        out.tempSave = stripped;
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
                    if (mode != "h27" && mode != "es" && mode != "save" && mode != "cleanup") {
                        return std::nullopt;
                    }
                    if ((out.cleanupOnly && mode != "cleanup") ||
                        (mode == "cleanup" && (out.routeOnly || out.enemyEsOnly || out.saveOnly))) {
                        return std::nullopt;
                    }
                    out.routeOnly = mode == "h27";
                    out.enemyEsOnly = mode == "es";
                    out.saveOnly = mode == "save";
                    out.cleanupOnly = mode == "cleanup";
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
            if (out.cleanupOnly) {
                if (!out.quitWhenDone) {
                    return std::nullopt;
                }
                try {
                    if (SameSaveStem(out.baselineSave, out.tempSave) ||
                        SameSaveStem(out.baselineSave, out.tempSave + kCleanupFinalSuffix)) {
                        return std::nullopt;
                    }
                } catch (const std::exception&) {
                    // Save names in a marker must be representable as Windows filenames.
                    return std::nullopt;
                }
            }
            // T-5: 기존 임시 세이브·H 체크포인트 충돌 검사.
            if (!out.baselineSave.empty() && !out.tempSave.empty() &&
                (ToLowerAscii(out.baselineSave) == ToLowerAscii(out.tempSave) ||
                    ToLowerAscii(out.baselineSave) == ToLowerAscii(out.tempSave + kHCheckpointSuffix))) {
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
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::ItemData* itemData,
            Core::Config* config,
            Core::SkillSystem* skillSystem = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        [[nodiscard]] static bool IsEnabled() noexcept;
        [[nodiscard]] static bool IsCleanupOnly() noexcept;

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
