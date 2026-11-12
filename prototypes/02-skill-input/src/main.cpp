#include "Core/Config.h"
#include "Runtime/Channel.h"
#include "Runtime/Hooks.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <vector>

namespace
{
    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoSkillInput.ini";
    constexpr auto kOarModule = L"OpenAnimationReplacer.dll";
    constexpr auto kInjectorModule = L"AnimationInjector.dll";

    bool InitializeLogging()
    {
        auto directory = SKSE::log::log_directory();
        if (!directory) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(*directory, error);
        if (error) {
            return false;
        }
        try {
            const auto path = *directory / "PoEProtoSkillInput.log";
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto logger = std::make_shared<spdlog::logger>("poe_proto_skill_input", std::move(sink));
            spdlog::set_default_logger(std::move(logger));
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::info);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }
        switch (message->type) {
        case SKSE::MessagingInterface::kInputLoaded:
            poe::InstallInputSink();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            {
                const bool oar = REX::W32::GetModuleHandleW(kOarModule) != nullptr;
                const bool injector = REX::W32::GetModuleHandleW(kInjectorModule) != nullptr;
                poe::Channel::Get().SetBackends(oar, injector);
                SKSE::log::info("Open Animation Replacer: {}", oar ? "로드됨" : "없음(oar 방식은 시작하지 않음)");
                SKSE::log::info("Animation Injector: {}", injector ? "로드됨" : "없음(injector 방식은 시작하지 않음)");
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            poe::Channel::Get().OnLoad(false);
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            poe::Channel::Get().OnLoad(true);
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    InitializeLogging();

    std::vector<std::string> warnings;
    const auto config = poe::LoadConfig(std::filesystem::path(kConfigPath), warnings);
    for (const auto& warning : warnings) {
        SKSE::log::warn("설정: {}", warning);
    }
    SKSE::log::info("설정: Enabled={} Motion={} Trigger={} HoldSeconds={:.2f} MaxSeconds={:.0f} CycleKey=0x{:02X} "
                    "ExtraChannelKey=0x{:02X} LogEvents={}",
        config.enabled, poe::MotionName(config.motion), poe::TriggerName(config.trigger), config.holdSeconds,
        config.maxSeconds, config.cycleKey, config.extraChannelKey, config.logEvents);

    if (!config.enabled) {
        SKSE::log::info("INI에서 꺼져 있어 아무 훅도 설치하지 않음");
        return true;
    }

    poe::Channel::Get().Configure(config);

    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함. 입력을 읽을 수 없어 훅을 설치하지 않습니다.");
        return true;
    }

    poe::InstallPlayerHooks();
    SKSE::log::info("PoEProtoSkillInput 준비 완료");
    return true;
}
