#include "Core/Application.h"

#include "Scene/SceneSerializer.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace Shadow::Core
{
Application::Application(HINSTANCE instance)
    : instance_(instance), editor_(Scene::SceneSerializer::DefaultScenePath())
{
    // 启用逐显示器 DPI 感知，确保窗口移动到不同缩放比例的显示器时，
    // 客户区尺寸和 ImGui 工具面板仍然清晰、正确。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CreateApplicationWindow();
    // Scene 数据先从 JSON 恢复，再交给 Renderer。文件不存在或损坏时保留默认场景。
    editor_.LoadInitialScene(scene_);
    const std::filesystem::path assetsDirectory =
        Scene::SceneSerializer::DefaultScenePath().parent_path().parent_path();
    std::string assetWarnings;
    assets_.DiscoverGltfAssets(assetsDirectory / "models", assetWarnings);
    if (!assetWarnings.empty())
    {
        OutputDebugStringA(assetWarnings.c_str());
    }
    assets_.LoadReferencedAssets(scene_, assetWarnings);
    if (!assetWarnings.empty())
    {
        OutputDebugStringA(assetWarnings.c_str());
    }
    renderer_.Initialize(window_, InitialWidth, InitialHeight, assets_);
}

Application::~Application()
{
    if (window_ != nullptr)
    {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    UnregisterClassW(windowClassName_, instance_);
}

void Application::CreateApplicationWindow()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = windowClassName_;

    if (RegisterClassExW(&windowClass) == 0)
    {
        throw std::runtime_error("RegisterClassExW failed.");
    }

    RECT windowRectangle{0, 0, static_cast<LONG>(InitialWidth), static_cast<LONG>(InitialHeight)};
    if (AdjustWindowRect(&windowRectangle, WS_OVERLAPPEDWINDOW, FALSE) == FALSE)
    {
        throw std::runtime_error("AdjustWindowRect failed.");
    }

    window_ = CreateWindowExW(
        0,
        windowClassName_,
        L"Shadow Engine - Scene Editor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRectangle.right - windowRectangle.left,
        windowRectangle.bottom - windowRectangle.top,
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ == nullptr)
    {
        UnregisterClassW(windowClassName_, instance_);
        throw std::runtime_error("CreateWindowExW failed.");
    }

    ShowWindow(window_, SW_SHOW);
}

int Application::Run()
{
    MSG message{};
    while (message.message != WM_QUIT)
    {
        // 有系统消息时先处理消息，消息队列空闲时才渲染。使用非阻塞的 PeekMessage
        // 而不是 GetMessage，才能形成持续运行的实时渲染主循环。
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        else
        {
            renderer_.Render(scene_, editor_, assets_);
        }
    }

    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK Application::WindowProcedure(
    HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    Application* application = nullptr;

    if (message == WM_NCCREATE)
    {
        // Win32 要求窗口过程是静态函数或自由函数。把 Application 指针保存到 HWND
        // 的用户数据中，后续消息才能转发回真正拥有窗口的 C++ 对象。
        const auto* createStructure = reinterpret_cast<CREATESTRUCTW*>(lParam);
        application = static_cast<Application*>(createStructure->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
    }
    else
    {
        application = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (application != nullptr)
    {
        return application->HandleWindowMessage(window, message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Application::HandleWindowMessage(
    HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    // N 是渲染调试快捷键，应先于 ImGui 的键盘消息处理。
    // 这样即使面板当前处于激活状态，也可以随时切换法线调试视图。
    constexpr LPARAM PreviousKeyStateMask = static_cast<LPARAM>(1) << 30;
    if (message == WM_KEYDOWN &&
        wParam == static_cast<WPARAM>('N') &&
        (lParam & PreviousKeyStateMask) == 0)
    {
        editor_.ToggleNormalDebugView();
        return 0;
    }

    // ImGui 的 Win32 Backend 先接收鼠标、键盘等输入。未初始化时 Renderer 会直接返回 false。
    if (renderer_.HandleWindowMessage(window, message, wParam, lParam))
    {
        return 0;
    }

    switch (message)
    {
    case WM_SIZE:
        // CreateWindow/ShowWindow 可能在 D3D12 初始化前就发送 WM_SIZE；窗口最小化
        // 时还会报告 0×0 客户区，而 ResizeBuffers 不接受这个尺寸。
        if (wParam != SIZE_MINIMIZED && renderer_.IsInitialized())
        {
            renderer_.Resize(
                static_cast<std::uint32_t>(LOWORD(lParam)),
                static_cast<std::uint32_t>(HIWORD(lParam)));
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}
} // namespace Shadow::Core
