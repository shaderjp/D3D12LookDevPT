#include "stdafx.h"
#include "DlssBackend.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>

#ifndef D3D12LOOKDEVPT_WITH_DLSS
#define D3D12LOOKDEVPT_WITH_DLSS 0
#endif

#if D3D12LOOKDEVPT_WITH_DLSS
#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss_d.h>
#include <sl_helpers.h>
#endif

namespace
{
    std::wstring JoinPath(const std::wstring& base, const wchar_t* child)
    {
        return (std::filesystem::path(base) / child).wstring();
    }

    std::string WideToNarrowLossy(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (wchar_t ch : text)
        {
            result.push_back(ch >= 0 && ch <= 127 ? static_cast<char>(ch) : '?');
        }
        return result;
    }

#if D3D12LOOKDEVPT_WITH_DLSS
    struct StreamlineApi
    {
        PFun_slInit* init = nullptr;
        PFun_slShutdown* shutdown = nullptr;
        PFun_slSetD3DDevice* setD3DDevice = nullptr;
        PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
        PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    };

    StreamlineApi& Api()
    {
        static StreamlineApi api;
        return api;
    }

    template <typename T>
    bool LoadFunction(HMODULE module, const char* name, T*& out)
    {
        out = reinterpret_cast<T*>(GetProcAddress(module, name));
        return out != nullptr;
    }

    const char* ResultName(sl::Result result)
    {
        return sl::getResultAsStr(result);
    }

    sl::DLSSMode ToStreamlineMode(DlssMode mode)
    {
        switch (mode)
        {
        case DlssMode::Balanced:
            return sl::DLSSMode::eBalanced;
        case DlssMode::Performance:
            return sl::DLSSMode::eMaxPerformance;
        case DlssMode::UltraPerformance:
            return sl::DLSSMode::eUltraPerformance;
        default:
            return sl::DLSSMode::eMaxQuality;
        }
    }

    bool TryLoadDlssdFunction(const char* name, void*& function)
    {
        function = nullptr;
        if (!Api().getFeatureFunction)
        {
            return false;
        }
        const sl::Result result = Api().getFeatureFunction(sl::kFeatureDLSS_RR, name, function);
        return result == sl::Result::eOk && function != nullptr;
    }
#endif
}

DlssBackend::DlssBackend()
{
    m_status.compiled = D3D12LOOKDEVPT_WITH_DLSS != 0;
    if (!m_status.compiled)
    {
        m_status.fallbackReason = "DLSS was disabled at build time.";
    }
}

DlssBackend::~DlssBackend()
{
    Shutdown();
}

void DlssBackend::InitializeBeforeDevice(const std::wstring& executableDirectory)
{
    Shutdown();
    m_status = {};
    m_status.compiled = D3D12LOOKDEVPT_WITH_DLSS != 0;

#if !D3D12LOOKDEVPT_WITH_DLSS
    (void)executableDirectory;
    m_status.fallbackReason = "DLSS was disabled at build time.";
    return;
#else
    const std::array<std::wstring, 4> candidates =
    {
        JoinPath(executableDirectory, L"Streamline\\sl.interposer.dll"),
        JoinPath(executableDirectory, L"sl.interposer.dll"),
        JoinPath(executableDirectory, L"Streamline\\bin\\x64\\sl.interposer.dll"),
        JoinPath(std::filesystem::current_path().wstring(), L"ThirdParty\\Streamline\\bin\\x64\\sl.interposer.dll"),
    };

    std::wstring runtimePath;
    for (const std::wstring& candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            runtimePath = candidate;
            break;
        }
    }

    if (runtimePath.empty())
    {
        m_status.fallbackReason = "Streamline runtime DLL was not found.";
        m_status.lastError = "Missing sl.interposer.dll. Place it in Bin\\x64\\<Config>\\Streamline.";
        return;
    }

    HMODULE module = LoadLibraryW(runtimePath.c_str());
    if (!module)
    {
        const DWORD error = GetLastError();
        m_status.fallbackReason = "Streamline runtime DLL could not be loaded.";
        m_status.lastError = "LoadLibrary failed with Win32 error " + std::to_string(error) + ".";
        m_status.runtimePath = runtimePath;
        return;
    }

    m_runtime = module;
    m_status.runtimeAvailable = true;
    m_status.runtimePath = runtimePath;

    StreamlineApi api{};
    if (!LoadFunction(module, "slInit", api.init) ||
        !LoadFunction(module, "slShutdown", api.shutdown) ||
        !LoadFunction(module, "slSetD3DDevice", api.setD3DDevice) ||
        !LoadFunction(module, "slIsFeatureSupported", api.isFeatureSupported) ||
        !LoadFunction(module, "slGetFeatureFunction", api.getFeatureFunction))
    {
        m_status.fallbackReason = "Streamline runtime is missing required exports.";
        m_status.lastError = "Required Streamline core functions were not found.";
        FreeLibrary(module);
        m_runtime = nullptr;
        return;
    }
    Api() = api;

    const std::filesystem::path pluginPath = std::filesystem::path(runtimePath).parent_path();
    const wchar_t* pluginPaths[] = { pluginPath.c_str() };
    const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
    sl::Preferences preferences{};
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "D3D12LookDevPT";
    preferences.projectId = "D3D12LookDevPT";
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    const sl::Result initResult = Api().init(preferences, sl::kSDKVersion);
    if (initResult != sl::Result::eOk)
    {
        m_status.fallbackReason = "Streamline initialization failed.";
        m_status.lastError = std::string("slInit returned ") + ResultName(initResult) + ".";
        return;
    }

    m_status.initialized = true;
    m_status.fallbackReason = "DLSS-RR device support has not been checked yet.";
#endif
}

void DlssBackend::SetD3DDevice(ID3D12Device* device, IDXGIAdapter1* adapter, uint32_t outputWidth, uint32_t outputHeight, DlssMode mode)
{
#if !D3D12LOOKDEVPT_WITH_DLSS
    (void)device;
    (void)adapter;
    (void)outputWidth;
    (void)outputHeight;
    (void)mode;
    return;
#else
    if (!m_status.initialized || !Api().setD3DDevice || !Api().isFeatureSupported)
    {
        return;
    }

    const sl::Result deviceResult = Api().setD3DDevice(device);
    if (deviceResult != sl::Result::eOk)
    {
        m_status.fallbackReason = "Streamline did not accept the D3D12 device.";
        m_status.lastError = std::string("slSetD3DDevice returned ") + ResultName(deviceResult) + ".";
        return;
    }
    m_status.deviceRegistered = true;

    DXGI_ADAPTER_DESC1 desc{};
    if (adapter)
    {
        adapter->GetDesc1(&desc);
    }
    sl::AdapterInfo adapterInfo{};
    adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
    adapterInfo.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);

    const sl::Result supportResult = Api().isFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
    if (supportResult != sl::Result::eOk)
    {
        m_status.featureSupported = false;
        m_status.evaluationReady = false;
        m_status.fallbackReason = "DLSS Ray Reconstruction is not supported on this adapter or driver.";
        m_status.lastError = std::string("slIsFeatureSupported returned ") + ResultName(supportResult) + ".";
        return;
    }

    m_status.featureSupported = true;
    UpdateMode(outputWidth, outputHeight, mode);

    void* setOptions = nullptr;
    void* getOptimalSettings = nullptr;
    const bool hasOptions = TryLoadDlssdFunction("slDLSSDSetOptions", setOptions);
    const bool hasOptimal = TryLoadDlssdFunction("slDLSSDGetOptimalSettings", getOptimalSettings);
    if (!hasOptions || !hasOptimal)
    {
        m_status.evaluationReady = false;
        m_status.fallbackReason = "DLSS-RR plugin functions are unavailable.";
        m_status.lastError = "The loaded Streamline runtime did not expose DLSS-RR option functions.";
        return;
    }

    m_status.evaluationReady = false;
    m_status.fallbackReason = "DLSS-RR support was detected, but resource-tag evaluation is not enabled in this build.";
#endif
}

void DlssBackend::UpdateMode(uint32_t outputWidth, uint32_t outputHeight, DlssMode mode)
{
#if !D3D12LOOKDEVPT_WITH_DLSS
    (void)outputWidth;
    (void)outputHeight;
    (void)mode;
    return;
#else
    if (!m_status.initialized || !m_status.featureSupported)
    {
        return;
    }

    m_status.recommendedRenderWidth = outputWidth;
    m_status.recommendedRenderHeight = outputHeight;

    void* rawGetOptimalSettings = nullptr;
    if (!TryLoadDlssdFunction("slDLSSDGetOptimalSettings", rawGetOptimalSettings))
    {
        return;
    }

    auto getOptimalSettings = reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>(rawGetOptimalSettings);
    sl::DLSSDOptions options{};
    options.mode = ToStreamlineMode(mode);
    options.outputWidth = outputWidth;
    options.outputHeight = outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;

    sl::DLSSDOptimalSettings settings{};
    const sl::Result result = getOptimalSettings(options, settings);
    if (result == sl::Result::eOk)
    {
        m_status.recommendedRenderWidth = settings.optimalRenderWidth;
        m_status.recommendedRenderHeight = settings.optimalRenderHeight;
    }
    else
    {
        m_status.lastError = std::string("slDLSSDGetOptimalSettings returned ") + ResultName(result) + ".";
    }
#endif
}

void DlssBackend::ResetHistory()
{
    m_status.historyResetRequested = true;
}

void DlssBackend::Shutdown()
{
#if D3D12LOOKDEVPT_WITH_DLSS
    if (m_status.initialized && Api().shutdown)
    {
        Api().shutdown();
    }
    if (m_runtime)
    {
        FreeLibrary(reinterpret_cast<HMODULE>(m_runtime));
        m_runtime = nullptr;
    }
    Api() = {};
#endif
}
