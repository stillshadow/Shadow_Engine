#include "Assets/GltfLoader.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace
{
using Shadow::Assets::CpuTexture;
using Shadow::Assets::MeshAsset;
using Shadow::Assets::MeshVertex;

const unsigned char* AccessorData(
    const tinygltf::Model& model,
    const tinygltf::Accessor& accessor,
    std::size_t& stride)
{
    const tinygltf::BufferView& view = model.bufferViews.at(
        static_cast<std::size_t>(accessor.bufferView));
    const tinygltf::Buffer& buffer = model.buffers.at(static_cast<std::size_t>(view.buffer));
    stride = accessor.ByteStride(view);
    return buffer.data.data() + view.byteOffset + accessor.byteOffset;
}

float ReadComponentAsFloat(const unsigned char* data, const int componentType)
{
    if (componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
    {
        float value{};
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    throw std::runtime_error("This learning loader currently requires floating-point attributes.");
}

std::uint32_t ReadIndex(const unsigned char* data, const int componentType)
{
    switch (componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return *data;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
    {
        std::uint16_t value{};
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    {
        std::uint32_t value{};
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    default:
        throw std::runtime_error("Unsupported glTF index component type.");
    }
}

std::size_t ComponentSize(const int componentType)
{
    switch (componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return 1;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return 2;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return 4;
    default:
        throw std::runtime_error("Unsupported glTF component size.");
    }
}

void ReadFloatAttribute(
    const tinygltf::Model& model,
    const tinygltf::Primitive& primitive,
    const char* semantic,
    const int expectedComponents,
    std::vector<float>& values)
{
    const auto iterator = primitive.attributes.find(semantic);
    if (iterator == primitive.attributes.end())
    {
        values.clear();
        return;
    }

    const tinygltf::Accessor& accessor = model.accessors.at(
        static_cast<std::size_t>(iterator->second));
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
    {
        throw std::runtime_error(std::string(semantic) + " must use FLOAT components.");
    }
    const int actualComponents = tinygltf::GetNumComponentsInType(accessor.type);
    if (actualComponents != expectedComponents)
    {
        throw std::runtime_error(std::string(semantic) + " has an unexpected component count.");
    }

    std::size_t stride = 0;
    const unsigned char* first = AccessorData(model, accessor, stride);
    values.resize(accessor.count * static_cast<std::size_t>(expectedComponents));
    for (std::size_t vertex = 0; vertex < accessor.count; ++vertex)
    {
        const unsigned char* source = first + vertex * stride;
        for (int component = 0; component < expectedComponents; ++component)
        {
            values[vertex * static_cast<std::size_t>(expectedComponents) + component] =
                ReadComponentAsFloat(source + component * sizeof(float), accessor.componentType);
        }
    }
}

CpuTexture ReadTexture(
    const tinygltf::Model& model, const int textureIndex, const std::string& fallbackName)
{
    if (textureIndex < 0)
    {
        return {};
    }
    const tinygltf::Texture& texture = model.textures.at(static_cast<std::size_t>(textureIndex));
    if (texture.source < 0)
    {
        return {};
    }
    const tinygltf::Image& image = model.images.at(static_cast<std::size_t>(texture.source));
    if (image.width <= 0 || image.height <= 0 || image.image.empty())
    {
        return {};
    }

    CpuTexture result;
    result.name = image.name.empty() ? fallbackName : image.name;
    result.width = static_cast<std::uint32_t>(image.width);
    result.height = static_cast<std::uint32_t>(image.height);
    result.rgba8.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    for (int pixel = 0; pixel < image.width * image.height; ++pixel)
    {
        for (int channel = 0; channel < 4; ++channel)
        {
            const int sourceChannel = std::min(channel, image.component - 1);
            result.rgba8[static_cast<std::size_t>(pixel) * 4 + channel] =
                channel == 3 && image.component < 4
                ? 255
                : image.image[static_cast<std::size_t>(pixel) * image.component + sourceChannel];
        }
    }
    return result;
}

void ComputeTangents(MeshAsset& mesh)
{
    std::vector<DirectX::XMFLOAT3> tangentSums(mesh.vertices.size());
    std::vector<DirectX::XMFLOAT3> bitangentSums(mesh.vertices.size());
    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3)
    {
        const std::uint32_t i0 = mesh.indices[triangle];
        const std::uint32_t i1 = mesh.indices[triangle + 1];
        const std::uint32_t i2 = mesh.indices[triangle + 2];
        const MeshVertex& v0 = mesh.vertices[i0];
        const MeshVertex& v1 = mesh.vertices[i1];
        const MeshVertex& v2 = mesh.vertices[i2];
        const float edge1X = v1.position.x - v0.position.x;
        const float edge1Y = v1.position.y - v0.position.y;
        const float edge1Z = v1.position.z - v0.position.z;
        const float edge2X = v2.position.x - v0.position.x;
        const float edge2Y = v2.position.y - v0.position.y;
        const float edge2Z = v2.position.z - v0.position.z;
        const float deltaU1 = v1.uv.x - v0.uv.x;
        const float deltaV1 = v1.uv.y - v0.uv.y;
        const float deltaU2 = v2.uv.x - v0.uv.x;
        const float deltaV2 = v2.uv.y - v0.uv.y;
        const float determinant = deltaU1 * deltaV2 - deltaV1 * deltaU2;
        if (std::abs(determinant) < 0.000001F)
        {
            continue;
        }
        const float reciprocal = 1.0F / determinant;
        const DirectX::XMFLOAT3 tangent{
            reciprocal * (deltaV2 * edge1X - deltaV1 * edge2X),
            reciprocal * (deltaV2 * edge1Y - deltaV1 * edge2Y),
            reciprocal * (deltaV2 * edge1Z - deltaV1 * edge2Z)};
        const DirectX::XMFLOAT3 bitangent{
            reciprocal * (-deltaU2 * edge1X + deltaU1 * edge2X),
            reciprocal * (-deltaU2 * edge1Y + deltaU1 * edge2Y),
            reciprocal * (-deltaU2 * edge1Z + deltaU1 * edge2Z)};
        for (const std::uint32_t index : {i0, i1, i2})
        {
            tangentSums[index].x += tangent.x;
            tangentSums[index].y += tangent.y;
            tangentSums[index].z += tangent.z;
            bitangentSums[index].x += bitangent.x;
            bitangentSums[index].y += bitangent.y;
            bitangentSums[index].z += bitangent.z;
        }
    }

    using namespace DirectX;
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
    {
        const XMVECTOR normal = XMLoadFloat3(&mesh.vertices[index].normal);
        XMVECTOR tangent = XMLoadFloat3(&tangentSums[index]);
        if (XMVectorGetX(XMVector3LengthSq(tangent)) < 0.000001F)
        {
            tangent = XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F);
        }
        tangent = XMVector3Normalize(XMVectorSubtract(
            tangent, XMVectorScale(normal, XMVectorGetX(XMVector3Dot(normal, tangent)))));
        const XMVECTOR bitangent = XMLoadFloat3(&bitangentSums[index]);
        const float handedness = XMVectorGetX(
            XMVector3Dot(XMVector3Cross(normal, tangent), bitangent)) < 0.0F ? -1.0F : 1.0F;
        XMFLOAT3 tangentValue{};
        XMStoreFloat3(&tangentValue, tangent);
        mesh.vertices[index].tangent = {
            tangentValue.x, tangentValue.y, tangentValue.z, handedness};
    }
}

} // namespace

namespace Shadow::Assets
{
bool GltfLoader::Load(
    const std::filesystem::path& path,
    std::vector<MeshAsset>& meshes,
    std::string& errorMessage) noexcept
{
    try
    {
        tinygltf::TinyGLTF loader;
        tinygltf::Model model;
        std::string warning;
        std::string error;
        const std::string pathString = path.string();
        const bool loaded = path.extension() == ".glb"
            ? loader.LoadBinaryFromFile(&model, &error, &warning, pathString)
            : loader.LoadASCIIFromFile(&model, &error, &warning, pathString);
        if (!loaded)
        {
            errorMessage = error.empty() ? "TinyGLTF could not load the model." : error;
            return false;
        }

        meshes.clear();
        for (std::size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
        {
            const tinygltf::Mesh& sourceMesh = model.meshes[meshIndex];
            for (std::size_t primitiveIndex = 0;
                 primitiveIndex < sourceMesh.primitives.size(); ++primitiveIndex)
            {
                const tinygltf::Primitive& primitive = sourceMesh.primitives[primitiveIndex];
                if (primitive.mode != TINYGLTF_MODE_TRIANGLES || primitive.indices < 0)
                {
                    continue;
                }

                std::vector<float> positions;
                std::vector<float> normals;
                std::vector<float> uvs;
                std::vector<float> tangents;
                ReadFloatAttribute(model, primitive, "POSITION", 3, positions);
                ReadFloatAttribute(model, primitive, "NORMAL", 3, normals);
                ReadFloatAttribute(model, primitive, "TEXCOORD_0", 2, uvs);
                ReadFloatAttribute(model, primitive, "TANGENT", 4, tangents);
                if (positions.empty())
                {
                    continue;
                }

                MeshAsset mesh;
                std::ostringstream key;
                key << "gltf://" << path.generic_string()
                    << "#mesh=" << meshIndex << "/primitive=" << primitiveIndex;
                mesh.key = key.str();
                const std::size_t vertexCount = positions.size() / 3;
                mesh.vertices.resize(vertexCount);
                DirectX::XMFLOAT3 minimum{
                    std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()};
                DirectX::XMFLOAT3 maximum{
                    std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest()};
                for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
                {
                    MeshVertex& destination = mesh.vertices[vertex];
                    destination.position = {
                        positions[vertex * 3], positions[vertex * 3 + 1], positions[vertex * 3 + 2]};
                    if (normals.size() == vertexCount * 3)
                    {
                        destination.normal = {
                            normals[vertex * 3], normals[vertex * 3 + 1], normals[vertex * 3 + 2]};
                    }
                    if (uvs.size() == vertexCount * 2)
                    {
                        destination.uv = {uvs[vertex * 2], uvs[vertex * 2 + 1]};
                    }
                    if (tangents.size() == vertexCount * 4)
                    {
                        destination.tangent = {
                            tangents[vertex * 4], tangents[vertex * 4 + 1],
                            tangents[vertex * 4 + 2], tangents[vertex * 4 + 3]};
                    }
                    minimum.x = std::min(minimum.x, destination.position.x);
                    minimum.y = std::min(minimum.y, destination.position.y);
                    minimum.z = std::min(minimum.z, destination.position.z);
                    maximum.x = std::max(maximum.x, destination.position.x);
                    maximum.y = std::max(maximum.y, destination.position.y);
                    maximum.z = std::max(maximum.z, destination.position.z);
                }

                const tinygltf::Accessor& indexAccessor = model.accessors.at(
                    static_cast<std::size_t>(primitive.indices));
                std::size_t indexStride = 0;
                const unsigned char* indexData = AccessorData(model, indexAccessor, indexStride);
                if (indexStride == 0)
                {
                    indexStride = ComponentSize(indexAccessor.componentType);
                }
                mesh.indices.resize(indexAccessor.count);
                for (std::size_t index = 0; index < indexAccessor.count; ++index)
                {
                    mesh.indices[index] = ReadIndex(
                        indexData + index * indexStride, indexAccessor.componentType);
                }
                mesh.boundsCenter = {
                    (minimum.x + maximum.x) * 0.5F,
                    (minimum.y + maximum.y) * 0.5F,
                    (minimum.z + maximum.z) * 0.5F};
                mesh.boundsExtents = {
                    (maximum.x - minimum.x) * 0.5F,
                    (maximum.y - minimum.y) * 0.5F,
                    (maximum.z - minimum.z) * 0.5F};

                if (primitive.material >= 0)
                {
                    const tinygltf::Material& material = model.materials.at(
                        static_cast<std::size_t>(primitive.material));
                    const auto& pbr = material.pbrMetallicRoughness;
                    if (pbr.baseColorFactor.size() == 4)
                    {
                        mesh.importedMaterial.baseColorFactor = {
                            static_cast<float>(pbr.baseColorFactor[0]),
                            static_cast<float>(pbr.baseColorFactor[1]),
                            static_cast<float>(pbr.baseColorFactor[2]),
                            static_cast<float>(pbr.baseColorFactor[3])};
                    }
                    mesh.importedMaterial.roughnessFactor =
                        static_cast<float>(pbr.roughnessFactor);
                    mesh.importedMaterial.metallicFactor =
                        static_cast<float>(pbr.metallicFactor);
                    mesh.importedMaterial.baseColorTexture = ReadTexture(
                        model, pbr.baseColorTexture.index, "BaseColor");
                    mesh.importedMaterial.normalTexture = ReadTexture(
                        model, material.normalTexture.index, "Normal");
                    mesh.importedMaterial.metallicRoughnessTexture = ReadTexture(
                        model, pbr.metallicRoughnessTexture.index, "MetallicRoughness");
                }

                if (tangents.size() != vertexCount * 4)
                {
                    ComputeTangents(mesh);
                }
                meshes.push_back(std::move(mesh));
            }
        }

        if (meshes.empty())
        {
            errorMessage = "No indexed triangle primitives were found in the glTF file.";
            return false;
        }
        errorMessage = warning;
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = std::string("glTF import failed: ") + exception.what();
        return false;
    }
}
} // namespace Shadow::Assets
