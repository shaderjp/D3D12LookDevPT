#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rb
{
enum class TextureSlot : std::uint32_t
{
    BaseColor,
    Normal,
    Roughness,
    Metallic,
    Occlusion,
    Emissive,
    Count
};

enum class AlphaMode : std::uint32_t
{
    Opaque,
    Mask,
    Blend
};

enum class LookDevBackgroundMode : std::uint32_t
{
    SkyColor,
    Hdri,
    TransparentChecker
};

enum class ToneMapper : std::uint32_t
{
    None,
    Reinhard,
    Aces
};

enum class LookDevDisplayMode : std::uint32_t
{
    Beauty,
    BaseColor,
    Normal,
    Roughness,
    Metallic,
    AmbientOcclusion,
    Emissive,
    LightingOnly,
    ShadowMask
};

struct MaterialAssignment
{
    std::string materialName = "Default Material";
    std::string shaderSetName = "LookDev PBR";
    std::array<std::wstring, static_cast<std::size_t>(TextureSlot::Count)> textureOverrides;
    std::array<bool, static_cast<std::size_t>(TextureSlot::Count)> textureOverrideEnabled = {};
    std::array<float, 4> baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, 4> emissiveFactor = { 0.0f, 0.0f, 0.0f, 1.0f };
    float roughnessFactor = 0.48f;
    float metallicFactor = 0.0f;
    float normalStrength = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool packedOcclusionRoughnessMetallic = false;
    bool flipNormalGreen = false;
};

struct ViewportCamera
{
    std::array<float, 3> target = { 0.0f, 0.0f, 0.0f };
    float yaw = 0.0f;
    float pitch = 0.12f;
    float distance = 4.0f;
    float fovDegrees = 45.0f;
};

struct CameraBookmark
{
    bool valid = false;
    ViewportCamera camera;
};

constexpr std::size_t CameraBookmarkCount = 3;

struct ModelTransform
{
    std::array<float, 3> translation = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> rotationDegrees = { 0.0f, 0.0f, 0.0f };
};

struct LookDevEnvironment
{
    std::wstring environmentPath;
    float rotationYaw = 0.0f;
    float intensity = 1.0f;
    LookDevBackgroundMode backgroundMode = LookDevBackgroundMode::SkyColor;
    std::array<float, 3> sunDirection = { -0.35f, -0.75f, 0.55f };
    std::array<float, 3> sunColor = { 1.0f, 0.96f, 0.88f };
    float sunIntensity = 10000.0f;
};

struct LookDevViewSettings
{
    float exposure = 0.0f;
    ToneMapper toneMapper = ToneMapper::Aces;
    float gamma = 2.2f;
    LookDevDisplayMode displayMode = LookDevDisplayMode::Beauty;
    bool turntableEnabled = false;
    float turntableSpeed = 0.35f;
};

// v1.2 uses one orthographic Sun shadow map. Softness is a PCF radius in
// shadow texels, while fitScale expands the fitted scene bounds.
struct LookDevShadowSettings
{
    bool enabled = true;
    std::uint32_t resolution = 2048;
    float strength = 0.85f;
    float bias = 0.0015f;
    float softness = 1.5f;
    float fitScale = 1.25f;
};

struct UiSettings
{
    float uiFontSize = 18.0f;
    float chatFontSize = 22.0f;
    float uiScale = 1.0f;
    float chatTranscriptHeightRatio = 0.50f;
    bool largeFramePadding = true;
};

struct ProjectFile
{
    std::wstring path;
    std::wstring scenePath;
    std::array<float, 4> skyTopColor = { 0.12f, 0.22f, 0.36f, 1.0f };
    std::array<float, 4> skyHorizonColor = { 0.035f, 0.045f, 0.055f, 1.0f };
    ModelTransform modelTransform;
    ViewportCamera viewportCamera;
    std::array<CameraBookmark, CameraBookmarkCount> cameraBookmarks;
    bool hasViewportCamera = false;
    LookDevEnvironment lookDevEnvironment;
    LookDevViewSettings lookDevViewSettings;
    LookDevShadowSettings lookDevShadowSettings;
    std::vector<MaterialAssignment> materialAssignments;
};
}
