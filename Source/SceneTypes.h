#pragma once

#include "EditorTypes.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rb
{
struct SceneVertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texcoord;
    DirectX::XMFLOAT4 tangent = DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);
};

struct SceneDraw
{
    std::uint32_t indexCount = 0;
    std::uint32_t startIndex = 0;
    std::int32_t baseVertex = 0;
    std::uint32_t materialIndex = 0;
};

struct SceneMaterial
{
    MaterialAssignment assignment;
    std::wstring baseColorTexturePath;
    std::wstring normalTexturePath;
    std::wstring roughnessTexturePath;
    std::wstring metallicTexturePath;
    std::wstring occlusionTexturePath;
    std::wstring emissiveTexturePath;
    DirectX::XMFLOAT4 baseColorFactor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    DirectX::XMFLOAT4 emissiveFactor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    bool hasBaseColorTexture = false;
    bool hasNormalTexture = false;
    bool hasRoughnessTexture = false;
    bool hasMetallicTexture = false;
    bool hasOcclusionTexture = false;
    bool hasEmissiveTexture = false;
};

struct ImportedScene
{
    std::wstring path;
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SceneDraw> draws;
    std::vector<SceneMaterial> materials;
    DirectX::XMFLOAT3 boundsMin = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT3 boundsMax = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
};
}
