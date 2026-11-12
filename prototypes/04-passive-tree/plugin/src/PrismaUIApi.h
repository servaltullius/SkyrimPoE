#pragma once

// Prisma UI의 플러그인용 인터페이스(v1).
// Prisma UI가 배포하는 PrismaUI_API.h("PrismaUI modder API - do whatever you want with it.")의 선언을 옮긴 것이다.
// 가상 함수의 순서가 Prisma UI의 DLL과 같아야 하므로 순서를 바꾸거나 중간에 끼워 넣지 않는다.
// 원본은 <Windows.h>를 직접 쓰지만, 여기서는 CommonLibSSE-NG의 REX::W32를 써서 헤더 충돌을 피한다.

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
        // 뷰를 만든다. htmlPath는 Data/PrismaUI/views/ 아래의 경로.
        virtual PrismaView CreateView(const char* htmlPath, OnDomReadyCallback onDomReadyCallback = nullptr) noexcept = 0;
        // 뷰에서 JS 코드를 실행한다.
        virtual void Invoke(PrismaView view, const char* script, JSCallback callback = nullptr) noexcept = 0;
        // 뷰의 전역 JS 함수를 문자열 인자 하나로 부른다.
        virtual void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept = 0;
        // 뷰에 전역 JS 함수를 만들어, 화면이 그것을 부르면 callback이 불리게 한다.
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

    // PrismaUI.dll이 로드돼 있으면 인터페이스를 돌려준다. 없으면 nullptr.
    // SKSE의 kPostLoad 이후에 불러야 한다.
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
