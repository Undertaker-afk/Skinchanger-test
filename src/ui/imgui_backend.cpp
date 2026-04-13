#include "imgui_backend.hpp"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace cs2 {

ID3D11Device* ImGuiBackend::s_device = nullptr;
ID3D11DeviceContext* ImGuiBackend::s_context = nullptr;
IDXGISwapChain* ImGuiBackend::s_swap_chain = nullptr;
ID3D11RenderTargetView* ImGuiBackend::s_render_target_view = nullptr;
std::function<void()> ImGuiBackend::s_render_callback;

static WNDPROC g_original_wnd_proc = nullptr;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        return true;
    return CallWindowProc(g_original_wnd_proc, hWnd, uMsg, wParam, lParam);
}

bool ImGuiBackend::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!hwnd || !device || !context) return false;

    s_device = device;
    s_context = context;

    device->AddRef();
    context->AddRef();

    // Get swap chain from device
    s_device->GetImmediateContext(&s_context);

    // Create ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(s_device, s_context);

    // Subclass window proc
    g_original_wnd_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

    return true;
}

void ImGuiBackend::Shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (s_render_target_view) {
        s_render_target_view->Release();
        s_render_target_view = nullptr;
    }
    if (s_context) {
        s_context->Release();
        s_context = nullptr;
    }
    if (s_device) {
        s_device->Release();
        s_device = nullptr;
    }
    if (g_original_wnd_proc) {
        // Restore original window proc when possible
        g_original_wnd_proc = nullptr;
    }
}

void ImGuiBackend::NewFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiBackend::EndFrame() {
    ImGui::Render();
}

void ImGuiBackend::Render() {
    if (!s_context || !s_render_target_view) return;

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data) return;

    s_context->OMSetRenderTargets(1, &s_render_target_view, nullptr);
    ImGui_ImplDX11_RenderDrawData(draw_data);

    if (s_render_callback) {
        s_render_callback();
    }
}

void ImGuiBackend::SetRenderCallback(std::function<void()> callback) {
    s_render_callback = std::move(callback);
}

std::function<void()> ImGuiBackend::GetRenderCallback() {
    return s_render_callback;
}

}
