cmake_minimum_required(VERSION 3.24)

project(ShadowEngine VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
include(CTest)

# 固定 Dear ImGui 版本，避免上游更新在没有审查的情况下改变工具面板行为。
# 只编译核心文件以及当前项目需要的 Win32、DirectX 12 官方 Backend。
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(imgui)

# ImGuizmo 提供场景视图中的移动、旋转、缩放手柄。仓库没有稳定 Release，
# 因此固定到经过项目验证的提交，避免 master 后续变化破坏构建。
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 18cef5e031d8c6973d80284c67f60549fafd78c1
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR shadow-engine-no-subdirectory
)
FetchContent_MakeAvailable(imguizmo)

# 场景文件使用可读的 JSON，固定版本保证序列化行为可复现。
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# TinyGLTF v2 保持稳定且直接支持 glTF/GLB、外部/内嵌 Buffer 与常见图片格式。
# v3 当前仍标记为实验性，因此课程使用固定的 v2.9.7。
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v2.9.7
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR shadow-engine-no-subdirectory
)
FetchContent_MakeAvailable(tinygltf)

add_executable(ShadowEngine WIN32
    src/main.cpp
    src/Core/Application.cpp
    src/Core/Application.h
    src/Core/DxException.h
    src/Assets/AssetManager.cpp
    src/Assets/AssetManager.h
    src/Assets/GltfLoader.cpp
    src/Assets/GltfLoader.h
    src/Assets/HdrEnvironment.cpp
    src/Assets/HdrEnvironment.h
    src/Assets/MeshAsset.cpp
    src/Assets/MeshAsset.h
    src/Editor/EditorCamera.cpp
    src/Editor/EditorCamera.h
    src/Editor/EditorLayer.cpp
    src/Editor/EditorLayer.h
    src/Editor/ScenePicking.cpp
    src/Editor/ScenePicking.h
    src/Renderer/D3D12Renderer.cpp
    src/Renderer/D3D12Renderer.h
    src/Scene/Scene.cpp
    src/Scene/Scene.h
    src/Scene/SceneSerializer.cpp
    src/Scene/SceneSerializer.h
    shaders/MaterialPreview.hlsl
    shaders/GBuffer.hlsl
    shaders/DeferredLighting.hlsl
    shaders/ShadowMap.hlsl
    shaders/PointShadow.hlsl
    shaders/SSAO.hlsl
    shaders/Bloom.hlsl
    shaders/PostProcess.hlsl
    shaders/Skybox.hlsl
    shaders/Outline.hlsl
    shaders/GeometryNormals.hlsl
)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_sources(ShadowEngine PRIVATE ${IMGUI_SOURCES})
set(IMGUIZMO_SOURCES ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_sources(ShadowEngine PRIVATE ${IMGUIZMO_SOURCES})

# HLSL 由 DXC 显式编译，不交给 Visual Studio 的旧版 HLSL 构建规则处理。
set_source_files_properties(
    shaders/MaterialPreview.hlsl
    shaders/GBuffer.hlsl
    shaders/DeferredLighting.hlsl
    shaders/ShadowMap.hlsl
    shaders/PointShadow.hlsl
    shaders/SSAO.hlsl
    shaders/Bloom.hlsl
    shaders/PostProcess.hlsl
    shaders/Skybox.hlsl
    shaders/Outline.hlsl
    shaders/GeometryNormals.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE)

target_compile_features(ShadowEngine PRIVATE cxx_std_20)

# 让所有编译单元使用一致的 Windows 宏。NOMINMAX 可以避免 Windows 的 min/max 宏
# 与 C++ 标准库中的同名函数发生冲突。
target_compile_definitions(ShadowEngine PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_include_directories(ShadowEngine PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}/src
    ${tinygltf_SOURCE_DIR}
)

# 这些都是 Windows SDK 自带的导入库，不是项目内置的第三方依赖。
target_link_libraries(ShadowEngine PRIVATE d3d12 dxgi dxguid user32)
target_link_libraries(ShadowEngine PRIVATE nlohmann_json::nlohmann_json)

# 在构建可执行文件前，把同一份 HLSL 分别编译成 Vertex Shader 和 Pixel Shader。
# 运行时只读取编译后的字节码，不需要动态加载 dxcompiler.dll。
#
# 从 VS Code 直接启动时，终端不一定带有 Developer PowerShell 设置的 SDK 环境变量。
# 因此这里同时查询 Windows SDK 的注册表安装位置，并自动选择最新版本的 x64 DXC。
set(DXC_SEARCH_HINTS)

if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    list(APPEND DXC_SEARCH_HINTS
        "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
    )
endif()

if(WIN32)
    cmake_host_system_information(
        RESULT WINDOWS_KITS_ROOT
        QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10"
    )

    if(WINDOWS_KITS_ROOT)
        file(GLOB DXC_CANDIDATES
            LIST_DIRECTORIES FALSE
            "${WINDOWS_KITS_ROOT}/bin/*/x64/dxc.exe"
        )

        if(DXC_CANDIDATES)
            list(SORT DXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
            list(GET DXC_CANDIDATES 0 LATEST_DXC_EXECUTABLE)
            get_filename_component(LATEST_DXC_DIRECTORY "${LATEST_DXC_EXECUTABLE}" DIRECTORY)
            list(APPEND DXC_SEARCH_HINTS "${LATEST_DXC_DIRECTORY}")
        endif()
    endif()
endif()

find_program(DXC_EXECUTABLE NAMES dxc HINTS ${DXC_SEARCH_HINTS})

if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC was not found. Install a recent Windows SDK or add dxc to PATH.")
endif()

add_custom_command(TARGET ShadowEngine PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ShadowEngine>/shaders"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/MaterialPreviewVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/MaterialPreview.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/MaterialPreviewPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/MaterialPreview.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/GBufferVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/GBuffer.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/GBufferPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/GBuffer.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/DeferredLightingVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/DeferredLighting.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/DeferredLightingPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/DeferredLighting.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/ShadowMapVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ShadowMap.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/PointShadowVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/PointShadow.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/PointShadowPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/PointShadow.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/SSAOVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/SSAO.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/SSAOPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/SSAO.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/BloomVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Bloom.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/BloomPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Bloom.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/PostProcessVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/PostProcess.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/PostProcessPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/PostProcess.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/SkyboxVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Skybox.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/SkyboxPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Skybox.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/OutlineVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Outline.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/OutlinePS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Outline.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T vs_6_0 -E VSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/GeometryNormalsVS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/GeometryNormals.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T gs_6_0 -E GSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/GeometryNormalsGS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/GeometryNormals.hlsl"
    COMMAND ${DXC_EXECUTABLE}
        -T ps_6_0 -E PSMain -HV 2021 -Ges -WX -Zi -Qembed_debug -Od
        -Fo "$<TARGET_FILE_DIR:ShadowEngine>/shaders/GeometryNormalsPS.cso"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/GeometryNormals.hlsl"
    VERBATIM
)

if(MSVC)
    # 项目代码保持 /W4 /WX；第三方源码使用自身的警告策略，避免把上游实现细节
    # 误当成 Shadow Engine 的项目警告。
    set_source_files_properties(${IMGUI_SOURCES} ${IMGUIZMO_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")
    target_compile_options(ShadowEngine PRIVATE
        /W4
        /WX
        /permissive-
        /utf-8
        /Zc:preprocessor
    )
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ShadowEngine)

if(BUILD_TESTING)
    add_executable(SceneSerializerTests
        tests/SceneSerializerTests.cpp
        src/Scene/Scene.cpp
        src/Scene/Scene.h
        src/Scene/SceneSerializer.cpp
        src/Scene/SceneSerializer.h
    )
    target_compile_features(SceneSerializerTests PRIVATE cxx_std_20)
    target_include_directories(SceneSerializerTests PRIVATE src)
    target_link_libraries(SceneSerializerTests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(SceneSerializerTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    if(MSVC)
        target_compile_options(SceneSerializerTests PRIVATE /W4 /WX /permissive- /utf-8)
    endif()
    add_test(NAME SceneSerializerRoundTrip COMMAND SceneSerializerTests)

    add_executable(ScenePickingTests
        tests/ScenePickingTests.cpp
        src/Assets/AssetManager.cpp
        src/Assets/AssetManager.h
        src/Assets/GltfLoader.cpp
        src/Assets/GltfLoader.h
        src/Assets/MeshAsset.cpp
        src/Assets/MeshAsset.h
        src/Editor/ScenePicking.cpp
        src/Editor/ScenePicking.h
        src/Scene/Scene.cpp
        src/Scene/Scene.h
    )
    target_compile_features(ScenePickingTests PRIVATE cxx_std_20)
    target_include_directories(ScenePickingTests PRIVATE src ${tinygltf_SOURCE_DIR})
    target_compile_definitions(ScenePickingTests PRIVATE NOMINMAX)
    if(MSVC)
        target_compile_options(ScenePickingTests PRIVATE /W4 /WX /permissive- /utf-8)
    endif()
    add_test(NAME ScenePickingProjectedSphere COMMAND ScenePickingTests)

    add_executable(SceneTransformTests
        tests/SceneTransformTests.cpp
        src/Scene/Scene.cpp
        src/Scene/Scene.h
    )
    target_compile_features(SceneTransformTests PRIVATE cxx_std_20)
    target_include_directories(SceneTransformTests PRIVATE src)
    target_compile_definitions(SceneTransformTests PRIVATE NOMINMAX)
    if(MSVC)
        target_compile_options(SceneTransformTests PRIVATE /W4 /WX /permissive- /utf-8)
    endif()
    add_test(NAME SceneNonUniformScaleNormalMatrix COMMAND SceneTransformTests)
endif()
