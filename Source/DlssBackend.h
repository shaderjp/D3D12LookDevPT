#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>

enum class DlssMode
{
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

struct DlssStatus
{
    bool compiled = false;
    bool runtimeAvailable = false;
    bool initialized = false;
    bool deviceRegistered = false;
    bool featureSupported = false;
    bool evaluationReady = false;
    bool historyResetRequested = false;
    uint32_t recommendedRenderWidth = 0;
    uint32_t recommendedRenderHeight = 0;
    std::wstring runtimePath;
    std::string lastError;
    std::string fallbackReason;
};

class DlssBackend
{
public:
    DlssBackend();
    ~DlssBackend();

    DlssBackend(const DlssBackend&) = delete;
    DlssBackend& operator=(const DlssBackend&) = delete;

    void InitializeBeforeDevice(const std::wstring& executableDirectory);
    void SetD3DDevice(ID3D12Device* device, IDXGIAdapter1* adapter, uint32_t outputWidth, uint32_t outputHeight, DlssMode mode);
    void UpdateMode(uint32_t outputWidth, uint32_t outputHeight, DlssMode mode);
    void ResetHistory();
    void Shutdown();

    const DlssStatus& Status() const { return m_status; }
    bool CanEvaluateRayReconstruction() const { return m_status.evaluationReady; }

private:
    void* m_runtime = nullptr;
    DlssStatus m_status;
};
