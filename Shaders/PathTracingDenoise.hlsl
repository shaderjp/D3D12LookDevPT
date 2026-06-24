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
    float4 stabilityOptions;
    float4 viewOptions;
    float4 materialFocusOptions;
};

struct RestirReservoir
{
    float4 radianceWeight;
    float4 meta;
};

VK_BINDING(1, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(2, 0) RWTexture2D<float4> g_accumulation : register(u1, space0);
VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(10, 0) RWStructuredBuffer<RestirReservoir> g_restirHistory : register(u3, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(16, 0) RWTexture2D<float4> g_reconstructionHistoryRadiance : register(u8, space0);
VK_BINDING(17, 0) RWTexture2D<float4> g_reconstructionHistoryMoments : register(u9, space0);
VK_BINDING(18, 0) RWTexture2D<float4> g_reconstructionHistoryLength : register(u10, space0);
VK_BINDING(19, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(20, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(21, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(25, 0) RWTexture2D<float4> g_signalCurrentRadiance : register(u17, space0);
VK_BINDING(26, 0) RWTexture2D<float4> g_signalDirect : register(u18, space0);
VK_BINDING(27, 0) RWTexture2D<float4> g_signalIndirect : register(u19, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(29, 0) RWTexture2D<float4> g_denoisePing : register(u21, space0);
VK_BINDING(30, 0) RWTexture2D<float4> g_denoisePong : register(u22, space0);

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 AcesTonemap(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 Tonemap(float3 color)
{
    color = max(color * exp2(g_scene.viewOptions.x), 0.0f.xxx);
    uint toneMapper = (uint)round(g_scene.viewOptions.z);
    if (toneMapper == 1u)
    {
        color = color / (1.0f.xxx + color);
    }
    else if (toneMapper == 2u)
    {
        color = AcesTonemap(color);
    }
    float gamma = max(g_scene.viewOptions.y, 0.01f);
    return pow(saturate(color), 1.0f / gamma);
}

float3 DecodeNormal(float4 aov)
{
    float3 encoded = aov.xyz * 2.0f - 1.0f;
    return dot(encoded, encoded) > 0.0001f ? normalize(encoded) : float3(0.0f, 1.0f, 0.0f);
}

float KernelWeight(int offset)
{
    int a = abs(offset);
    return a == 0 ? 6.0f : (a == 1 ? 4.0f : 1.0f);
}

uint PixelIndex(uint2 pixel, uint2 dimensions)
{
    return pixel.y * dimensions.x + pixel.x;
}

bool IsInside(int2 pixel, uint2 dimensions)
{
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < (int)dimensions.x && pixel.y < (int)dimensions.y;
}

bool ValidateAov(float4 currentAov0, float4 currentAov1, float4 currentAov2, float4 historyAov0, float4 historyAov1, float4 historyAov2)
{
    if (currentAov2.w <= 0.5f || historyAov2.w <= 0.5f || currentAov0.w <= 0.0f || historyAov0.w <= 0.0f)
    {
        return false;
    }

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(historyAov0));
    float depthDelta = abs(currentAov0.w - historyAov0.w) / max(currentAov0.w, 1.0f);
    float albedoDelta = length(currentAov1.rgb - historyAov1.rgb);
    float roughnessDelta = abs(currentAov1.w - historyAov1.w);
    return normalDot >= g_scene.validationOptions.x &&
        depthDelta <= g_scene.validationOptions.y &&
        albedoDelta <= g_scene.validationOptions.z &&
        roughnessDelta <= g_scene.validationOptions.w;
}

float AovMatchScore(float4 currentAov0, float4 currentAov1, float4 currentAov2, float4 historyAov0, float4 historyAov1, float4 historyAov2)
{
    float normalError = 1.0f - saturate(dot(DecodeNormal(currentAov0), DecodeNormal(historyAov0)));
    float depthError = abs(currentAov0.w - historyAov0.w) / max(currentAov0.w, 1.0f);
    float albedoError = length(currentAov1.rgb - historyAov1.rgb);
    float roughnessError = abs(currentAov1.w - historyAov1.w);
    return normalError * 2.0f + depthError * 8.0f + albedoError + roughnessError;
}

float3 CurrentSignal(uint2 pixel)
{
    float3 raw = max(g_signalCurrentRadiance[pixel].rgb, 0.0f.xxx);
    if (g_scene.signalDenoiseOptions.x < 0.5f)
    {
        return raw;
    }

    float3 direct = max(g_signalDirect[pixel].rgb, 0.0f.xxx);
    float3 indirect = max(g_signalIndirect[pixel].rgb, 0.0f.xxx);
    float3 residual = max(g_signalResidual[pixel].rgb, 0.0f.xxx);
    return max(direct + indirect + residual, 0.0f.xxx);
}

bool FindHistoryPixel(uint2 pixel, uint2 dimensions, float4 centerAov0, float4 centerAov1, float4 centerAov2, out int2 bestPixel, out float bestScore)
{
    float2 motion = centerAov2.xy;
    int2 basePixel = int2(round(float2(pixel) - motion * float2(dimensions)));
    bestPixel = basePixel;
    bestScore = 1e20f;

    if (g_scene.stabilityOptions.w <= 0.5f)
    {
        return false;
    }

    bool found = false;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 candidate = basePixel + int2(x, y);
            if (!IsInside(candidate, dimensions))
            {
                continue;
            }

            float4 historyAov0 = g_previousDenoiseAov0[uint2(candidate)];
            float4 historyAov1 = g_previousDenoiseAov1[uint2(candidate)];
            float4 historyAov2 = g_previousDenoiseAov2[uint2(candidate)];
            if (!ValidateAov(centerAov0, centerAov1, centerAov2, historyAov0, historyAov1, historyAov2))
            {
                continue;
            }

            float score = AovMatchScore(centerAov0, centerAov1, centerAov2, historyAov0, historyAov1, historyAov2);
            if (score < bestScore)
            {
                bestScore = score;
                bestPixel = candidate;
                found = true;
            }
        }
    }

    return found;
}

void CurrentNeighborhoodBounds(uint2 pixel, uint2 dimensions, out float3 lowValue, out float3 highValue)
{
    float3 center = CurrentSignal(pixel);
    lowValue = center;
    highValue = center;
    int2 centerPixel = int2(pixel);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 samplePixel = clamp(centerPixel + int2(x, y), int2(0, 0), int2((int)dimensions.x, (int)dimensions.y) - int2(1, 1));
            float3 sampleColor = CurrentSignal(uint2(samplePixel));
            lowValue = min(lowValue, sampleColor);
            highValue = max(highValue, sampleColor);
        }
    }
}

float3 ClampHistoryToNeighborhood(uint2 pixel, uint2 dimensions, float3 history)
{
    if (g_scene.stabilityOptions.x < 0.5f)
    {
        return history;
    }

    float3 lowValue;
    float3 highValue;
    CurrentNeighborhoodBounds(pixel, dimensions, lowValue, highValue);
    float3 extent = max(highValue - lowValue, 0.03f.xxx);
    return clamp(history, lowValue - extent * 0.35f, highValue + extent * 0.35f);
}

float ComputeHistoryConfidence(bool validHistory, float historyScore, float4 currentAov0, float4 currentAov1, float4 currentAov2,
    float4 historyAov0, float4 historyAov1, float4 historyAov2, float currentLum, float previousLum)
{
    if (!validHistory)
    {
        return 0.0f;
    }

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(historyAov0));
    float normalConfidence = saturate((normalDot - g_scene.validationOptions.x) / max(1.0f - g_scene.validationOptions.x, 0.001f));
    float depthDelta = abs(currentAov0.w - historyAov0.w) / max(currentAov0.w, 1.0f);
    float depthConfidence = saturate(1.0f - depthDelta / max(g_scene.validationOptions.y, 0.001f));
    float albedoDelta = length(currentAov1.rgb - historyAov1.rgb);
    float albedoConfidence = saturate(1.0f - albedoDelta / max(g_scene.validationOptions.z, 0.001f));
    float roughnessDelta = abs(currentAov1.w - historyAov1.w);
    float roughnessConfidence = saturate(1.0f - roughnessDelta / max(g_scene.validationOptions.w, 0.001f));
    float motionConfidence = saturate(1.0f - length(currentAov2.xy) * 32.0f);
    float luminanceDelta = abs(currentLum - previousLum) / max(max(currentLum, previousLum), 1.0f);
    float luminanceConfidence = saturate(1.0f - luminanceDelta / max(g_scene.signalDenoiseOptions.z, 0.05f));
    float scoreConfidence = saturate(1.0f - historyScore * 0.25f);
    return normalConfidence * depthConfidence * albedoConfidence * roughnessConfidence * motionConfidence * luminanceConfidence * scoreConfidence;
}

float3 ApplyHistoryClamp(float3 current, float3 history, float variance, float historyLength)
{
    if (historyLength <= 0.0f)
    {
        return current;
    }

    float sigma = sqrt(max(variance, 0.0f));
    float clampSigma = max(g_scene.signalDenoiseOptions.y, 0.01f);
    float3 radius = max((sigma * clampSigma).xxx, max(abs(history) * 0.12f, 0.05f.xxx));
    return clamp(current, history - radius, history + radius);
}

float3 LoadAtrousInput(uint2 pixel, uint passIndex)
{
    return (passIndex & 1u) == 0u ? g_denoisePing[pixel].rgb : g_denoisePong[pixel].rgb;
}

void StoreAtrousOutput(uint2 pixel, uint passIndex, float3 value)
{
    if ((passIndex & 1u) == 0u)
    {
        g_denoisePong[pixel] = float4(value, 1.0f);
    }
    else
    {
        g_denoisePing[pixel] = float4(value, 1.0f);
    }
}

float3 LoadFinalAtrous(uint2 pixel)
{
    uint passCount = min((uint)round(g_scene.atrousOptions.x), 5u);
    if (passCount == 0u)
    {
        return g_denoisePing[pixel].rgb;
    }
    return (passCount & 1u) == 0u ? g_denoisePing[pixel].rgb : g_denoisePong[pixel].rgb;
}

[numthreads(8, 8, 1)]
void DenoiseTemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    float3 currentColor = CurrentSignal(pixel);
    float currentLum = Luminance(currentColor);
    float4 centerAov0 = g_denoiseAov0[pixel];
    float4 centerAov1 = g_denoiseAov1[pixel];
    float4 centerAov2 = g_denoiseAov2[pixel];

    int2 historyPixel;
    float historyScore;
    bool validHistory = FindHistoryPixel(pixel, dimensions, centerAov0, centerAov1, centerAov2, historyPixel, historyScore);
    float4 previousHistoryLength = validHistory ? g_reconstructionHistoryLength[uint2(historyPixel)] : 0.0f.xxxx;
    float historyLength = validHistory ? previousHistoryLength.x : 0.0f;
    float historyMatch = validHistory ? saturate(1.0f - historyScore) : 0.0f;
    float disoccluded = validHistory ? 0.0f : 1.0f;
    float3 reprojectedHistory = validHistory ? max(g_reconstructionHistoryRadiance[uint2(historyPixel)].rgb, 0.0f.xxx) : currentColor;

    float4 previousMoments = validHistory ? g_reconstructionHistoryMoments[uint2(historyPixel)] : 0.0f.xxxx;
    float previousVariance = validHistory ? max(previousMoments.z, 0.0f) : currentLum * currentLum;
    float previousLum = Luminance(reprojectedHistory);
    float4 historyAov0 = validHistory ? g_previousDenoiseAov0[uint2(historyPixel)] : centerAov0;
    float4 historyAov1 = validHistory ? g_previousDenoiseAov1[uint2(historyPixel)] : centerAov1;
    float4 historyAov2 = validHistory ? g_previousDenoiseAov2[uint2(historyPixel)] : centerAov2;
    float historyConfidence = ComputeHistoryConfidence(validHistory, historyScore, centerAov0, centerAov1, centerAov2, historyAov0, historyAov1, historyAov2, currentLum, previousLum);
    float reactive = (disoccluded > 0.5f || historyConfidence < 0.25f || abs(currentLum - previousLum) > max(previousLum, 1.0f) * g_scene.signalDenoiseOptions.z) ? 1.0f : 0.0f;

    float maxHistoryFrames = max(g_scene.reconstructionOptions.y, 1.0f);
    float historyFactor = saturate(historyLength / maxHistoryFrames);
    float alpha = lerp(g_scene.reconstructionOptions.w, g_scene.reconstructionOptions.z, historyFactor);
    alpha = lerp(alpha, max(alpha, 0.65f), reactive);
    float roughness = saturate(centerAov1.w);
    alpha = lerp(max(alpha, g_scene.signalDenoiseOptions.w), alpha, roughness);
    alpha = lerp(max(alpha, 0.85f), alpha, historyConfidence);
    alpha = max(alpha, saturate(g_scene.stabilityOptions.y * 0.55f));

    if (g_scene.reconstructionOptions.x < 0.5f)
    {
        alpha = 1.0f;
        historyLength = 0.0f;
        disoccluded = 1.0f;
        reactive = 1.0f;
    }

    float3 clampedHistory = ClampHistoryToNeighborhood(pixel, dimensions, reprojectedHistory);
    float3 clampedCurrent = ApplyHistoryClamp(currentColor, clampedHistory, previousVariance, historyLength);
    float3 temporal = lerp(clampedHistory, clampedCurrent, saturate(alpha));

    float previousMean = validHistory ? previousMoments.x : currentLum;
    float previousMean2 = validHistory ? previousMoments.y : currentLum * currentLum;
    float momentAlpha = historyLength > 0.0f ? saturate(alpha) : 1.0f;
    float mean = lerp(previousMean, currentLum, momentAlpha);
    float mean2 = lerp(previousMean2, currentLum * currentLum, momentAlpha);
    float variance = max(mean2 - mean * mean, 0.0f);
    historyLength = min(historyLength + 1.0f, maxHistoryFrames);

    g_denoisePing[pixel] = float4(temporal, 1.0f);
    g_reconstructionHistoryRadiance[pixel] = float4(temporal, 1.0f);
    g_reconstructionHistoryMoments[pixel] = float4(mean, mean2, variance, historyConfidence);
    g_reconstructionHistoryLength[pixel] = float4(historyLength, disoccluded, historyMatch, reactive);
}

void RunAtrousPass(uint3 dispatchThreadId, uint passIndex)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y || passIndex >= (uint)round(g_scene.atrousOptions.x))
    {
        return;
    }

    float4 centerAov0 = g_denoiseAov0[pixel];
    float4 centerAov1 = g_denoiseAov1[pixel];
    float4 centerAov2 = g_denoiseAov2[pixel];
    float centerDepth = centerAov0.w;
    float3 temporalColor = LoadAtrousInput(pixel, passIndex);
    if (centerDepth <= 0.0f || centerAov2.w <= 0.5f)
    {
        StoreAtrousOutput(pixel, passIndex, temporalColor);
        return;
    }

    float3 centerNormal = DecodeNormal(centerAov0);
    float3 centerAlbedo = max(centerAov1.rgb, 0.03f.xxx);
    float centerRoughness = saturate(centerAov1.w);
    float variance = max(g_reconstructionHistoryMoments[pixel].z, 0.0f);
    float reactive = g_reconstructionHistoryLength[pixel].w;
    float normalSigma = max(g_scene.denoiseOptions.z, 0.001f);
    float depthSigma = max(g_scene.denoiseOptions.w, 0.0005f);
    float luminanceSigma = max(g_scene.denoiseOptions2.x + sqrt(variance) * g_scene.atrousOptions.w, 0.001f);
    float albedoSigma = max(g_scene.denoiseOptions2.y, 0.001f);
    float diffuseStrength = saturate(g_scene.atrousOptions.y);
    float specularStrength = saturate(g_scene.atrousOptions.z * lerp(g_scene.signalDenoiseOptions.w, 1.0f, centerRoughness));
    float materialStrength = lerp(specularStrength, diffuseStrength, centerRoughness);
    float strength = saturate(g_scene.denoiseOptions2.z * materialStrength * (1.0f - reactive * 0.35f));
    int stepWidth = (int)(1u << passIndex);
    float centerLum = Luminance(temporalColor);
    bool demodulate = g_scene.signalDenoiseOptions.x > 0.5f && centerRoughness > 0.45f;
    float3 centerValue = demodulate ? temporalColor / centerAlbedo : temporalColor;

    float3 sumColor = centerValue;
    float sumWeight = 1.0f;
    int2 centerPixel = int2(pixel);

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            int2 samplePixel = clamp(centerPixel + int2(x, y) * stepWidth, int2(0, 0), int2((int)dimensions.x, (int)dimensions.y) - int2(1, 1));
            float4 sampleAov0 = g_denoiseAov0[uint2(samplePixel)];
            float4 sampleAov1 = g_denoiseAov1[uint2(samplePixel)];
            float4 sampleAov2 = g_denoiseAov2[uint2(samplePixel)];
            if (!ValidateAov(centerAov0, centerAov1, centerAov2, sampleAov0, sampleAov1, sampleAov2))
            {
                continue;
            }

            float3 sampleColor = LoadAtrousInput(uint2(samplePixel), passIndex);
            float3 sampleAlbedo = max(sampleAov1.rgb, 0.03f.xxx);
            float3 sampleValue = demodulate ? sampleColor / sampleAlbedo : sampleColor;
            float3 sampleNormal = DecodeNormal(sampleAov0);
            float normalWeight = exp((dot(centerNormal, sampleNormal) - 1.0f) / normalSigma);
            float depthWeight = exp(-abs(sampleAov0.w - centerDepth) / max(centerDepth * depthSigma, 0.02f));
            float luminanceWeight = exp(-abs(Luminance(sampleColor) - centerLum) / luminanceSigma);
            float albedoWeight = exp(-length(sampleAlbedo - centerAlbedo) / albedoSigma);
            float spatialWeight = KernelWeight(x) * KernelWeight(y) / 256.0f;
            float weight = spatialWeight * normalWeight * depthWeight * luminanceWeight * albedoWeight;
            sumColor += sampleValue * weight;
            sumWeight += weight;
        }
    }

    float3 filtered = sumColor / max(sumWeight, 0.0001f);
    if (demodulate)
    {
        filtered *= centerAlbedo;
    }
    StoreAtrousOutput(pixel, passIndex, lerp(temporalColor, filtered, strength));
}

[numthreads(8, 8, 1)]
void DenoiseAtrous0CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 0u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous1CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 1u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous2CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 2u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous3CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 3u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous4CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 4u);
}

[numthreads(8, 8, 1)]
void DenoiseCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    uint debugMode = (uint)round(g_scene.debugOptions.x);
    float3 currentColor = CurrentSignal(pixel);
    float3 temporalColor = g_reconstructionHistoryRadiance[pixel].rgb;
    float3 filtered = LoadFinalAtrous(pixel);
    float3 directSignal = max(g_signalDirect[pixel].rgb, 0.0f.xxx);
    float3 indirectSignal = max(g_signalIndirect[pixel].rgb, 0.0f.xxx);
    float3 residualSignal = max(g_signalResidual[pixel].rgb, 0.0f.xxx);
    float4 centerAov2 = g_denoiseAov2[pixel];
    float4 historyLength = g_reconstructionHistoryLength[pixel];
    float variance = max(g_reconstructionHistoryMoments[pixel].z, 0.0f);
    uint pixelIndex = PixelIndex(pixel, dimensions);
    RestirReservoir reservoir = g_restirHistory[pixelIndex];
    float maxHistory = max(g_scene.reconstructionOptions.y, 1.0f);
    float maxReservoirAge = max(g_scene.restirStabilityOptions.w, 1.0f);

    if (debugMode == 16u) filtered = currentColor;
    if (debugMode == 17u) filtered = temporalColor;
    if (debugMode == 18u) filtered = saturate(historyLength.x / maxHistory).xxx;
    if (debugMode == 19u) filtered = saturate(variance / max(variance + 1.0f, 0.0001f)).xxx;
    if (debugMode == 20u) filtered = float3(0.5f + centerAov2.x * 20.0f, 0.5f + centerAov2.y * 20.0f, 0.5f);
    if (debugMode == 21u) filtered = historyLength.y.xxx;
    if (debugMode == 22u) filtered = indirectSignal;
    if (debugMode == 23u) filtered = abs(temporalColor - filtered) * 4.0f;
    if (debugMode == 24u) filtered = saturate(reservoir.meta.x / maxReservoirAge).xxx;
    if (debugMode == 25u) filtered = reservoir.meta.w.xxx;
    if (debugMode == 32u) filtered = directSignal;
    if (debugMode == 33u) filtered = indirectSignal;
    if (debugMode == 34u) filtered = residualSignal;
    if (debugMode == 35u) filtered = currentColor;
    if (debugMode == 36u) filtered = temporalColor;
    if (debugMode == 37u) filtered = LoadFinalAtrous(pixel);
    if (debugMode == 38u) filtered = historyLength.w.xxx;
    if (debugMode == 39u) filtered = historyLength.z.xxx;
    if (debugMode == 40u) filtered = g_reconstructionHistoryMoments[pixel].w.xxx;

    g_previousDenoiseAov0[pixel] = g_denoiseAov0[pixel];
    g_previousDenoiseAov1[pixel] = g_denoiseAov1[pixel];
    g_previousDenoiseAov2[pixel] = g_denoiseAov2[pixel];
    g_output[pixel] = float4(Tonemap(filtered), 1.0f);
}
