#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#ifndef _WIN64
#error This test targets the 64-bit Garry's Mod client.
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

    // Layout copied from ExempleSDK/client/CViewSetup.h.
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

    using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);
    using RenderViewFn = void(__fastcall*)(void*, const CViewSetup&, int, int);

    constexpr std::size_t kRenderViewIndex = 27;

    constexpr int kViewClearColor = 1 << 0;
    constexpr int kViewClearDepth = 1 << 1;
    constexpr int kViewClearStencil = 1 << 5;

    constexpr int kRenderViewDrawViewModel = 1 << 0;
    constexpr int kRenderViewDrawHud = 1 << 1;

    // Rear view is enabled by default for the first runtime diagnostic.
    std::atomic<ViewMode> g_viewMode{ ViewMode::Rear };
    std::atomic<std::uint64_t> g_renderViewCalls{ 0 };
    RenderViewFn g_originalRenderView = nullptr;
    void** g_renderViewSlot = nullptr;
    thread_local bool g_renderingSecondaryView = false;

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

    void __fastcall HookedRenderView(
        void* client,
        const CViewSetup& view,
        int clearFlags,
        int whatToDraw)
    {
        const RenderViewFn original = g_originalRenderView;
        if (original == nullptr)
            return;

        original(client, view, clearFlags, whatToDraw);
        g_renderViewCalls.fetch_add(1, std::memory_order_relaxed);

        if (g_renderingSecondaryView)
            return;

        UpdateViewMode();
        const ViewMode mode = g_viewMode.load(std::memory_order_relaxed);
        if (mode == ViewMode::Off)
            return;

        CViewSetup secondary = view;
        secondary.x = 275;
        secondary.unscaledX = secondary.x;
        secondary.y = 535;
        secondary.unscaledY = secondary.y;
        secondary.width = (std::max)(1, static_cast<int>(view.width / 1.4f));
        secondary.unscaledWidth = secondary.width;
        secondary.height = (std::max)(1, view.height / 2);
        secondary.unscaledHeight = secondary.height;
        secondary.stereoEye = StereoEye::Mono;
        secondary.renderToSubrectOfLargerScreen = true;
        secondary.aspectRatio = 0.0f;
        secondary.angles.y = NormalizeYaw(
            view.angles.y + (mode == ViewMode::Rear ? -180.0f : -90.0f));

        // Do not draw the first-person hands or a second HUD in the inset.
        const int secondaryDrawFlags =
            whatToDraw & ~(kRenderViewDrawViewModel | kRenderViewDrawHud);
        const int secondaryClearFlags =
            kViewClearColor | kViewClearDepth | kViewClearStencil;

        g_renderingSecondaryView = true;
        original(client, secondary, secondaryClearFlags, secondaryDrawFlags);
        g_renderingSecondaryView = false;
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

        g_renderViewSlot = &vtable[kRenderViewIndex];
        void* original = *g_renderViewSlot;
        if (original == nullptr)
        {
            g_renderViewSlot = nullptr;
            return false;
        }

        // Publish the original before installing the hook so a render thread
        // can never observe the hook with a null forwarding target.
        g_originalRenderView = reinterpret_cast<RenderViewFn>(original);
        if (!ReplaceVTableEntry(
                g_renderViewSlot,
                original,
                reinterpret_cast<void*>(&HookedRenderView)))
        {
            g_renderViewSlot = nullptr;
            g_originalRenderView = nullptr;
            return false;
        }
        return true;
    }

    void RemoveHook()
    {
        if (g_renderViewSlot == nullptr || g_originalRenderView == nullptr)
            return;

        DWORD oldProtection = 0;
        if (!VirtualProtect(
                g_renderViewSlot,
                sizeof(void*),
                PAGE_EXECUTE_READWRITE,
                &oldProtection))
        {
            return;
        }

        if (*g_renderViewSlot == reinterpret_cast<void*>(&HookedRenderView))
        {
            InterlockedExchangePointer(
                reinterpret_cast<void* volatile*>(g_renderViewSlot),
                reinterpret_cast<void*>(g_originalRenderView));
        }

        DWORD ignored = 0;
        VirtualProtect(
            g_renderViewSlot, sizeof(void*), oldProtection, &ignored);
        FlushInstructionCache(
            GetCurrentProcess(), g_renderViewSlot, sizeof(void*));

        g_renderViewSlot = nullptr;
        g_originalRenderView = nullptr;
    }

    DWORD WINAPI Initialize(void*)
    {
        if (InstallHook())
        {
            MessageBoxA(
                nullptr,
                "VClient017 a ete trouve et l'entree RenderView 27 a ete accrochee.",
                "TestDll - etape 1/2",
                MB_OK | MB_ICONINFORMATION);

            Sleep(250);
            if (g_renderViewCalls.load(std::memory_order_relaxed) != 0)
            {
                MessageBoxA(
                    nullptr,
                    "Le hook RenderView recoit bien les appels du moteur.\n"
                    "La vue arriere est activee par defaut pour ce test.",
                    "TestDll - etape 2/2",
                    MB_OK | MB_ICONINFORMATION);
            }
            else
            {
                MessageBoxA(
                    nullptr,
                    "La vtable a ete modifiee, mais RenderView n'a recu aucun appel.",
                    "TestDll - diagnostic",
                    MB_OK | MB_ICONWARNING);
            }
        }
        else
        {
            MessageBoxA(
                nullptr,
                "Echec de l'installation du hook VClient017::RenderView.",
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
