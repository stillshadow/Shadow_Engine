#pragma once

#include "Assets/AssetManager.h"
#include "Editor/EditorLayer.h"
#include "Renderer/D3D12Renderer.h"
#include "Scene/Scene.h"

#include <Windows.h>

#include <cstdint>

namespace Shadow::Core
{
class Application final
{
public:
    explicit Application(HINSTANCE instance);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int Run();

private:
    static constexpr std::uint32_t InitialWidth = 1280;
    static constexpr std::uint32_t InitialHeight = 720;

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void CreateApplicationWindow();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    const wchar_t* windowClassName_ = L"ShadowEngineWindowClass";
    Assets::AssetManager assets_;
    Scene::Scene scene_;
    Editor::EditorLayer editor_;
    Renderer::D3D12Renderer renderer_;
};
} // namespace Shadow::Core
