#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
bool NearlyEqual(const float left, const float right)
{
    return std::abs(left - right) < 0.0001F;
}
}

int main()
{
    Shadow::Scene::Scene source;
    auto& sourceObject = source.Objects()[0];
    sourceObject.transform.position = {2.5F, 1.25F, -0.75F};
    sourceObject.transform.rotationDegrees = {15.0F, 40.0F, -5.0F};
    sourceObject.transform.scale = {1.5F, 0.8F, 2.0F};
    sourceObject.material.roughness = 0.37F;
    sourceObject.material.metallic = 0.65F;
    sourceObject.material.emissiveStrength = 4.25F;
    sourceObject.assetKey = "gltf://assets/models/test.glb#mesh=0/primitive=0";
    source.Environment().intensity = 2.25F;
    source.Environment().rotationDegrees = -35.0F;
    source.Environment().iblSpecularStrength = 0.42F;
    source.Environment().renderPath = Shadow::Scene::RenderPath::Deferred;
    source.Environment().msaaEnabled = true;
    source.Environment().showGeometryNormals = true;
    source.Lights().push_back({
        "Fill Point",
        Shadow::Scene::LightType::Point,
        {0.0F, -1.0F, 0.0F},
        {1.0F, 2.0F, 3.0F},
        {0.25F, 0.50F, 1.0F},
        6.0F,
        12.0F,
        1.0F,
        1.0F,
    });
    Shadow::Scene::SceneLight spot;
    spot.name = "Test Spot";
    spot.type = Shadow::Scene::LightType::Spot;
    spot.position = {-2.0F, 4.0F, 1.0F};
    spot.innerConeDegrees = 18.0F;
    spot.outerConeDegrees = 33.0F;
    source.Lights().push_back(spot);

    const std::filesystem::path testDirectory =
        std::filesystem::temp_directory_path() / "ShadowEngineSceneSerializerTest";
    const std::filesystem::path testPath = testDirectory / "round-trip.scene.json";
    std::string errorMessage;
    if (!Shadow::Scene::SceneSerializer::Save(testPath, source, errorMessage))
    {
        std::cerr << errorMessage << '\n';
        return 1;
    }

    Shadow::Scene::Scene loaded;
    if (!Shadow::Scene::SceneSerializer::Load(testPath, loaded, errorMessage))
    {
        std::cerr << errorMessage << '\n';
        return 1;
    }

    const auto& loadedObject = loaded.Objects()[0];
    const bool lightsMatch = loaded.Lights().size() == 3 &&
        loaded.Lights()[1].name == "Fill Point" &&
        loaded.Lights()[1].type == Shadow::Scene::LightType::Point &&
        NearlyEqual(loaded.Lights()[1].position.y, 2.0F) &&
        NearlyEqual(loaded.Lights()[1].color.z, 1.0F) &&
        NearlyEqual(loaded.Lights()[1].intensity, 6.0F) &&
        NearlyEqual(loaded.Lights()[1].range, 12.0F) &&
        loaded.Lights()[2].type == Shadow::Scene::LightType::Spot &&
        NearlyEqual(loaded.Lights()[2].innerConeDegrees, 18.0F) &&
        NearlyEqual(loaded.Lights()[2].outerConeDegrees, 33.0F);
    const bool valuesMatch =
        NearlyEqual(loadedObject.transform.position.x, 2.5F) &&
        NearlyEqual(loadedObject.transform.position.y, 1.25F) &&
        NearlyEqual(loadedObject.transform.position.z, -0.75F) &&
        NearlyEqual(loadedObject.transform.rotationDegrees.y, 40.0F) &&
        NearlyEqual(loadedObject.transform.scale.z, 2.0F) &&
        NearlyEqual(loadedObject.material.roughness, 0.37F) &&
        NearlyEqual(loadedObject.material.metallic, 0.65F) &&
        NearlyEqual(loadedObject.material.emissiveStrength, 4.25F) &&
        loadedObject.assetKey == sourceObject.assetKey &&
        NearlyEqual(loaded.Environment().intensity, 2.25F) &&
        NearlyEqual(loaded.Environment().rotationDegrees, -35.0F) &&
        NearlyEqual(loaded.Environment().iblSpecularStrength, 0.42F) &&
        loaded.Environment().renderPath == Shadow::Scene::RenderPath::Deferred &&
        loaded.Environment().msaaEnabled &&
        loaded.Environment().showGeometryNormals &&
        lightsMatch;
    if (!valuesMatch)
    {
        std::cerr << "Round-trip scene values do not match.\n";
        return 1;
    }

    // 即使旧场景把 lights 写成空数组，加载器也要恢复默认直射光。
    const std::filesystem::path emptyLightsPath = testDirectory / "empty-lights.scene.json";
    Shadow::Scene::Scene sceneWithoutLights;
    sceneWithoutLights.Lights().clear();
    if (!Shadow::Scene::SceneSerializer::Save(
            emptyLightsPath, sceneWithoutLights, errorMessage))
    {
        std::cerr << errorMessage << '\n';
        return 1;
    }
    Shadow::Scene::Scene restoredDefaultLightScene;
    if (!Shadow::Scene::SceneSerializer::Load(
            emptyLightsPath, restoredDefaultLightScene, errorMessage))
    {
        std::cerr << errorMessage << '\n';
        return 1;
    }
    if (restoredDefaultLightScene.Lights().size() != 1 ||
        restoredDefaultLightScene.Lights()[0].type != Shadow::Scene::LightType::Directional)
    {
        std::cerr << "The default Directional light was not restored.\n";
        return 1;
    }

    std::filesystem::remove(testPath);
    std::filesystem::remove(emptyLightsPath);
    std::filesystem::remove(testDirectory);
    return 0;
}
