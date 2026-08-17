#include "Scene/SceneSerializer.h"

#include "Scene/Scene.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace
{
using json = nlohmann::json;

double ReadableFloat(const float value)
{
    return static_cast<double>(std::lround(value * 10000.0F)) / 10000.0;
}

json Float3ToJson(const DirectX::XMFLOAT3& value)
{
    return json::array({ReadableFloat(value.x), ReadableFloat(value.y), ReadableFloat(value.z)});
}

json Float4ToJson(const DirectX::XMFLOAT4& value)
{
    return json::array({
        ReadableFloat(value.x), ReadableFloat(value.y),
        ReadableFloat(value.z), ReadableFloat(value.w)});
}

DirectX::XMFLOAT3 JsonToFloat3(const json& value)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw std::runtime_error("Expected a three-component array.");
    }
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

DirectX::XMFLOAT4 JsonToFloat4(const json& value)
{
    if (!value.is_array() || value.size() != 4)
    {
        throw std::runtime_error("Expected a four-component array.");
    }
    return {
        value.at(0).get<float>(), value.at(1).get<float>(),
        value.at(2).get<float>(), value.at(3).get<float>()};
}

std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, 32768> pathBuffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (length == 0 || length == static_cast<DWORD>(pathBuffer.size()))
    {
        throw std::runtime_error("Unable to locate the executable directory.");
    }
    return std::filesystem::path(std::wstring(pathBuffer.data(), length)).parent_path();
}
} // namespace

namespace Shadow::Scene
{
std::filesystem::path SceneSerializer::DefaultScenePath()
{
    // 开发构建位于 build/... 下。向上寻找 CMakeLists.txt 可以稳定定位仓库，
    // 而不需要把当前电脑的绝对路径写进项目配置。
    std::filesystem::path directory = ExecutableDirectory();
    for (int depth = 0; depth < 6; ++depth)
    {
        if (std::filesystem::exists(directory / "CMakeLists.txt"))
        {
            return directory / "assets" / "scenes" / "current.scene.json";
        }
        if (!directory.has_parent_path())
        {
            break;
        }
        directory = directory.parent_path();
    }
    return ExecutableDirectory() / "scenes" / "current.scene.json";
}

bool SceneSerializer::Load(
    const std::filesystem::path& path, Scene& scene, std::string& errorMessage) noexcept
{
    try
    {
        std::ifstream input(path);
        if (!input)
        {
            errorMessage = "Scene file does not exist yet; using defaults.";
            return false;
        }

        json root;
        input >> root;
        if (root.at("version").get<int>() != 1)
        {
            throw std::runtime_error("Unsupported scene version.");
        }

        const json& savedObjects = root.at("objects");
        if (!savedObjects.is_array() || savedObjects.empty() || savedObjects.size() > 128)
        {
            throw std::runtime_error("Scene must contain between 1 and 128 objects.");
        }

        Scene loadedScene;
        if (root.contains("environment"))
        {
            loadedScene.Environment().intensity = std::clamp(
                root.at("environment").value("intensity", 1.0F), 0.0F, 8.0F);
            loadedScene.Environment().rotationDegrees =
                root.at("environment").value("rotationDegrees", 0.0F);
            loadedScene.Environment().exposure = std::clamp(
                root.at("environment").value("exposure", 1.0F), 0.1F, 8.0F);
            loadedScene.Environment().iblSpecularStrength = std::clamp(
                root.at("environment").value("iblSpecularStrength", 0.25F),
                0.0F, 1.0F);
            const std::uint32_t renderPath =
                root.at("environment").value("renderPath", 0U);
            loadedScene.Environment().renderPath = renderPath == 1U
                ? RenderPath::Deferred
                : RenderPath::Forward;
            loadedScene.Environment().ssaoStrength = std::clamp(
                root.at("environment").value("ssaoStrength", 0.0F), 0.0F, 1.0F);
            loadedScene.Environment().bloomEnabled = root.at("environment").value(
                "bloomEnabled", true);
            loadedScene.Environment().msaaEnabled = root.at("environment").value(
                "msaaEnabled", false);
            loadedScene.Environment().showGeometryNormals = root.at("environment").value(
                "showGeometryNormals", false);
            loadedScene.Environment().bloomThreshold = std::clamp(
                root.at("environment").value("bloomThreshold", 1.0F), 0.0F, 8.0F);
            loadedScene.Environment().bloomStrength = std::clamp(
                root.at("environment").value("bloomStrength", 0.08F), 0.0F, 2.0F);
        }
        if (root.contains("lights"))
        {
            const json& savedLights = root.at("lights");
            if (!savedLights.is_array() || savedLights.size() > 8)
            {
                throw std::runtime_error("Scene must contain at most 8 lights.");
            }
            auto& lights = loadedScene.Lights();
            lights.clear();
            for (const json& saved : savedLights)
            {
                SceneLight light;
                light.name = saved.value("name", "Light");
                const std::uint32_t type = saved.value("type", 0U);
                if (type > static_cast<std::uint32_t>(LightType::Spot))
                {
                    throw std::runtime_error("Unsupported scene light type.");
                }
                light.type = static_cast<LightType>(type);
                light.direction = JsonToFloat3(saved.value(
                    "direction", Float3ToJson(light.direction)));
                light.position = JsonToFloat3(saved.value(
                    "position", Float3ToJson(light.position)));
                light.color = JsonToFloat3(saved.value(
                    "color", Float3ToJson(light.color)));
                light.intensity = std::clamp(saved.value("intensity", light.intensity), 0.0F, 100.0F);
                light.range = std::max(saved.value("range", light.range), 0.1F);
                light.width = std::max(saved.value("width", light.width), 0.01F);
                light.height = std::max(saved.value("height", light.height), 0.01F);
                light.innerConeDegrees = std::clamp(
                    saved.value("innerConeDegrees", light.innerConeDegrees), 1.0F, 89.0F);
                light.outerConeDegrees = std::clamp(
                    saved.value("outerConeDegrees", light.outerConeDegrees),
                    light.innerConeDegrees, 89.0F);
                lights.push_back(std::move(light));
            }

            // 直射光是这个编辑器的默认主光，即使旧场景手动删掉了它，加载后也要补回来。
            // 这样场景不会因为 JSON 中只有点光/面光而失去基础照明和阴影来源。
            const bool hasDirectional = std::any_of(
                lights.begin(), lights.end(), [](const SceneLight& light) {
                    return light.type == LightType::Directional;
                });
            if (!hasDirectional)
            {
                const SceneLight defaultDirectional{
                    "Key Directional",
                    LightType::Directional,
                    {0.35F, 0.80F, 0.45F},
                    {0.0F, 5.0F, 4.0F},
                    {1.0F, 0.92F, 0.80F},
                    4.0F,
                    8.0F,
                    2.0F,
                    2.0F,
                };
                if (lights.size() < 8)
                {
                    lights.insert(lights.begin(), defaultDirectional);
                }
                else
                {
                    // 保持最多 8 盏灯：当旧文件已占满容量时，用第一项让出默认主光位置。
                    lights.front() = defaultDirectional;
                }
            }
        }
        auto& objects = loadedScene.Objects();
        objects.resize(savedObjects.size());
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            const json& saved = savedObjects.at(index);
            SceneObject& object = objects[index];
            object.name = saved.at("name").get<std::string>();
            object.assetKey = saved.contains("assetKey")
                ? saved.at("assetKey").get<std::string>()
                : (saved.value("mesh", "Sphere") == "Ground"
                    ? "builtin://ground" : "builtin://sphere");
            object.transform.position = JsonToFloat3(saved.at("transform").at("position"));
            object.transform.rotationDegrees =
                JsonToFloat3(saved.at("transform").at("rotationDegrees"));
            object.transform.scale = JsonToFloat3(saved.at("transform").at("scale"));
            object.transform.scale.x = std::max(object.transform.scale.x, 0.01F);
            object.transform.scale.y = std::max(object.transform.scale.y, 0.01F);
            object.transform.scale.z = std::max(object.transform.scale.z, 0.01F);
            object.material.baseColor = JsonToFloat4(saved.at("material").at("baseColor"));
            object.material.roughness =
                std::clamp(saved.at("material").at("roughness").get<float>(), 0.0F, 1.0F);
            object.material.metallic =
                std::clamp(saved.at("material").at("metallic").get<float>(), 0.0F, 1.0F);
            object.material.normalStrength = std::clamp(
                saved.at("material").value("normalStrength", 1.0F), 0.0F, 2.0F);
            object.material.emissiveStrength = std::clamp(
                saved.at("material").value("emissiveStrength", 1.0F), 0.0F, 20.0F);
            object.material.parallaxHeightScale = std::clamp(
                saved.at("material").value("parallaxHeightScale", 0.0F), 0.0F, 0.1F);
        }

        scene = std::move(loadedScene);
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = std::string("Scene load failed: ") + exception.what();
        return false;
    }
}

bool SceneSerializer::Save(
    const std::filesystem::path& path, const Scene& scene, std::string& errorMessage) noexcept
{
    try
    {
        json root;
        root["version"] = 1;
        root["environment"] = {
            {"intensity", ReadableFloat(scene.Environment().intensity)},
            {"rotationDegrees", ReadableFloat(scene.Environment().rotationDegrees)},
            {"exposure", ReadableFloat(scene.Environment().exposure)},
            {"iblSpecularStrength", ReadableFloat(
                scene.Environment().iblSpecularStrength)},
            {"renderPath", static_cast<std::uint32_t>(scene.Environment().renderPath)},
            {"ssaoStrength", ReadableFloat(scene.Environment().ssaoStrength)},
            {"bloomEnabled", scene.Environment().bloomEnabled},
            {"msaaEnabled", scene.Environment().msaaEnabled},
            {"showGeometryNormals", scene.Environment().showGeometryNormals},
            {"bloomThreshold", ReadableFloat(scene.Environment().bloomThreshold)},
            {"bloomStrength", ReadableFloat(scene.Environment().bloomStrength)},
        };
        root["lights"] = json::array();
        for (const SceneLight& light : scene.Lights())
        {
            root["lights"].push_back({
                {"name", light.name},
                {"type", static_cast<std::uint32_t>(light.type)},
                {"direction", Float3ToJson(light.direction)},
                {"position", Float3ToJson(light.position)},
                {"color", Float3ToJson(light.color)},
                {"intensity", ReadableFloat(light.intensity)},
                {"range", ReadableFloat(light.range)},
                {"width", ReadableFloat(light.width)},
                {"height", ReadableFloat(light.height)},
                {"innerConeDegrees", ReadableFloat(light.innerConeDegrees)},
                {"outerConeDegrees", ReadableFloat(light.outerConeDegrees)},
            });
        }
        root["objects"] = json::array();
        for (const SceneObject& object : scene.Objects())
        {
            root["objects"].push_back({
                {"name", object.name},
                {"assetKey", object.assetKey},
                {"transform", {
                    {"position", Float3ToJson(object.transform.position)},
                    {"rotationDegrees", Float3ToJson(object.transform.rotationDegrees)},
                    {"scale", Float3ToJson(object.transform.scale)},
                }},
                {"material", {
                    {"baseColor", Float4ToJson(object.material.baseColor)},
                    {"roughness", ReadableFloat(object.material.roughness)},
                    {"metallic", ReadableFloat(object.material.metallic)},
                    {"normalStrength", ReadableFloat(object.material.normalStrength)},
                    {"emissiveStrength", ReadableFloat(object.material.emissiveStrength)},
                    {"parallaxHeightScale", ReadableFloat(object.material.parallaxHeightScale)},
                }},
            });
        }

        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        if (!output)
        {
            throw std::runtime_error("Unable to open the scene file for writing.");
        }
        output << root.dump(2) << '\n';
        errorMessage.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = std::string("Scene save failed: ") + exception.what();
        return false;
    }
}
} // namespace Shadow::Scene
