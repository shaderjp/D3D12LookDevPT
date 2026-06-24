#pragma once

#include "DXSample.h"
#include "DlssBackend.h"
#include "FpsCamera.h"
#include "McpDispatcher.h"
#include "McpServer.h"
#include "PathTracingScene.h"
#include "SceneImporter.h"
#include "SimpleJson.h"

#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

enum class PathTracingMode
{
    Pathtracing,
    ReSTIR,
    ReSTIRDI,
    ReSTIRCombined
};

class D3D12PathTracingBackend : public DXSample, public mcp::IServerHost
{
public:
    D3D12PathTracingBackend(UINT width, UINT height, std::wstring name, PathTracingMode mode);

    void OnInit() override;
    void OnUpdate() override;
    void OnRender() override;
    void OnDestroy() override;
    void OnKeyDown(UINT8 key) override;
    void OnWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void ParseCommandLineArgs(_In_reads_(argc) WCHAR* argv[], int argc) override;

    enum class NoisePreset
    {
        InteractiveStable,
        SharpPreview,
        StillCapture,
    };

    enum class JitterMode
    {
        Stable16,
        Halton,
        Off,
    };

    enum class DenoiseBackend
    {
        Internal,
        DlssRayReconstruction,
        Off,
    };

    enum class ToneMapper
    {
        None,
        Reinhard,
        Aces,
    };

    enum class MaterialFocusMode
    {
        Normal,
        Isolate,
        Dim,
    };

private:
    static const UINT FrameCount = 2;
    static const UINT TextureSlotCount = Bistro::TextureSlotCount;
    static const UINT DenoiseAtrousPipelineCount = 5;

    enum DescriptorSlot : UINT
    {
        DescriptorOutputUav = 0,
        DescriptorAccumulationUav,
        DescriptorRestirCurrentUav,
        DescriptorRestirHistoryUav,
        DescriptorRestirSpatialUav,
        DescriptorDenoiseAov0Uav,
        DescriptorDenoiseAov1Uav,
        DescriptorDenoiseAov2Uav,
        DescriptorReconstructionHistoryRadianceUav,
        DescriptorReconstructionHistoryMomentsUav,
        DescriptorReconstructionHistoryLengthUav,
        DescriptorPreviousDenoiseAov0Uav,
        DescriptorPreviousDenoiseAov1Uav,
        DescriptorPreviousDenoiseAov2Uav,
        DescriptorRestirDiCurrentUav,
        DescriptorRestirDiHistoryUav,
        DescriptorRestirDiSpatialUav,
        DescriptorSignalCurrentRadianceUav,
        DescriptorSignalDirectUav,
        DescriptorSignalIndirectUav,
        DescriptorSignalResidualUav,
        DescriptorDenoisePingUav,
        DescriptorDenoisePongUav,
        DescriptorVertexBuffer,
        DescriptorIndexBuffer,
        DescriptorGeometryBuffer,
        DescriptorMaterialBuffer,
        DescriptorLightBuffer,
        DescriptorTextureBase
    };

    enum RootParameter : UINT
    {
        RootOutputTable = 0,
        RootAccelerationStructure,
        RootSceneConstants,
        RootSceneBuffers,
        RootTextureTable,
        RootParameterCount
    };

    enum class PendingFileDialog
    {
        None,
        OpenScene,
        OpenEnvironment,
        OpenProject,
        SaveProjectAs,
    };

    struct SceneConstantBuffer
    {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4X4 viewProjection;
        XMFLOAT4X4 previousViewProjection;
        XMFLOAT4 cameraPosition;
        XMFLOAT4 lightDirection;
        XMFLOAT4 lightColor;
        XMFLOAT4 debugOptions;
        XMFLOAT4 skyColor;
        XMFLOAT4 skyHorizonColor;
        XMFLOAT4 skyZenithColor;
        XMFLOAT4 skyGroundColor;
        XMFLOAT4 skyOptions;
        XMFLOAT4 rayOptions;
        XMFLOAT4 frameOptions;
        XMFLOAT4 giOptions;
        XMFLOAT4 pathOptions;
        XMFLOAT4 restirOptions;
        XMFLOAT4 restirDiOptions;
        XMFLOAT4 lightOptions;
        XMFLOAT4 environmentOptions;
        XMFLOAT4 denoiseOptions;
        XMFLOAT4 denoiseOptions2;
        XMFLOAT4 jitterOptions;
        XMFLOAT4 reconstructionOptions;
        XMFLOAT4 validationOptions;
        XMFLOAT4 atrousOptions;
        XMFLOAT4 adaptiveOptions;
        XMFLOAT4 restirStabilityOptions;
        XMFLOAT4 signalDenoiseOptions;
        XMFLOAT4 denoisePassOptions;
        XMFLOAT4 stabilityOptions;
        XMFLOAT4 viewOptions;
        XMFLOAT4 materialFocusOptions;
    };

    struct GpuTexture
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12Resource> upload;
        std::wstring path;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t mipLevels = 1;
        bool fallback = false;
    };

    struct AccelerationStructureBuffers
    {
        ComPtr<ID3D12Resource> scratch;
        ComPtr<ID3D12Resource> result;
        ComPtr<ID3D12Resource> instanceDesc;
    };

    struct ShaderTableInfo
    {
        ComPtr<ID3D12Resource> resource;
        UINT recordSize = 0;
        UINT recordCount = 0;
    };

    struct MaterialSnapshot
    {
        XMFLOAT4 baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        XMFLOAT4 emissiveFactor = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        float roughnessFactor = 0.48f;
        float metallicFactor = 0.0f;
        float occlusionStrength = 1.0f;
        float normalStrength = 1.0f;
        float alphaCutoff = 0.33f;
        bool alphaMasked = false;
        bool packedOcclusionRoughnessMetallic = false;
        std::array<std::wstring, TextureSlotCount> textures;
        std::array<bool, TextureSlotCount> textureOverrideEnabled = {};
    };

    struct MaterialVariant
    {
        std::string name;
        int materialIndex = 0;
        std::wstring materialName;
        MaterialSnapshot snapshot;
    };

    struct MaterialPreset
    {
        std::string name;
        std::string category;
        std::wstring sourcePath;
        MaterialSnapshot snapshot;
    };

    struct MaterialUsage
    {
        uint32_t meshCount = 0;
        uint64_t triangleCount = 0;
    };

    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Device5> m_device;
    ComPtr<ID3D12GraphicsCommandList4> m_commandList;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    ComPtr<ID3D12DescriptorHeap> m_imguiDescriptorHeap;
    ComPtr<ID3D12RootSignature> m_globalRootSignature;
    ComPtr<ID3D12StateObject> m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;
    ComPtr<ID3D12PipelineState> m_restirReusePipeline;
    ComPtr<ID3D12PipelineState> m_restirDiReusePipeline;
    ComPtr<ID3D12PipelineState> m_denoiseTemporalPipeline;
    std::array<ComPtr<ID3D12PipelineState>, DenoiseAtrousPipelineCount> m_denoiseAtrousPipelines;
    ComPtr<ID3D12PipelineState> m_denoiseCompositePipeline;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_PathtracingOutput;
    ComPtr<ID3D12Resource> m_accumulationOutput;
    ComPtr<ID3D12Resource> m_denoiseAov0;
    ComPtr<ID3D12Resource> m_denoiseAov1;
    ComPtr<ID3D12Resource> m_denoiseAov2;
    ComPtr<ID3D12Resource> m_reconstructionHistoryRadiance;
    ComPtr<ID3D12Resource> m_reconstructionHistoryMoments;
    ComPtr<ID3D12Resource> m_reconstructionHistoryLength;
    ComPtr<ID3D12Resource> m_previousDenoiseAov0;
    ComPtr<ID3D12Resource> m_previousDenoiseAov1;
    ComPtr<ID3D12Resource> m_previousDenoiseAov2;
    ComPtr<ID3D12Resource> m_restirReservoirCurrent;
    ComPtr<ID3D12Resource> m_restirReservoirHistory;
    ComPtr<ID3D12Resource> m_restirReservoirSpatial;
    ComPtr<ID3D12Resource> m_restirDiReservoirCurrent;
    ComPtr<ID3D12Resource> m_restirDiReservoirHistory;
    ComPtr<ID3D12Resource> m_restirDiReservoirSpatial;
    ComPtr<ID3D12Resource> m_signalCurrentRadiance;
    ComPtr<ID3D12Resource> m_signalDirect;
    ComPtr<ID3D12Resource> m_signalIndirect;
    ComPtr<ID3D12Resource> m_signalResidual;
    ComPtr<ID3D12Resource> m_denoisePing;
    ComPtr<ID3D12Resource> m_denoisePong;
    ComPtr<ID3D12Resource> m_sceneConstantBuffer;
    UINT8* m_mappedSceneConstants = nullptr;
    UINT m_frameIndex = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_descriptorSize = 0;
    UINT m_imguiDescriptorSize = 0;
    UINT m_imguiDescriptorCursor = 0;
    UINT m_descriptorCount = 0;
    UINT m_restirReservoirElementCount = 1;
    UINT64 m_restirReservoirBufferSize = 0;

    PathTracingMode m_mode = PathTracingMode::Pathtracing;
    Bistro::Scene m_scene;
    std::vector<Bistro::RtGeometryRecord> m_geometryRecords;
    std::vector<Bistro::RtMaterial> m_rtMaterials;
    std::vector<GpuTexture> m_textures;
    std::vector<std::array<UINT, Bistro::TextureSlotCount>> m_materialTextureIndices;
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    ComPtr<ID3D12Resource> m_geometryBuffer;
    ComPtr<ID3D12Resource> m_materialBuffer;
    ComPtr<ID3D12Resource> m_lightBuffer;
    std::vector<ComPtr<ID3D12Resource>> m_uploadBuffers;
    AccelerationStructureBuffers m_bottomLevelAs;
    AccelerationStructureBuffers m_topLevelAs;
    ShaderTableInfo m_rayGenTable;
    ShaderTableInfo m_missTable;
    ShaderTableInfo m_hitGroupTable;

    Bistro::FpsCamera m_camera;
    XMFLOAT3 m_defaultCameraPosition = XMFLOAT3(-16.32f, 4.66f, -10.41f);
    float m_defaultCameraYaw = DirectX::XMConvertToRadians(18.1f);
    float m_defaultCameraPitch = DirectX::XMConvertToRadians(2.8f);
    std::chrono::steady_clock::time_point m_lastUpdate;
    float m_lightDirection[3] = { -0.35f, -0.8f, 0.45f };
    float m_lightColor[3] = { 1.0f, 0.96f, 0.88f };
    float m_lightIntensity = 4.0f;
    float m_skyColor[3] = { 0.015f, 0.08f, 0.16f };
    float m_skyHorizonColor[3] = { 0.42f, 0.63f, 0.86f };
    float m_skyZenithColor[3] = { 0.05f, 0.20f, 0.52f };
    float m_skyGroundColor[3] = { 0.025f, 0.035f, 0.045f };
    float m_skyIntensity = 1.0f;
    float m_sunIntensity = 8.0f;
    float m_sunAngularRadius = 0.012f;
    float m_skyGroundBlend = 0.35f;
    float m_emissiveLightIntensity = 4.0f;
    float m_proceduralLightIntensity = 12.0f;
    float m_environmentIntensity = 1.0f;
    float m_environmentRotation = 0.0f;
    float m_rayTMin = 0.03f;
    float m_rayTMax = 10000.0f;
    float m_giStrength = 0.6f;
    int m_giSamplesPerFrame = 2;
    float m_giRadianceClamp = 8.0f;
    float m_giTemporalClampScale = 1.5f;
    float m_giTemporalClampMin = 0.25f;
    int m_maxPathBounces = 4;
    int m_minPathBounces = 2;
    int m_maxAccumulatedFrames = 256;
    uint32_t m_accumulatedFrames = 0;
    uint32_t m_frameCounter = 0;
    bool m_freezeAccumulation = false;
    bool m_resetAccumulationRequested = true;
    float m_baseMoveSpeed = 17.0f;
    float m_fastMoveSpeed = 58.2f;
    int m_debugViewMode = 0;
    bool m_debugNormalMapYFlip = true;
    bool m_shadowEnabled = true;
    bool m_skyNeeEnabled = true;
    bool m_emissiveLightsEnabled = true;
    bool m_proceduralLightsEnabled = true;
    bool m_environmentMapEnabled = false;
    bool m_restirTemporalReuse = true;
    int m_restirSpatialReusePasses = 2;
    int m_restirSpatialRadius = 16;
    int m_restirCandidateSamples = 1;
    float m_restirMClamp = 20.0f;
    bool m_restirDiTemporalReuse = true;
    int m_restirDiSpatialReusePasses = 2;
    int m_restirDiCandidateSamples = 1;
    float m_restirDiMClamp = 20.0f;
    bool m_denoiserEnabled = true;
    DenoiseBackend m_denoiseBackend = DenoiseBackend::Internal;
    DlssMode m_dlssMode = DlssMode::Quality;
    bool m_dlssEnabledWhenAvailable = false;
    int m_denoiserSpatialIterations = 2;
    float m_denoiserNormalSigma = 0.25f;
    float m_denoiserDepthSigma = 0.02f;
    float m_denoiserLuminanceSigma = 1.5f;
    float m_denoiserAlbedoSigma = 0.35f;
    float m_denoiserStrength = 0.85f;
    bool m_splitSignalDenoise = true;
    NoisePreset m_noisePreset = NoisePreset::InteractiveStable;
    bool m_temporalStabilityEnabled = true;
    float m_historyClampSigma = 1.5f;
    float m_reactiveThreshold = 0.35f;
    float m_specularHistoryScale = 0.45f;
    bool m_realtimeReconstruction = true;
    bool m_cameraJitter = true;
    JitterMode m_jitterMode = JitterMode::Stable16;
    float m_movingJitterScale = 0.25f;
    int m_reconstructionMaxHistoryFrames = 32;
    float m_temporalAlphaMin = 0.04f;
    float m_temporalAlphaMax = 0.22f;
    float m_validationNormalDotThreshold = 0.82f;
    float m_validationDepthRelativeThreshold = 0.035f;
    float m_validationAlbedoThreshold = 0.55f;
    float m_validationRoughnessThreshold = 0.35f;
    int m_atrousPassCount = 3;
    float m_atrousDiffuseStrength = 0.85f;
    float m_atrousSpecularStrength = 0.35f;
    float m_atrousVarianceScale = 1.25f;
    bool m_adaptiveSamplingEnabled = true;
    int m_maxAdaptiveSamplesPerPixel = 2;
    float m_adaptiveVarianceThreshold = 0.18f;
    float m_adaptiveDisocclusionBoost = 1.0f;
    bool m_reservoirReprojection = true;
    bool m_reservoirValidation = true;
    bool m_restirGiValidationRay = false;
    int m_reservoirMaxAge = 8;
    bool m_skyEnabled = true;
    bool m_vsyncEnabled = false;
    bool m_tearingSupported = false;
    D3D12_RAYTRACING_TIER m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    XMFLOAT4X4 m_previousViewProjection =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    bool m_hasPreviousViewProjection = false;
    XMFLOAT2 m_currentJitter = XMFLOAT2(0.0f, 0.0f);
    XMFLOAT2 m_previousJitter = XMFLOAT2(0.0f, 0.0f);
    float m_currentJitterStrength = 1.0f;
    float m_cameraMotionAmount = 0.0f;
    uint32_t m_framesSinceCameraMotion = 0;
    bool m_cameraMotionTrackingInitialized = false;
    bool m_denoiseHistoryValid = false;
    bool m_resetDenoiseHistoryRequested = true;
    XMFLOAT4 m_previousCameraMotionState = XMFLOAT4(0, 0, 0, 0);
    float m_previousCameraMotionPitch = 0.0f;
    XMFLOAT4 m_lastCameraAndYaw = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastLighting = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastGiOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastPathOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastRestirOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastRestirDiOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastLightSystemOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastSignalDenoiseOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastViewOptions = XMFLOAT4(0, 0, 0, 0);
    std::wstring m_environmentTexturePath;
    UINT m_environmentDescriptorIndex = 0;
    uint32_t m_activeLightCount = 0;
    uint32_t m_emissiveTriangleLightCount = 0;
    uint32_t m_proceduralAreaLightCount = 0;
    std::vector<Bistro::RtLight> m_lights;
    std::vector<Bistro::Material> m_sourceMaterials;
    std::vector<std::array<bool, TextureSlotCount>> m_textureOverrideEnabled;
    std::vector<MaterialUsage> m_materialUsage;
    std::vector<MaterialVariant> m_materialVariants;
    std::vector<MaterialPreset> m_materialPresets;
    MaterialSnapshot m_materialCompareA;
    MaterialSnapshot m_materialCompareB;
    bool m_hasMaterialCompareA = false;
    bool m_hasMaterialCompareB = false;
    int m_selectedMaterial = 0;
    char m_materialSearch[128] = {};
    char m_materialVariantName[128] = "Variant";
    char m_materialPresetName[128] = "Preset";
    ToneMapper m_toneMapper = ToneMapper::Aces;
    float m_exposure = 0.0f;
    float m_gamma = 2.2f;
    MaterialFocusMode m_materialFocusMode = MaterialFocusMode::Normal;

    HANDLE m_fenceEvent = nullptr;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;

    void LoadPipeline();
    void LoadAssets();
    void CreateGpuResourcesForCurrentScene();
    bool LoadScenePath(const std::wstring& path, std::string& diagnostics);
    bool LoadEnvironmentPath(const std::wstring& path, std::string& diagnostics);
    bool SaveProjectToDisk(const std::wstring& path);
    bool LoadProjectFromDisk(const std::wstring& path, std::string& diagnostics);
    bool ApplyAction(const std::string& method, const cld::JsonValue& params, std::string& diagnostics, bool validateOnly);
    mcp::ToolResult CallMcpTool(const std::string& name, const cld::JsonValue& arguments, int timeoutMs) override;
    mcp::ResourceResult ReadMcpResource(const std::string& uri) override;
    size_t PendingMcpCommandCount() const override;
    void LoadMcpUserSettings();
    void SaveMcpUserSettings();
    void StartMcpServer();
    void StopMcpServer();
    void ProcessMcpCommands();
    void UpdateMcpSnapshots();
    void BuildMcpServerUI();
    std::string BuildMcpStateJson() const;
    std::string BuildMcpStatsJson() const;
    std::string BuildMcpMaterialsJson() const;
    mcp::ToolResult SubmitMcpActionTool(const std::string& toolName, const std::string& actionMethod, const cld::JsonValue& params, bool validateOnly, int timeoutMs);
    mcp::ToolResult MakeMcpJsonToolResult(bool ok, const std::string& text, const std::string& structuredJson) const;
    bool CaptureViewportPng(std::string& base64Png, std::string& diagnostics);
    bool RenderPathTracingOutputForCapture(std::string& diagnostics);
    uint64_t StoreMcpCapture(std::string base64Png, int debugView, const std::string& label);
    bool FindMcpCapture(uint64_t id, std::string& base64Png, std::string& label) const;
    std::string BuildMcpDiagnosticsJson() const;
    std::string BuildMcpProjectJson() const;
    std::string BuildMcpSceneSummaryJson() const;
    std::string BuildMcpCaptureIndexJson() const;
    mcp::ToolResult SubmitMcpCommandTool(const std::string& toolName, const std::string& actionMethod, const cld::JsonValue& params, bool mutation, int timeoutMs);
    mcp::CommandResult ExecuteMcpCommand(const mcp::CommandRequest& request);
    void CreateDescriptorHeap();
    void CreateOutputResources();
    void CreateGlobalRootSignature();
    void CreatePathtracingStateObject();
    void CreateSceneBuffers();
    void CreateTextures();
    void BuildAccelerationStructures();
    void CreateShaderTables();
    void CreateRestirReusePipeline();
    void CreateDenoisePipeline();
    void PopulateCommandList();
    void DispatchRays();
    void RunRestirReusePass();
    void RunDenoisePass();
    void CopyOutputToBackBuffer();
    void WaitForPreviousFrame();
    void UpdateConstantBuffer(float deltaSeconds);
    void InitializeImGui();
    void ShutdownImGui();
    void BuildUI();
    void BuildRendererStatsUI();
    void ResetLight();
    void ResetCameraView();
    void ResetCameraSpeeds();
    void ResetAccumulation();
    void ResetDenoiseHistory();
    void ResetRenderingHistory();
    void ApplyNoisePreset(NoisePreset preset);
    bool ShouldRunInternalDenoiser() const;
    bool IsDlssSelected() const;
    std::string BuildDlssStatusJson() const;
    void InitializeMaterialLookDevState(bool clearVariants);
    void RebuildMaterialUsage();
    int ResolveMaterialIndex(const cld::JsonValue& params) const;
    MaterialSnapshot CaptureMaterialSnapshot(int materialIndex) const;
    void ApplyMaterialSnapshot(int materialIndex, const MaterialSnapshot& snapshot, bool useSnapshotTextureFlags);
    void ResetMaterialToSource(int materialIndex);
    bool ValidateMaterialTexturePath(const std::wstring& path, std::string& diagnostics) const;
    bool TryParseTextureSlot(const cld::JsonValue& value, UINT& slot) const;
    bool ApplyMaterialTextureOverride(int materialIndex, UINT slot, const std::wstring& path, bool enableOverride, std::string& diagnostics);
    void LoadMaterialPresets();
    bool SaveUserMaterialPreset(const std::string& name, int materialIndex, std::string& diagnostics);
    bool ApplyMaterialPreset(int materialIndex, size_t presetIndex, std::string& diagnostics);
    std::string BuildMaterialTexturesJson(size_t materialIndex) const;
    std::string BuildMaterialVariantsJson() const;
    std::string BuildMaterialPresetsJson() const;
    void UpdateCameraMotionState();
    bool HasAccumulationStateChanged();
    UINT CreateTextureResource(const std::wstring& path, bool srgb, const uint8_t fallback[4], std::map<std::wstring, UINT>& cache);
    ComPtr<ID3D12Resource> CreateDefaultBuffer(const void* data, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES finalState, const wchar_t* name);
    ComPtr<ID3D12Resource> CreateUploadBuffer(const void* data, UINT64 size, const wchar_t* name);
    ComPtr<ID3D12Resource> CreateUavBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState, const wchar_t* name);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptor(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor(UINT index) const;
    std::wstring ShaderFileName() const;
    std::wstring RestirReuseShaderFileName() const;
    std::wstring RestirDiReuseShaderFileName() const;
    std::wstring DenoiseShaderFileName() const;
    UINT MaxTraceRecursionDepth() const;
    void CreateRenderTargetViews();
    void Resize(UINT width, UINT height);
    static UINT Align(UINT value, UINT alignment);
    static void AllocateImGuiDescriptor(struct ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle);
    static void FreeImGuiDescriptor(struct ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);

    rb::SceneImporter m_sceneImporter;
    std::wstring m_scenePath;
    std::wstring m_projectPath;
    std::wstring m_startupScenePath;
    std::wstring m_startupEnvironmentPath;
    std::string m_startupMcpToken;
    UINT m_startupMcpPort = 0;
    mcp::AccessMode m_startupMcpAccessMode = mcp::AccessMode::ConfirmMutations;
    bool m_startupMcpServer = false;
    bool m_hasStartupMcpAccessMode = false;
    std::wstring m_pendingProjectPath;
    std::wstring m_pendingScenePath;
    std::wstring m_pendingEnvironmentPath;
    std::string m_sceneDiagnostics = "Using built-in preview scene.";
    std::string m_projectDiagnostics;
    PendingFileDialog m_pendingFileDialog = PendingFileDialog::None;
    UINT m_pendingResizeWidth = 0;
    UINT m_pendingResizeHeight = 0;
    UINT m_renderWidth = 0;
    UINT m_renderHeight = 0;
    bool m_pendingGpuResourceRefresh = false;
    bool m_resizePending = false;
    bool m_minimized = false;
    bool m_projectDirty = false;
    std::wstring m_adapterDescription = L"Unknown";
    DlssBackend m_dlssBackendRuntime;
    mcp::ServerSettings m_mcpSettings;
    mutable std::mutex m_mcpSettingsMutex;
    mcp::Server m_mcpServer;
    mcp::Dispatcher m_mcpDispatcher;
    mutable std::mutex m_mcpSnapshotMutex;
    std::string m_mcpStateJson = "{}";
    std::string m_mcpStatsJson = "{}";
    std::string m_mcpMaterialsJson = "{}";
    std::string m_mcpDiagnosticsJson = "{}";
    std::string m_mcpProjectJson = "{}";
    std::string m_mcpSceneSummaryJson = "{}";
    std::string m_mcpMaterialVariantsJson = "{}";
    std::string m_mcpMaterialPresetsJson = "{}";
    std::string m_mcpLatestCaptureBase64;
    std::string m_mcpLastCaptureDiagnostics = "No capture has been requested.";
    struct McpCapture
    {
        uint64_t id = 0;
        int debugView = 0;
        std::string label;
        std::string base64Png;
    };
    std::deque<McpCapture> m_mcpCaptures;
    uint64_t m_nextMcpCaptureId = 1;
    int m_displayResolutionPreset = 1;
    std::string m_mcpUiDiagnostics;
};
