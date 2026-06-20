#include "SceneImporter.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace DirectX;

namespace
{
std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        std::string fallback;
        fallback.reserve(text.size());
        for (const wchar_t ch : text)
        {
            fallback.push_back(static_cast<char>(ch & 0x7f));
        }
        return fallback;
    }

    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return std::wstring(text.begin(), text.end());
    }

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring CleanTexturePath(const aiString& path)
{
    const std::string raw = path.C_Str();
    if (raw.empty() || raw[0] == '*')
    {
        return {};
    }
    return Utf8ToWide(raw);
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::wstring ResolveEmbeddedTexturePath(const aiScene* sourceScene, const aiString& rawPath)
{
    const std::string raw = rawPath.C_Str();
    if (!sourceScene || raw.empty() || raw[0] != '*')
    {
        return {};
    }

    const int textureIndex = std::atoi(raw.c_str() + 1);
    if (textureIndex < 0 || static_cast<unsigned int>(textureIndex) >= sourceScene->mNumTextures)
    {
        return {};
    }

    const aiTexture* texture = sourceScene->mTextures[textureIndex];
    if (!texture || texture->mHeight != 0 || texture->mWidth == 0 || !texture->pcData)
    {
        return {};
    }

    std::wstring extension = Utf8ToWide(texture->achFormatHint);
    if (extension.empty())
    {
        extension = L"bin";
    }
    if (extension == L"jpeg")
    {
        extension = L"jpg";
    }

    std::filesystem::path directory = std::filesystem::temp_directory_path() / L"D3D12LookDevPTEmbedded";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path path = directory / (L"embedded_" + std::to_wstring(textureIndex) + L"." + extension);
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }
    file.write(reinterpret_cast<const char*>(texture->pcData), static_cast<std::streamsize>(texture->mWidth));
    return path.wstring();
}

std::wstring ResolveTexturePath(const aiScene* sourceScene, const std::filesystem::path& sceneDirectory, const aiMaterial* material, aiTextureType textureType)
{
    if (!material)
    {
        return {};
    }

    aiString rawPath;
    if (material->GetTexture(textureType, 0, &rawPath) != AI_SUCCESS)
    {
        return {};
    }

    if (!rawPath.C_Str()[0])
    {
        return {};
    }
    if (rawPath.C_Str()[0] == '*')
    {
        return ResolveEmbeddedTexturePath(sourceScene, rawPath);
    }

    const std::filesystem::path authoredPath(CleanTexturePath(rawPath));
    if (authoredPath.empty())
    {
        return {};
    }

    const std::filesystem::path directPath = authoredPath.is_absolute() ? authoredPath : sceneDirectory / authoredPath;
    if (Exists(directPath))
    {
        return directPath.wstring();
    }

    const std::filesystem::path fileName = authoredPath.filename();
    for (const std::filesystem::path& candidateRoot : { sceneDirectory, sceneDirectory / L"Textures", sceneDirectory / L"textures" })
    {
        const std::filesystem::path candidate = candidateRoot / fileName;
        if (Exists(candidate))
        {
            return candidate.wstring();
        }
    }

    return {};
}

std::string MaterialName(const aiMaterial* material, std::uint32_t index)
{
    aiString name;
    if (material && material->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0)
    {
        return name.C_Str();
    }
    return "Material " + std::to_string(index);
}

XMFLOAT4 MaterialBaseColor(const aiMaterial* material)
{
    aiColor4D color(1.0f, 1.0f, 1.0f, 1.0f);
    if (!material ||
        (aiGetMaterialColor(material, AI_MATKEY_BASE_COLOR, &color) != AI_SUCCESS &&
         aiGetMaterialColor(material, AI_MATKEY_COLOR_DIFFUSE, &color) != AI_SUCCESS))
    {
        return XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    return XMFLOAT4(color.r, color.g, color.b, color.a);
}

XMFLOAT4 MaterialEmissiveColor(const aiMaterial* material)
{
    aiColor4D color(0.0f, 0.0f, 0.0f, 1.0f);
    if (!material || aiGetMaterialColor(material, AI_MATKEY_COLOR_EMISSIVE, &color) != AI_SUCCESS)
    {
        return XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float intensity = 1.0f;
    material->Get(AI_MATKEY_EMISSIVE_INTENSITY, intensity);
    return XMFLOAT4(color.r * intensity, color.g * intensity, color.b * intensity, color.a);
}

void ExpandBounds(rb::ImportedScene& scene, const XMFLOAT3& position)
{
    scene.boundsMin.x = (std::min)(scene.boundsMin.x, position.x);
    scene.boundsMin.y = (std::min)(scene.boundsMin.y, position.y);
    scene.boundsMin.z = (std::min)(scene.boundsMin.z, position.z);
    scene.boundsMax.x = (std::max)(scene.boundsMax.x, position.x);
    scene.boundsMax.y = (std::max)(scene.boundsMax.y, position.y);
    scene.boundsMax.z = (std::max)(scene.boundsMax.z, position.z);
}

XMMATRIX ToMatrix(const aiMatrix4x4& value)
{
    return XMMatrixSet(
        value.a1, value.b1, value.c1, value.d1,
        value.a2, value.b2, value.c2, value.d2,
        value.a3, value.b3, value.c3, value.d3,
        value.a4, value.b4, value.c4, value.d4);
}

void ProcessNode(const aiScene* sourceScene, const aiNode* node, const XMMATRIX& parentTransform, rb::ImportedScene& scene)
{
    const XMMATRIX world = ToMatrix(node->mTransformation) * parentTransform;
    const XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

    for (std::uint32_t meshSlot = 0; meshSlot < node->mNumMeshes; ++meshSlot)
    {
        const aiMesh* mesh = sourceScene->mMeshes[node->mMeshes[meshSlot]];
        const std::uint32_t baseVertex = static_cast<std::uint32_t>(scene.vertices.size());
        const std::uint32_t startIndex = static_cast<std::uint32_t>(scene.indices.size());

        for (std::uint32_t vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
        {
            const aiVector3D p = mesh->mVertices[vertexIndex];
            const aiVector3D n = mesh->HasNormals() ? mesh->mNormals[vertexIndex] : aiVector3D(0.0f, 1.0f, 0.0f);
            const aiVector3D uv = mesh->HasTextureCoords(0) ? mesh->mTextureCoords[0][vertexIndex] : aiVector3D(0.0f, 0.0f, 0.0f);
            const aiVector3D t = mesh->HasTangentsAndBitangents() ? mesh->mTangents[vertexIndex] : aiVector3D(1.0f, 0.0f, 0.0f);
            const aiVector3D b = mesh->HasTangentsAndBitangents() ? mesh->mBitangents[vertexIndex] : aiVector3D(0.0f, 0.0f, 1.0f);

            XMVECTOR position = XMVector3Transform(XMVectorSet(p.x, p.y, p.z, 1.0f), world);
            XMVECTOR normal = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(n.x, n.y, n.z, 0.0f), normalMatrix));
            XMVECTOR tangent = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(t.x, t.y, t.z, 0.0f), world));
            XMVECTOR bitangent = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(b.x, b.y, b.z, 0.0f), world));
            tangent = XMVector3Normalize(tangent - normal * XMVector3Dot(normal, tangent));
            const float tangentSign = XMVectorGetX(XMVector3Dot(XMVector3Cross(normal, tangent), bitangent)) < 0.0f ? -1.0f : 1.0f;

            rb::SceneVertex vertex = {};
            XMStoreFloat3(&vertex.position, position);
            XMStoreFloat3(&vertex.normal, normal);
            XMStoreFloat4(&vertex.tangent, XMVectorSetW(tangent, tangentSign));
            vertex.texcoord = XMFLOAT2(uv.x, uv.y);
            scene.vertices.push_back(vertex);
            ExpandBounds(scene, vertex.position);
        }

        for (std::uint32_t faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
        {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices == 3)
            {
                scene.indices.push_back(baseVertex + face.mIndices[0]);
                scene.indices.push_back(baseVertex + face.mIndices[1]);
                scene.indices.push_back(baseVertex + face.mIndices[2]);
            }
        }

        rb::SceneDraw draw = {};
        draw.indexCount = static_cast<std::uint32_t>(scene.indices.size()) - startIndex;
        draw.startIndex = startIndex;
        draw.baseVertex = 0;
        draw.materialIndex = mesh->mMaterialIndex;
        if (draw.indexCount > 0)
        {
            scene.draws.push_back(draw);
        }
    }

    for (std::uint32_t childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
    {
        ProcessNode(sourceScene, node->mChildren[childIndex], world, scene);
    }
}
}

namespace rb
{
SceneImportResult SceneImporter::ImportScene(const std::wstring& path)
{
    SceneImportResult result;
    const std::filesystem::path scenePath(path);
    if (!std::filesystem::exists(scenePath))
    {
        result.diagnostics = "Scene file was not found.";
        return result;
    }

    Assimp::Importer importer;
    const unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_ConvertToLeftHanded |
        aiProcess_CalcTangentSpace |
        aiProcess_GenSmoothNormals |
        aiProcess_ImproveCacheLocality |
        aiProcess_OptimizeMeshes |
        aiProcess_SortByPType;

    const aiScene* sourceScene = importer.ReadFile(WideToUtf8(scenePath.wstring()), flags);
    if (!sourceScene || !sourceScene->mRootNode)
    {
        result.diagnostics = importer.GetErrorString();
        if (result.diagnostics.empty())
        {
            result.diagnostics = "Assimp failed to read the scene.";
        }
        return result;
    }

    result.scene.path = path;
    result.scene.boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    result.scene.boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    const std::uint32_t materialCount = (std::max)(1u, sourceScene->mNumMaterials);
    result.scene.materials.reserve(materialCount);
    std::size_t packedOrmMaterialCount = 0;
    for (std::uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        const aiMaterial* material = materialIndex < sourceScene->mNumMaterials ? sourceScene->mMaterials[materialIndex] : nullptr;
        SceneMaterial sceneMaterial = {};
        sceneMaterial.assignment = { MaterialName(material, materialIndex), "LookDev PBR" };
        sceneMaterial.baseColorTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_BASE_COLOR);
        if (sceneMaterial.baseColorTexturePath.empty())
        {
            sceneMaterial.baseColorTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_DIFFUSE);
        }
        sceneMaterial.normalTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_NORMALS);
        if (sceneMaterial.normalTexturePath.empty())
        {
            sceneMaterial.normalTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_HEIGHT);
        }
        sceneMaterial.roughnessTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_DIFFUSE_ROUGHNESS);
        sceneMaterial.metallicTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_METALNESS);
        sceneMaterial.occlusionTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_AMBIENT_OCCLUSION);
        if (sceneMaterial.occlusionTexturePath.empty())
        {
            sceneMaterial.occlusionTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_LIGHTMAP);
        }
        const std::wstring specularTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_SPECULAR);
        if (!specularTexturePath.empty()
            && sceneMaterial.roughnessTexturePath.empty()
            && sceneMaterial.metallicTexturePath.empty()
            && sceneMaterial.occlusionTexturePath.empty())
        {
            sceneMaterial.occlusionTexturePath = specularTexturePath;
            sceneMaterial.roughnessTexturePath = specularTexturePath;
            sceneMaterial.metallicTexturePath = specularTexturePath;
            sceneMaterial.assignment.packedOcclusionRoughnessMetallic = true;
            ++packedOrmMaterialCount;
        }
        sceneMaterial.emissiveTexturePath = ResolveTexturePath(sourceScene, scenePath.parent_path(), material, aiTextureType_EMISSIVE);
        sceneMaterial.hasBaseColorTexture = !sceneMaterial.baseColorTexturePath.empty();
        sceneMaterial.hasNormalTexture = !sceneMaterial.normalTexturePath.empty();
        sceneMaterial.hasRoughnessTexture = !sceneMaterial.roughnessTexturePath.empty();
        sceneMaterial.hasMetallicTexture = !sceneMaterial.metallicTexturePath.empty();
        sceneMaterial.hasOcclusionTexture = !sceneMaterial.occlusionTexturePath.empty();
        sceneMaterial.hasEmissiveTexture = !sceneMaterial.emissiveTexturePath.empty();
        sceneMaterial.baseColorFactor = MaterialBaseColor(material);
        sceneMaterial.emissiveFactor = MaterialEmissiveColor(material);
        sceneMaterial.assignment.baseColorFactor =
        {
            sceneMaterial.baseColorFactor.x,
            sceneMaterial.baseColorFactor.y,
            sceneMaterial.baseColorFactor.z,
            sceneMaterial.baseColorFactor.w,
        };
        sceneMaterial.assignment.emissiveFactor =
        {
            sceneMaterial.emissiveFactor.x,
            sceneMaterial.emissiveFactor.y,
            sceneMaterial.emissiveFactor.z,
            sceneMaterial.emissiveFactor.w,
        };
        float roughnessFactor = sceneMaterial.hasRoughnessTexture ? 1.0f : 0.48f;
        float metallicFactor = sceneMaterial.hasMetallicTexture ? 1.0f : 0.0f;
        if (material)
        {
            material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor);
            material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor);
        }
        sceneMaterial.assignment.roughnessFactor = roughnessFactor;
        sceneMaterial.assignment.metallicFactor = metallicFactor;
        result.scene.materials.push_back(sceneMaterial);
    }

    ProcessNode(sourceScene, sourceScene->mRootNode, XMMatrixIdentity(), result.scene);
    if (result.scene.vertices.empty() || result.scene.indices.empty() || result.scene.draws.empty())
    {
        result.diagnostics = "The scene did not contain renderable triangle meshes.";
        result.scene = {};
        return result;
    }

    std::ostringstream diagnostics;
    diagnostics << "Loaded " << scenePath.filename().string()
                << " with " << result.scene.vertices.size() << " vertices, "
                << result.scene.indices.size() << " indices, "
                << result.scene.draws.size() << " draws, and "
                << result.scene.materials.size() << " materials.";
    if (packedOrmMaterialCount > 0)
    {
        diagnostics << "\nDetected " << packedOrmMaterialCount
                    << " materials using packed Specular/ORM maps (R=occlusion, G=roughness, B=metalness).";
    }
    result.diagnostics = diagnostics.str();
    result.succeeded = true;
    return result;
}
}
