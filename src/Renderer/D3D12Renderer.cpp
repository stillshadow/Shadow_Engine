#include "Renderer/D3D12Renderer.h"

#include "Assets/AssetManager.h"
#include "Assets/HdrEnvironment.h"
#include "Core/DxException.h"
#include "Editor/EditorLayer.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

#include <DirectXMath.h>
#include <d3d12sdklayers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

// 官方 Win32 Backend 刻意不在头文件中包含 Windows.h，因此消息处理函数由宿主声明。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;
using Shadow::Core::ThrowIfFailed;

namespace
{
constexpr std::size_t MaxSceneLights = 8;

struct GpuLightConstants
{
    DirectX::XMFLOAT4 positionType;
    DirectX::XMFLOAT4 directionIntensity;
    DirectX::XMFLOAT4 colorRange;
    DirectX::XMFLOAT4 areaSize;
};

DirectX::XMFLOAT3 SafeNormalize(const DirectX::XMFLOAT3& direction) noexcept
{
    using namespace DirectX;
    const XMVECTOR vector = XMLoadFloat3(&direction);
    if (XMVectorGetX(XMVector3LengthSq(vector)) < 0.000001F)
    {
        return {0.0F, 1.0F, 0.0F};
    }
    XMFLOAT3 normalized{};
    XMStoreFloat3(&normalized, XMVector3Normalize(vector));
    return normalized;
}

struct ObjectConstants
{
    DirectX::XMFLOAT4X4 model;
    DirectX::XMFLOAT4X4 modelViewProjection;
    DirectX::XMFLOAT4 baseColor;
    DirectX::XMFLOAT3 cameraPosition;
    float roughness;
    DirectX::XMFLOAT3 lightDirection;
    float iblIntensity;
    DirectX::XMFLOAT3 lightColor;
    float metallic;
    float debugViewMode;
    float environmentRotationRadians;
    float materialPadding;
    float normalStrength;
    float parallaxHeightScale;
    float ssaoStrength;
    float iblSpecularStrength;
    float renderPadding;
    DirectX::XMFLOAT4X4 lightViewProjection;
    DirectX::XMFLOAT4X4 normalMatrix;
    std::array<GpuLightConstants, MaxSceneLights> lights;
    std::uint32_t lightCount;
    std::uint32_t shadowLightIndex;
    std::uint32_t pointShadowLightIndex;
    float pointShadowRange;
};

struct InstanceData
{
    DirectX::XMFLOAT4X4 model;
    DirectX::XMFLOAT4X4 modelViewProjection;
    DirectX::XMFLOAT4X4 normalMatrix;
};
static_assert(sizeof(InstanceData) == 192);

// C++ 与 HLSL 必须用完全相同的 16 字节分组解释这段内存。
static_assert(offsetof(ObjectConstants, roughness) == 156);
static_assert(offsetof(ObjectConstants, metallic) == 188);
static_assert(offsetof(ObjectConstants, debugViewMode) == 192);
static_assert(offsetof(ObjectConstants, lightViewProjection) == 224);
static_assert(offsetof(ObjectConstants, lights) == 352);
static_assert(offsetof(ObjectConstants, lightCount) == 864);
static_assert(sizeof(ObjectConstants) == 880);

struct SkyConstants
{
    DirectX::XMFLOAT4X4 inverseViewProjection{};
    DirectX::XMFLOAT3 cameraPosition{};
    float intensity = 1.0F;
    float rotationRadians = 0.0F;
    float padding[3]{};
};

struct SsaoConstants
{
    DirectX::XMFLOAT2 invResolution{};
    float radius = 1.5F;
    float strength = 0.0F;
};

struct PostProcessConstants
{
    float exposure = 1.0F;
    float bloomThreshold = 1.0F;
    float bloomStrength = 0.0F;
    float bloomEnabled = 0.0F;
    DirectX::XMFLOAT2 invResolution{};
    DirectX::XMFLOAT2 direction{};
};

constexpr UINT AlignUp(const UINT value, const UINT alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, 32768> pathBuffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (length == 0 || length == static_cast<DWORD>(pathBuffer.size()))
    {
        throw std::runtime_error("GetModuleFileNameW failed while locating shader files.");
    }

    return std::filesystem::path(std::wstring(pathBuffer.data(), length)).parent_path();
}

std::filesystem::path FindProjectAsset(const std::filesystem::path& relativePath)
{
    std::filesystem::path directory = ExecutableDirectory();
    for (int depth = 0; depth < 8; ++depth)
    {
        const std::filesystem::path candidate = directory / relativePath;
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }
        if (!directory.has_parent_path() || directory.parent_path() == directory)
        {
            break;
        }
        directory = directory.parent_path();
    }
    throw std::runtime_error("Failed to locate project asset: " + relativePath.string());
}

std::vector<std::byte> LoadShaderBytecode(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Failed to open compiled shader: " + path.string());
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0)
    {
        throw std::runtime_error("Compiled shader is empty: " + path.string());
    }

    std::vector<std::byte> bytecode(static_cast<std::size_t>(fileSize));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytecode.data()), fileSize))
    {
        throw std::runtime_error("Failed to read compiled shader: " + path.string());
    }

    return bytecode;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after)
{
    // OpenGL 通常由驱动隐式跟踪资源状态；D3D12 则要求应用程序在执行命令前，
    // 明确声明资源接下来会以什么方式被使用。
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

} // namespace

namespace Shadow::Renderer
{
D3D12Renderer::~D3D12Renderer()
{
    if (commandQueue_ && fence_ && fenceEvent_ != nullptr)
    {
        try
        {
            WaitForGpu();
        }
        catch (...)
        {
            // 析构函数不能向外抛异常；设备移除等详细错误应在正常调用路径中报告。
        }
    }

    if (imguiInitialized_)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }

    if (constantBuffer_ && mappedConstantBufferData_ != nullptr)
    {
        constantBuffer_->Unmap(0, nullptr);
        mappedConstantBufferData_ = nullptr;
    }
    if (instanceBuffer_ && mappedInstanceData_ != nullptr)
    {
        instanceBuffer_->Unmap(0, nullptr);
        mappedInstanceData_ = nullptr;
    }

    if (ssaoConstantsBuffer_ && mappedSsaoConstants_ != nullptr)
    {
        ssaoConstantsBuffer_->Unmap(0, nullptr);
        mappedSsaoConstants_ = nullptr;
    }
    if (postProcessConstantsBuffer_ && mappedPostProcessConstants_ != nullptr)
    {
        postProcessConstantsBuffer_->Unmap(0, nullptr);
        mappedPostProcessConstants_ = nullptr;
    }
    if (skyConstantsBuffer_ && mappedSkyConstants_ != nullptr)
    {
        skyConstantsBuffer_->Unmap(0, nullptr);
        mappedSkyConstants_ = nullptr;
    }

    if (fenceEvent_ != nullptr)
    {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

void D3D12Renderer::Initialize(
    HWND window,
    const std::uint32_t width,
    const std::uint32_t height,
    const Assets::AssetManager& assets)
{
    if (window == nullptr || width == 0 || height == 0)
    {
        throw std::invalid_argument("D3D12Renderer requires a valid window and non-zero dimensions.");
    }

    window_ = window;
    width_ = width;
    height_ = height;

    CreateDevice();
    CreateCommandQueue();
    CreateSwapChain();
    CreateDescriptorHeaps();
    CreateRenderTargets();
    CreateDepthBuffer();
    CreateGBufferResources();
    CreateShadowResources();
    CreateCommandObjects();
    CreateSynchronizationObjects();
    CreateGraphicsPipeline();
    CreateSceneGeometry(assets);
    CreateMaterialTextures(assets);
    CreateGBufferViews();
    CreateEnvironmentTexture();
    CreateConstantBuffer();
    CreateImGui();
    UpdateViewportAndScissor();
    CheckDebugLayerMessages("renderer initialization");

    startTime_ = std::chrono::steady_clock::now();
    initialized_ = true;
}

void D3D12Renderer::CreateDevice()
{
    UINT factoryFlags = 0;

#if defined(_DEBUG)
    // Debug Layer 必须在创建设备前启用。它会验证 API 用法，并报告对象生命周期、
    // 资源状态切换和同步方面的错误。
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");

    // 优先枚举系统认为“高性能”的硬件适配器。对于同时拥有核显和独显的笔记本，
    // 这能避免渲染器无意中运行在低性能 GPU 上。
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumResult = factory_->EnumAdapterByGpuPreference(
            adapterIndex,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter));

        if (enumResult == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        ThrowIfFailed(enumResult, "EnumAdapterByGpuPreference");

        DXGI_ADAPTER_DESC1 description{};
        ThrowIfFailed(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(
                adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))))
        {
            break;
        }
    }

    if (!device_)
    {
        // WARP 是微软提供的软件光栅器。没有兼容硬件适配器时，仍可用它运行
        // 正确性测试，但它不代表真实 GPU 性能。
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)), "EnumWarpAdapter");
        ThrowIfFailed(
            D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
            "D3D12CreateDevice (WARP)");
    }


#if defined(_DEBUG)
    // 遇到严重错误时立即中断，能让调试器停在真正出错的 API 附近，
    // 而不是等到很久之后在 Present 阶段才表现为失败。
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device_.As(&infoQueue)))
    {
        // 由渲染器读取 InfoQueue 并输出完整错误，避免调试器只显示无上下文的 0x87A。
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    }
#endif
}

void D3D12Renderer::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    description.NodeMask = 0;

    ThrowIfFailed(
        device_->CreateCommandQueue(&description, IID_PPV_ARGS(&commandQueue_)),
        "ID3D12Device::CreateCommandQueue");
}

void D3D12Renderer::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width_;
    description.Height = height_;
    description.Format = BackBufferFormat;
    description.Stereo = FALSE;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Flip-discard 是现代交换链的呈现模式。三缓冲允许显示器或 GPU 仍在使用前面
    // 的缓冲时，CPU 已经开始准备后续帧。
    description.BufferCount = FrameCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    description.Flags = 0;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(
        factory_->CreateSwapChainForHwnd(
            commandQueue_.Get(), window_, &description, nullptr, nullptr, &swapChain),
        "IDXGIFactory::CreateSwapChainForHwnd");
    ThrowIfFailed(factory_->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER),
                  "IDXGIFactory::MakeWindowAssociation");
    ThrowIfFailed(swapChain.As(&swapChain_), "Query IDXGISwapChain3");

    // 每次 Present 后由 DXGI 决定下一张后备缓冲，而不是引擎自行轮换索引，
    // 因此必须始终向交换链查询当前缓冲编号。
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void D3D12Renderer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = FrameCount + GBufferCount + PostProcessTargetCount +
        MsaaTargetCount + PointShadowFaceCount;
    rtvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvDescription.NodeMask = 0;

    ThrowIfFailed(
        device_->CreateDescriptorHeap(&rtvDescription, IID_PPV_ARGS(&rtvHeap_)),
        "ID3D12Device::CreateDescriptorHeap (RTV)");
    rtvDescriptorSize_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Depth Buffer 只有一张，因此 DSV Heap 只需要一个描述符。RTV 和 DSV
    // 属于不同的 Descriptor Heap 类型，不能放进同一个 Heap。
    D3D12_DESCRIPTOR_HEAP_DESC dsvDescription{};
    dsvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDescription.NumDescriptors = 4;
    dsvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvDescription.NodeMask = 0;

    ThrowIfFailed(
        device_->CreateDescriptorHeap(&dsvDescription, IID_PPV_ARGS(&dsvHeap_)),
        "ID3D12Device::CreateDescriptorHeap (DSV)");
}

void D3D12Renderer::CreateRenderTargets()
{
    // 后备缓冲资源由交换链持有。我们为每张缓冲创建一个 RTV 描述符，
    // 让输出合并阶段能够把当前图像作为渲染目标访问。
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < FrameCount; ++index)
    {
        ThrowIfFailed(
            swapChain_->GetBuffer(index, IID_PPV_ARGS(&renderTargets_[index])),
            "IDXGISwapChain::GetBuffer");
        device_->CreateRenderTargetView(renderTargets_[index].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize_;
    }
}

void D3D12Renderer::CreateDepthBuffer()
{
    // 深度值只由 GPU 写入和读取，因此使用位于显存中的 Default Heap。
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDescription.Width = width_;
    resourceDescription.Height = height_;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DepthBufferFormat;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optimizedClearValue{};
    optimizedClearValue.Format = DepthBufferFormat;
    optimizedClearValue.DepthStencil.Depth = 1.0F;
    optimizedClearValue.DepthStencil.Stencil = 0;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescription,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &optimizedClearValue,
            IID_PPV_ARGS(&depthBuffer_)),
        "ID3D12Device::CreateCommittedResource (depth buffer)");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
    dsvDescription.Format = DepthBufferFormat;
    dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDescription.Flags = D3D12_DSV_FLAG_NONE;
    device_->CreateDepthStencilView(
        depthBuffer_.Get(), &dsvDescription, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

void D3D12Renderer::CreateGBufferResources()
{
    const std::array<DXGI_FORMAT, GBufferCount> formats = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };
    const std::array<std::array<float, 4>, GBufferCount> clearColors = {
        std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F},
        std::array<float, 4>{0.5F, 0.5F, 1.0F, 0.0F},
        std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F},
    };

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width_;
    description.Height = height_;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(FrameCount) * rtvDescriptorSize_;
    for (std::size_t index = 0; index < GBufferCount; ++index)
    {
        description.Format = formats[index];
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = formats[index];
        std::copy(
            clearColors[index].begin(), clearColors[index].end(),
            clearValue.Color);
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                IID_PPV_ARGS(&gBufferTargets_[index])),
            "ID3D12Device::CreateCommittedResource (GBuffer target)");
        gBufferRtvs_[index] = rtvHandle;
        device_->CreateRenderTargetView(gBufferTargets_[index].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize_;
    }

    // 后处理目标都以 SRV 开始，第一帧使用前会显式切换为 RTV。
    const auto createRenderTarget = [&](ComPtr<ID3D12Resource>& target,
                                        const DXGI_FORMAT format,
                                        D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
                                        const char* debugName)
    {
        D3D12_RESOURCE_DESC targetDescription = description;
        targetDescription.Format = format;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = format;
        clearValue.Color[0] = 0.0F;
        clearValue.Color[1] = 0.0F;
        clearValue.Color[2] = 0.0F;
        clearValue.Color[3] = 1.0F;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &targetDescription,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                IID_PPV_ARGS(&target)),
            debugName);
        rtv = rtvHandle;
        device_->CreateRenderTargetView(target.Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize_;
    };
    createRenderTarget(
        ssaoTarget_, DXGI_FORMAT_R8_UNORM, ssaoRtv_,
        "ID3D12Device::CreateCommittedResource (SSAO target)");
    createRenderTarget(
        hdrSceneTarget_, DXGI_FORMAT_R16G16B16A16_FLOAT, hdrSceneRtv_,
        "ID3D12Device::CreateCommittedResource (HDR scene target)");
    createRenderTarget(
        bloomExtractTarget_, DXGI_FORMAT_R16G16B16A16_FLOAT, bloomExtractRtv_,
        "ID3D12Device::CreateCommittedResource (bloom extract target)");
    createRenderTarget(
        bloomBlurTarget_, DXGI_FORMAT_R16G16B16A16_FLOAT, bloomBlurRtv_,
        "ID3D12Device::CreateCommittedResource (bloom blur target)");

    // 4x MSAA 使用独立的多采样颜色/深度缓冲，结束后 Resolve 到原来的单采样 HDR 目标。
    // 先查询硬件支持，避免在不支持该格式采样数的设备上创建非法资源。
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels{};
    qualityLevels.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    qualityLevels.SampleCount = MsaaSampleCount;
    qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depthQualityLevels = qualityLevels;
    depthQualityLevels.Format = DepthBufferFormat;
    const bool colorMsaaSupported = SUCCEEDED(device_->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
        &qualityLevels, sizeof(qualityLevels))) && qualityLevels.NumQualityLevels > 0;
    const bool depthMsaaSupported = SUCCEEDED(device_->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
        &depthQualityLevels, sizeof(depthQualityLevels))) &&
        depthQualityLevels.NumQualityLevels > 0;
    msaaSupported_ = colorMsaaSupported && depthMsaaSupported;
    if (msaaSupported_)
    {
        D3D12_RESOURCE_DESC msaaColorDescription = description;
        msaaColorDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        msaaColorDescription.SampleDesc.Count = MsaaSampleCount;
        // Quality 0 是所有支持该采样数的设备都必须提供的标准模式。
        msaaColorDescription.SampleDesc.Quality = 0;
        D3D12_CLEAR_VALUE colorClear{};
        colorClear.Format = msaaColorDescription.Format;
        colorClear.Color[3] = 1.0F;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &msaaColorDescription,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
                IID_PPV_ARGS(&msaaHdrSceneTarget_)),
            "ID3D12Device::CreateCommittedResource (MSAA HDR target)");
        msaaHdrSceneRtv_ = rtvHandle;
        D3D12_RENDER_TARGET_VIEW_DESC msaaRtvDescription{};
        msaaRtvDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        msaaRtvDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        device_->CreateRenderTargetView(
            msaaHdrSceneTarget_.Get(), &msaaRtvDescription, msaaHdrSceneRtv_);

        D3D12_RESOURCE_DESC msaaDepthDescription = msaaColorDescription;
        msaaDepthDescription.Format = DepthBufferFormat;
        msaaDepthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DepthBufferFormat;
        depthClear.DepthStencil.Depth = 1.0F;
        depthClear.DepthStencil.Stencil = 0;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &msaaDepthDescription,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                IID_PPV_ARGS(&msaaDepthBuffer_)),
            "ID3D12Device::CreateCommittedResource (MSAA depth)");
        msaaDepthDsv_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        msaaDepthDsv_.ptr += static_cast<SIZE_T>(3) *
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_DEPTH_STENCIL_VIEW_DESC msaaDsvDescription{};
        msaaDsvDescription.Format = DepthBufferFormat;
        msaaDsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device_->CreateDepthStencilView(
            msaaDepthBuffer_.Get(), &msaaDsvDescription, msaaDepthDsv_);
    }
    gBufferReadyForSampling_ = true;

}

void D3D12Renderer::CreateGBufferViews()
{
    const UINT descriptorSize =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        materialSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(MaterialDescriptorCount + 1) * descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
        materialSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += static_cast<UINT64>(MaterialDescriptorCount + 1) * descriptorSize;
    for (std::size_t index = 0; index < GBufferCount; ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(
            gBufferTargets_[index].Get(), &description, cpuHandle);
        gBufferSrvs_[index] = gpuHandle;
        cpuHandle.ptr += descriptorSize;
        gpuHandle.ptr += descriptorSize;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC ssaoDescription{};
    ssaoDescription.Format = DXGI_FORMAT_R8_UNORM;
    ssaoDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ssaoDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ssaoDescription.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(ssaoTarget_.Get(), &ssaoDescription, cpuHandle);
    ssaoSrv_ = gpuHandle;
    cpuHandle.ptr += descriptorSize;
    gpuHandle.ptr += descriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC postProcessDescription{};
    postProcessDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    postProcessDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    postProcessDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    postProcessDescription.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        hdrSceneTarget_.Get(), &postProcessDescription, cpuHandle);
    hdrSceneSrv_ = gpuHandle;
    cpuHandle.ptr += descriptorSize;
    gpuHandle.ptr += descriptorSize;
    device_->CreateShaderResourceView(
        bloomExtractTarget_.Get(), &postProcessDescription, cpuHandle);
    bloomExtractSrv_ = gpuHandle;
    cpuHandle.ptr += descriptorSize;
    gpuHandle.ptr += descriptorSize;
    device_->CreateShaderResourceView(
        bloomBlurTarget_.Get(), &postProcessDescription, cpuHandle);
    bloomBlurSrv_ = gpuHandle;
}

void D3D12Renderer::CreateShadowResources()
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = ShadowMapSize;
    description.Height = ShadowMapSize;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    // 同一份资源需要作为 D32_FLOAT 写入，又要作为 R32_FLOAT 在 Shader 中读取。
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0F;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(&shadowMap_)),
        "ID3D12Device::CreateCommittedResource (shadow map)");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
    dsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    shadowDsv.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device_->CreateDepthStencilView(shadowMap_.Get(), &dsvDescription, shadowDsv);

    shadowViewport_ = {
        0.0F, 0.0F, static_cast<float>(ShadowMapSize),
        static_cast<float>(ShadowMapSize), 0.0F, 1.0F};
    shadowScissorRect_ = {
        0, 0, static_cast<LONG>(ShadowMapSize), static_cast<LONG>(ShadowMapSize)};

    // Point Shadow 使用六面 R32_FLOAT Render Target 保存“到光源的线性距离”。
    // 与透视深度相比，线性距离可以直接通过世界坐标重建并用于 Cubemap 比较。
    D3D12_RESOURCE_DESC pointDescription{};
    pointDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    pointDescription.Width = PointShadowMapSize;
    pointDescription.Height = PointShadowMapSize;
    pointDescription.DepthOrArraySize = PointShadowFaceCount;
    pointDescription.MipLevels = 1;
    pointDescription.Format = DXGI_FORMAT_R32_FLOAT;
    pointDescription.SampleDesc.Count = 1;
    pointDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    pointDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE pointClear{};
    pointClear.Format = DXGI_FORMAT_R32_FLOAT;
    pointClear.Color[0] = 1.0F;
    pointClear.Color[1] = 1.0F;
    pointClear.Color[2] = 1.0F;
    pointClear.Color[3] = 1.0F;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &pointDescription,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &pointClear,
            IID_PPV_ARGS(&pointShadowMap_)),
        "ID3D12Device::CreateCommittedResource (point shadow cubemap)");
    D3D12_CPU_DESCRIPTOR_HANDLE pointRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    // RTV Heap 排列：SwapChain | GBuffer | PostProcess | MSAA HDR | Point Shadow x6。
    // Resize 只重建 MSAA 描述符，因此 Point Shadow 必须从它之后开始，不能共用槽位。
    pointRtv.ptr += static_cast<SIZE_T>(
        FrameCount + GBufferCount + PostProcessTargetCount + MsaaTargetCount) *
        rtvDescriptorSize_;
    for (UINT face = 0; face < PointShadowFaceCount; ++face)
    {
        D3D12_RENDER_TARGET_VIEW_DESC pointRtvDescription{};
        pointRtvDescription.Format = DXGI_FORMAT_R32_FLOAT;
        pointRtvDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        pointRtvDescription.Texture2DArray.MipSlice = 0;
        pointRtvDescription.Texture2DArray.FirstArraySlice = face;
        pointRtvDescription.Texture2DArray.ArraySize = 1;
        pointShadowRtvs_[face] = pointRtv;
        device_->CreateRenderTargetView(
            pointShadowMap_.Get(), &pointRtvDescription, pointRtv);
        pointRtv.ptr += rtvDescriptorSize_;
    }

    D3D12_RESOURCE_DESC pointDepthDescription{};
    pointDepthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    pointDepthDescription.Width = PointShadowMapSize;
    pointDepthDescription.Height = PointShadowMapSize;
    pointDepthDescription.DepthOrArraySize = 1;
    pointDepthDescription.MipLevels = 1;
    pointDepthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    pointDepthDescription.SampleDesc.Count = 1;
    pointDepthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    pointDepthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE pointDepthClear{};
    pointDepthClear.Format = DXGI_FORMAT_D32_FLOAT;
    pointDepthClear.DepthStencil.Depth = 1.0F;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &pointDepthDescription,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &pointDepthClear,
            IID_PPV_ARGS(&pointShadowDepth_)),
        "ID3D12Device::CreateCommittedResource (point shadow depth)");
    D3D12_CPU_DESCRIPTOR_HANDLE pointDsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    pointDsv.ptr += static_cast<SIZE_T>(2) *
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device_->CreateDepthStencilView(pointShadowDepth_.Get(), nullptr, pointDsv);

    pointShadowViewport_ = {
        0.0F, 0.0F, static_cast<float>(PointShadowMapSize),
        static_cast<float>(PointShadowMapSize), 0.0F, 1.0F};
    pointShadowScissorRect_ = {
        0, 0, static_cast<LONG>(PointShadowMapSize),
        static_cast<LONG>(PointShadowMapSize)};
}

void D3D12Renderer::CreateCommandObjects()
{
    // 命令分配器持有已记录命令背后的内存。每帧使用独立分配器，可以避免 CPU
    // 重置内存时覆盖 GPU 仍在读取的命令。
    for (auto& allocator : commandAllocators_)
    {
        ThrowIfFailed(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
            "ID3D12Device::CreateCommandAllocator");
    }

    ThrowIfFailed(
        device_->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocators_[frameIndex_].Get(),
            nullptr,
            IID_PPV_ARGS(&commandList_)),
        "ID3D12Device::CreateCommandList");
    ThrowIfFailed(commandList_->Close(), "ID3D12GraphicsCommandList::Close");
}

void D3D12Renderer::CreateGraphicsPipeline()
{
    // 这两个文件由 DXC 在构建阶段生成。运行时加载字节码，可以把“编译 Shader”
    // 和“让 GPU 执行 Shader”清楚地分成两个步骤。
    const std::filesystem::path shaderDirectory = ExecutableDirectory() / L"shaders";
    const std::vector<std::byte> vertexShader =
        LoadShaderBytecode(shaderDirectory / L"MaterialPreviewVS.cso");
    const std::vector<std::byte> pixelShader =
        LoadShaderBytecode(shaderDirectory / L"MaterialPreviewPS.cso");
    const std::vector<std::byte> shadowVertexShader =
        LoadShaderBytecode(shaderDirectory / L"ShadowMapVS.cso");
    const std::vector<std::byte> pointShadowVertexShader =
        LoadShaderBytecode(shaderDirectory / L"PointShadowVS.cso");
    const std::vector<std::byte> pointShadowPixelShader =
        LoadShaderBytecode(shaderDirectory / L"PointShadowPS.cso");
    const std::vector<std::byte> gBufferVertexShader =
        LoadShaderBytecode(shaderDirectory / L"GBufferVS.cso");
    const std::vector<std::byte> gBufferPixelShader =
        LoadShaderBytecode(shaderDirectory / L"GBufferPS.cso");
    const std::vector<std::byte> ssaoVertexShader =
        LoadShaderBytecode(shaderDirectory / L"SSAOVS.cso");
    const std::vector<std::byte> ssaoPixelShader =
        LoadShaderBytecode(shaderDirectory / L"SSAOPS.cso");
    const std::vector<std::byte> bloomVertexShader =
        LoadShaderBytecode(shaderDirectory / L"BloomVS.cso");
    const std::vector<std::byte> bloomPixelShader =
        LoadShaderBytecode(shaderDirectory / L"BloomPS.cso");
    const std::vector<std::byte> postProcessVertexShader =
        LoadShaderBytecode(shaderDirectory / L"PostProcessVS.cso");
    const std::vector<std::byte> postProcessPixelShader =
        LoadShaderBytecode(shaderDirectory / L"PostProcessPS.cso");
    const std::vector<std::byte> skyVertexShader =
        LoadShaderBytecode(shaderDirectory / L"SkyboxVS.cso");
    const std::vector<std::byte> skyPixelShader =
        LoadShaderBytecode(shaderDirectory / L"SkyboxPS.cso");
    const std::vector<std::byte> deferredVertexShader =
        LoadShaderBytecode(shaderDirectory / L"DeferredLightingVS.cso");
    const std::vector<std::byte> deferredPixelShader =
        LoadShaderBytecode(shaderDirectory / L"DeferredLightingPS.cso");
    const std::vector<std::byte> outlineVertexShader =
        LoadShaderBytecode(shaderDirectory / L"OutlineVS.cso");
    const std::vector<std::byte> outlinePixelShader =
        LoadShaderBytecode(shaderDirectory / L"OutlinePS.cso");
    const std::vector<std::byte> geometryNormalsVertexShader =
        LoadShaderBytecode(shaderDirectory / L"GeometryNormalsVS.cso");
    const std::vector<std::byte> geometryNormalsGeometryShader =
        LoadShaderBytecode(shaderDirectory / L"GeometryNormalsGS.cso");
    const std::vector<std::byte> geometryNormalsPixelShader =
        LoadShaderBytecode(shaderDirectory / L"GeometryNormalsPS.cso");

    D3D12_DESCRIPTOR_RANGE materialTextureRange{};
    materialTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialTextureRange.NumDescriptors = 3;
    materialTextureRange.BaseShaderRegister = 0;
    materialTextureRange.RegisterSpace = 0;
    materialTextureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE shadowTextureRange{};
    shadowTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowTextureRange.NumDescriptors = 1;
    shadowTextureRange.BaseShaderRegister = 3;
    shadowTextureRange.RegisterSpace = 0;
    shadowTextureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE gBufferTextureRange{};
    gBufferTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gBufferTextureRange.NumDescriptors = GBufferCount;
    gBufferTextureRange.BaseShaderRegister = 4;
    gBufferTextureRange.RegisterSpace = 0;
    gBufferTextureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE ssaoTextureRange{};
    ssaoTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssaoTextureRange.NumDescriptors = 1;
    ssaoTextureRange.BaseShaderRegister = 8;
    ssaoTextureRange.RegisterSpace = 0;
    ssaoTextureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE environmentTextureRange{};
    environmentTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    environmentTextureRange.NumDescriptors = EnvironmentDescriptorCount;
    environmentTextureRange.BaseShaderRegister = 9;
    environmentTextureRange.RegisterSpace = 0;
    environmentTextureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE pointShadowTextureRange{};
    pointShadowTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    pointShadowTextureRange.NumDescriptors = 1;
    pointShadowTextureRange.BaseShaderRegister = 13;
    pointShadowTextureRange.RegisterSpace = 0;
    pointShadowTextureRange.OffsetInDescriptorsFromTableStart = 0;

    std::array<D3D12_ROOT_PARAMETER, 8> rootParameters{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].Descriptor.RegisterSpace = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &materialTextureRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[2].DescriptorTable.pDescriptorRanges = &shadowTextureRange;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[3].DescriptorTable.pDescriptorRanges = &gBufferTextureRange;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[4].DescriptorTable.pDescriptorRanges = &ssaoTextureRange;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[5].DescriptorTable.pDescriptorRanges = &environmentTextureRange;
    rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[6].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[6].DescriptorTable.pDescriptorRanges = &pointShadowTextureRange;
    rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // 根 SRV 直接指向当前批次的 InstanceData 起点，SV_InstanceID 负责在批次内索引。
    rootParameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[7].Descriptor.ShaderRegister = 14;
    rootParameters[7].Descriptor.RegisterSpace = 0;
    rootParameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> staticSamplers{};
    D3D12_STATIC_SAMPLER_DESC& materialSampler = staticSamplers[0];
    materialSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    materialSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    materialSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    materialSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    materialSampler.MipLODBias = 0.0F;
    materialSampler.MaxAnisotropy = 1;
    materialSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    materialSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    materialSampler.MinLOD = 0.0F;
    materialSampler.MaxLOD = D3D12_FLOAT32_MAX;
    materialSampler.ShaderRegister = 0;
    materialSampler.RegisterSpace = 0;
    materialSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC& shadowSampler = staticSamplers[1];
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MinLOD = 0.0F;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 1;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};
    rootSignatureDescription.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSignatureDescription.pParameters = rootParameters.data();
    rootSignatureDescription.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
    rootSignatureDescription.pStaticSamplers = staticSamplers.data();
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> serializationErrors;
    const HRESULT serializationResult = D3D12SerializeRootSignature(
        &rootSignatureDescription,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSignature,
        &serializationErrors);
    if (FAILED(serializationResult))
    {
        std::string errorMessage = "D3D12SerializeRootSignature failed.";
        if (serializationErrors)
        {
            errorMessage += " ";
            errorMessage.append(
                static_cast<const char*>(serializationErrors->GetBufferPointer()),
                serializationErrors->GetBufferSize());
        }
        throw std::runtime_error(errorMessage);
    }

    ThrowIfFailed(
        device_->CreateRootSignature(
            0,
            serializedRootSignature->GetBufferPointer(),
            serializedRootSignature->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)),
        "ID3D12Device::CreateRootSignature");

    // glTF 顶点固定为 Position/Normal/UV/Tangent。即使某个材质暂时没有贴图，
    // 保持统一布局也能让所有 Mesh 共享同一个 PSO 和 Shader。
    const D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_RASTERIZER_DESC rasterizerDescription{};
    rasterizerDescription.FillMode = D3D12_FILL_MODE_SOLID;
    // glTF 的默认正面在当前左手坐标约定下对应顺时针；背面剔除减少不可见三角形开销。
    rasterizerDescription.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDescription.FrontCounterClockwise = FALSE;
    rasterizerDescription.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerDescription.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerDescription.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerDescription.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blendDescription{};
    D3D12_RENDER_TARGET_BLEND_DESC& renderTargetBlend = blendDescription.RenderTarget[0];
    renderTargetBlend.BlendEnable = FALSE;
    renderTargetBlend.LogicOpEnable = FALSE;
    renderTargetBlend.SrcBlend = D3D12_BLEND_ONE;
    renderTargetBlend.DestBlend = D3D12_BLEND_ZERO;
    renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;
    renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    renderTargetBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    renderTargetBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    renderTargetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthStencilDescription{};
    depthStencilDescription.DepthEnable = TRUE;
    depthStencilDescription.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDescription.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDescription.StencilEnable = FALSE;
    depthStencilDescription.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDescription.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    depthStencilDescription.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDescription.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDescription.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDescription.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDescription.BackFace = depthStencilDescription.FrontFace;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = rootSignature_.Get();
    pipelineDescription.VS = {vertexShader.data(), vertexShader.size()};
    pipelineDescription.PS = {pixelShader.data(), pixelShader.size()};
    pipelineDescription.BlendState = blendDescription;
    pipelineDescription.SampleMask = std::numeric_limits<UINT>::max();
    pipelineDescription.RasterizerState = rasterizerDescription;
    pipelineDescription.DepthStencilState = depthStencilDescription;
    pipelineDescription.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipelineDescription.DSVFormat = DepthBufferFormat;
    pipelineDescription.SampleDesc.Count = 1;

    // PSO 把 Shader、顶点布局、光栅化和混合等固定状态组合在一起。
    // Draw 时 GPU 只需绑定这一份已经验证过的完整管线描述。
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &pipelineDescription, IID_PPV_ARGS(&pipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState");

    // 半透明仍复用完整 PBR Shader，只改变混合和深度写入状态。
    // 关闭深度写入可避免前面的透明表面挡掉后面的透明表面；CPU 会负责从远到近排序。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentDescription = pipelineDescription;
    auto& transparentBlend = transparentDescription.BlendState.RenderTarget[0];
    transparentBlend.BlendEnable = TRUE;
    transparentBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    transparentBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    transparentBlend.BlendOp = D3D12_BLEND_OP_ADD;
    transparentBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    transparentBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    transparentDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &transparentDescription, IID_PPV_ARGS(&transparentPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (transparent)");

    // 第一遍只把选中物体写成模板值 1，不输出颜色，也不改写已有深度。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC stencilMaskDescription = pipelineDescription;
    stencilMaskDescription.PS = {};
    stencilMaskDescription.NumRenderTargets = 0;
    stencilMaskDescription.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    stencilMaskDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    stencilMaskDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    stencilMaskDescription.DepthStencilState.StencilEnable = TRUE;
    stencilMaskDescription.DepthStencilState.StencilWriteMask = 0xFF;
    stencilMaskDescription.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    stencilMaskDescription.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    stencilMaskDescription.DepthStencilState.BackFace =
        stencilMaskDescription.DepthStencilState.FrontFace;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &stencilMaskDescription, IID_PPV_ARGS(&stencilMaskPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (stencil mask)");

    // 第二遍绘制略微外扩的模型；模板不等于 1 的像素才显示，因此只剩轮廓。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC outlineDescription = pipelineDescription;
    outlineDescription.VS = {outlineVertexShader.data(), outlineVertexShader.size()};
    outlineDescription.PS = {outlinePixelShader.data(), outlinePixelShader.size()};
    // 轮廓仍参与深度测试：选中地面时，地面后方的模型不会被整块黄色外壳覆盖。
    outlineDescription.DepthStencilState.DepthEnable = TRUE;
    outlineDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    outlineDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    outlineDescription.DepthStencilState.StencilEnable = TRUE;
    outlineDescription.DepthStencilState.StencilReadMask = 0xFF;
    outlineDescription.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
    outlineDescription.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    outlineDescription.DepthStencilState.BackFace =
        outlineDescription.DepthStencilState.FrontFace;
    // 调试轮廓也要支持地面等单面 Mesh，因此不能依赖“只画背面外壳”。
    outlineDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &outlineDescription, IID_PPV_ARGS(&outlinePipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (outline)");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryNormalsDescription = pipelineDescription;
    geometryNormalsDescription.VS = {
        geometryNormalsVertexShader.data(), geometryNormalsVertexShader.size()};
    geometryNormalsDescription.GS = {
        geometryNormalsGeometryShader.data(), geometryNormalsGeometryShader.size()};
    geometryNormalsDescription.PS = {
        geometryNormalsPixelShader.data(), geometryNormalsPixelShader.size()};
    geometryNormalsDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    geometryNormalsDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &geometryNormalsDescription,
            IID_PPV_ARGS(&geometryNormalsPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (geometry normals)");

    if (msaaSupported_)
    {
        const auto createMsaaVariant = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC description,
                                           ComPtr<ID3D12PipelineState>& destination,
                                           const char* errorText)
        {
            description.SampleDesc.Count = MsaaSampleCount;
            description.SampleDesc.Quality = 0;
            ThrowIfFailed(
                device_->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(&destination)),
                errorText);
        };
        createMsaaVariant(
            pipelineDescription, msaaPipelineState_,
            "ID3D12Device::CreateGraphicsPipelineState (MSAA opaque)");
        createMsaaVariant(
            transparentDescription, msaaTransparentPipelineState_,
            "ID3D12Device::CreateGraphicsPipelineState (MSAA transparent)");
        createMsaaVariant(
            stencilMaskDescription, msaaStencilMaskPipelineState_,
            "ID3D12Device::CreateGraphicsPipelineState (MSAA stencil mask)");
        createMsaaVariant(
            outlineDescription, msaaOutlinePipelineState_,
            "ID3D12Device::CreateGraphicsPipelineState (MSAA outline)");
        createMsaaVariant(
            geometryNormalsDescription, msaaGeometryNormalsPipelineState_,
            "ID3D12Device::CreateGraphicsPipelineState (MSAA geometry normals)");
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gBufferPipelineDescription = pipelineDescription;
    gBufferPipelineDescription.VS = {
        gBufferVertexShader.data(), gBufferVertexShader.size()};
    gBufferPipelineDescription.PS = {
        gBufferPixelShader.data(), gBufferPixelShader.size()};
    gBufferPipelineDescription.NumRenderTargets = GBufferCount;
    gBufferPipelineDescription.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gBufferPipelineDescription.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gBufferPipelineDescription.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &gBufferPipelineDescription, IID_PPV_ARGS(&gBufferPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (GBuffer)");

    // Shadow Pass 只输出深度：沿用相同顶点布局和 Root Signature，但不绑定 Pixel Shader/RTV。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPipelineDescription = pipelineDescription;
    shadowPipelineDescription.VS = {shadowVertexShader.data(), shadowVertexShader.size()};
    shadowPipelineDescription.PS = {};
    shadowPipelineDescription.NumRenderTargets = 0;
    shadowPipelineDescription.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    // 主深度缓冲为了 Stencil 使用 D24S8；Shadow Map 仍是纯 D32_FLOAT。
    // PSO 的 DSVFormat 必须与 Draw 时实际绑定的 DSV 完全一致，否则 Debug Layer 会以 0x87A 中断。
    shadowPipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadowPipelineDescription.RasterizerState.DepthBias = 1200;
    shadowPipelineDescription.RasterizerState.SlopeScaledDepthBias = 1.5F;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &shadowPipelineDescription, IID_PPV_ARGS(&shadowPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (shadow)");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pointShadowPipelineDescription =
        pipelineDescription;
    pointShadowPipelineDescription.VS = {
        pointShadowVertexShader.data(), pointShadowVertexShader.size()};
    pointShadowPipelineDescription.PS = {
        pointShadowPixelShader.data(), pointShadowPixelShader.size()};
    pointShadowPipelineDescription.NumRenderTargets = 1;
    pointShadowPipelineDescription.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    pointShadowPipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &pointShadowPipelineDescription,
            IID_PPV_ARGS(&pointShadowPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (point shadow)");

    // SSAO、Bloom 和最终 Tone Mapping 都是全屏三角形 Pass。
    const auto createFullscreenRoot = [&](const UINT srvCount,
                                          ComPtr<ID3D12RootSignature>& destination,
                                          const char* errorText)
    {
        D3D12_DESCRIPTOR_RANGE sourceRange{};
        sourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sourceRange.NumDescriptors = srvCount;
        sourceRange.BaseShaderRegister = 0;
        sourceRange.RegisterSpace = 0;
        sourceRange.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER fullscreenParameters[2]{};
        fullscreenParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        fullscreenParameters[0].Descriptor.ShaderRegister = 0;
        fullscreenParameters[0].Descriptor.RegisterSpace = 0;
        fullscreenParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        fullscreenParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        fullscreenParameters[1].DescriptorTable.NumDescriptorRanges = 1;
        fullscreenParameters[1].DescriptorTable.pDescriptorRanges = &sourceRange;
        fullscreenParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC fullscreenDescription{};
        fullscreenDescription.NumParameters = 2;
        fullscreenDescription.pParameters = fullscreenParameters;
        fullscreenDescription.NumStaticSamplers = 1;
        fullscreenDescription.pStaticSamplers = &sampler;
        fullscreenDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3D12SerializeRootSignature(
            &fullscreenDescription, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors);
        if (FAILED(result))
        {
            throw std::runtime_error(errorText);
        }
        ThrowIfFailed(
            device_->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&destination)),
            errorText);
    };
    createFullscreenRoot(2, ssaoRootSignature_, "Create SSAO root signature");
    createFullscreenRoot(1, bloomRootSignature_, "Create bloom root signature");
    createFullscreenRoot(2, postProcessRootSignature_, "Create post process root signature");
    createFullscreenRoot(1, skyRootSignature_, "Create sky root signature");

    // Deferred Lighting 需要同时读取 GBuffer、Shadow、SSAO 和 HDRI。
    D3D12_DESCRIPTOR_RANGE deferredRanges[5]{};
    deferredRanges[0] = {
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBufferCount, 0, 0, 0};
    deferredRanges[1] = {
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0};
    deferredRanges[2] = {
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 0};
    deferredRanges[3] = {
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, EnvironmentDescriptorCount, 5, 0, 0};
    deferredRanges[4] = {
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 9, 0, 0};
    D3D12_ROOT_PARAMETER deferredParameters[6]{};
    deferredParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    deferredParameters[0].Descriptor.ShaderRegister = 0;
    deferredParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT index = 0; index < 5; ++index)
    {
        deferredParameters[index + 1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        deferredParameters[index + 1].DescriptorTable.NumDescriptorRanges = 1;
        deferredParameters[index + 1].DescriptorTable.pDescriptorRanges =
            &deferredRanges[index];
        deferredParameters[index + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC deferredSamplers[2]{};
    deferredSamplers[0] = materialSampler;
    deferredSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    deferredSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    deferredSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    deferredSamplers[1] = shadowSampler;
    D3D12_ROOT_SIGNATURE_DESC deferredDescription{};
    deferredDescription.NumParameters = 6;
    deferredDescription.pParameters = deferredParameters;
    deferredDescription.NumStaticSamplers = 2;
    deferredDescription.pStaticSamplers = deferredSamplers;
    deferredDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> deferredSerialized;
    ComPtr<ID3DBlob> deferredErrors;
    ThrowIfFailed(
        D3D12SerializeRootSignature(
            &deferredDescription, D3D_ROOT_SIGNATURE_VERSION_1,
            &deferredSerialized, &deferredErrors),
        "D3D12SerializeRootSignature (deferred lighting)");
    ThrowIfFailed(
        device_->CreateRootSignature(
            0, deferredSerialized->GetBufferPointer(),
            deferredSerialized->GetBufferSize(),
            IID_PPV_ARGS(&deferredRootSignature_)),
        "ID3D12Device::CreateRootSignature (deferred lighting)");

    D3D12_RASTERIZER_DESC fullscreenRasterizer = rasterizerDescription;
    fullscreenRasterizer.CullMode = D3D12_CULL_MODE_NONE;
    D3D12_DEPTH_STENCIL_DESC fullscreenDepth{};
    fullscreenDepth.DepthEnable = FALSE;
    fullscreenDepth.StencilEnable = FALSE;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC fullscreenPipeline{};
    fullscreenPipeline.BlendState = blendDescription;
    fullscreenPipeline.SampleMask = std::numeric_limits<UINT>::max();
    fullscreenPipeline.RasterizerState = fullscreenRasterizer;
    fullscreenPipeline.DepthStencilState = fullscreenDepth;
    fullscreenPipeline.InputLayout = {nullptr, 0};
    fullscreenPipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    fullscreenPipeline.NumRenderTargets = 1;
    fullscreenPipeline.SampleDesc.Count = 1;
    fullscreenPipeline.VS = {ssaoVertexShader.data(), ssaoVertexShader.size()};
    fullscreenPipeline.PS = {ssaoPixelShader.data(), ssaoPixelShader.size()};
    fullscreenPipeline.pRootSignature = ssaoRootSignature_.Get();
    fullscreenPipeline.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &fullscreenPipeline, IID_PPV_ARGS(&ssaoPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (SSAO)");

    fullscreenPipeline.pRootSignature = bloomRootSignature_.Get();
    fullscreenPipeline.VS = {bloomVertexShader.data(), bloomVertexShader.size()};
    fullscreenPipeline.PS = {bloomPixelShader.data(), bloomPixelShader.size()};
    fullscreenPipeline.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &fullscreenPipeline, IID_PPV_ARGS(&bloomPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (bloom)");

    fullscreenPipeline.pRootSignature = postProcessRootSignature_.Get();
    fullscreenPipeline.VS = {postProcessVertexShader.data(), postProcessVertexShader.size()};
    fullscreenPipeline.PS = {postProcessPixelShader.data(), postProcessPixelShader.size()};
    fullscreenPipeline.RTVFormats[0] = BackBufferFormat;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &fullscreenPipeline, IID_PPV_ARGS(&postProcessPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (post process)");

    fullscreenPipeline.pRootSignature = skyRootSignature_.Get();
    fullscreenPipeline.VS = {skyVertexShader.data(), skyVertexShader.size()};
    fullscreenPipeline.PS = {skyPixelShader.data(), skyPixelShader.size()};
    fullscreenPipeline.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &fullscreenPipeline, IID_PPV_ARGS(&skyPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (sky)");
    if (msaaSupported_)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC msaaSkyDescription = fullscreenPipeline;
        msaaSkyDescription.SampleDesc.Count = MsaaSampleCount;
        ThrowIfFailed(
            device_->CreateGraphicsPipelineState(
                &msaaSkyDescription, IID_PPV_ARGS(&msaaSkyPipelineState_)),
            "ID3D12Device::CreateGraphicsPipelineState (MSAA sky)");
    }

    fullscreenPipeline.pRootSignature = deferredRootSignature_.Get();
    fullscreenPipeline.VS = {
        deferredVertexShader.data(), deferredVertexShader.size()};
    fullscreenPipeline.PS = {
        deferredPixelShader.data(), deferredPixelShader.size()};
    fullscreenPipeline.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &fullscreenPipeline, IID_PPV_ARGS(&deferredPipelineState_)),
        "ID3D12Device::CreateGraphicsPipelineState (deferred lighting)");
}

void D3D12Renderer::CreateSceneGeometry(const Assets::AssetManager& assets)
{
    std::vector<Assets::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    meshRanges_.clear();
    for (const std::string& key : assets.MeshKeys())
    {
        const Assets::MeshAsset* mesh = assets.FindMesh(key);
        if (mesh == nullptr || mesh->vertices.empty() || mesh->indices.empty())
        {
            continue;
        }
        const std::uint32_t baseVertex = static_cast<std::uint32_t>(vertices.size());
        const std::uint32_t startIndex = static_cast<std::uint32_t>(indices.size());
        vertices.insert(vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
        for (const std::uint32_t index : mesh->indices)
        {
            indices.push_back(baseVertex + index);
        }
        meshRanges_.emplace(key, MeshRange{
            static_cast<std::uint32_t>(mesh->indices.size()),
            static_cast<std::uint32_t>(mesh->vertices.size()), startIndex, baseVertex});
    }
    if (vertices.empty() || indices.empty())
    {
        throw std::runtime_error("AssetManager contains no drawable mesh geometry.");
    }

    const UINT vertexBufferSize =
        static_cast<UINT>(vertices.size() * sizeof(Assets::MeshVertex));
    const UINT indexBufferSize =
        static_cast<UINT>(indices.size() * sizeof(std::uint32_t));

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = vertexBufferSize;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vertexBuffer_)),
        "ID3D12Device::CreateCommittedResource (scene vertex buffer)");

    std::byte* mappedVertexData = nullptr;
    const D3D12_RANGE noCpuReads{0, 0};
    ThrowIfFailed(
        vertexBuffer_->Map(
            0, &noCpuReads, reinterpret_cast<void**>(&mappedVertexData)),
        "ID3D12Resource::Map (scene vertex buffer)");
    std::memcpy(mappedVertexData, vertices.data(), vertexBufferSize);
    vertexBuffer_->Unmap(0, nullptr);

    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.SizeInBytes = vertexBufferSize;
    vertexBufferView_.StrideInBytes = static_cast<UINT>(sizeof(Assets::MeshVertex));

    resourceDescription.Width = indexBufferSize;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&indexBuffer_)),
        "ID3D12Device::CreateCommittedResource (scene index buffer)");

    std::byte* mappedIndexData = nullptr;
    ThrowIfFailed(
        indexBuffer_->Map(
            0, &noCpuReads, reinterpret_cast<void**>(&mappedIndexData)),
        "ID3D12Resource::Map (scene index buffer)");
    std::memcpy(mappedIndexData, indices.data(), indexBufferSize);
    indexBuffer_->Unmap(0, nullptr);

    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.SizeInBytes = indexBufferSize;
    indexBufferView_.Format = DXGI_FORMAT_R32_UINT;
    uploadedAssetRevision_ = assets.Revision();
}

void D3D12Renderer::CreateMaterialTextures(const Assets::AssetManager& assets)
{
    const std::vector<std::string> meshKeys = assets.MeshKeys();
    if (meshKeys.size() > MaxSceneObjects)
    {
        throw std::runtime_error("Too many mesh assets for the material descriptor heap.");
    }

    // 每个 Mesh 固定占用连续的三个 SRV：Base Color、Normal、Metallic-Roughness。
    // 固定槽位比“有几张贴图就绑定几张”更容易追踪，也与 glTF 材质语义一一对应。
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors =
        MaterialDescriptorCount + 1 + GBufferCount + PostProcessDescriptorCount +
        EnvironmentDescriptorCount + PointShadowDescriptorCount;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(
        device_->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&materialSrvHeap_)),
        "ID3D12Device::CreateDescriptorHeap (material SRV)");
    gpuMaterialTextures_.clear();

    auto& allocator = commandAllocators_[frameIndex_];
    ThrowIfFailed(allocator->Reset(), "ID3D12CommandAllocator::Reset (texture upload)");
    ThrowIfFailed(commandList_->Reset(allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset (texture upload)");

    const UINT descriptorSize =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        materialSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
        materialSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    std::vector<ComPtr<ID3D12Resource>> uploadBuffers;

    const std::array<std::uint8_t, 4> whitePixel{255, 255, 255, 255};
    const std::array<std::uint8_t, 4> flatNormalPixel{128, 128, 255, 255};

    auto uploadTexture = [&](const Assets::CpuTexture& source,
                             const std::array<std::uint8_t, 4>& fallback,
                             const DXGI_FORMAT srvFormat,
                             ComPtr<ID3D12Resource>& destination)
    {
        const bool hasSource = source.IsValid();
        const UINT width = hasSource ? source.width : 1U;
        const UINT height = hasSource ? source.height : 1U;
        const std::uint8_t* pixels = hasSource ? source.rgba8.data() : fallback.data();

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC textureDescription{};
        textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDescription.Width = width;
        textureDescription.Height = height;
        textureDescription.DepthOrArraySize = 1;
        textureDescription.MipLevels = 1;
        textureDescription.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDescription,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&destination)),
            "ID3D12Device::CreateCommittedResource (material texture)");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        device_->GetCopyableFootprints(
            &textureDescription, 0, 1, 0, &footprint, &rowCount, &rowSize, &uploadSize);

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC uploadDescription{};
        uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDescription.Width = uploadSize;
        uploadDescription.Height = 1;
        uploadDescription.DepthOrArraySize = 1;
        uploadDescription.MipLevels = 1;
        uploadDescription.SampleDesc.Count = 1;
        uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> uploadBuffer;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)),
            "ID3D12Device::CreateCommittedResource (texture upload buffer)");

        std::byte* mappedData = nullptr;
        const D3D12_RANGE noCpuReads{0, 0};
        ThrowIfFailed(uploadBuffer->Map(0, &noCpuReads, reinterpret_cast<void**>(&mappedData)),
                      "ID3D12Resource::Map (texture upload buffer)");
        const std::size_t sourceRowSize = static_cast<std::size_t>(width) * 4;
        for (UINT row = 0; row < rowCount; ++row)
        {
            std::memcpy(
                mappedData + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                pixels + static_cast<std::size_t>(row) * sourceRowSize,
                sourceRowSize);
        }
        uploadBuffer->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = destination.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = uploadBuffer.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        commandList_->CopyTextureRegion(
            &destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        D3D12_RESOURCE_BARRIER toShaderResource = TransitionBarrier(
            destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &toShaderResource);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
        srvDescription.Format = srvFormat;
        srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDescription.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(destination.Get(), &srvDescription, cpuHandle);
        cpuHandle.ptr += descriptorSize;
        uploadBuffers.push_back(std::move(uploadBuffer));
    };

    for (const std::string& key : meshKeys)
    {
        const Assets::MeshAsset* mesh = assets.FindMesh(key);
        if (mesh == nullptr)
        {
            continue;
        }
        GpuMaterialTextures gpuMaterial;
        gpuMaterial.firstSrv = gpuHandle;
        uploadTexture(
            mesh->importedMaterial.baseColorTexture, whitePixel,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, gpuMaterial.resources[0]);
        uploadTexture(
            mesh->importedMaterial.normalTexture, flatNormalPixel,
            DXGI_FORMAT_R8G8B8A8_UNORM, gpuMaterial.resources[1]);
        uploadTexture(
            mesh->importedMaterial.metallicRoughnessTexture, whitePixel,
            DXGI_FORMAT_R8G8B8A8_UNORM, gpuMaterial.resources[2]);
        gpuHandle.ptr += static_cast<UINT64>(descriptorSize) * 3;
        gpuMaterialTextures_.emplace(key, std::move(gpuMaterial));
    }

    // Shadow Map 与材质纹理必须位于同一个 Shader-Visible Heap；D3D12 同一类型一次只能绑定一个 Heap。
    cpuHandle = materialSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(MaterialDescriptorCount) * descriptorSize;
    gpuHandle = materialSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += static_cast<UINT64>(MaterialDescriptorCount) * descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDescription{};
    shadowSrvDescription.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowSrvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrvDescription.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(shadowMap_.Get(), &shadowSrvDescription, cpuHandle);
    shadowMapSrv_ = gpuHandle;

    ThrowIfFailed(commandList_->Close(), "ID3D12GraphicsCommandList::Close (texture upload)");
    ID3D12CommandList* commandLists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);
    // Upload Buffer 只有在复制命令执行完之后才能释放；导入不是逐帧操作，因此这里一次等待最直观。
    WaitForGpu();
}

void D3D12Renderer::CreateEnvironmentTexture()
{
    constexpr std::uint32_t faceSize = 256;
    const std::filesystem::path hdrPath = FindProjectAsset(
        L"assets/environments/NewportLoft/newport_loft.hdr");
    const Assets::HdrCubemap source = Assets::LoadHdrCubemap(hdrPath, faceSize);
    if (!source.IsValid())
    {
        throw std::runtime_error("Newport Loft HDR environment conversion failed.");
    }
    const Assets::IblTextures ibl = Assets::BuildSplitSumIbl(source);
    if (!ibl.IsValid())
    {
        throw std::runtime_error("Split-Sum IBL precomputation failed.");
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC textureDescription{};
    textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescription.Width = faceSize;
    textureDescription.Height = faceSize;
    textureDescription.DepthOrArraySize = 6;
    textureDescription.MipLevels = 1;
    textureDescription.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    environmentCubemap_.Reset();
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&environmentCubemap_)),
        "ID3D12Device::CreateCommittedResource (environment cubemap)");

    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 6> footprints{};
    std::array<UINT, 6> rowCounts{};
    std::array<UINT64, 6> rowSizes{};
    UINT64 uploadSize = 0;
    device_->GetCopyableFootprints(
        &textureDescription, 0, 6, 0, footprints.data(), rowCounts.data(),
        rowSizes.data(), &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC uploadDescription{};
    uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDescription.Width = uploadSize;
    uploadDescription.Height = 1;
    uploadDescription.DepthOrArraySize = 1;
    uploadDescription.MipLevels = 1;
    uploadDescription.SampleDesc.Count = 1;
    uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&uploadBuffer)),
        "ID3D12Device::CreateCommittedResource (environment upload)");

    std::byte* mappedData = nullptr;
    const D3D12_RANGE noCpuReads{0, 0};
    ThrowIfFailed(
        uploadBuffer->Map(0, &noCpuReads, reinterpret_cast<void**>(&mappedData)),
        "ID3D12Resource::Map (environment upload)");
    const std::size_t sourceRowBytes =
        static_cast<std::size_t>(faceSize) * sizeof(DirectX::XMFLOAT4);
    const std::size_t sourceFacePixels =
        static_cast<std::size_t>(faceSize) * faceSize;
    for (UINT face = 0; face < 6; ++face)
    {
        const std::byte* sourceFace = reinterpret_cast<const std::byte*>(
            source.rgba32f.data() + static_cast<std::size_t>(face) * sourceFacePixels);
        for (UINT row = 0; row < rowCounts[face]; ++row)
        {
            std::memcpy(
                mappedData + footprints[face].Offset +
                    static_cast<std::size_t>(row) * footprints[face].Footprint.RowPitch,
                sourceFace + static_cast<std::size_t>(row) * sourceRowBytes,
                sourceRowBytes);
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    auto& allocator = commandAllocators_[frameIndex_];
    ThrowIfFailed(
        allocator->Reset(),
        "ID3D12CommandAllocator::Reset (environment upload)");
    ThrowIfFailed(
        commandList_->Reset(allocator.Get(), nullptr),
        "ID3D12GraphicsCommandList::Reset (environment upload)");
    for (UINT face = 0; face < 6; ++face)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = environmentCubemap_.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = face;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = uploadBuffer.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprints[face];
        commandList_->CopyTextureRegion(
            &destination, 0, 0, 0, &sourceLocation, nullptr);
    }
    const D3D12_RESOURCE_BARRIER toShaderResource = TransitionBarrier(
        environmentCubemap_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1, &toShaderResource);

    // 下面三张资源才是标准 Split-Sum IBL 的运行时输入。环境积分只在启动时执行一次；
    // 每个像素随后只需采样 Irradiance、Roughness Mip 和 BRDF LUT。
    std::vector<ComPtr<ID3D12Resource>> iblUploadBuffers;
    const auto uploadCubemap = [&](const std::vector<Assets::HdrCubemap>& mips,
                                   ComPtr<ID3D12Resource>& destination,
                                   const char* resourceName)
    {
        D3D12_RESOURCE_DESC cubeDescription = textureDescription;
        cubeDescription.Width = mips.front().faceSize;
        cubeDescription.Height = mips.front().faceSize;
        cubeDescription.MipLevels = static_cast<UINT16>(mips.size());
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &cubeDescription,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&destination)),
            resourceName);

        const UINT mipCount = static_cast<UINT>(mips.size());
        const UINT subresourceCount = mipCount * 6U;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> cubeFootprints(subresourceCount);
        std::vector<UINT> cubeRows(subresourceCount);
        std::vector<UINT64> cubeRowSizes(subresourceCount);
        UINT64 cubeUploadSize = 0;
        device_->GetCopyableFootprints(
            &cubeDescription, 0, subresourceCount, 0,
            cubeFootprints.data(), cubeRows.data(), cubeRowSizes.data(),
            &cubeUploadSize);

        D3D12_RESOURCE_DESC cubeUploadDescription = uploadDescription;
        cubeUploadDescription.Width = cubeUploadSize;
        ComPtr<ID3D12Resource> cubeUpload;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &cubeUploadDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&cubeUpload)),
            "ID3D12Device::CreateCommittedResource (IBL cube upload)");
        std::byte* cubeMapped = nullptr;
        ThrowIfFailed(
            cubeUpload->Map(0, &noCpuReads, reinterpret_cast<void**>(&cubeMapped)),
            "ID3D12Resource::Map (IBL cube upload)");
        for (UINT face = 0; face < 6; ++face)
        {
            for (UINT mip = 0; mip < mipCount; ++mip)
            {
                const UINT subresource = mip + face * mipCount;
                const Assets::HdrCubemap& sourceMip = mips[mip];
                const std::size_t facePixels =
                    static_cast<std::size_t>(sourceMip.faceSize) * sourceMip.faceSize;
                const std::byte* sourceFace = reinterpret_cast<const std::byte*>(
                    sourceMip.rgba32f.data() + static_cast<std::size_t>(face) * facePixels);
                const std::size_t rowBytes =
                    static_cast<std::size_t>(sourceMip.faceSize) * sizeof(DirectX::XMFLOAT4);
                for (UINT row = 0; row < cubeRows[subresource]; ++row)
                {
                    std::memcpy(
                        cubeMapped + cubeFootprints[subresource].Offset +
                            static_cast<std::size_t>(row) *
                                cubeFootprints[subresource].Footprint.RowPitch,
                        sourceFace + static_cast<std::size_t>(row) * rowBytes,
                        rowBytes);
                }
                D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
                destinationLocation.pResource = destination.Get();
                destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destinationLocation.SubresourceIndex = subresource;
                D3D12_TEXTURE_COPY_LOCATION uploadLocation{};
                uploadLocation.pResource = cubeUpload.Get();
                uploadLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                uploadLocation.PlacedFootprint = cubeFootprints[subresource];
                commandList_->CopyTextureRegion(
                    &destinationLocation, 0, 0, 0, &uploadLocation, nullptr);
            }
        }
        cubeUpload->Unmap(0, nullptr);
        const D3D12_RESOURCE_BARRIER cubeToShader = TransitionBarrier(
            destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &cubeToShader);
        iblUploadBuffers.push_back(std::move(cubeUpload));
    };

    const std::vector<Assets::HdrCubemap> irradianceMips{ibl.irradiance};
    uploadCubemap(
        irradianceMips, irradianceCubemap_,
        "ID3D12Device::CreateCommittedResource (irradiance cubemap)");
    uploadCubemap(
        ibl.prefilteredMips, prefilteredEnvironment_,
        "ID3D12Device::CreateCommittedResource (prefiltered environment)");

    D3D12_RESOURCE_DESC brdfDescription{};
    brdfDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    brdfDescription.Width = ibl.brdfSize;
    brdfDescription.Height = ibl.brdfSize;
    brdfDescription.DepthOrArraySize = 1;
    brdfDescription.MipLevels = 1;
    brdfDescription.Format = DXGI_FORMAT_R32G32_FLOAT;
    brdfDescription.SampleDesc.Count = 1;
    brdfDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &brdfDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&brdfLut_)),
        "ID3D12Device::CreateCommittedResource (BRDF LUT)");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT brdfFootprint{};
    UINT brdfRows = 0;
    UINT64 brdfRowSize = 0;
    UINT64 brdfUploadSize = 0;
    device_->GetCopyableFootprints(
        &brdfDescription, 0, 1, 0, &brdfFootprint,
        &brdfRows, &brdfRowSize, &brdfUploadSize);
    D3D12_RESOURCE_DESC brdfUploadDescription = uploadDescription;
    brdfUploadDescription.Width = brdfUploadSize;
    ComPtr<ID3D12Resource> brdfUpload;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &brdfUploadDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&brdfUpload)),
        "ID3D12Device::CreateCommittedResource (BRDF LUT upload)");
    std::byte* brdfMapped = nullptr;
    ThrowIfFailed(
        brdfUpload->Map(0, &noCpuReads, reinterpret_cast<void**>(&brdfMapped)),
        "ID3D12Resource::Map (BRDF LUT upload)");
    const std::size_t brdfSourceRowBytes =
        static_cast<std::size_t>(ibl.brdfSize) * sizeof(DirectX::XMFLOAT2);
    for (UINT row = 0; row < brdfRows; ++row)
    {
        std::memcpy(
            brdfMapped + brdfFootprint.Offset +
                static_cast<std::size_t>(row) * brdfFootprint.Footprint.RowPitch,
            reinterpret_cast<const std::byte*>(ibl.brdfLut.data()) +
                static_cast<std::size_t>(row) * brdfSourceRowBytes,
            brdfSourceRowBytes);
    }
    brdfUpload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION brdfDestination{};
    brdfDestination.pResource = brdfLut_.Get();
    brdfDestination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION brdfSource{};
    brdfSource.pResource = brdfUpload.Get();
    brdfSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    brdfSource.PlacedFootprint = brdfFootprint;
    commandList_->CopyTextureRegion(
        &brdfDestination, 0, 0, 0, &brdfSource, nullptr);
    const D3D12_RESOURCE_BARRIER brdfToShader = TransitionBarrier(
        brdfLut_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1, &brdfToShader);

    const UINT descriptorSize = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    constexpr UINT descriptorIndex = MaterialDescriptorCount + 1 + GBufferCount +
        PostProcessDescriptorCount;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        materialSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * descriptorSize;
    environmentCubemapSrv_ = materialSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    environmentCubemapSrv_.ptr +=
        static_cast<UINT64>(descriptorIndex) * descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.TextureCube.MipLevels = 1;
    device_->CreateShaderResourceView(
        environmentCubemap_.Get(), &srvDescription, cpuHandle);
    cpuHandle.ptr += descriptorSize;
    irradianceCubemapSrv_ = environmentCubemapSrv_;
    irradianceCubemapSrv_.ptr += descriptorSize;
    srvDescription.TextureCube.MipLevels = 1;
    device_->CreateShaderResourceView(
        irradianceCubemap_.Get(), &srvDescription, cpuHandle);
    cpuHandle.ptr += descriptorSize;
    prefilteredEnvironmentSrv_ = irradianceCubemapSrv_;
    prefilteredEnvironmentSrv_.ptr += descriptorSize;
    srvDescription.TextureCube.MipLevels = static_cast<UINT>(
        ibl.prefilteredMips.size());
    device_->CreateShaderResourceView(
        prefilteredEnvironment_.Get(), &srvDescription, cpuHandle);
    cpuHandle.ptr += descriptorSize;
    brdfLutSrv_ = prefilteredEnvironmentSrv_;
    brdfLutSrv_.ptr += descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC brdfSrvDescription{};
    brdfSrvDescription.Format = DXGI_FORMAT_R32G32_FLOAT;
    brdfSrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    brdfSrvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    brdfSrvDescription.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(brdfLut_.Get(), &brdfSrvDescription, cpuHandle);
    cpuHandle.ptr += descriptorSize;
    pointShadowMapSrv_ = brdfLutSrv_;
    pointShadowMapSrv_.ptr += descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC pointShadowSrvDescription{};
    pointShadowSrvDescription.Format = DXGI_FORMAT_R32_FLOAT;
    pointShadowSrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    pointShadowSrvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    pointShadowSrvDescription.TextureCube.MipLevels = 1;
    device_->CreateShaderResourceView(
        pointShadowMap_.Get(), &pointShadowSrvDescription, cpuHandle);

    ThrowIfFailed(
        commandList_->Close(),
        "ID3D12GraphicsCommandList::Close (environment upload)");
    ID3D12CommandList* commandLists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);
    // 上传缓冲必须活到 GPU 完成复制；环境贴图只在初始化或资产同步时创建。
    WaitForGpu();
}

void D3D12Renderer::CreateConstantBuffer()
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Width = static_cast<UINT64>(ConstantBufferStride) *
        FrameCount * MaxSceneObjects * SceneConstantPassCount;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constantBuffer_)),
        "ID3D12Device::CreateCommittedResource (scene constant buffer)");

    // Constant Buffer 每帧都会更新，因此保持映射可以避免重复 Map/Unmap。
    // 空读取范围表示 CPU 只写入，不会从该资源读取数据。
    const D3D12_RANGE noCpuReads{0, 0};
    ThrowIfFailed(
        constantBuffer_->Map(
            0, &noCpuReads, reinterpret_cast<void**>(&mappedConstantBufferData_)),
        "ID3D12Resource::Map (scene constant buffer)");

    // StructuredBuffer 不要求 256 字节对齐；每个实例紧凑保存三张矩阵。
    D3D12_RESOURCE_DESC instanceDescription = resourceDescription;
    instanceDescription.Width = static_cast<UINT64>(FrameCount) *
        MaxSceneObjects * InstanceDataStride;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &instanceDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&instanceBuffer_)),
        "ID3D12Device::CreateCommittedResource (instance buffer)");
    ThrowIfFailed(
        instanceBuffer_->Map(
            0, &noCpuReads, reinterpret_cast<void**>(&mappedInstanceData_)),
        "ID3D12Resource::Map (instance buffer)");

    const auto createSmallConstants = [&](ComPtr<ID3D12Resource>& buffer,
                                          std::byte*& mappedData,
                                          const UINT64 byteSize,
                                          const char* resourceName,
                                          const char* mapName)
    {
        D3D12_RESOURCE_DESC smallDescription = resourceDescription;
        smallDescription.Width = byteSize;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &smallDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&buffer)),
            resourceName);
        ThrowIfFailed(
            buffer->Map(0, &noCpuReads, reinterpret_cast<void**>(&mappedData)),
            mapName);
    };
    createSmallConstants(
        ssaoConstantsBuffer_, mappedSsaoConstants_, FrameCount * 256ULL,
        "ID3D12Device::CreateCommittedResource (SSAO constants)",
        "ID3D12Resource::Map (SSAO constants)");
    createSmallConstants(
        postProcessConstantsBuffer_, mappedPostProcessConstants_,
        FrameCount * 4ULL * 256ULL,
        "ID3D12Device::CreateCommittedResource (post process constants)",
        "ID3D12Resource::Map (post process constants)");
    createSmallConstants(
        skyConstantsBuffer_, mappedSkyConstants_, FrameCount * 256ULL,
        "ID3D12Device::CreateCommittedResource (sky constants)",
        "ID3D12Resource::Map (sky constants)");
}

void D3D12Renderer::UpdateViewportAndScissor()
{
    // Viewport 把裁剪空间 [-1, 1] 映射到窗口像素；Scissor 则限制允许写入的区域。
    viewport_.TopLeftX = 0.0F;
    viewport_.TopLeftY = 0.0F;
    viewport_.Width = static_cast<float>(width_);
    viewport_.Height = static_cast<float>(height_);
    viewport_.MinDepth = 0.0F;
    viewport_.MaxDepth = 1.0F;

    scissorRect_.left = 0;
    scissorRect_.top = 0;
    scissorRect_.right = static_cast<LONG>(width_);
    scissorRect_.bottom = static_cast<LONG>(height_);
}

void D3D12Renderer::UpdateObjectConstants(
    const Scene::Scene& scene,
    const Editor::DebugViewMode debugViewMode,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection)
{
    using namespace DirectX;

    // 相机现在由 EditorCamera 控制。View Matrix 的逆矩阵会把相机局部原点还原到世界空间。
    XMFLOAT3 cameraPosition{};
    const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
    XMStoreFloat3(&cameraPosition, inverseView.r[3]);
    const XMMATRIX viewProjection = view * projection;

    // 这里把 Light Direction 定义为“从表面指向光源”的单位方向。方向光没有位置，
    // 所以只让这个方向缓慢绕 Y 轴旋转，就能观察高光如何沿球面移动。
    // 第一盏 Directional Light 负责现有的 Shadow Map；方向来自 Scene，不再随时间旋转。
    XMFLOAT3 lightDirection{0.35F, 0.80F, 0.45F};
    XMFLOAT3 lightColor{1.0F, 0.92F, 0.80F};
    float lightIntensity = 4.0F;
    std::uint32_t shadowLightIndex = 0;
    bool foundDirectionalLight = false;
    for (std::size_t index = 0; index < scene.Lights().size(); ++index)
    {
        const Scene::SceneLight& light = scene.Lights()[index];
        if (light.type != Scene::LightType::Directional)
        {
            continue;
        }
        lightDirection = light.direction;
        lightColor = light.color;
        lightIntensity = light.intensity;
        shadowLightIndex = static_cast<std::uint32_t>(index);
        foundDirectionalLight = true;
        break;
    }
    if (!foundDirectionalLight)
    {
        lightIntensity = 0.0F;
    }
    lightDirection = SafeNormalize(lightDirection);
    // PBR 在物理上允许 HDR Radiance 超过 1，最后再统一 Tone Mapping 到显示范围。
    const XMVECTOR lightPosition = XMVectorScale(XMLoadFloat3(&lightDirection), 18.0F);
    const XMMATRIX lightView = XMMatrixLookAtLH(
        lightPosition, XMVectorZero(), XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    const XMMATRIX lightProjection = XMMatrixOrthographicLH(18.0F, 18.0F, 0.1F, 40.0F);
    const XMMATRIX lightViewProjection = lightView * lightProjection;

    std::uint32_t pointShadowLightIndex = std::numeric_limits<std::uint32_t>::max();
    XMFLOAT3 pointShadowPosition{};
    float pointShadowRange = 1.0F;
    for (std::size_t index = 0; index < scene.Lights().size(); ++index)
    {
        if (scene.Lights()[index].type == Scene::LightType::Point)
        {
            pointShadowLightIndex = static_cast<std::uint32_t>(index);
            pointShadowPosition = scene.Lights()[index].position;
            pointShadowRange = std::max(scene.Lights()[index].range, 0.1F);
            break;
        }
    }
    const std::array<XMVECTOR, PointShadowFaceCount> pointDirections = {
        XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F), XMVectorSet(-1.0F, 0.0F, 0.0F, 0.0F),
        XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F), XMVectorSet(0.0F, -1.0F, 0.0F, 0.0F),
        XMVectorSet(0.0F, 0.0F, 1.0F, 0.0F), XMVectorSet(0.0F, 0.0F, -1.0F, 0.0F)};
    const std::array<XMVECTOR, PointShadowFaceCount> pointUps = {
        XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F), XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F),
        XMVectorSet(0.0F, 0.0F, -1.0F, 0.0F), XMVectorSet(0.0F, 0.0F, 1.0F, 0.0F),
        XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F), XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F)};
    std::array<XMMATRIX, PointShadowFaceCount> pointViewProjections{};
    const XMVECTOR pointPositionVector = XMLoadFloat3(&pointShadowPosition);
    const XMMATRIX pointProjection = XMMatrixPerspectiveFovLH(
        XM_PIDIV2, 1.0F, 0.1F, pointShadowRange);
    for (std::size_t face = 0; face < PointShadowFaceCount; ++face)
    {
        pointViewProjections[face] = XMMatrixLookAtLH(
            pointPositionVector,
            XMVectorAdd(pointPositionVector, pointDirections[face]),
            pointUps[face]) * pointProjection;
    }

    const std::uint32_t objectCount = static_cast<std::uint32_t>(scene.Objects().size());
    if (objectCount > MaxSceneObjects)
    {
        throw std::runtime_error("Scene exceeds the 128-object teaching limit.");
    }
    for (std::uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        const Scene::SceneObject& object = scene.Objects()[objectIndex];
        const XMMATRIX model = Scene::TransformMatrix(object.transform);
        const XMMATRIX normalMatrix = Scene::NormalMatrix(model);

        InstanceData instance{};
        XMStoreFloat4x4(&instance.model, XMMatrixTranspose(model));
        XMStoreFloat4x4(
            &instance.modelViewProjection,
            XMMatrixTranspose(model * viewProjection));
        XMStoreFloat4x4(&instance.normalMatrix, XMMatrixTranspose(normalMatrix));
        const std::size_t instanceIndex =
            static_cast<std::size_t>(frameIndex_) * MaxSceneObjects + objectIndex;
        std::memcpy(
            mappedInstanceData_ + instanceIndex * InstanceDataStride,
            &instance, sizeof(instance));

        ObjectConstants constants{};
        XMStoreFloat4x4(&constants.model, XMMatrixTranspose(model));
        XMStoreFloat4x4(
            &constants.modelViewProjection,
            XMMatrixTranspose(model * viewProjection));
        constants.baseColor = object.material.baseColor;
        constants.cameraPosition = cameraPosition;
        constants.roughness = object.material.roughness;
        constants.lightDirection = lightDirection;
        constants.iblIntensity = scene.Environment().intensity;
        constants.lightColor = {
            lightColor.x * lightIntensity,
            lightColor.y * lightIntensity,
            lightColor.z * lightIntensity,
        };
        constants.metallic = object.material.metallic;
        constants.debugViewMode = static_cast<float>(debugViewMode);
        constants.environmentRotationRadians =
            XMConvertToRadians(scene.Environment().rotationDegrees);
        constants.normalStrength = object.material.normalStrength;
        constants.parallaxHeightScale = object.material.parallaxHeightScale;
        constants.ssaoStrength = scene.Environment().renderPath == Scene::RenderPath::Deferred
            ? scene.Environment().ssaoStrength
            : 0.0F;
        constants.iblSpecularStrength = scene.Environment().iblSpecularStrength;
        const std::size_t lightCount = std::min(scene.Lights().size(), MaxSceneLights);
        constants.lightCount = static_cast<std::uint32_t>(lightCount);
        constants.shadowLightIndex = lightCount == 0
            ? 0
            : std::min(shadowLightIndex, static_cast<std::uint32_t>(lightCount - 1));
        constants.pointShadowLightIndex = pointShadowLightIndex < lightCount
            ? pointShadowLightIndex
            : std::numeric_limits<std::uint32_t>::max();
        constants.pointShadowRange = pointShadowRange;
        for (std::size_t lightIndex = 0; lightIndex < lightCount; ++lightIndex)
        {
            const Scene::SceneLight& light = scene.Lights()[lightIndex];
            const XMFLOAT3 direction = SafeNormalize(light.direction);
            constants.lights[lightIndex].positionType = {
                light.position.x,
                light.position.y,
                light.position.z,
                static_cast<float>(light.type),
            };
            constants.lights[lightIndex].directionIntensity = {
                direction.x,
                direction.y,
                direction.z,
                light.intensity,
            };
            constants.lights[lightIndex].colorRange = {
                light.color.x,
                light.color.y,
                light.color.z,
                light.range,
            };
            constants.lights[lightIndex].areaSize = {
                light.type == Scene::LightType::Spot
                    ? std::cos(XMConvertToRadians(light.innerConeDegrees))
                    : light.width,
                light.type == Scene::LightType::Spot
                    ? std::cos(XMConvertToRadians(light.outerConeDegrees))
                    : light.height,
                0.0F,
                0.0F,
            };
        }
        XMStoreFloat4x4(
            &constants.lightViewProjection, XMMatrixTranspose(lightViewProjection));
        XMStoreFloat4x4(
            &constants.normalMatrix, XMMatrixTranspose(normalMatrix));

        const std::size_t sliceIndex =
            static_cast<std::size_t>(frameIndex_) * MaxSceneObjects *
                SceneConstantPassCount + objectIndex;
        std::byte* destination =
            mappedConstantBufferData_ + sliceIndex * ConstantBufferStride;
        std::memcpy(destination, &constants, sizeof(constants));

        ObjectConstants shadowConstants = constants;
        XMStoreFloat4x4(
            &shadowConstants.modelViewProjection,
            XMMatrixTranspose(model * lightViewProjection));
        const std::size_t shadowSliceIndex = sliceIndex + MaxSceneObjects;
        std::byte* shadowDestination =
            mappedConstantBufferData_ + shadowSliceIndex * ConstantBufferStride;
        std::memcpy(shadowDestination, &shadowConstants, sizeof(shadowConstants));

        for (std::size_t face = 0; face < PointShadowFaceCount; ++face)
        {
            ObjectConstants pointConstants = constants;
            pointConstants.cameraPosition = pointShadowPosition;
            pointConstants.roughness = pointShadowRange;
            XMStoreFloat4x4(
                &pointConstants.modelViewProjection,
                XMMatrixTranspose(model * pointViewProjections[face]));
            const std::size_t pointSliceIndex = sliceIndex +
                (2 + face) * MaxSceneObjects;
            std::memcpy(
                mappedConstantBufferData_ + pointSliceIndex * ConstantBufferStride,
                &pointConstants, sizeof(pointConstants));
        }
    }
}

D3D12Renderer::MeshRange D3D12Renderer::MeshFor(const std::string& assetKey) const noexcept
{
    const auto iterator = meshRanges_.find(assetKey);
    return iterator == meshRanges_.end() ? MeshRange{} : iterator->second;
}

void D3D12Renderer::SynchronizeAssets(const Assets::AssetManager& assets)
{
    if (uploadedAssetRevision_ == assets.Revision())
    {
        return;
    }

    // 当前学习版把全部 Mesh 合并进一组 Upload Buffer。导入发生频率很低，等待一次 GPU
    // 并重建 Buffer 比引入复杂的增量上传器更容易理解；渲染帧中不会重复执行。
    WaitForGpu();
    vertexBuffer_.Reset();
    indexBuffer_.Reset();
    CreateSceneGeometry(assets);
    CreateMaterialTextures(assets);
    CreateGBufferViews();
    CreateEnvironmentTexture();
}

void D3D12Renderer::CreateSynchronizationObjects()
{
    ThrowIfFailed(
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
        "ID3D12Device::CreateFence");

    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr)
    {
        throw std::runtime_error("CreateEventW failed for the D3D12 fence.");
    }
}

void D3D12Renderer::CreateImGui()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = ImGuiDescriptorCount;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(
        device_->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&imguiSrvHeap_)),
        "ID3D12Device::CreateDescriptorHeap (ImGui SRV)");
    imguiSrvDescriptorSize_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 当前面板由代码固定在窗口右侧，不需要在仓库根目录生成 imgui.ini。
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(window_))
    {
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui_ImplWin32_Init failed.");
    }

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = device_.Get();
    initInfo.CommandQueue = commandQueue_.Get();
    initInfo.NumFramesInFlight = static_cast<int>(FrameCount);
    initInfo.RTVFormat = BackBufferFormat;
    initInfo.DSVFormat = DepthBufferFormat;
    initInfo.SrvDescriptorHeap = imguiSrvHeap_.Get();
    initInfo.UserData = this;
    initInfo.SrvDescriptorAllocFn = AllocateImGuiDescriptor;
    initInfo.SrvDescriptorFreeFn = FreeImGuiDescriptor;
    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui_ImplDX12_Init failed.");
    }

    imguiInitialized_ = true;
}

void D3D12Renderer::AllocateImGuiDescriptor(
    ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle)
{
    auto* renderer = static_cast<D3D12Renderer*>(info->UserData);
    for (std::uint32_t index = 0; index < ImGuiDescriptorCount; ++index)
    {
        if (renderer->imguiDescriptorUsed_[index])
        {
            continue;
        }

        renderer->imguiDescriptorUsed_[index] = true;
        *cpuHandle = renderer->imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        *gpuHandle = renderer->imguiSrvHeap_->GetGPUDescriptorHandleForHeapStart();
        cpuHandle->ptr +=
            static_cast<SIZE_T>(index) * renderer->imguiSrvDescriptorSize_;
        gpuHandle->ptr +=
            static_cast<UINT64>(index) * renderer->imguiSrvDescriptorSize_;
        return;
    }

    throw std::runtime_error("ImGui SRV descriptor heap is full.");
}

void D3D12Renderer::FreeImGuiDescriptor(
    ImGui_ImplDX12_InitInfo* info,
    const D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    auto* renderer = static_cast<D3D12Renderer*>(info->UserData);
    const SIZE_T firstHandle =
        renderer->imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart().ptr;
    const SIZE_T offset = cpuHandle.ptr - firstHandle;
    const std::uint32_t index =
        static_cast<std::uint32_t>(offset / renderer->imguiSrvDescriptorSize_);
    if (index < ImGuiDescriptorCount)
    {
        renderer->imguiDescriptorUsed_[index] = false;
    }

    // CPU/GPU Handle 指向同一个描述符槽；通过 CPU Handle 计算一次索引即可。
    static_cast<void>(gpuHandle);
}

bool D3D12Renderer::HandleWindowMessage(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept
{
    if (!imguiInitialized_)
    {
        return false;
    }
    return ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
}

void D3D12Renderer::BeginImGuiFrame(
    Scene::Scene& scene,
    Editor::EditorLayer& editor,
    Assets::AssetManager& assets)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    editor.Draw(scene, assets, static_cast<float>(width_), static_cast<float>(height_));
    ImGui::Render();
}

void D3D12Renderer::Render(
    Scene::Scene& scene,
    Editor::EditorLayer& editor,
    Assets::AssetManager& assets)
{
    if (!initialized_ || width_ == 0 || height_ == 0)
    {
        return;
    }

    SynchronizeAssets(assets);

    // 上一轮末尾的 WaitForFrame 已保证 GPU 不再引用当前帧的命令分配器，
    // 所以这里可以安全地重置并重新记录命令。
    auto& allocator = commandAllocators_[frameIndex_];
    ThrowIfFailed(allocator->Reset(), "ID3D12CommandAllocator::Reset");
    ThrowIfFailed(commandList_->Reset(allocator.Get(), pipelineState_.Get()),
                  "ID3D12GraphicsCommandList::Reset");

    const float aspectRatio = static_cast<float>(width_) / static_cast<float>(height_);

    // EditorCamera 先处理视角输入，Editor 再修改 Scene；Renderer 只读取最终结果。
    BeginImGuiFrame(scene, editor, assets);
    const DirectX::XMMATRIX view = editor.ViewMatrix();
    const DirectX::XMMATRIX projection = editor.ProjectionMatrix(aspectRatio);
    UpdateObjectConstants(scene, editor.DebugView(), view, projection);
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* materialHeaps[] = {materialSrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, materialHeaps);
    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRect_);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->IASetIndexBuffer(&indexBufferView_);

    const D3D12_GPU_VIRTUAL_ADDRESS frameConstantBufferAddress =
        constantBuffer_->GetGPUVirtualAddress() +
        static_cast<UINT64>(frameIndex_) * MaxSceneObjects *
            SceneConstantPassCount * ConstantBufferStride;
    const D3D12_GPU_VIRTUAL_ADDRESS frameInstanceBufferAddress =
        instanceBuffer_->GetGPUVirtualAddress() +
        static_cast<UINT64>(frameIndex_) * MaxSceneObjects * InstanceDataStride;
    const auto sameInstanceBatch = [&](const std::uint32_t left,
                                       const std::uint32_t right)
    {
        const Scene::SceneObject& a = scene.Objects()[left];
        const Scene::SceneObject& b = scene.Objects()[right];
        const auto& x = a.material;
        const auto& y = b.material;
        // 一个实例 Draw 共享 Pixel Shader 材质常量和纹理，所以只有完全相同的材质才能合批。
        return a.assetKey == b.assetKey &&
            x.baseColor.x == y.baseColor.x && x.baseColor.y == y.baseColor.y &&
            x.baseColor.z == y.baseColor.z && x.baseColor.w == y.baseColor.w &&
            x.roughness == y.roughness && x.metallic == y.metallic &&
            x.normalStrength == y.normalStrength &&
            x.parallaxHeightScale == y.parallaxHeightScale;
    };

    SsaoConstants ssaoConstants{};
    ssaoConstants.invResolution = {
        1.0F / static_cast<float>(width_), 1.0F / static_cast<float>(height_)};
    ssaoConstants.radius = 1.5F;
    ssaoConstants.strength = scene.Environment().ssaoStrength;
    const UINT64 ssaoConstantsOffset = static_cast<UINT64>(frameIndex_) * 256ULL;
    std::memcpy(
        mappedSsaoConstants_ + ssaoConstantsOffset,
        &ssaoConstants, sizeof(ssaoConstants));

    const bool needsGBuffer =
        scene.Environment().renderPath == Scene::RenderPath::Deferred ||
        static_cast<std::uint32_t>(editor.DebugView()) >= 8U;
    if (needsGBuffer)
    {
        // Deferred 或 GBuffer Debug 模式才执行几何预写；普通 Forward Lit 不支付这笔开销。
        if (gBufferReadyForSampling_)
        {
            std::array<D3D12_RESOURCE_BARRIER, GBufferCount> toRenderTargets{};
            for (std::size_t index = 0; index < GBufferCount; ++index)
            {
                toRenderTargets[index] = TransitionBarrier(
                    gBufferTargets_[index].Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            commandList_->ResourceBarrier(GBufferCount, toRenderTargets.data());
        }
        commandList_->SetPipelineState(gBufferPipelineState_.Get());
        commandList_->RSSetViewports(1, &viewport_);
        commandList_->RSSetScissorRects(1, &scissorRect_);
        const D3D12_CPU_DESCRIPTOR_HANDLE depthDsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        commandList_->OMSetRenderTargets(
            GBufferCount, gBufferRtvs_.data(), FALSE, &depthDsv);
        const float gBufferClearValues[GBufferCount][4] = {
            {0.0F, 0.0F, 0.0F, 0.0F},
            {0.5F, 0.5F, 1.0F, 0.0F},
            {0.0F, 0.0F, 0.0F, 0.0F},
        };
        for (std::size_t index = 0; index < GBufferCount; ++index)
        {
            commandList_->ClearRenderTargetView(
                gBufferRtvs_[index], gBufferClearValues[index], 0, nullptr);
        }
        commandList_->ClearDepthStencilView(
            depthDsv,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0F, 0, 0, nullptr);
        for (std::uint32_t objectIndex = 0;
             objectIndex < static_cast<std::uint32_t>(scene.Objects().size());)
        {
            // 半透明物体不写入 GBuffer；Deferred 完成不透明光照后，再由 Forward 混合。
            if (scene.Objects()[objectIndex].material.baseColor.w < 0.999F)
            {
                ++objectIndex;
                continue;
            }
            const MeshRange mesh = MeshFor(scene.Objects()[objectIndex].assetKey);
            if (mesh.indexCount == 0)
            {
                ++objectIndex;
                continue;
            }
            const auto materialIterator = gpuMaterialTextures_.find(
                scene.Objects()[objectIndex].assetKey);
            if (materialIterator == gpuMaterialTextures_.end())
            {
                ++objectIndex;
                continue;
            }
            std::uint32_t instanceCount = 1;
            while (objectIndex + instanceCount < scene.Objects().size() &&
                   scene.Objects()[objectIndex + instanceCount].material.baseColor.w >= 0.999F &&
                   sameInstanceBatch(objectIndex, objectIndex + instanceCount))
            {
                ++instanceCount;
            }
            commandList_->SetGraphicsRootConstantBufferView(
                0, frameConstantBufferAddress +
                    static_cast<UINT64>(objectIndex) * ConstantBufferStride);
            commandList_->SetGraphicsRootShaderResourceView(
                7, frameInstanceBufferAddress +
                    static_cast<UINT64>(objectIndex) * InstanceDataStride);
            commandList_->SetGraphicsRootDescriptorTable(
                1, materialIterator->second.firstSrv);
            commandList_->DrawIndexedInstanced(
                mesh.indexCount, instanceCount, mesh.startIndex, 0, 0);
            objectIndex += instanceCount;
        }
        std::array<D3D12_RESOURCE_BARRIER, GBufferCount> toShaderResources{};
        for (std::size_t index = 0; index < GBufferCount; ++index)
        {
            toShaderResources[index] = TransitionBarrier(
                gBufferTargets_[index].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        commandList_->ResourceBarrier(GBufferCount, toShaderResources.data());
        gBufferReadyForSampling_ = true;

        // SSAO 读取 GBuffer 的法线和世界坐标，生成一张单通道环境遮蔽纹理。
        const D3D12_RESOURCE_BARRIER ssaoToRenderTarget = TransitionBarrier(
            ssaoTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList_->ResourceBarrier(1, &ssaoToRenderTarget);
        commandList_->SetPipelineState(ssaoPipelineState_.Get());
        commandList_->SetGraphicsRootSignature(ssaoRootSignature_.Get());
        commandList_->SetGraphicsRootConstantBufferView(
            0, ssaoConstantsBuffer_->GetGPUVirtualAddress() + ssaoConstantsOffset);
        commandList_->SetGraphicsRootDescriptorTable(1, gBufferSrvs_[1]);
        commandList_->OMSetRenderTargets(1, &ssaoRtv_, FALSE, nullptr);
        const float ssaoClear[] = {1.0F, 1.0F, 1.0F, 1.0F};
        commandList_->ClearRenderTargetView(ssaoRtv_, ssaoClear, 0, nullptr);
        commandList_->DrawInstanced(3, 1, 0, 0);
        const D3D12_RESOURCE_BARRIER ssaoToShader = TransitionBarrier(
            ssaoTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &ssaoToShader);

        commandList_->SetGraphicsRootSignature(rootSignature_.Get());
        commandList_->SetGraphicsRootDescriptorTable(4, ssaoSrv_);
    }

    // 第一盏 Point Light 生成六面线性深度 Cubemap。每个面复用同一张临时 DSV，
    // 但写入 Cubemap Array 的不同 RTV Slice。
    const auto pointShadowLight = std::find_if(
        scene.Lights().begin(), scene.Lights().end(), [](const Scene::SceneLight& light) {
            return light.type == Scene::LightType::Point;
        });
    if (pointShadowLight != scene.Lights().end())
    {
        const D3D12_RESOURCE_BARRIER pointShadowToRenderTarget = TransitionBarrier(
            pointShadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList_->ResourceBarrier(1, &pointShadowToRenderTarget);
        commandList_->SetPipelineState(pointShadowPipelineState_.Get());
        commandList_->SetGraphicsRootSignature(rootSignature_.Get());
        commandList_->RSSetViewports(1, &pointShadowViewport_);
        commandList_->RSSetScissorRects(1, &pointShadowScissorRect_);
        D3D12_CPU_DESCRIPTOR_HANDLE pointShadowDsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        pointShadowDsv.ptr += static_cast<SIZE_T>(2) *
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const float pointShadowClear[] = {1.0F, 1.0F, 1.0F, 1.0F};
        for (std::size_t face = 0; face < PointShadowFaceCount; ++face)
        {
            commandList_->OMSetRenderTargets(
                1, &pointShadowRtvs_[face], FALSE, &pointShadowDsv);
            commandList_->ClearRenderTargetView(
                pointShadowRtvs_[face], pointShadowClear, 0, nullptr);
            commandList_->ClearDepthStencilView(
                pointShadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0, 0, nullptr);
            for (std::uint32_t objectIndex = 0;
                 objectIndex < static_cast<std::uint32_t>(scene.Objects().size());
                 ++objectIndex)
            {
                const MeshRange mesh = MeshFor(scene.Objects()[objectIndex].assetKey);
                if (mesh.indexCount == 0)
                {
                    continue;
                }
                const D3D12_GPU_VIRTUAL_ADDRESS pointConstantsAddress =
                    frameConstantBufferAddress +
                    static_cast<UINT64>((2 + face) * MaxSceneObjects + objectIndex) *
                        ConstantBufferStride;
                commandList_->SetGraphicsRootConstantBufferView(
                    0, pointConstantsAddress);
                commandList_->DrawIndexedInstanced(
                    mesh.indexCount, 1, mesh.startIndex, 0, 0);
            }
        }
        const D3D12_RESOURCE_BARRIER pointShadowToShader = TransitionBarrier(
            pointShadowMap_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &pointShadowToShader);
    }

    // Pass 1：从方向光观察场景，只把最近深度写进 Shadow Map。
    D3D12_RESOURCE_BARRIER shadowToDepth = TransitionBarrier(
        shadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList_->ResourceBarrier(1, &shadowToDepth);
    commandList_->SetPipelineState(shadowPipelineState_.Get());
    commandList_->RSSetViewports(1, &shadowViewport_);
    commandList_->RSSetScissorRects(1, &shadowScissorRect_);
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    shadowDsv.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    commandList_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    commandList_->ClearDepthStencilView(
        shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0, 0, nullptr);
    const D3D12_GPU_VIRTUAL_ADDRESS shadowFrameConstantBufferAddress =
        frameConstantBufferAddress +
        static_cast<UINT64>(MaxSceneObjects) * ConstantBufferStride;
    for (std::uint32_t objectIndex = 0;
         objectIndex < static_cast<std::uint32_t>(scene.Objects().size()); ++objectIndex)
    {
        const MeshRange mesh = MeshFor(scene.Objects()[objectIndex].assetKey);
        if (mesh.indexCount == 0)
        {
            continue;
        }
        commandList_->SetGraphicsRootConstantBufferView(
            0, shadowFrameConstantBufferAddress +
                static_cast<UINT64>(objectIndex) * ConstantBufferStride);
        commandList_->DrawIndexedInstanced(mesh.indexCount, 1, mesh.startIndex, 0, 0);
    }
    D3D12_RESOURCE_BARRIER shadowToShader = TransitionBarrier(
        shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1, &shadowToShader);

    // Pass 2：恢复相机 Viewport 和主 PSO，Shadow Map 现在可以作为 t3 被 Pixel Shader 采样。
    commandList_->SetPipelineState(pipelineState_.Get());
    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRect_);
    commandList_->SetGraphicsRootDescriptorTable(2, shadowMapSrv_);
    commandList_->SetGraphicsRootDescriptorTable(3, gBufferSrvs_[0]);
    commandList_->SetGraphicsRootDescriptorTable(4, ssaoSrv_);
    commandList_->SetGraphicsRootDescriptorTable(5, environmentCubemapSrv_);
    commandList_->SetGraphicsRootDescriptorTable(6, pointShadowMapSrv_);

    // 处于 PRESENT 状态的缓冲不能直接作为渲染目标写入。必须先把它显式切换为
    // RENDER_TARGET 状态，这正是 D3D12 显式资源管理的一部分。
    const bool useMsaa = msaaSupported_ && scene.Environment().msaaEnabled &&
        scene.Environment().renderPath == Scene::RenderPath::Forward;
    if (!useMsaa)
    {
        const D3D12_RESOURCE_BARRIER toRenderTarget = TransitionBarrier(
            hdrSceneTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList_->ResourceBarrier(1, &toRenderTarget);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        useMsaa ? msaaHdrSceneRtv_ : hdrSceneRtv_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = useMsaa
        ? msaaDepthDsv_
        : dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // 让清屏颜色随时间变化，可以直接从画面判断呈现循环是否仍在持续运行。
    const float clearColor[] = {0.018F, 0.024F, 0.040F, 1.0F};
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    // Deferred 必须保留 GBuffer Pass 写下的深度；Forward 则在自己的目标上重新开始。
    if (scene.Environment().renderPath == Scene::RenderPath::Forward)
    {
        commandList_->ClearDepthStencilView(
            dsvHandle,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0F, 0, 0, nullptr);
    }

    // 天空先写入线性 HDR 目标，之后场景几何自然覆盖它；背景与 IBL 使用同一张 Cubemap。
    SkyConstants skyConstants{};
    const DirectX::XMMATRIX inverseViewProjection = DirectX::XMMatrixInverse(
        nullptr, view * projection);
    DirectX::XMStoreFloat4x4(
        &skyConstants.inverseViewProjection,
        DirectX::XMMatrixTranspose(inverseViewProjection));
    const DirectX::XMMATRIX skyInverseView =
        DirectX::XMMatrixInverse(nullptr, view);
    DirectX::XMStoreFloat3(&skyConstants.cameraPosition, skyInverseView.r[3]);
    skyConstants.intensity = scene.Environment().intensity;
    skyConstants.rotationRadians =
        DirectX::XMConvertToRadians(scene.Environment().rotationDegrees);
    const UINT64 skyConstantsOffset = static_cast<UINT64>(frameIndex_) * 256ULL;
    std::memcpy(
        mappedSkyConstants_ + skyConstantsOffset,
        &skyConstants, sizeof(skyConstants));
    commandList_->SetPipelineState(
        useMsaa ? msaaSkyPipelineState_.Get() : skyPipelineState_.Get());
    commandList_->SetGraphicsRootSignature(skyRootSignature_.Get());
    commandList_->SetGraphicsRootConstantBufferView(
        0, skyConstantsBuffer_->GetGPUVirtualAddress() + skyConstantsOffset);
    commandList_->SetGraphicsRootDescriptorTable(1, environmentCubemapSrv_);
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    commandList_->DrawInstanced(3, 1, 0, 0);

    if (scene.Environment().renderPath == Scene::RenderPath::Deferred)
    {
        // Deferred：几何已经写完 GBuffer，此处只需一次全屏 Draw 完成全部像素光照。
        commandList_->SetPipelineState(deferredPipelineState_.Get());
        commandList_->SetGraphicsRootSignature(deferredRootSignature_.Get());
        commandList_->SetGraphicsRootConstantBufferView(0, frameConstantBufferAddress);
        commandList_->SetGraphicsRootDescriptorTable(1, gBufferSrvs_[0]);
        commandList_->SetGraphicsRootDescriptorTable(2, shadowMapSrv_);
        commandList_->SetGraphicsRootDescriptorTable(3, ssaoSrv_);
        commandList_->SetGraphicsRootDescriptorTable(4, environmentCubemapSrv_);
        commandList_->SetGraphicsRootDescriptorTable(5, pointShadowMapSrv_);
        commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        commandList_->DrawInstanced(3, 1, 0, 0);
    }
    else
    {
        // Forward：每个 SceneObject 的 Pixel Shader 在几何 Draw 中直接完成光照。
        commandList_->SetPipelineState(
            useMsaa ? msaaPipelineState_.Get() : pipelineState_.Get());
        commandList_->SetGraphicsRootSignature(rootSignature_.Get());
        commandList_->SetGraphicsRootDescriptorTable(2, shadowMapSrv_);
        commandList_->SetGraphicsRootDescriptorTable(3, gBufferSrvs_[0]);
        commandList_->SetGraphicsRootDescriptorTable(4, ssaoSrv_);
        commandList_->SetGraphicsRootDescriptorTable(5, environmentCubemapSrv_);
        commandList_->SetGraphicsRootDescriptorTable(6, pointShadowMapSrv_);
        commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        for (std::uint32_t objectIndex = 0;
             objectIndex < static_cast<std::uint32_t>(scene.Objects().size());)
        {
            const Scene::SceneObject& object = scene.Objects()[objectIndex];
            if (object.material.baseColor.w < 0.999F)
            {
                ++objectIndex;
                continue;
            }
            const MeshRange mesh = MeshFor(object.assetKey);
            if (mesh.indexCount == 0)
            {
                ++objectIndex;
                continue;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS objectConstantBufferAddress =
                frameConstantBufferAddress +
                static_cast<UINT64>(objectIndex) * ConstantBufferStride;
            commandList_->SetGraphicsRootConstantBufferView(
                0, objectConstantBufferAddress);
            const auto materialIterator = gpuMaterialTextures_.find(object.assetKey);
            if (materialIterator == gpuMaterialTextures_.end())
            {
                ++objectIndex;
                continue;
            }
            std::uint32_t instanceCount = 1;
            while (objectIndex + instanceCount < scene.Objects().size() &&
                   scene.Objects()[objectIndex + instanceCount].material.baseColor.w >= 0.999F &&
                   sameInstanceBatch(objectIndex, objectIndex + instanceCount))
            {
                ++instanceCount;
            }
            commandList_->SetGraphicsRootDescriptorTable(
                1, materialIterator->second.firstSrv);
            commandList_->SetGraphicsRootShaderResourceView(
                7, frameInstanceBufferAddress +
                    static_cast<UINT64>(objectIndex) * InstanceDataStride);
            commandList_->DrawIndexedInstanced(
                mesh.indexCount, instanceCount, mesh.startIndex, 0, 0);
            objectIndex += instanceCount;
        }
    }

    // 选中物体描边使用两遍模板：先写模板，再绘制稍微外扩且模板不等于 1 的外壳。
    if (editor.IsObjectSelected() &&
        editor.SelectedObjectIndex() < scene.Objects().size())
    {
        const std::uint32_t selectedIndex =
            static_cast<std::uint32_t>(editor.SelectedObjectIndex());
        const Scene::SceneObject& selectedObject = scene.Objects()[selectedIndex];
        const MeshRange selectedMesh = MeshFor(selectedObject.assetKey);
        if (selectedMesh.indexCount != 0)
        {
            commandList_->SetGraphicsRootSignature(rootSignature_.Get());
            commandList_->SetGraphicsRootConstantBufferView(
                0, frameConstantBufferAddress +
                    static_cast<UINT64>(selectedIndex) * ConstantBufferStride);
            commandList_->SetGraphicsRootShaderResourceView(
                7, frameInstanceBufferAddress +
                    static_cast<UINT64>(selectedIndex) * InstanceDataStride);
            commandList_->OMSetStencilRef(1);

            commandList_->SetPipelineState(
                useMsaa ? msaaStencilMaskPipelineState_.Get()
                        : stencilMaskPipelineState_.Get());
            commandList_->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
            commandList_->DrawIndexedInstanced(
                selectedMesh.indexCount, 1, selectedMesh.startIndex, 0, 0);

            commandList_->SetPipelineState(
                useMsaa ? msaaOutlinePipelineState_.Get()
                        : outlinePipelineState_.Get());
            commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
            commandList_->DrawIndexedInstanced(
                selectedMesh.indexCount, 1, selectedMesh.startIndex, 0, 0);
        }
    }

    if (scene.Environment().showGeometryNormals && editor.IsObjectSelected() &&
        editor.SelectedObjectIndex() < scene.Objects().size())
    {
        const std::uint32_t selectedIndex =
            static_cast<std::uint32_t>(editor.SelectedObjectIndex());
        const MeshRange mesh = MeshFor(scene.Objects()[selectedIndex].assetKey);
        if (mesh.indexCount != 0)
        {
            commandList_->SetPipelineState(
                useMsaa ? msaaGeometryNormalsPipelineState_.Get()
                        : geometryNormalsPipelineState_.Get());
            commandList_->SetGraphicsRootSignature(rootSignature_.Get());
            commandList_->SetGraphicsRootConstantBufferView(
                0, frameConstantBufferAddress +
                    static_cast<UINT64>(selectedIndex) * ConstantBufferStride);
            commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
            commandList_->DrawIndexedInstanced(
                mesh.indexCount, 1, mesh.startIndex, 0, 0);
        }
    }

    // 透明物体不能进入 GBuffer。无论当前选择 Forward 还是 Deferred，都在最后用
    // Forward PBR 从远到近绘制，从而得到稳定的 Alpha 混合结果。
    std::vector<std::uint32_t> transparentObjects;
    transparentObjects.reserve(scene.Objects().size());
    const DirectX::XMMATRIX inverseView = DirectX::XMMatrixInverse(nullptr, view);
    DirectX::XMFLOAT3 cameraPosition{};
    DirectX::XMStoreFloat3(&cameraPosition, inverseView.r[3]);
    for (std::uint32_t objectIndex = 0;
         objectIndex < static_cast<std::uint32_t>(scene.Objects().size());
         ++objectIndex)
    {
        if (scene.Objects()[objectIndex].material.baseColor.w < 0.999F)
        {
            transparentObjects.push_back(objectIndex);
        }
    }
    std::sort(
        transparentObjects.begin(), transparentObjects.end(),
        [&](const std::uint32_t left, const std::uint32_t right)
        {
            const auto distanceSquared = [&](const std::uint32_t index)
            {
                const auto& position = scene.Objects()[index].transform.position;
                const float x = position.x - cameraPosition.x;
                const float y = position.y - cameraPosition.y;
                const float z = position.z - cameraPosition.z;
                return x * x + y * y + z * z;
            };
            return distanceSquared(left) > distanceSquared(right);
        });
    if (!transparentObjects.empty())
    {
        commandList_->SetPipelineState(
            useMsaa ? msaaTransparentPipelineState_.Get()
                    : transparentPipelineState_.Get());
        commandList_->SetGraphicsRootSignature(rootSignature_.Get());
        commandList_->SetGraphicsRootDescriptorTable(2, shadowMapSrv_);
        commandList_->SetGraphicsRootDescriptorTable(3, gBufferSrvs_[0]);
        commandList_->SetGraphicsRootDescriptorTable(4, ssaoSrv_);
        commandList_->SetGraphicsRootDescriptorTable(5, environmentCubemapSrv_);
        commandList_->SetGraphicsRootDescriptorTable(6, pointShadowMapSrv_);
        commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        for (const std::uint32_t objectIndex : transparentObjects)
        {
            const Scene::SceneObject& object = scene.Objects()[objectIndex];
            const MeshRange mesh = MeshFor(object.assetKey);
            const auto materialIterator = gpuMaterialTextures_.find(object.assetKey);
            if (mesh.indexCount == 0 || materialIterator == gpuMaterialTextures_.end())
            {
                continue;
            }
            commandList_->SetGraphicsRootConstantBufferView(
                0, frameConstantBufferAddress +
                    static_cast<UINT64>(objectIndex) * ConstantBufferStride);
            commandList_->SetGraphicsRootShaderResourceView(
                7, frameInstanceBufferAddress +
                    static_cast<UINT64>(objectIndex) * InstanceDataStride);
            commandList_->SetGraphicsRootDescriptorTable(
                1, materialIterator->second.firstSrv);
            commandList_->DrawIndexedInstanced(
                mesh.indexCount, 1, mesh.startIndex, 0, 0);
        }
    }

    // ImGui 使用自己的 Shader 和 SRV Descriptor Heap，放在场景绘制之后作为界面叠加层。
    if (useMsaa)
    {
        // ResolveSubresource 把每像素四个样本合成为单采样 HDR，后处理无需知道 MSAA 的存在。
        const std::array<D3D12_RESOURCE_BARRIER, 2> toResolve = {
            TransitionBarrier(
                msaaHdrSceneTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
            TransitionBarrier(
                hdrSceneTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RESOLVE_DEST),
        };
        commandList_->ResourceBarrier(
            static_cast<UINT>(toResolve.size()), toResolve.data());
        commandList_->ResolveSubresource(
            hdrSceneTarget_.Get(), 0, msaaHdrSceneTarget_.Get(), 0,
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        const std::array<D3D12_RESOURCE_BARRIER, 2> afterResolve = {
            TransitionBarrier(
                msaaHdrSceneTarget_.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET),
            TransitionBarrier(
                hdrSceneTarget_.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        commandList_->ResourceBarrier(
            static_cast<UINT>(afterResolve.size()), afterResolve.data());
    }
    else
    {
        const D3D12_RESOURCE_BARRIER hdrToShader = TransitionBarrier(
            hdrSceneTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &hdrToShader);
    }

    PostProcessConstants postProcessConstants{};
    postProcessConstants.exposure = scene.Environment().exposure;
    postProcessConstants.bloomThreshold = scene.Environment().bloomThreshold;
    postProcessConstants.bloomStrength = scene.Environment().bloomStrength;
    postProcessConstants.bloomEnabled = scene.Environment().bloomEnabled ? 1.0F : 0.0F;
    postProcessConstants.invResolution = {
        1.0F / static_cast<float>(width_), 1.0F / static_cast<float>(height_)};
    const UINT64 postProcessFrameOffset =
        static_cast<UINT64>(frameIndex_) * 4ULL * 256ULL;
    const auto writePostProcessConstants = [&](const UINT64 passIndex)
    {
        const UINT64 offset = postProcessFrameOffset + passIndex * 256ULL;
        std::memcpy(
            mappedPostProcessConstants_ + offset,
            &postProcessConstants, sizeof(postProcessConstants));
        return postProcessConstantsBuffer_->GetGPUVirtualAddress() + offset;
    };

    // 亮部提取：HDR -> Bloom Extract。
    postProcessConstants.direction = {0.0F, 0.0F};
    const D3D12_GPU_VIRTUAL_ADDRESS bloomExtractConstantsAddress =
        writePostProcessConstants(0);
    const D3D12_RESOURCE_BARRIER bloomExtractToRenderTarget = TransitionBarrier(
        bloomExtractTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &bloomExtractToRenderTarget);
    commandList_->SetPipelineState(bloomPipelineState_.Get());
    commandList_->SetGraphicsRootSignature(bloomRootSignature_.Get());
    commandList_->SetGraphicsRootConstantBufferView(
        0, bloomExtractConstantsAddress);
    commandList_->SetGraphicsRootDescriptorTable(1, hdrSceneSrv_);
    commandList_->OMSetRenderTargets(1, &bloomExtractRtv_, FALSE, nullptr);
    commandList_->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER bloomExtractToShader = TransitionBarrier(
        bloomExtractTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1, &bloomExtractToShader);

    // 两次一维模糊复用两个目标，避免读写同一张纹理。
    postProcessConstants.direction = {1.0F, 0.0F};
    const D3D12_GPU_VIRTUAL_ADDRESS bloomHorizontalConstantsAddress =
        writePostProcessConstants(1);
    const D3D12_RESOURCE_BARRIER bloomBlurToRenderTarget = TransitionBarrier(
        bloomBlurTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &bloomBlurToRenderTarget);
    commandList_->SetGraphicsRootConstantBufferView(
        0, bloomHorizontalConstantsAddress);
    commandList_->SetGraphicsRootDescriptorTable(1, bloomExtractSrv_);
    commandList_->OMSetRenderTargets(1, &bloomBlurRtv_, FALSE, nullptr);
    commandList_->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER bloomBlurToShader = TransitionBarrier(
        bloomBlurTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1, &bloomBlurToShader);

    postProcessConstants.direction = {0.0F, 1.0F};
    const D3D12_GPU_VIRTUAL_ADDRESS bloomVerticalConstantsAddress =
        writePostProcessConstants(2);
    const D3D12_RESOURCE_BARRIER bloomExtractToBlurTarget = TransitionBarrier(
        bloomExtractTarget_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &bloomExtractToBlurTarget);
    commandList_->SetGraphicsRootConstantBufferView(
        0, bloomVerticalConstantsAddress);
    commandList_->SetGraphicsRootDescriptorTable(1, bloomBlurSrv_);
    commandList_->OMSetRenderTargets(1, &bloomExtractRtv_, FALSE, nullptr);
    commandList_->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER bloomExtractToFinalShader = TransitionBarrier(
        bloomExtractTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1, &bloomExtractToFinalShader);

    // 最终合成到交换链，并把 ImGui 叠加在已经完成 Tone Mapping 的画面上。
    D3D12_RESOURCE_BARRIER toFinalRenderTarget = TransitionBarrier(
        renderTargets_[frameIndex_].Get(), D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &toFinalRenderTarget);
    D3D12_CPU_DESCRIPTOR_HANDLE finalRtvHandle =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    finalRtvHandle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;
    commandList_->SetPipelineState(postProcessPipelineState_.Get());
    commandList_->SetGraphicsRootSignature(postProcessRootSignature_.Get());
    const D3D12_GPU_VIRTUAL_ADDRESS finalPostProcessConstantsAddress =
        writePostProcessConstants(3);
    commandList_->SetGraphicsRootConstantBufferView(
        0, finalPostProcessConstantsAddress);
    commandList_->SetGraphicsRootDescriptorTable(1, hdrSceneSrv_);
    commandList_->OMSetRenderTargets(1, &finalRtvHandle, FALSE, nullptr);
    commandList_->DrawInstanced(3, 1, 0, 0);

    // ImGui 使用自己的 Shader 和 SRV Descriptor Heap，叠加在最终画面上。
    ID3D12DescriptorHeap* imguiHeaps[] = {imguiSrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, imguiHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList_.Get());

    // Present 要求后备缓冲处于 PRESENT 状态，因此本帧的渲染命令记录完毕后，
    // 需要把它从 RENDER_TARGET 切换回来。
    D3D12_RESOURCE_BARRIER toPresent = TransitionBarrier(
        renderTargets_[frameIndex_].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    commandList_->ResourceBarrier(1, &toPresent);
    ThrowIfFailed(commandList_->Close(), "ID3D12GraphicsCommandList::Close");
    CheckDebugLayerMessages("frame command recording");

    ID3D12CommandList* commandLists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);
    ThrowIfFailed(swapChain_->Present(1, 0), "IDXGISwapChain::Present");
    CheckDebugLayerMessages("frame execution/present");

    // 在提交和呈现之后插入 Fence 信号。该数值代表此前所有使用当前缓冲和命令
    // 分配器的 GPU 工作，稍后可用它判断这些资源能否复用。
    const std::uint64_t fenceValue = nextFenceValue_++;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), fenceValue),
                  "ID3D12CommandQueue::Signal");
    frameFenceValues_[frameIndex_] = fenceValue;

    // 切换到 DXGI 指定的下一张缓冲；只有 GPU 仍占用该帧资源时才等待。
    // 正因为不是每帧都无条件 Flush，CPU 和 GPU 才能同时处理不同帧。
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    WaitForFrame(frameIndex_);
}

void D3D12Renderer::Resize(const std::uint32_t width, const std::uint32_t height)
{
    if (!initialized_ || width == 0 || height == 0 || (width == width_ && height == height_))
    {
        return;
    }

    // GPU 或 ComPtr 仍引用旧后备缓冲时，ResizeBuffers 会失败。必须先等待 GPU，
    // 再释放所有旧渲染目标引用，最后才能重建交换链缓冲。
    WaitForGpu();
    for (auto& renderTarget : renderTargets_)
    {
        renderTarget.Reset();
    }
    depthBuffer_.Reset();
    for (auto& target : gBufferTargets_)
    {
        target.Reset();
    }
    ssaoTarget_.Reset();
    hdrSceneTarget_.Reset();
    msaaHdrSceneTarget_.Reset();
    msaaDepthBuffer_.Reset();
    bloomExtractTarget_.Reset();
    bloomBlurTarget_.Reset();
    gBufferReadyForSampling_ = false;

    DXGI_SWAP_CHAIN_DESC description{};
    ThrowIfFailed(swapChain_->GetDesc(&description), "IDXGISwapChain::GetDesc");
    ThrowIfFailed(
        swapChain_->ResizeBuffers(FrameCount, width, height, BackBufferFormat, description.Flags),
        "IDXGISwapChain::ResizeBuffers");

    width_ = width;
    height_ = height;
    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    CreateRenderTargets();
    CreateDepthBuffer();
    CreateGBufferResources();
    CreateGBufferViews();
    UpdateViewportAndScissor();
}

void D3D12Renderer::CheckDebugLayerMessages(const char* context)
{
#if defined(_DEBUG)
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (!device_ || FAILED(device_.As(&infoQueue)))
    {
        return;
    }

    std::string errors;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < messageCount; ++index)
    {
        SIZE_T messageSize = 0;
        if (FAILED(infoQueue->GetMessage(index, nullptr, &messageSize)) || messageSize == 0)
        {
            continue;
        }
        std::vector<std::byte> storage(messageSize);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(infoQueue->GetMessage(index, message, &messageSize)))
        {
            continue;
        }
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        {
            errors += "D3D12 message ";
            errors += std::to_string(static_cast<unsigned int>(message->ID));
            errors += ": ";
            errors += message->pDescription != nullptr
                ? message->pDescription : "(no description)";
            errors += '\n';
        }
    }
    infoQueue->ClearStoredMessages();
    if (!errors.empty())
    {
        throw std::runtime_error(
            std::string("D3D12 Debug Layer error during ") + context + ":\n" + errors);
    }
#else
    (void)context;
#endif
}

void D3D12Renderer::WaitForFrame(const std::uint32_t frameIndex)
{
    const std::uint64_t fenceValue = frameFenceValues_[frameIndex];
    if (fenceValue == 0 || fence_->GetCompletedValue() >= fenceValue)
    {
        return;
    }

    // 使用事件等待可以让线程休眠，避免 GPU 尚未完成时让 CPU 一直空转轮询。
    ThrowIfFailed(fence_->SetEventOnCompletion(fenceValue, fenceEvent_),
                  "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObject(fenceEvent_, INFINITE);
}

void D3D12Renderer::WaitForGpu()
{
    // 在队列末尾插入一个新的 Fence 信号可形成完整的 Flush 点：GPU 到达该值时，
    // 此前提交到同一队列的所有命令都已经执行完毕。
    const std::uint64_t fenceValue = nextFenceValue_++;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), fenceValue),
                  "ID3D12CommandQueue::Signal (flush)");
    ThrowIfFailed(fence_->SetEventOnCompletion(fenceValue, fenceEvent_),
                  "ID3D12Fence::SetEventOnCompletion (flush)");
    WaitForSingleObject(fenceEvent_, INFINITE);
    frameFenceValues_.fill(0);
}
} // namespace Shadow::Renderer
