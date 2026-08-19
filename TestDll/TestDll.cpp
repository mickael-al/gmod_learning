#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#ifndef _WIN64
#error This test targets the 64-bit Garrys Mod client.
#endif

namespace
{
    struct Vector
    {
        float x;
        float y;
        float z;
    };

    using QAngle = Vector;

    struct VMatrix
    {
        float value[4][4];
    };

    enum class StereoEye : int
    {
        Mono = 0,
        Left,
        Right,
        Count
    };

    // Layout from the current GMod-compatible sourcesdk-minimal/view_shared.h.
    struct CViewSetup
    {
        int x;
        int unscaledX;
        int y;
        int unscaledY;
        int width;
        int unscaledWidth;
        int height;
        StereoEye stereoEye;
        int unscaledHeight;
        bool ortho;
        float orthoLeft;
        float orthoTop;
        float orthoRight;
        float orthoBottom;
        float fov;
        float viewModelFov;
        Vector origin;
        QAngle angles;
        float zNear;
        float zFar;
        float viewModelZNear;
        float viewModelZFar;
        bool renderToSubrectOfLargerScreen;
        float aspectRatio;
        bool offCenter;
        float offCenterTop;
        float offCenterBottom;
        float offCenterLeft;
        float offCenterRight;
        bool doBloomAndToneMapping;
        bool cacheFullSceneState;
        bool viewToProjectionOverride;
        VMatrix viewToProjection;
    };

    static_assert(
        sizeof(CViewSetup) == 200,
        "CViewSetup must match the current 64-bit Garry's Mod SDK layout.");

    enum class ViewMode : std::uint8_t
    {
        Off,
        Rear,
        Left
    };

    struct VRect
    {
        int x;
        int y;
        int width;
        int height;
        VRect* next;
    };

    using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);
    using ViewRenderFn = void(__fastcall*)(void*, VRect*);
    using RenderViewFn = void(__fastcall*)(void*, const CViewSetup&, int, int);
    using GetPlayerViewFn = bool(__fastcall*)(void*, CViewSetup&);

    constexpr std::size_t kViewRenderIndex = 26;
    constexpr std::size_t kRenderViewIndex = 27;
    constexpr std::size_t kGetPlayerViewIndex = 59;

    constexpr int kViewClearColor = 1 << 0;
    constexpr int kViewClearDepth = 1 << 1;
    constexpr int kViewClearStencil = 1 << 5;

    constexpr int kRenderViewSuppressMonitorRendering = 1 << 2;

    // Rear view is enabled by default for the first runtime diagnostic.
    std::atomic<ViewMode> g_viewMode{ ViewMode::Rear };
    std::atomic<std::uint64_t> g_viewRenderCalls{ 0 };
    ViewRenderFn g_originalViewRender = nullptr;
    RenderViewFn g_renderView = nullptr;
    GetPlayerViewFn g_getPlayerView = nullptr;
    void** g_viewRenderSlot = nullptr;

    void UpdateViewMode()
    {
        // Same order as the Lua script: MOUSE_5 overrides MOUSE_4,
        // then the middle button overrides both.
        if ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0)
            g_viewMode.store(ViewMode::Rear, std::memory_order_relaxed);

        if ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0)
            g_viewMode.store(ViewMode::Off, std::memory_order_relaxed);

        if ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0)
            g_viewMode.store(ViewMode::Left, std::memory_order_relaxed);
    }

    float NormalizeYaw(float yaw)
    {
        while (yaw > 180.0f)
            yaw -= 360.0f;
        while (yaw < -180.0f)
            yaw += 360.0f;
        return yaw;
    }

    void __fastcall HookedViewRender(void* client, VRect* rect)
    {
        const ViewRenderFn original = g_originalViewRender;
        const RenderViewFn renderView = g_renderView;
        const GetPlayerViewFn getPlayerView = g_getPlayerView;
        if (original == nullptr || renderView == nullptr || getPlayerView == nullptr)
            return;

        original(client, rect);
        g_viewRenderCalls.fetch_add(1, std::memory_order_relaxed);

        if (rect == nullptr || rect->width <= 0 || rect->height <= 0)
            return;

        UpdateViewMode();
        const ViewMode mode = g_viewMode.load(std::memory_order_relaxed);
        if (mode == ViewMode::Off)
            return;

        CViewSetup secondary{};
        if (!getPlayerView(client, secondary))
            return;

        secondary.x = 275;
        secondary.unscaledX = secondary.x;
        secondary.y = 535;
        secondary.unscaledY = secondary.y;
        secondary.width = (std::max)(1, static_cast<int>(secondary.width / 1.4f));
        secondary.unscaledWidth = secondary.width;
        secondary.height = (std::max)(1, secondary.height / 2);
        secondary.unscaledHeight = secondary.height;
        secondary.stereoEye = StereoEye::Mono;
        secondary.renderToSubrectOfLargerScreen = true;
        secondary.aspectRatio = 0.0f;
        secondary.angles.y = NormalizeYaw(
            secondary.angles.y + (mode == ViewMode::Rear ? -180.0f : -90.0f));

        // Do not draw the first-person hands or a second HUD in the inset.
        const int secondaryClearFlags =
            kViewClearColor | kViewClearDepth | kViewClearStencil;

        renderView(
            client,
            secondary,
            secondaryClearFlags,
            kRenderViewSuppressMonitorRendering);
    }

    bool ReplaceVTableEntry(void** slot, void* expected, void* replacement)
    {
        DWORD oldProtection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;

        void* previous = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(slot), replacement, expected);

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        return previous == expected;
    }

    bool InstallHook()
    {
        const HMODULE clientModule = GetModuleHandleW(L"client.dll");
        if (clientModule == nullptr)
            return false;

        const auto createInterface = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(clientModule, "CreateInterface"));
        if (createInterface == nullptr)
            return false;

        void* client = createInterface("VClient017", nullptr);
        if (client == nullptr)
            return false;

        void** vtable = *reinterpret_cast<void***>(client);
        if (vtable == nullptr)
            return false;

        g_viewRenderSlot = &vtable[kViewRenderIndex];
        void* original = *g_viewRenderSlot;
        g_renderView = reinterpret_cast<RenderViewFn>(vtable[kRenderViewIndex]);
        g_getPlayerView =
            reinterpret_cast<GetPlayerViewFn>(vtable[kGetPlayerViewIndex]);
        if (original == nullptr || g_renderView == nullptr || g_getPlayerView == nullptr)
        {
            g_viewRenderSlot = nullptr;
            g_renderView = nullptr;
            g_getPlayerView = nullptr;
            return false;
        }

        // Publish the original before installing the hook so a render thread
        // can never observe the hook with a null forwarding target.
        g_originalViewRender = reinterpret_cast<ViewRenderFn>(original);
        if (!ReplaceVTableEntry(
                g_viewRenderSlot,
                original,
                reinterpret_cast<void*>(&HookedViewRender)))
        {
            g_viewRenderSlot = nullptr;
            g_originalViewRender = nullptr;
            g_renderView = nullptr;
            g_getPlayerView = nullptr;
            return false;
        }
        return true;
    }

    void RemoveHook()
    {
        if (g_viewRenderSlot == nullptr || g_originalViewRender == nullptr)
            return;

        DWORD oldProtection = 0;
        if (!VirtualProtect(
                g_viewRenderSlot,
                sizeof(void*),
                PAGE_EXECUTE_READWRITE,
                &oldProtection))
        {
            return;
        }

        if (*g_viewRenderSlot == reinterpret_cast<void*>(&HookedViewRender))
        {
            InterlockedExchangePointer(
                reinterpret_cast<void* volatile*>(g_viewRenderSlot),
                reinterpret_cast<void*>(g_originalViewRender));
        }

        DWORD ignored = 0;
        VirtualProtect(
            g_viewRenderSlot, sizeof(void*), oldProtection, &ignored);
        FlushInstructionCache(
            GetCurrentProcess(), g_viewRenderSlot, sizeof(void*));

        g_viewRenderSlot = nullptr;
        g_originalViewRender = nullptr;
        g_renderView = nullptr;
        g_getPlayerView = nullptr;
    }

    DWORD WINAPI Initialize(void*)
    {
        if (InstallHook())
        {
            MessageBoxA(
                nullptr,
                "VClient017 a ete trouve et l'entree View_Render 26 a ete accrochee.",
                "TestDll - etape 1/2",
                MB_OK | MB_ICONINFORMATION);

            Sleep(250);
            if (g_viewRenderCalls.load(std::memory_order_relaxed) != 0)
            {
                MessageBoxA(
                    nullptr,
                    "Le hook View_Render recoit bien les appels du moteur.\n"
                    "La vue arriere est activee par defaut pour ce test.",
                    "TestDll - etape 2/2",
                    MB_OK | MB_ICONINFORMATION);
            }
            else
            {
                MessageBoxA(
                    nullptr,
                    "La vtable a ete modifiee, mais View_Render n'a recu aucun appel.",
                    "TestDll - diagnostic",
                    MB_OK | MB_ICONWARNING);
            }
        }
        else
        {
            MessageBoxA(
                nullptr,
                "Echec de l'installation du hook VClient017::View_Render.",
                "TestDll - erreur",
                MB_OK | MB_ICONERROR);
        }
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        RemoveHook();
    }

    return TRUE;
}
