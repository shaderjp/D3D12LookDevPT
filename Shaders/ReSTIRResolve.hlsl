#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
#endif

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

struct RestirReservoir
{
    float4 radianceWeight;
    float4 meta;
};

VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(9, 0) RWStructuredBuffer<RestirReservoir> g_restirCurrent : register(u2, space0);
VK_BINDING(10, 0) RWStructuredBuffer<RestirReservoir> g_restirHistory : register(u3, space0);
VK_BINDING(11, 0) RWStructuredBuffer<RestirReservoir> g_restirSpatial : register(u4, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(19, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(20, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(21, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(22, 0) RWStructuredBuffer<RestirReservoir> g_restirDiCurrent : register(u14, space0);
VK_BINDING(23, 0) RWStructuredBuffer<RestirReservoir> g_restirDiHistory : register(u15, space0);
VK_BINDING(24, 0) RWStructuredBuffer<RestirReservoir> g_restirDiSpatial : register(u16, space0);
VK_BINDING(25, 0) RWTexture2D<float4> g_signalCurrentRadiance : register(u17, space0);

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

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint PixelIndex(uint2 pixel, uint2 dimensions)
{
    return pixel.y * dimensions.x + pixel.x;
}

float3 DecodeNormal(float4 aov)
{
    float3 encoded = aov.xyz * 2.0f - 1.0f;
    return dot(encoded, encoded) > 0.0001f ? normalize(encoded) : float3(0.0f, 1.0f, 0.0f);
}

bool IsInside(int2 pixel, uint2 dimensions)
{
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < (int)dimensions.x && pixel.y < (int)dimensions.y;
}

uint SampleRotation8x8(uint2 pixel, uint frame)
{
    static const uint table[64] =
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
    uint index = ((pixel.y & 7u) << 3u) | (pixel.x & 7u);
    return table[index] ^ (frame * 13u);
}

int2 StableRestirOffset(uint sampleIndex, int radius, uint frame, uint2 pixel)
{
    static const int2 offsets[16] =
    {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(2, 1), int2(-2, 1), int2(2, -1), int2(-2, -1),
        int2(1, 2), int2(-1, 2), int2(1, -2), int2(-1, -2),
        int2(3, 0), int2(0, 3), int2(-3, 0), int2(0, -3)
    };
    uint rotation = SampleRotation8x8(pixel, frame + sampleIndex);
    int2 offset = offsets[(sampleIndex + rotation) & 15u];
    int scale = 1 + (int)((sampleIndex + (rotation >> 2u)) % (uint)max(radius, 1));
    return clamp(offset * scale, int2(-radius, -radius), int2(radius, radius));
}

float CurrentRadianceClampLimit(uint2 pixel, float fixedLimit, float multiplier)
{
    float currentLum = Luminance(max(g_signalCurrentRadiance[pixel].rgb, 0.0f.xxx));
    float dynamicLimit = max(currentLum * multiplier + 0.5f, 1.0f);
    return min(max(fixedLimit, 0.001f), dynamicLimit);
}

bool ValidateAovPair(float4 currentAov0, float4 currentAov1, float4 currentAov2, float4 candidateAov0, float4 candidateAov1, float4 candidateAov2)
{
    if (g_scene.restirStabilityOptions.y < 0.5f)
    {
        return true;
    }
    if (currentAov2.w <= 0.5f || candidateAov2.w <= 0.5f || currentAov0.w <= 0.0f || candidateAov0.w <= 0.0f)
    {
        return false;
    }

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(candidateAov0));
    float depthDelta = abs(currentAov0.w - candidateAov0.w) / max(currentAov0.w, 1.0f);
    float albedoDelta = length(currentAov1.rgb - candidateAov1.rgb);
    float roughnessDelta = abs(currentAov1.w - candidateAov1.w);
    return normalDot >= g_scene.validationOptions.x &&
        depthDelta <= g_scene.validationOptions.y &&
        albedoDelta <= g_scene.validationOptions.z &&
        roughnessDelta <= g_scene.validationOptions.w;
}

RestirReservoir EmptyReservoir()
{
    RestirReservoir reservoir;
    reservoir.radianceWeight = 0.0f.xxxx;
    reservoir.meta = 0.0f.xxxx;
    return reservoir;
}

void CombineReservoir(inout RestirReservoir reservoir, RestirReservoir candidate, float scale, float temporalFlag, float spatialFlag, float mClamp)
{
    if (candidate.meta.w <= 0.5f || candidate.radianceWeight.w <= 0.0f)
    {
        return;
    }

    candidate.radianceWeight.w *= scale;
    if (candidate.radianceWeight.w <= 0.0f)
    {
        return;
    }

    if (reservoir.meta.w <= 0.5f || reservoir.radianceWeight.w <= 0.0f)
    {
        reservoir = candidate;
        reservoir.meta.y = max(reservoir.meta.y, temporalFlag);
        reservoir.meta.z = max(reservoir.meta.z, spatialFlag);
        return;
    }

    float totalWeight = reservoir.radianceWeight.w + candidate.radianceWeight.w;
    reservoir.radianceWeight.rgb = (reservoir.radianceWeight.rgb * reservoir.radianceWeight.w + candidate.radianceWeight.rgb * candidate.radianceWeight.w) / max(totalWeight, 0.0001f);
    reservoir.radianceWeight.w = min(totalWeight, max(mClamp, 0.001f));
    reservoir.meta.x = min(reservoir.meta.x + candidate.meta.x, max(mClamp, 1.0f));
    reservoir.meta.y = max(reservoir.meta.y, temporalFlag);
    reservoir.meta.z += spatialFlag;
    reservoir.meta.w = 1.0f;
}

RestirReservoir LoadCurrentReservoir(int2 pixel, uint2 dimensions)
{
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= (int)dimensions.x || pixel.y >= (int)dimensions.y)
    {
        return EmptyReservoir();
    }
    return g_restirCurrent[PixelIndex((uint2)pixel, dimensions)];
}

RestirReservoir LoadHistoryReservoir(int2 pixel, uint2 dimensions)
{
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= (int)dimensions.x || pixel.y >= (int)dimensions.y)
    {
        return EmptyReservoir();
    }
    return g_restirHistory[PixelIndex((uint2)pixel, dimensions)];
}

RestirReservoir LoadValidatedHistoryReservoir(uint2 currentPixel, int2 historyPixel, uint2 dimensions)
{
    if (!IsInside(historyPixel, dimensions))
    {
        return EmptyReservoir();
    }
    float4 currentAov0 = g_denoiseAov0[currentPixel];
    float4 currentAov1 = g_denoiseAov1[currentPixel];
    float4 currentAov2 = g_denoiseAov2[currentPixel];
    float4 historyAov0 = g_previousDenoiseAov0[uint2(historyPixel)];
    float4 historyAov1 = g_previousDenoiseAov1[uint2(historyPixel)];
    float4 historyAov2 = g_previousDenoiseAov2[uint2(historyPixel)];
    if (!ValidateAovPair(currentAov0, currentAov1, currentAov2, historyAov0, historyAov1, historyAov2))
    {
        return EmptyReservoir();
    }
    RestirReservoir reservoir = g_restirHistory[PixelIndex((uint2)historyPixel, dimensions)];
    float maxAge = max(g_scene.restirStabilityOptions.w, 1.0f);
    reservoir.radianceWeight.w *= saturate(1.0f - reservoir.meta.x / (maxAge + 1.0f));
    reservoir.meta.x = min(reservoir.meta.x + 1.0f, maxAge);
    return reservoir;
}

RestirReservoir LoadValidatedCurrentReservoir(uint2 currentPixel, int2 candidatePixel, uint2 dimensions)
{
    if (!IsInside(candidatePixel, dimensions))
    {
        return EmptyReservoir();
    }
    if (g_scene.restirStabilityOptions.y > 0.5f &&
        !ValidateAovPair(g_denoiseAov0[currentPixel], g_denoiseAov1[currentPixel], g_denoiseAov2[currentPixel],
            g_denoiseAov0[uint2(candidatePixel)], g_denoiseAov1[uint2(candidatePixel)], g_denoiseAov2[uint2(candidatePixel)]))
    {
        return EmptyReservoir();
    }
    return g_restirCurrent[PixelIndex((uint2)candidatePixel, dimensions)];
}

RestirReservoir LoadValidatedHistoryReservoirDI(uint2 currentPixel, int2 historyPixel, uint2 dimensions)
{
    if (!IsInside(historyPixel, dimensions))
    {
        return EmptyReservoir();
    }
    float4 currentAov0 = g_denoiseAov0[currentPixel];
    float4 currentAov1 = g_denoiseAov1[currentPixel];
    float4 currentAov2 = g_denoiseAov2[currentPixel];
    float4 historyAov0 = g_previousDenoiseAov0[uint2(historyPixel)];
    float4 historyAov1 = g_previousDenoiseAov1[uint2(historyPixel)];
    float4 historyAov2 = g_previousDenoiseAov2[uint2(historyPixel)];
    if (!ValidateAovPair(currentAov0, currentAov1, currentAov2, historyAov0, historyAov1, historyAov2))
    {
        return EmptyReservoir();
    }
    RestirReservoir reservoir = g_restirDiHistory[PixelIndex((uint2)historyPixel, dimensions)];
    float maxAge = max(g_scene.restirStabilityOptions.w, 1.0f);
    reservoir.radianceWeight.w *= saturate(1.0f - reservoir.meta.x / (maxAge + 1.0f));
    reservoir.meta.x = min(reservoir.meta.x + 1.0f, maxAge);
    return reservoir;
}

RestirReservoir LoadValidatedCurrentReservoirDI(uint2 currentPixel, int2 candidatePixel, uint2 dimensions)
{
    if (!IsInside(candidatePixel, dimensions))
    {
        return EmptyReservoir();
    }
    if (g_scene.restirStabilityOptions.y > 0.5f &&
        !ValidateAovPair(g_denoiseAov0[currentPixel], g_denoiseAov1[currentPixel], g_denoiseAov2[currentPixel],
            g_denoiseAov0[uint2(candidatePixel)], g_denoiseAov1[uint2(candidatePixel)], g_denoiseAov2[uint2(candidatePixel)]))
    {
        return EmptyReservoir();
    }
    return g_restirDiCurrent[PixelIndex((uint2)candidatePixel, dimensions)];
}

[numthreads(8, 8, 1)]
void RestirTemporalSpatialCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    uint pixelIndex = PixelIndex(pixel, dimensions);
    uint accumulatedFrames = (uint)round(g_scene.frameOptions.x);
    uint spatialPasses = min((uint)round(g_scene.restirOptions.y), 4u);
    RestirReservoir reservoir = g_restirCurrent[pixelIndex];

    if (accumulatedFrames > 0u && g_scene.restirOptions.x > 0.5f)
    {
        int2 historyPixel = int2(pixel);
        if (g_scene.restirStabilityOptions.x > 0.5f)
        {
            float2 motion = g_denoiseAov2[pixel].xy;
            historyPixel = int2(round(float2(pixel) - motion * float2(dimensions)));
        }
        CombineReservoir(reservoir, LoadValidatedHistoryReservoir(pixel, historyPixel, dimensions), 0.75f, 1.0f, 0.0f, g_scene.restirOptions.w);
    }

    if (accumulatedFrames > 0u && spatialPasses > 0u)
    {
        int radius = max((int)round(g_scene.restirOptions.z), 1);
        uint sampleCount = min(spatialPasses * 4u, 16u);
        uint frameCounter = (uint)round(g_scene.frameOptions.w);

        for (uint i = 0u; i < 16u; ++i)
        {
            if (i >= sampleCount)
            {
                break;
            }

            int2 offset = StableRestirOffset(i, radius, frameCounter, pixel);
            RestirReservoir currentNeighbor = LoadValidatedCurrentReservoir(pixel, int2(pixel) + offset, dimensions);
            CombineReservoir(reservoir, currentNeighbor, 0.5f, 0.0f, 1.0f, g_scene.restirOptions.w);

            RestirReservoir historyNeighbor = LoadValidatedHistoryReservoir(pixel, int2(pixel) + offset, dimensions);
            CombineReservoir(reservoir, historyNeighbor, 0.35f, 1.0f, 1.0f, g_scene.restirOptions.w);
        }
    }

    if (reservoir.meta.w > 0.5f)
    {
        float limit = CurrentRadianceClampLimit(pixel, g_scene.restirOptions.w, 4.0f);
        float lum = Luminance(max(reservoir.radianceWeight.rgb, 0.0f.xxx));
        if (lum > limit)
        {
            reservoir.radianceWeight.rgb *= limit / max(lum, 0.0001f);
        }
    }

    g_restirSpatial[pixelIndex] = reservoir;
}

[numthreads(8, 8, 1)]
void RestirTemporalSpatialDICS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    uint pixelIndex = PixelIndex(pixel, dimensions);
    uint accumulatedFrames = (uint)round(g_scene.frameOptions.x);
    uint spatialPasses = min((uint)round(g_scene.restirDiOptions.y), 4u);
    float mClamp = max(g_scene.restirDiOptions.w, 0.001f);
    RestirReservoir reservoir = g_restirDiCurrent[pixelIndex];

    if (accumulatedFrames > 0u && g_scene.restirDiOptions.x > 0.5f)
    {
        int2 historyPixel = int2(pixel);
        if (g_scene.restirStabilityOptions.x > 0.5f)
        {
            float2 motion = g_denoiseAov2[pixel].xy;
            historyPixel = int2(round(float2(pixel) - motion * float2(dimensions)));
        }
        CombineReservoir(reservoir, LoadValidatedHistoryReservoirDI(pixel, historyPixel, dimensions), 0.75f, 1.0f, 0.0f, mClamp);
    }

    if (accumulatedFrames > 0u && spatialPasses > 0u)
    {
        int radius = max((int)round(g_scene.restirOptions.z), 1);
        uint sampleCount = min(spatialPasses * 4u, 16u);
        uint frameCounter = (uint)round(g_scene.frameOptions.w) ^ 37u;

        for (uint i = 0u; i < 16u; ++i)
        {
            if (i >= sampleCount)
            {
                break;
            }

            int2 offset = StableRestirOffset(i, radius, frameCounter, pixel);
            RestirReservoir currentNeighbor = LoadValidatedCurrentReservoirDI(pixel, int2(pixel) + offset, dimensions);
            CombineReservoir(reservoir, currentNeighbor, 0.5f, 0.0f, 1.0f, mClamp);

            RestirReservoir historyNeighbor = LoadValidatedHistoryReservoirDI(pixel, int2(pixel) + offset, dimensions);
            CombineReservoir(reservoir, historyNeighbor, 0.35f, 1.0f, 1.0f, mClamp);
        }
    }

    if (reservoir.meta.w > 0.5f)
    {
        float limit = CurrentRadianceClampLimit(pixel, mClamp, 5.0f);
        float lum = Luminance(max(reservoir.radianceWeight.rgb, 0.0f.xxx));
        if (lum > limit)
        {
            reservoir.radianceWeight.rgb *= limit / max(lum, 0.0001f);
        }
    }

    g_restirDiSpatial[pixelIndex] = reservoir;
}
