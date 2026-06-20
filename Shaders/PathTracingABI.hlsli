#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
#endif

#ifndef PT_RESTIR
#define PT_RESTIR 0
#endif
#ifndef PT_RESTIR_DI
#define PT_RESTIR_DI 0
#endif
#ifndef PT_RESTIR_GI
#define PT_RESTIR_GI PT_RESTIR
#endif

static const float PI = 3.14159265359f;
static const uint TextureSlotBaseColor = 0;
static const uint TextureSlotNormal = 1;
static const uint TextureSlotRoughness = 2;
static const uint TextureSlotMetallic = 3;
static const uint TextureSlotOcclusion = 4;
static const uint TextureSlotEmissive = 5;

struct SceneConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 lightColor;
    float4 debugOptions;
    float4 skyColor;
    float4 skyHorizonColor;
    float4 skyZenithColor;
    float4 skyGroundColor;
    float4 skyOptions;
    float4 rayOptions;
    float4 frameOptions;
    float4 giOptions;
    float4 pathOptions;
    float4 restirOptions;
    float4 restirDiOptions;
    float4 lightOptions;
    float4 environmentOptions;
    float4 denoiseOptions;
    float4 denoiseOptions2;
    float4 jitterOptions;
    float4 reconstructionOptions;
    float4 validationOptions;
    float4 atrousOptions;
    float4 adaptiveOptions;
    float4 restirStabilityOptions;
    float4 signalDenoiseOptions;
    float4 denoisePassOptions;
};

struct MeshVertex
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 texcoord;
};

struct RtMaterial
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    uint textureBaseIndex;
    uint alphaMasked;
    float alphaCutoff;
    float normalStrength;
    float roughnessFactor;
    float metallicFactor;
    float occlusionStrength;
    uint packedOcclusionRoughnessMetallic;
};

struct RtGeometryRecord
{
    uint indexOffset;
    uint indexCount;
    int baseVertex;
    uint materialIndex;
};

struct RayPayload
{
    float3 position;
    float hitT;
    float3 normal;
    uint hit;
    float3 baseColor;
    float roughness;
    float3 normalTexture;
    float metallic;
    float3 emissive;
    float ao;
};

struct ShadowPayload
{
    uint occluded;
};

struct SurfaceData
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 texcoord;
    float3 baseColor;
    float3 normalTexture;
    float ao;
    float roughness;
    float metallic;
    float3 emissive;
    RtMaterial material;
};

struct RestirReservoir
{
    float4 radianceWeight;
    float4 meta;
};

struct RtLight
{
    float4 positionArea;
    float4 edge0Type;
    float4 edge1;
    float4 radianceCdf;
};

VK_BINDING(0, 0) RaytracingAccelerationStructure g_sceneAs : register(t0, space0);
VK_BINDING(1, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(2, 0) RWTexture2D<float4> g_accumulation : register(u1, space0);
VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(4, 0) StructuredBuffer<MeshVertex> g_vertices : register(t1, space0);
VK_BINDING(5, 0) StructuredBuffer<uint> g_indices : register(t2, space0);
VK_BINDING(6, 0) StructuredBuffer<RtGeometryRecord> g_geometries : register(t3, space0);
VK_BINDING(7, 0) StructuredBuffer<RtMaterial> g_materials : register(t4, space0);
VK_BINDING(8, 0) SamplerState g_linearSampler : register(s0, space0);
VK_BINDING(9, 0) RWStructuredBuffer<RestirReservoir> g_restirCurrent : register(u2, space0);
VK_BINDING(10, 0) RWStructuredBuffer<RestirReservoir> g_restirHistory : register(u3, space0);
VK_BINDING(11, 0) RWStructuredBuffer<RestirReservoir> g_restirSpatial : register(u4, space0);
VK_BINDING(12, 0) StructuredBuffer<RtLight> g_lights : register(t5, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(16, 0) RWTexture2D<float4> g_reconstructionHistoryRadiance : register(u8, space0);
VK_BINDING(17, 0) RWTexture2D<float4> g_reconstructionHistoryMoments : register(u9, space0);
VK_BINDING(18, 0) RWTexture2D<float4> g_reconstructionHistoryLength : register(u10, space0);
VK_BINDING(19, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(20, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(21, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(22, 0) RWStructuredBuffer<RestirReservoir> g_restirDiCurrent : register(u14, space0);
VK_BINDING(23, 0) RWStructuredBuffer<RestirReservoir> g_restirDiHistory : register(u15, space0);
VK_BINDING(24, 0) RWStructuredBuffer<RestirReservoir> g_restirDiSpatial : register(u16, space0);
VK_BINDING(25, 0) RWTexture2D<float4> g_signalCurrentRadiance : register(u17, space0);
VK_BINDING(26, 0) RWTexture2D<float4> g_signalDirect : register(u18, space0);
VK_BINDING(27, 0) RWTexture2D<float4> g_signalIndirect : register(u19, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(29, 0) RWTexture2D<float4> g_denoisePing : register(u21, space0);
VK_BINDING(30, 0) RWTexture2D<float4> g_denoisePong : register(u22, space0);
VK_BINDING(0, 1) Texture2D g_textures[] : register(t0, space1);

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 2246822519u;
    value ^= value >> 13;
    value *= 3266489917u;
    value ^= value >> 16;
    return value;
}

float Random01(inout uint state)
{
    state = Hash(state);
    return (float)(state & 0x00ffffffu) / 16777216.0f;
}

uint SampleRotation8x8(uint2 pixel, uint frame)
{
    static const uint rotations[64] =
    {
        0u, 32u, 8u, 40u, 2u, 34u, 10u, 42u,
        48u, 16u, 56u, 24u, 50u, 18u, 58u, 26u,
        12u, 44u, 4u, 36u, 14u, 46u, 6u, 38u,
        60u, 28u, 52u, 20u, 62u, 30u, 54u, 22u,
        3u, 35u, 11u, 43u, 1u, 33u, 9u, 41u,
        51u, 19u, 59u, 27u, 49u, 17u, 57u, 25u,
        15u, 47u, 7u, 39u, 13u, 45u, 5u, 37u,
        63u, 31u, 55u, 23u, 61u, 29u, 53u, 21u
    };
    return rotations[((pixel.y & 7u) << 3u) | (pixel.x & 7u)] + frame * 17u;
}

int2 StableRestirOffset(uint sampleIndex, int radius, uint frame, uint2 pixel)
{
    static const int2 offsets[16] =
    {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
        int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1),
        int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
        int2(-2, -1), int2(2, -1), int2(-2, 1), int2(2, 1)
    };
    uint index = (sampleIndex + SampleRotation8x8(pixel, frame)) & 15u;
    int scale = max(radius / 2, 1);
    return offsets[index] * scale;
}

float2 Random2(inout uint state)
{
    return float2(Random01(state), Random01(state));
}

float BlueNoise01(uint2 pixel, uint frame, uint dimension)
{
    uint value = Hash(pixel.x * 0x8da6b343u ^ pixel.y * 0xd8163841u ^ frame * 0xcb1ab31fu ^ dimension * 0x165667b1u);
    value ^= Hash((pixel.x + pixel.y * 17u + dimension * 131u) * 0x9e3779b9u);
    return (float)(value & 0x00ffffffu) / 16777216.0f;
}

float3 Tonemap(float3 value)
{
    value = max(value, 0.0f.xxx);
    value = value / (1.0f.xxx + value);
    return pow(value, 1.0f / 2.2f);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float MaxComponent(float3 value)
{
    return max(value.x, max(value.y, value.z));
}

uint PixelIndex(uint2 pixel, uint2 dimensions)
{
    return pixel.y * dimensions.x + pixel.x;
}

RestirReservoir EmptyRestirReservoir()
{
    RestirReservoir reservoir;
    reservoir.radianceWeight = 0.0f.xxxx;
    reservoir.meta = 0.0f.xxxx;
    return reservoir;
}

float RestirTarget(float3 radiance, float mClamp)
{
    return min(max(Luminance(max(radiance, 0.0f.xxx)), 0.0f), max(mClamp, 0.001f));
}

RestirReservoir MakeRestirReservoir(float3 radiance, uint sampleCount, float mClamp)
{
    RestirReservoir reservoir = EmptyRestirReservoir();
    float target = RestirTarget(radiance, mClamp);
    reservoir.radianceWeight = float4(radiance, target);
    reservoir.meta = float4((float)sampleCount, 0.0f, 0.0f, target > 0.0001f ? 1.0f : 0.0f);
    return reservoir;
}

void CombineRestirReservoir(inout RestirReservoir reservoir, RestirReservoir candidate, float mClamp)
{
    if (candidate.meta.w <= 0.5f || candidate.radianceWeight.w <= 0.0f)
    {
        return;
    }

    if (reservoir.meta.w <= 0.5f || reservoir.radianceWeight.w <= 0.0f)
    {
        reservoir = candidate;
        return;
    }

    float totalWeight = reservoir.radianceWeight.w + candidate.radianceWeight.w;
    reservoir.radianceWeight.rgb = (reservoir.radianceWeight.rgb * reservoir.radianceWeight.w + candidate.radianceWeight.rgb * candidate.radianceWeight.w) / max(totalWeight, 0.0001f);
    reservoir.radianceWeight.w = min(totalWeight, max(mClamp, 0.001f));
    reservoir.meta.x = min(reservoir.meta.x + candidate.meta.x, max(mClamp, 1.0f));
    reservoir.meta.w = 1.0f;
}

RestirReservoir LoadRestirHistory(int2 pixel, uint2 dimensions)
{
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= (int)dimensions.x || pixel.y >= (int)dimensions.y)
    {
        return EmptyRestirReservoir();
    }
    return g_restirHistory[PixelIndex((uint2)pixel, dimensions)];
}

RestirReservoir LoadRestirDiHistory(int2 pixel, uint2 dimensions)
{
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= (int)dimensions.x || pixel.y >= (int)dimensions.y)
    {
        return EmptyRestirReservoir();
    }
#if PT_RESTIR_GI && PT_RESTIR_DI
    return g_restirDiHistory[PixelIndex((uint2)pixel, dimensions)];
#else
    return g_restirHistory[PixelIndex((uint2)pixel, dimensions)];
#endif
}

float2 EnvironmentUv(float3 rayDirection)
{
    float phi = atan2(rayDirection.x, rayDirection.z) + g_scene.environmentOptions.z;
    float u = frac(phi / (2.0f * PI) + 0.5f);
    float v = acos(clamp(rayDirection.y, -1.0f, 1.0f)) / PI;
    return float2(u, v);
}

float3 EvaluateEnvironmentMap(float3 rayDirection)
{
    uint environmentTextureIndex = (uint)round(g_scene.lightOptions.w);
    return g_textures[NonUniformResourceIndex(environmentTextureIndex)].SampleLevel(g_linearSampler, EnvironmentUv(rayDirection), 0.0f).rgb * g_scene.environmentOptions.y;
}

float3 EvaluateSky(float3 rayDirection)
{
    if (g_scene.environmentOptions.x > 0.5f)
    {
        return EvaluateEnvironmentMap(rayDirection);
    }

    float3 fallback = g_scene.skyColor.rgb * g_scene.skyColor.a;
    if (g_scene.skyOptions.w < 0.5f)
    {
        return fallback;
    }

    float y = rayDirection.y;
    float horizonBlend = saturate(y * 0.5f + 0.5f);
    float3 sky = lerp(g_scene.skyGroundColor.rgb, g_scene.skyHorizonColor.rgb, smoothstep(-g_scene.skyOptions.z, 0.15f, y));
    sky = lerp(sky, g_scene.skyZenithColor.rgb, horizonBlend * horizonBlend);

    float3 sunDirection = normalize(-g_scene.lightDirection.xyz);
    float sunDot = saturate(dot(rayDirection, sunDirection));
    float sunSize = max(g_scene.skyOptions.y, 0.001f);
    float sunDisk = smoothstep(cos(sunSize * 2.0f), cos(sunSize), sunDot);
    float3 sun = g_scene.lightColor.rgb * g_scene.skyOptions.x * sunDisk;
    return (sky * g_scene.skyColor.a) + sun + fallback * 0.05f;
}

float3 ClampRadiance(float3 radiance)
{
    float limit = max(g_scene.giOptions.y, 0.0f);
    if (limit <= 0.0f)
    {
        return radiance;
    }
    float lum = Luminance(radiance);
    if (lum > limit)
    {
        radiance *= limit / max(lum, 0.0001f);
    }
    return radiance;
}

float3 ApplyTemporalClamp(float3 current, float3 history, uint accumulatedFrames)
{
    if (accumulatedFrames == 0u)
    {
        return current;
    }
    float clampScale = max(g_scene.giOptions.z, 0.0f);
    float clampMin = max(g_scene.giOptions.w, 0.0f);
    float3 delta = max(abs(history) * clampScale, clampMin.xxx);
    return clamp(current, history - delta, history + delta);
}

float3 Barycentric3(float2 barycentrics)
{
    return float3(1.0f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
}

float3 Interpolate3(float3 a, float3 b, float3 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

float2 Interpolate2(float2 a, float2 b, float2 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

float4 Interpolate4(float4 a, float4 b, float4 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

uint3 LoadTriangleIndices(RtGeometryRecord geometry, uint primitiveIndex)
{
    uint index = geometry.indexOffset + primitiveIndex * 3u;
    return uint3(g_indices[index + 0], g_indices[index + 1], g_indices[index + 2]);
}

SurfaceData LoadSurface(uint geometryIndex, uint primitiveIndex, float2 barycentrics)
{
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    uint3 tri = LoadTriangleIndices(geometry, primitiveIndex);
    MeshVertex v0 = g_vertices[tri.x];
    MeshVertex v1 = g_vertices[tri.y];
    MeshVertex v2 = g_vertices[tri.z];
    float3 bary = Barycentric3(barycentrics);

    SurfaceData surface;
    surface.position = Interpolate3(v0.position, v1.position, v2.position, bary);
    surface.normal = normalize(Interpolate3(v0.normal, v1.normal, v2.normal, bary));
    surface.tangent = Interpolate4(v0.tangent, v1.tangent, v2.tangent, bary);
    surface.texcoord = Interpolate2(v0.texcoord, v1.texcoord, v2.texcoord, bary);
    surface.material = g_materials[geometry.materialIndex];

    float4 baseSample = g_textures[NonUniformResourceIndex(surface.material.textureBaseIndex + TextureSlotBaseColor)].SampleLevel(g_linearSampler, surface.texcoord, 0.0f);
    float4 roughnessSample = g_textures[NonUniformResourceIndex(surface.material.textureBaseIndex + TextureSlotRoughness)].SampleLevel(g_linearSampler, surface.texcoord, 0.0f);
    float4 metallicSample = g_textures[NonUniformResourceIndex(surface.material.textureBaseIndex + TextureSlotMetallic)].SampleLevel(g_linearSampler, surface.texcoord, 0.0f);
    float4 occlusionSample = g_textures[NonUniformResourceIndex(surface.material.textureBaseIndex + TextureSlotOcclusion)].SampleLevel(g_linearSampler, surface.texcoord, 0.0f);
    float3 normalTexture = g_textures[NonUniformResourceIndex(surface.material.textureBaseIndex + TextureSlotNormal)].SampleLevel(g_linearSampler, surface.texcoord, 0.0f).xyz;
    surface.baseColor = baseSample.rgb * surface.material.baseColorFactor.rgb;
    if (surface.material.packedOcclusionRoughnessMetallic != 0)
    {
        surface.ao = saturate(roughnessSample.r * surface.material.occlusionStrength);
        surface.roughness = clamp(roughnessSample.g * surface.material.roughnessFactor, 0.04f, 1.0f);
        surface.metallic = saturate(roughnessSample.b * surface.material.metallicFactor);
    }
    else
    {
        surface.ao = saturate(occlusionSample.r * surface.material.occlusionStrength);
        surface.roughness = clamp(roughnessSample.r * surface.material.roughnessFactor, 0.04f, 1.0f);
        surface.metallic = saturate(metallicSample.r * surface.material.metallicFactor);
    }
    surface.emissive = g_textures[NonUniformResourceIndex(surface.material.textureBaseIndex + TextureSlotEmissive)].SampleLevel(g_linearSampler, surface.texcoord, 0.0f).rgb * surface.material.emissiveFactor.rgb * surface.material.emissiveFactor.a * g_scene.lightOptions.y;
    surface.normalTexture = normalTexture;

    float2 normalXY = (normalTexture.xy * 2.0f - 1.0f) * surface.material.normalStrength;
    normalXY.y *= g_scene.debugOptions.y > 0.5f ? -1.0f : 1.0f;
    float3 normalSample = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
    float3 n = surface.normal;
    float3 t = normalize(surface.tangent.xyz - n * dot(n, surface.tangent.xyz));
    float3 b = normalize(cross(n, t) * surface.tangent.w);
    surface.normal = normalize(normalSample.x * t + normalSample.y * b + normalSample.z * n);
    return surface;
}

bool IsAlphaTransparent(uint geometryIndex, uint primitiveIndex, float2 barycentrics)
{
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    RtMaterial material = g_materials[geometry.materialIndex];
    if (material.alphaMasked == 0)
    {
        return false;
    }

    uint3 tri = LoadTriangleIndices(geometry, primitiveIndex);
    MeshVertex v0 = g_vertices[tri.x];
    MeshVertex v1 = g_vertices[tri.y];
    MeshVertex v2 = g_vertices[tri.z];
    float3 bary = Barycentric3(barycentrics);
    float2 texcoord = Interpolate2(v0.texcoord, v1.texcoord, v2.texcoord, bary);
    float alpha = g_textures[NonUniformResourceIndex(material.textureBaseIndex + TextureSlotBaseColor)].SampleLevel(g_linearSampler, texcoord, 0.0f).a * material.baseColorFactor.a;
    return alpha < material.alphaCutoff;
}

void BuildBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

float3 TangentToWorld(float3 localDirection, float3 normal)
{
    float3 tangent;
    float3 bitangent;
    BuildBasis(normal, tangent, bitangent);
    return normalize(localDirection.x * tangent + localDirection.y * bitangent + localDirection.z * normal);
}

float3 SampleCosineHemisphere(float2 sample, float3 normal)
{
    float phi = 2.0f * PI * sample.x;
    float r = sqrt(sample.y);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0f, 1.0f - sample.y));
    return TangentToWorld(float3(x, y, z), normal);
}

float3 SampleGGXDirection(float2 sample, float roughness, float3 normal, float3 incomingDirection)
{
    float a = max(roughness * roughness, 0.001f);
    float phi = 2.0f * PI * sample.x;
    float cosTheta = sqrt((1.0f - sample.y) / max(1.0f + (a * a - 1.0f) * sample.y, 0.0001f));
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    float3 halfVector = TangentToWorld(float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), normal);
    float3 direction = reflect(incomingDirection, halfVector);
    return dot(direction, normal) > 0.001f ? normalize(direction) : SampleCosineHemisphere(sample.yx, normal);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - saturate(f0)) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = saturate(dot(normal, halfVector));
    float nDotH2 = nDotH * nDotH;
    float denominator = nDotH2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denominator * denominator, 0.0001f);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return nDotV / max(nDotV * (1.0f - k) + k, 0.0001f);
}

float GeometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
{
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
}

float3 EvaluateBsdf(SurfaceData surface, float3 viewDirection, float3 lightDirection)
{
    float nDotL = saturate(dot(surface.normal, lightDirection));
    if (nDotL <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 halfVector = normalize(viewDirection + lightDirection);
    float3 f0 = lerp(0.04f.xxx, surface.baseColor, surface.metallic);
    float3 fresnel = FresnelSchlick(saturate(dot(halfVector, viewDirection)), f0);
    float distribution = DistributionGGX(surface.normal, halfVector, surface.roughness);
    float geometry = GeometrySmith(surface.normal, viewDirection, lightDirection, surface.roughness);
    float nDotV = saturate(dot(surface.normal, viewDirection));
    float3 specular = distribution * geometry * fresnel / max(4.0f * nDotV * nDotL, 0.0001f);
    float3 diffuse = (1.0f.xxx - fresnel) * (1.0f - surface.metallic) * surface.baseColor / PI;
    return (diffuse + specular) * nDotL;
}

float TraceVisibilityRay(float3 origin, float3 normal, float3 direction, float tMax)
{
    ShadowPayload payload;
    payload.occluded = 1;

    RayDesc ray;
    ray.Origin = origin + normal * g_scene.rayOptions.x;
    ray.Direction = direction;
    ray.TMin = g_scene.rayOptions.x;
    ray.TMax = tMax;
    TraceRay(g_sceneAs, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xff, 1, 0, 1, ray, payload);
    return payload.occluded == 0 ? 1.0f : 0.0f;
}

float3 EvaluateSunNEE(SurfaceData surface, float3 viewDirection)
{
    if (g_scene.debugOptions.z < 0.5f)
    {
        return 0.0f.xxx;
    }

    float3 lightDirection = normalize(-g_scene.lightDirection.xyz);
    float visibility = TraceVisibilityRay(surface.position, surface.normal, lightDirection, g_scene.rayOptions.y);
    float nDotL = saturate(dot(surface.normal, lightDirection));
    if (visibility <= 0.0f || nDotL <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 radiance = g_scene.lightColor.rgb * g_scene.lightColor.a;
    return EvaluateBsdf(surface, viewDirection, lightDirection) * radiance * visibility;
}

float3 EvaluateSkyNEE(SurfaceData surface, inout uint rngState)
{
    if (g_scene.debugOptions.w < 0.5f)
    {
        return 0.0f.xxx;
    }

    float3 direction = SampleCosineHemisphere(Random2(rngState), surface.normal);
    float visibility = TraceVisibilityRay(surface.position, surface.normal, direction, g_scene.rayOptions.y);
    float3 sky = EvaluateSky(direction);
    return sky * surface.baseColor * surface.ao * visibility;
}

float PreviousLightCdf(uint lightIndex)
{
    return lightIndex == 0u ? 0.0f : g_lights[lightIndex - 1u].radianceCdf.w;
}

uint SelectLightIndex(float sample, uint lightCount)
{
    uint low = 0u;
    uint high = lightCount;
    for (uint i = 0u; i < 16u; ++i)
    {
        if (low >= high)
        {
            break;
        }

        uint mid = (low + high) >> 1u;
        if (sample <= g_lights[mid].radianceCdf.w)
        {
            high = mid;
        }
        else
        {
            low = mid + 1u;
        }
    }
    return min(low, lightCount - 1u);
}

float3 EvaluateAreaLightNEE(SurfaceData surface, float3 viewDirection, inout uint rngState)
{
    uint lightCount = (uint)round(g_scene.lightOptions.x);
    if (lightCount == 0u)
    {
        return 0.0f.xxx;
    }

    float lightSample = Random01(rngState);
    uint lightIndex = SelectLightIndex(lightSample, lightCount);
    RtLight light = g_lights[lightIndex];
    float2 uv = Random2(rngState);
    float3 edge0 = light.edge0Type.xyz;
    float3 edge1 = light.edge1.xyz;
    float3 lightPosition = light.positionArea.xyz + edge0 * uv.x + edge1 * uv.y;
    float3 toLight = lightPosition - surface.position;
    float distanceSquared = max(dot(toLight, toLight), 0.0001f);
    float distanceToLight = sqrt(distanceSquared);
    float3 lightDirection = toLight / distanceToLight;
    float nDotL = saturate(dot(surface.normal, lightDirection));
    if (nDotL <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 lightNormal = normalize(cross(edge0, edge1));
    float lightCos = abs(dot(lightNormal, -lightDirection));
    if (lightCos <= 0.0001f)
    {
        return 0.0f.xxx;
    }

    float selectionPdf = max(light.radianceCdf.w - PreviousLightCdf(lightIndex), 0.0001f);
    float areaPdf = selectionPdf / max(light.positionArea.w, 0.0001f);
    float solidAnglePdf = max(areaPdf * distanceSquared / lightCos, 0.0001f);
    float visibility = TraceVisibilityRay(surface.position, surface.normal, lightDirection, distanceToLight - g_scene.rayOptions.x * 2.0f);
    if (visibility <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float type = light.edge0Type.w;
    float intensity = type < 0.5f ? g_scene.lightOptions.y : g_scene.lightOptions.z;
    float3 radiance = light.radianceCdf.rgb * intensity;
    return EvaluateBsdf(surface, viewDirection, lightDirection) * radiance * visibility / solidAnglePdf;
}

RayPayload EmptyPayload()
{
    RayPayload payload;
    payload.position = 0.0f.xxx;
    payload.hitT = 0.0f;
    payload.normal = 0.0f.xxx;
    payload.hit = 0u;
    payload.baseColor = 0.0f.xxx;
    payload.roughness = 1.0f;
    payload.normalTexture = 0.5f.xxx;
    payload.metallic = 0.0f;
    payload.emissive = 0.0f.xxx;
    payload.ao = 1.0f;
    return payload;
}

SurfaceData SurfaceFromPayload(RayPayload payload)
{
    SurfaceData surface;
    surface.position = payload.position;
    surface.normal = payload.normal;
    surface.tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
    surface.texcoord = 0.0f.xx;
    surface.baseColor = payload.baseColor;
    surface.normalTexture = payload.normalTexture;
    surface.ao = payload.ao;
    surface.roughness = payload.roughness;
    surface.metallic = payload.metallic;
    surface.emissive = payload.emissive;
    return surface;
}

float3 GenerateCameraDirection(uint2 pixel)
{
    uint2 dimensions = DispatchRaysDimensions().xy;
    float2 uv = (float2(pixel) + 0.5f.xx + g_scene.jitterOptions.xy) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), g_scene.inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), g_scene.inverseViewProjection);
    nearPoint.xyz /= nearPoint.w;
    farPoint.xyz /= farPoint.w;
    return normalize(farPoint.xyz - nearPoint.xyz);
}

float3 TracePathSample(uint2 pixel, uint sampleIndex, out RayPayload firstHit, out float3 directNee, out float3 indirect, out uint bounceCount)
{
    uint frameCounter = (uint)round(g_scene.frameOptions.w);
    uint rngState = Hash(pixel.x * 1973u ^ pixel.y * 9277u ^ sampleIndex * 26699u ^ 0x9e3779b9u);
    rngState ^= Hash((uint)(BlueNoise01(pixel, frameCounter, sampleIndex & 7u) * 16777216.0f) + sampleIndex * 747796405u);
    float3 rayOrigin = g_scene.cameraPosition.xyz;
    float3 rayDirection = GenerateCameraDirection(pixel);
    float3 throughput = 1.0f.xxx;
    float3 radiance = 0.0f.xxx;
    directNee = 0.0f.xxx;
    indirect = 0.0f.xxx;
    bounceCount = 0u;
    firstHit = EmptyPayload();

    uint maxBounces = clamp((uint)round(g_scene.pathOptions.x), 1u, 8u);
    uint minBounces = min((uint)round(g_scene.pathOptions.y), maxBounces);

    for (uint bounce = 0u; bounce < 8u; ++bounce)
    {
        if (bounce >= maxBounces)
        {
            break;
        }

        RayPayload payload = EmptyPayload();
        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDirection;
        ray.TMin = g_scene.rayOptions.x;
        ray.TMax = g_scene.rayOptions.y;
        TraceRay(g_sceneAs, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray, payload);

        if (payload.hit == 0u)
        {
            float3 sky = EvaluateSky(rayDirection);
            radiance += throughput * sky;
            if (bounce > 0u)
            {
                indirect += throughput * sky;
            }
            break;
        }

        SurfaceData surface = SurfaceFromPayload(payload);
        surface.normal = dot(surface.normal, -rayDirection) > 0.0f ? surface.normal : -surface.normal;
        if (bounce == 0u)
        {
            firstHit = payload;
            firstHit.normal = surface.normal;
        }

        radiance += throughput * surface.emissive;
        float3 viewDirection = normalize(-rayDirection);
        float3 sun = EvaluateSunNEE(surface, viewDirection);
        float3 skyNee = EvaluateSkyNEE(surface, rngState);
        float3 areaNee = EvaluateAreaLightNEE(surface, viewDirection, rngState);
        float3 nee = sun + skyNee + areaNee;
        radiance += throughput * nee;
        if (bounce == 0u)
        {
            directNee += sun + areaNee;
            indirect += skyNee;
        }
        else
        {
            indirect += throughput * nee;
        }

        float3 f0 = lerp(0.04f.xxx, surface.baseColor, surface.metallic);
        float fresnelWeight = saturate(MaxComponent(FresnelSchlick(saturate(dot(surface.normal, viewDirection)), f0)));
        float specularProbability = saturate(0.2f + 0.55f * (1.0f - surface.roughness) + 0.25f * surface.metallic + 0.25f * fresnelWeight);
        float chooseSpecular = Random01(rngState);
        float3 nextDirection;
        float3 bsdfWeight;

        if (chooseSpecular < specularProbability)
        {
            nextDirection = SampleGGXDirection(Random2(rngState), surface.roughness, surface.normal, rayDirection);
            float3 fresnel = FresnelSchlick(saturate(dot(surface.normal, nextDirection)), f0);
            bsdfWeight = fresnel / max(specularProbability, 0.05f);
        }
        else
        {
            nextDirection = SampleCosineHemisphere(Random2(rngState), surface.normal);
            bsdfWeight = surface.baseColor * (1.0f - surface.metallic) / max(1.0f - specularProbability, 0.05f);
        }

        throughput *= bsdfWeight;
        throughput = min(throughput, 16.0f.xxx);
        rayOrigin = surface.position + surface.normal * g_scene.rayOptions.x;
        rayDirection = normalize(nextDirection);
        bounceCount = bounce + 1u;

        if (bounce + 1u >= minBounces)
        {
            float continueProbability = clamp(MaxComponent(throughput), 0.05f, 0.95f);
            if (Random01(rngState) > continueProbability)
            {
                break;
            }
            throughput /= continueProbability;
        }
    }

    return ClampRadiance(radiance);
}

float2 ProjectWorldToUv(float3 worldPosition, float4x4 viewProjection)
{
    float4 clip = mul(float4(worldPosition, 1.0f), viewProjection);
    float2 ndc = clip.xy / max(abs(clip.w), 0.0001f);
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dimensions = DispatchRaysDimensions().xy;
    uint accumulatedFrames = (uint)round(g_scene.frameOptions.x);
    uint maxAccumulatedFrames = max((uint)round(g_scene.frameOptions.y), 1u);
    uint frameCounter = (uint)round(g_scene.frameOptions.w);
    uint samplesPerFrame = clamp((uint)round(g_scene.giOptions.x), 1u, 8u);
    uint giCandidateMultiplier = PT_RESTIR_GI ? clamp((uint)round(g_scene.pathOptions.z), 1u, 4u) : 1u;
    uint diCandidateMultiplier = PT_RESTIR_DI ? clamp((uint)round(g_scene.restirDiOptions.z), 1u, 4u) : 1u;
    uint candidateMultiplier = max(giCandidateMultiplier, diCandidateMultiplier);
    uint totalSamples = samplesPerFrame * candidateMultiplier;
    if (g_scene.adaptiveOptions.x > 0.5f)
    {
        float4 historyInfo = frameCounter > 0u ? g_reconstructionHistoryLength[pixel] : 0.0f.xxxx;
        float variance = frameCounter > 0u ? g_reconstructionHistoryMoments[pixel].z : 0.0f;
        float historyLength = historyInfo.x;
        float disocclusion = historyInfo.y;
        uint adaptiveMax = clamp((uint)round(g_scene.adaptiveOptions.y), 1u, 4u);
        if (variance > g_scene.adaptiveOptions.z || historyLength < 2.0f || disocclusion > 0.5f)
        {
            totalSamples = max(totalSamples, adaptiveMax);
        }
    }
    uint firstSampleIndex = frameCounter * totalSamples + SampleRotation8x8(pixel, frameCounter);

    RayPayload firstHit = EmptyPayload();
    float3 color = 0.0f.xxx;
    float3 directNee = 0.0f.xxx;
    float3 indirect = 0.0f.xxx;
    uint bounceCount = 0u;

    for (uint i = 0u; i < 32u; ++i)
    {
        if (i >= totalSamples)
        {
            break;
        }
        RayPayload sampleFirstHit;
        float3 sampleDirect;
        float3 sampleIndirect;
        uint sampleBounces;
        color += TracePathSample(pixel, firstSampleIndex + i, sampleFirstHit, sampleDirect, sampleIndirect, sampleBounces);
        if (i == 0u)
        {
            firstHit = sampleFirstHit;
        }
        directNee += sampleDirect;
        indirect += sampleIndirect;
        bounceCount += sampleBounces;
    }

    color /= (float)totalSamples;
    directNee /= (float)totalSamples;
    indirect /= (float)totalSamples;
    float averageBounces = (float)bounceCount / (float)max(totalSamples, 1u);
    float3 skyRadiance = EvaluateSky(GenerateCameraDirection(pixel));

    uint debugMode = (uint)round(g_scene.debugOptions.x);
    float3 debugColor = color;
    if (debugMode == 1u) debugColor = firstHit.baseColor;
    if (debugMode == 2u) debugColor = firstHit.hit != 0u ? firstHit.normal * 0.5f + 0.5f : 0.0f.xxx;
    if (debugMode == 3u) debugColor = firstHit.normalTexture;
    if (debugMode == 4u) debugColor = firstHit.roughness.xxx;
    if (debugMode == 5u) debugColor = firstHit.metallic.xxx;
    if (debugMode == 6u) debugColor = firstHit.emissive;
    if (debugMode == 7u) debugColor = firstHit.hit != 0u ? saturate(firstHit.hitT / 250.0f).xxx : 0.0f.xxx;
    if (debugMode == 8u) debugColor = directNee;
    if (debugMode == 9u) debugColor = indirect;
    if (debugMode == 10u) debugColor = (averageBounces / max(g_scene.pathOptions.x, 1.0f)).xxx;
    if (debugMode == 12u) debugColor = skyRadiance;
#if PT_RESTIR_GI
    {
    uint pixelIndex = PixelIndex(pixel, dimensions);
    uint restirSpatialPasses = min((uint)round(g_scene.restirOptions.y), 4u);
    RestirReservoir reservoir = MakeRestirReservoir(indirect, totalSamples, g_scene.restirOptions.w);

    if (accumulatedFrames > 0u && g_scene.restirOptions.x > 0.5f)
    {
        RestirReservoir history = g_restirHistory[pixelIndex];
        float historyWeightScale = saturate((float)accumulatedFrames / 8.0f);
        history.radianceWeight.w *= historyWeightScale;
        CombineRestirReservoir(reservoir, history, g_scene.restirOptions.w);
        reservoir.meta.y = max(reservoir.meta.y, history.meta.w);
    }

    if (accumulatedFrames > 0u && restirSpatialPasses > 0u)
    {
        int radius = max((int)round(g_scene.restirOptions.z), 1);
        uint spatialSamples = min(restirSpatialPasses * 2u, 8u);
        for (uint i = 0u; i < 8u; ++i)
        {
            if (i >= spatialSamples)
            {
                break;
            }
            int2 offset = StableRestirOffset(i, radius, frameCounter, pixel);
            RestirReservoir neighbor = LoadRestirHistory(int2(pixel) + offset, dimensions);
            neighbor.radianceWeight.w *= 0.5f;
            CombineRestirReservoir(reservoir, neighbor, g_scene.restirOptions.w);
            reservoir.meta.z += neighbor.meta.w;
        }
    }

    if (reservoir.meta.w > 0.5f)
    {
        float3 restirIndirect = ClampRadiance(reservoir.radianceWeight.rgb);
        color += restirIndirect - indirect;
        indirect = restirIndirect;
    }
    g_restirCurrent[pixelIndex] = reservoir;

    if (debugMode == 0u) debugColor = color;
    if (debugMode == 9u) debugColor = indirect;
    if (debugMode == 13u) debugColor = saturate(reservoir.radianceWeight.w / max(g_scene.restirOptions.w, 0.001f)).xxx;
    if (debugMode == 14u) debugColor = reservoir.meta.y.xxx;
    if (debugMode == 15u) debugColor = saturate(reservoir.meta.z / max(restirSpatialPasses * 2.0f, 1.0f)).xxx;
    if (debugMode == 26u) debugColor = saturate(reservoir.radianceWeight.w / max(g_scene.restirOptions.w, 0.001f)).xxx;
    if (debugMode == 28u) debugColor = reservoir.meta.y.xxx;
    if (debugMode == 30u) debugColor = saturate(reservoir.meta.z / max(restirSpatialPasses * 2.0f, 1.0f)).xxx;
    }
#endif
#if PT_RESTIR_DI
    {
    uint diPixelIndex = PixelIndex(pixel, dimensions);
    float diMClamp = max(g_scene.restirDiOptions.w, 0.001f);
    uint diSpatialPasses = min((uint)round(g_scene.restirDiOptions.y), 4u);
    RestirReservoir reservoir = MakeRestirReservoir(directNee, totalSamples, diMClamp);

    if (accumulatedFrames > 0u && g_scene.restirDiOptions.x > 0.5f)
    {
#if PT_RESTIR_GI && PT_RESTIR_DI
        RestirReservoir history = g_restirDiHistory[diPixelIndex];
#else
        RestirReservoir history = g_restirHistory[diPixelIndex];
#endif
        float historyWeightScale = saturate((float)accumulatedFrames / 6.0f);
        history.radianceWeight.w *= historyWeightScale;
        CombineRestirReservoir(reservoir, history, diMClamp);
        reservoir.meta.y = max(reservoir.meta.y, history.meta.w);
    }

    if (accumulatedFrames > 0u && diSpatialPasses > 0u)
    {
        int radius = max((int)round(g_scene.restirOptions.z), 1);
        uint spatialSamples = min(diSpatialPasses * 4u, 16u);
        for (uint i = 0u; i < 16u; ++i)
        {
            if (i >= spatialSamples)
            {
                break;
            }
            int2 offset = StableRestirOffset(i, radius, frameCounter ^ 0x9e3779b9u, pixel);
            RestirReservoir neighbor = LoadRestirDiHistory(int2(pixel) + offset, dimensions);
            neighbor.radianceWeight.w *= 0.5f;
            CombineRestirReservoir(reservoir, neighbor, diMClamp);
            reservoir.meta.z += neighbor.meta.w;
        }
    }

    if (reservoir.meta.w > 0.5f)
    {
        float3 restirDirect = ClampRadiance(reservoir.radianceWeight.rgb);
        color += restirDirect - directNee;
        directNee = restirDirect;
    }
#if PT_RESTIR_GI && PT_RESTIR_DI
    g_restirDiCurrent[diPixelIndex] = reservoir;
#else
    g_restirCurrent[diPixelIndex] = reservoir;
#endif

    if (debugMode == 0u) debugColor = color;
    if (debugMode == 8u) debugColor = directNee;
    if (debugMode == 13u) debugColor = saturate(reservoir.radianceWeight.w / diMClamp).xxx;
    if (debugMode == 14u) debugColor = reservoir.meta.y.xxx;
    if (debugMode == 15u) debugColor = saturate(reservoir.meta.z / max(diSpatialPasses * 4.0f, 1.0f)).xxx;
    if (debugMode == 27u) debugColor = saturate(reservoir.radianceWeight.w / diMClamp).xxx;
    if (debugMode == 29u) debugColor = reservoir.meta.y.xxx;
    if (debugMode == 31u) debugColor = saturate(reservoir.meta.z / max(diSpatialPasses * 4.0f, 1.0f)).xxx;
    }
#endif

    float3 currentFrameColor = color;
    float3 residualSignal = max(currentFrameColor - directNee - indirect, 0.0f.xxx);
    g_signalCurrentRadiance[pixel] = float4(currentFrameColor, 1.0f);
    g_signalDirect[pixel] = float4(max(directNee, 0.0f.xxx), 1.0f);
    g_signalIndirect[pixel] = float4(max(indirect, 0.0f.xxx), 1.0f);
    g_signalResidual[pixel] = float4(residualSignal, firstHit.roughness);

    if (debugMode == 0u)
    {
        if (g_scene.frameOptions.z > 0.5f)
        {
            debugColor = g_accumulation[pixel].rgb;
        }
        else
        {
            float3 history = g_accumulation[pixel].rgb;
            debugColor = ApplyTemporalClamp(debugColor, history, accumulatedFrames);
            float weight = 1.0f / (float)(min(accumulatedFrames, maxAccumulatedFrames - 1u) + 1u);
            debugColor = lerp(history, debugColor, weight);
            g_accumulation[pixel] = float4(debugColor, 1.0f);
        }
    }

    if (debugMode == 11u)
    {
        debugColor = ((float)accumulatedFrames / (float)maxAccumulatedFrames).xxx;
    }

    float firstHitDepth = firstHit.hit != 0u ? firstHit.hitT : -1.0f;
    float3 firstHitNormal = firstHit.hit != 0u ? firstHit.normal * 0.5f + 0.5f : 0.0f.xxx;
    float2 motionVector = 0.0f.xx;
    if (firstHit.hit != 0u)
    {
        float2 currentUv = ProjectWorldToUv(firstHit.position, g_scene.viewProjection);
        float2 previousUv = ProjectWorldToUv(firstHit.position, g_scene.previousViewProjection);
        motionVector = currentUv - previousUv;
    }
    if (debugMode > 15u)
    {
        g_accumulation[pixel] = float4(color, 1.0f);
    }
    g_denoiseAov0[pixel] = float4(firstHitNormal, firstHitDepth);
    g_denoiseAov1[pixel] = float4(firstHit.baseColor, firstHit.roughness);
    g_denoiseAov2[pixel] = float4(motionVector, firstHit.metallic, firstHit.hit != 0u ? 1.0f : 0.0f);

    g_output[pixel] = float4(Tonemap(debugColor), 1.0f);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload = EmptyPayload();
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.occluded = 0;
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    if (IsAlphaTransparent(GeometryIndex(), PrimitiveIndex(), attributes.barycentrics))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    if (IsAlphaTransparent(GeometryIndex(), PrimitiveIndex(), attributes.barycentrics))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    SurfaceData surface = LoadSurface(GeometryIndex(), PrimitiveIndex(), attributes.barycentrics);
    payload.position = surface.position;
    payload.hitT = RayTCurrent();
    payload.normal = surface.normal;
    payload.hit = 1u;
    payload.baseColor = surface.baseColor;
    payload.roughness = surface.roughness;
    payload.normalTexture = surface.normalTexture;
    payload.metallic = surface.metallic;
    payload.emissive = surface.emissive;
    payload.ao = surface.ao;
}
