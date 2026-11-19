#pragma once

// Prisma UI 플러그인용 인터페이스(v1).
// 프로토타입 4(prototypes/04-passive-tree/plugin/src/PrismaUIApi.h)에서 AE 1.6.1170 동작 검증 완료.
// 가상 함수 순서는 PrismaUI.dll과 반드시 일치해야 한다.

#include <cstdint>

namespace PRISMA_UI_API
{
    using PrismaView = std::uint64_t;

    enum class InterfaceVersion : std::uint8_t
    {
        V1,
    };

    using OnDomReadyCallback = void (*)(PrismaView view);
    using JSCallback = void (*)(const char* result);
    using JSListenerCallback = void (*)(const char* argument);

    class IVPrismaUI1
    {
    public:
        virtual PrismaView CreateView(const char* htmlPath, OnDomReadyCallback onDomReadyCallback = nullptr) noexcept = 0;
        virtual void Invoke(PrismaView view, const char* script, JSCallback callback = nullptr) noexcept = 0;
        virtual void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept = 0;
        virtual void RegisterJSListener(PrismaView view, const char* functionName, JSListenerCallback callback) noexcept = 0;
        virtual bool HasFocus(PrismaView view) noexcept = 0;
        virtual bool Focus(PrismaView view, bool pauseGame = false, bool disableFocusMenu = false) noexcept = 0;
        virtual void Unfocus(PrismaView view) noexcept = 0;
        virtual void Show(PrismaView view) noexcept = 0;
        virtual void Hide(PrismaView view) noexcept = 0;
        virtual bool IsHidden(PrismaView view) noexcept = 0;
        virtual int GetScrollingPixelSize(PrismaView view) noexcept = 0;
        virtual void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept = 0;
        virtual bool IsValid(PrismaView view) noexcept = 0;
        virtual void Destroy(PrismaView view) noexcept = 0;
        virtual void SetOrder(PrismaView view, int order) noexcept = 0;
        virtual int GetOrder(PrismaView view) noexcept = 0;
        virtual void CreateInspectorView(PrismaView view) noexcept = 0;
        virtual void SetInspectorVisibility(PrismaView view, bool visible) noexcept = 0;
        virtual bool IsInspectorVisible(PrismaView view) noexcept = 0;
        virtual void SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned int width,
            unsigned int height) noexcept = 0;
        virtual bool HasAnyActiveFocus() noexcept = 0;
    };

    [[nodiscard]] inline IVPrismaUI1* RequestPluginAPI(InterfaceVersion version = InterfaceVersion::V1)
    {
        const auto module = REX::W32::GetModuleHandleW(L"PrismaUI.dll");
        if (!module) {
            return nullptr;
        }
        using RequestFn = void* (*)(InterfaceVersion);
        const auto request = reinterpret_cast<RequestFn>(REX::W32::GetProcAddress(module, "RequestPluginAPI"));
        if (!request) {
            return nullptr;
        }
        return static_cast<IVPrismaUI1*>(request(version));
    }
}
