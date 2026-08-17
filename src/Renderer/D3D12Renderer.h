#pragma once

#include "Scene/Scene.h"

#include <Windows.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

struct ImGui_ImplDX12_InitInfo;

namespace Shadow::Editor
{
class EditorLayer;
enum class DebugViewMode : std::uint32_t;
}

namespace Shadow::Assets
{
class AssetManager;
}

namespace Shadow::Renderer
{
// 管理最小但完整的 D3D12 显式渲染生命周期。当前阶段刻意让帧资源和同步逻辑
// 保持可见，避免过早封装后看不到 D3D12 真正要求应用程序负责的内容。
class D3D12Renderer final
{
public:
    D3D12Renderer() = default;
    ~D3D12Renderer();

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    void Initialize(
        HWND window, std::uint32_t width, std::uint32_t height,
        const Assets::AssetManager& assets);
    void Render(
        Scene::Scene& scene, Editor::EditorLayer& editor,
        Assets::AssetManager& assets);
    void Resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] bool HandleWindowMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }

private:
    struct MeshRange
    {
        std::uint32_t indexCount = 0;
        std::uint32_t vertexCount = 0;
        std::uint32_t startIndex = 0;
        std::uint32_t startVertex = 0;
    };

    static constexpr std::uint32_t FrameCount = 3;
    static constexpr std::uint32_t MaxSceneObjects = 128;
    static constexpr std::uint32_t ImGuiDescriptorCount = 16;
    static constexpr std::uint32_t MaterialDescriptorCount = MaxSceneObjects * 4;
    static constexpr std::uint32_t GBufferCount = 4;
    // SSAO 与 HDR/Bloom 的 SRV 放在材质共用的 shader-visible heap 中。
    static constexpr std::uint32_t PostProcessDescriptorCount = 4;
    // 可见环境、Diffuse Irradiance、Specular Prefilter 和 BRDF LUT。
    static constexpr std::uint32_t EnvironmentDescriptorCount = 4;
    static constexpr std::uint32_t PointShadowDescriptorCount = 1;
    static constexpr std::uint32_t PostProcessTargetCount = 4;
    static constexpr std::uint32_t MsaaTargetCount = 1;
    static constexpr std::uint32_t ShadowMapSize = 2048;
    static constexpr std::uint32_t PointShadowMapSize = 512;
    static constexpr std::uint32_t PointShadowFaceCount = 6;
    static constexpr std::uint32_t SceneConstantPassCount = 8;
    static constexpr std::uint32_t MsaaSampleCount = 4;
    static constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    // 主深度缓冲同时保留 8 位模板值，用来标记选中物体并绘制轮廓。
    static constexpr DXGI_FORMAT DepthBufferFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    // ObjectConstants 现在包含 8 个光源；Stride 仍按 256 字节对齐，便于每帧按物体切片。
    static constexpr std::uint32_t ConstantBufferStride = 1024;
    static constexpr std::uint32_t InstanceDataStride = 192;

    void CreateDevice();
    void CreateCommandQueue();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthBuffer();
    void CreateGBufferResources();
    void CreateGBufferViews();
    void CreateEnvironmentTexture();
    void CreateShadowResources();
    void CreateCommandObjects();
    void CreateGraphicsPipeline();
    void CreateSceneGeometry(const Assets::AssetManager& assets);
    void CreateMaterialTextures(const Assets::AssetManager& assets);
    void SynchronizeAssets(const Assets::AssetManager& assets);
    void CreateConstantBuffer();
    void CreateSynchronizationObjects();
    void CreateImGui();
    void BeginImGuiFrame(
        Scene::Scene& scene,
        Editor::EditorLayer& editor,
        Assets::AssetManager& assets);
    void UpdateViewportAndScissor();
    void UpdateObjectConstants(
        const Scene::Scene& scene,
        Editor::DebugViewMode debugViewMode,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& projection);
    [[nodiscard]] MeshRange MeshFor(const std::string& assetKey) const noexcept;

    static void AllocateImGuiDescriptor(
        ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    static void FreeImGuiDescriptor(
        ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);

    void WaitForFrame(std::uint32_t frameIndex);
    void WaitForGpu();
    void CheckDebugLayerMessages(const char* context);

    HWND window_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t frameIndex_ = 0;
    std::uint32_t rtvDescriptorSize_ = 0;
    bool initialized_ = false;

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> imguiSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> materialSrvHeap_;
    std::uint32_t imguiSrvDescriptorSize_ = 0;
    std::array<bool, ImGuiDescriptorCount> imguiDescriptorUsed_{};
    bool imguiInitialized_ = false;
    struct GpuMaterialTextures
    {
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> resources;
        D3D12_GPU_DESCRIPTOR_HANDLE firstSrv{};
        DirectX::XMFLOAT3 emissiveFactor{};
    };
    std::unordered_map<std::string, GpuMaterialTextures> gpuMaterialTextures_;
    // 每张交换链后备缓冲都对应一个命令分配器。只有 GPU 执行完使用该分配器
    // 记录的命令后，CPU 才能安全地重置并复用它。
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> renderTargets_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, GBufferCount> gBufferTargets_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, GBufferCount> gBufferRtvs_{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, GBufferCount> gBufferSrvs_{};
    bool gBufferReadyForSampling_ = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> ssaoTarget_;
    D3D12_CPU_DESCRIPTOR_HANDLE ssaoRtv_{};
    D3D12_GPU_DESCRIPTOR_HANDLE ssaoSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> hdrSceneTarget_;
    Microsoft::WRL::ComPtr<ID3D12Resource> msaaHdrSceneTarget_;
    Microsoft::WRL::ComPtr<ID3D12Resource> msaaDepthBuffer_;
    D3D12_CPU_DESCRIPTOR_HANDLE msaaHdrSceneRtv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE msaaDepthDsv_{};
    bool msaaSupported_ = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> bloomExtractTarget_;
    Microsoft::WRL::ComPtr<ID3D12Resource> bloomBlurTarget_;
    D3D12_CPU_DESCRIPTOR_HANDLE hdrSceneRtv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE bloomExtractRtv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE bloomBlurRtv_{};
    D3D12_GPU_DESCRIPTOR_HANDLE hdrSceneSrv_{};
    D3D12_GPU_DESCRIPTOR_HANDLE bloomExtractSrv_{};
    D3D12_GPU_DESCRIPTOR_HANDLE bloomBlurSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> environmentCubemap_;
    D3D12_GPU_DESCRIPTOR_HANDLE environmentCubemapSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> irradianceCubemap_;
    D3D12_GPU_DESCRIPTOR_HANDLE irradianceCubemapSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> prefilteredEnvironment_;
    D3D12_GPU_DESCRIPTOR_HANDLE prefilteredEnvironmentSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> brdfLut_;
    D3D12_GPU_DESCRIPTOR_HANDLE brdfLutSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> shadowMap_;
    D3D12_GPU_DESCRIPTOR_HANDLE shadowMapSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> pointShadowMap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, PointShadowFaceCount> pointShadowRtvs_{};
    D3D12_GPU_DESCRIPTOR_HANDLE pointShadowMapSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> pointShadowDepth_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, FrameCount> commandAllocators_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;

    // Root Signature 描述 Shader 能访问哪些外部资源。当前开放的 b0 同时向
    // Vertex Shader 提供矩阵，并向 Pixel Shader 提供材质、灯光和调试参数。
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> transparentPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> stencilMaskPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> outlinePipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> msaaPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> msaaTransparentPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> msaaStencilMaskPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> msaaOutlinePipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> geometryNormalsPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> msaaGeometryNormalsPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gBufferPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadowPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pointShadowPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> ssaoRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ssaoPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> bloomRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bloomPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> postProcessRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> postProcessPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> skyRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> skyPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> msaaSkyPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> deferredRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> deferredPipelineState_;

    // 第一版场景几何直接放在 Upload Heap，便于 CPU 写入和理解数据流。
    // 后续处理大型静态网格时，再学习如何复制到更高效的 Default Heap。
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView_{};
    std::unordered_map<std::string, MeshRange> meshRanges_;
    std::uint64_t uploadedAssetRevision_ = 0;

    // 每张 Back Buffer 为材质球和地面各保留一份 256 字节切片。这样 CPU 更新
    // 当前帧的逐物体矩阵时，不会覆盖 GPU 仍在读取的前一帧数据。
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    std::byte* mappedConstantBufferData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> instanceBuffer_;
    std::byte* mappedInstanceData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> ssaoConstantsBuffer_;
    std::byte* mappedSsaoConstants_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> postProcessConstantsBuffer_;
    std::byte* mappedPostProcessConstants_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> skyConstantsBuffer_;
    std::byte* mappedSkyConstants_ = nullptr;

    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissorRect_{};
    D3D12_VIEWPORT shadowViewport_{};
    D3D12_RECT shadowScissorRect_{};
    D3D12_VIEWPORT pointShadowViewport_{};
    D3D12_RECT pointShadowScissorRect_{};

    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    // 每个值标记最后一次使用对应帧资源的 GPU 提交位置。
    std::array<std::uint64_t, FrameCount> frameFenceValues_{};
    std::uint64_t nextFenceValue_ = 1;
    HANDLE fenceEvent_ = nullptr;

    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();
};
} // namespace Shadow::Renderer
