#include <Windows.h>
#include <thread>
#include <d3d11.h>
#include <dxgi.h>

// ImGui
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

// Project includes
#include "sdk/offsets.hpp"
#include "sdk/schema.hpp"
#include "sdk/inventory.hpp"
#include "core/memory.hpp"
#include "core/logger.hpp"
#include "features/skinchanger/skinchanger.hpp"
#include "features/skinchanger/items.hpp"
#include "features/skinchanger/config.hpp"
#include "ui/skin_panel.hpp"

// ============================================================================
// GLOBALS
// ============================================================================

static HWND g_hwnd = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target_view = nullptr;
static WNDPROC g_original_wnd_proc = nullptr;
static bool g_initialized = false;
static bool g_show_menu = true;

// ============================================================================
// HOOKS
// ============================================================================

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_original_present = nullptr;

using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffersFn g_original_resize_buffers = nullptr;

// FrameStageNotify hook type
using FrameStageNotifyFn = void(__thiscall*)(void*, int);
static FrameStageNotifyFn g_original_frame_stage = nullptr;

// SetModel hook type
using SetModelFn = void(__thiscall*)(void*, const char*);
static SetModelFn g_original_set_model = nullptr;

static HRESULT __stdcall HookedPresent(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    // Initialize on first present
    if (!g_initialized) {
        if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) {
            g_device->GetImmediateContext(&g_context);

            DXGI_SWAP_CHAIN_DESC sd;
            swap_chain->GetDesc(&sd);
            g_hwnd = sd.OutputWindow;

            // Create render target
            ID3D11Texture2D* back_buffer = nullptr;
            swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
            g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
            back_buffer->Release();

            // Initialize ImGui
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef IMGUI_HAS_DOCK
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
            ImGui::StyleColorsDark();

            ImGui_ImplWin32_Init(g_hwnd);
            ImGui_ImplDX11_Init(g_device, g_context);

            // Initialize skinchanger
            cs2::SkinChanger::Initialize();
            cs2::SkinPanel::Initialize();

            // Subclass window
            g_original_wnd_proc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));

            g_initialized = true;
        }
    }

    if (g_initialized) {
        // ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render menu
        if (g_show_menu) {
            ImGui::Begin("Skin Changer", &g_show_menu, ImGuiWindowFlags_NoCollapse);
            cs2::SkinPanel::Render();
            ImGui::End();
        }

        // Render
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_render_target_view, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return g_original_present(swap_chain, sync_interval, flags);
}

static HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    if (g_render_target_view) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
    }

    ImGui_ImplDX11_InvalidateDeviceObjects();

    HRESULT hr = g_original_resize_buffers(swap_chain, buffer_count, width, height, format, flags);

    if (SUCCEEDED(hr) && g_device) {
        ID3D11Texture2D* back_buffer = nullptr;
        swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
        back_buffer->Release();
        ImGui_ImplDX11_CreateDeviceObjects();
    }

    return hr;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_show_menu) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    }

    // Toggle menu with Insert key
    if (msg == WM_KEYDOWN && wparam == VK_INSERT) {
        g_show_menu = !g_show_menu;
        return true;
    }

    return CallWindowProc(g_original_wnd_proc, hwnd, msg, wparam, lparam);
}

static void __fastcall HookedFrameStageNotify(void* ecx, void* edx, int stage) {
    // Call skinchanger frame stage handler
    cs2::SkinChanger::OnFrameStageNotify(stage);

    // Call original
    g_original_frame_stage(ecx, stage);
}

static void __fastcall HookedSetModel(void* ecx, void* edx, const char* model_path) {
    const char* new_model = model_path;
    cs2::SkinChanger::OnSetModel(ecx, new_model);

    g_original_set_model(ecx, new_model);
}

// ============================================================================
// VTABLE HOOKING
// ============================================================================

static void* HookVTable(void* instance, size_t index, void* hook_fn, void** original_fn) {
    void** vtable = *reinterpret_cast<void***>(instance);
    *original_fn = vtable[index];

    DWORD old_protect;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect);
    vtable[index] = hook_fn;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);

    return vtable;
}

// ============================================================================
// HOOK INITIALIZATION
// ============================================================================

static void InitializeHooks() {
    // Wait for D3D11 device to be created
    while (!g_device) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Hook Present via vtable
    void** swap_chain_vtable = *reinterpret_cast<void***>(g_swap_chain);

    DWORD old_protect;
    VirtualProtect(&swap_chain_vtable[8], sizeof(void*), PAGE_READWRITE, &old_protect);
    g_original_present = reinterpret_cast<PresentFn>(swap_chain_vtable[8]);
    swap_chain_vtable[8] = &HookedPresent;
    VirtualProtect(&swap_chain_vtable[8], sizeof(void*), old_protect, &old_protect);

    VirtualProtect(&swap_chain_vtable[13], sizeof(void*), PAGE_READWRITE, &old_protect);
    g_original_resize_buffers = reinterpret_cast<ResizeBuffersFn>(swap_chain_vtable[13]);
    swap_chain_vtable[13] = &HookedResizeBuffers;
    VirtualProtect(&swap_chain_vtable[13], sizeof(void*), old_protect, &old_protect);

    // TODO: Hook FrameStageNotify and SetModel when client interface is available
    // These require finding the client vtable via pattern scanning
    // Pattern for client vtable: 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? (Source2Client002)
}

// ============================================================================
// CLEANUP
// ============================================================================

static void Cleanup() {
#ifdef SKINCHANGER_DEBUG
    SC_LOG_INFO("Cleanup: Shutting down SkinChanger...");
#endif
    cs2::SkinChanger::Shutdown();

    if (g_render_target_view) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
#ifdef SKINCHANGER_DEBUG
        SC_LOG_DEBUG("Cleanup: Render target view released");
#endif
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
#ifdef SKINCHANGER_DEBUG
    SC_LOG_DEBUG("Cleanup: ImGui shutdown complete");
#endif

    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
#ifdef SKINCHANGER_DEBUG
    SC_LOG_INFO("Cleanup: All resources released");
#endif
}

// ============================================================================
// DLL ENTRY POINT
// ============================================================================

DWORD WINAPI MainThread(LPVOID) {
#ifdef SKINCHANGER_DEBUG
    // Generate session ID from timestamp
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    std::string session_id = "sc-debug-" + std::to_string(ms);
    cs2::Logger::Initialize(session_id);
    SC_LOG_INFO("MainThread started");
    SC_LOG_INFO("Session: " + session_id);
    SC_LOG_INFO("Build: DEBUG (console + full logging enabled)");
#endif

    // Get D3D11 swap chain by creating a temporary device
    D3D_FEATURE_LEVEL feature_level;
    DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
    swap_chain_desc.BufferCount = 1;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = GetForegroundWindow();
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* temp_device = nullptr;
    ID3D11DeviceContext* temp_context = nullptr;
    IDXGISwapChain* temp_swap_chain = nullptr;

#ifdef SKINCHANGER_DEBUG
    SC_LOG_INFO("Creating D3D11 device and swap chain...");
#endif

    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &swap_chain_desc,
            &temp_swap_chain, &temp_device, &feature_level, &temp_context))) {

        g_swap_chain = temp_swap_chain;

#ifdef SKINCHANGER_DEBUG
        SC_LOG_INFO("D3D11 device created successfully");
        SC_LOG_INFO("Initializing hooks...");
#endif

        InitializeHooks();

        // Clean up temp device (hooks are installed)
        temp_device->Release();
        temp_context->Release();
        // Keep swap chain alive - it will be released in Cleanup()
    } else {
#ifdef SKINCHANGER_DEBUG
        SC_LOG_ERROR("Failed to create D3D11 device and swap chain");
#endif
    }

    // Main loop - wait for uninject
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Check for unload signal (END key)
        if (GetAsyncKeyState(VK_END) & 1) {
#ifdef SKINCHANGER_DEBUG
            SC_LOG_INFO("END key pressed — shutting down");
#endif
            break;
        }
    }

#ifdef SKINCHANGER_DEBUG
    SC_LOG_INFO("Cleaning up resources...");
#endif
    Cleanup();
#ifdef SKINCHANGER_DEBUG
    cs2::Logger::Shutdown();
#endif
    FreeLibraryAndExitThread(static_cast<HMODULE>(g_h_module), 0);
    return 0;
}

static HMODULE g_h_module = nullptr;

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_h_module = h_module;
        DisableThreadLibraryCalls(h_module);

#ifdef SKINCHANGER_DEBUG
        // Early console for DllMain logging
        AllocConsole();
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        printf("[DLLMAIN] DLL_PROCESS_ATTACH — spawning MainThread\n");
#endif

        // Spawn main thread
        HANDLE h_thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (h_thread) {
#ifdef SKINCHANGER_DEBUG
            printf("[DLLMAIN] MainThread spawned (handle: %p)\n", h_thread);
#endif
            CloseHandle(h_thread);
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
#ifdef SKINCHANGER_DEBUG
        printf("[DLLMAIN] DLL_PROCESS_DETACH\n");
#endif
        if (!g_h_module) Cleanup();
    }

    return TRUE;
}
