#include "stdafx.h"
#include "D3D12PathTracingBackend.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "TextureLoader.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <commdlg.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    void ApplyBistroDisplayResolution(HWND hwnd, int presetIndex)
    {
        if (hwnd == nullptr)
        {
            return;
        }

        constexpr UINT Widths[] = { 1280u, 1920u, 3840u };
        constexpr UINT Heights[] = { 720u, 1080u, 2160u };
        if (presetIndex < 0)
        {
            presetIndex = 0;
        }
        if (presetIndex > 2)
        {
            presetIndex = 2;
        }

        RECT rect = { 0, 0, static_cast<LONG>(Widths[presetIndex]), static_cast<LONG>(Heights[presetIndex]) };
        const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
        AdjustWindowRectEx(&rect, style, GetMenu(hwnd) != nullptr, exStyle);
        SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void SubmitMainDockSpace()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        constexpr ImGuiWindowFlags HostWindowFlags =
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground;

        ImGui::Begin("D3D12LookDevPT DockSpace", nullptr, HostWindowFlags);
        ImGui::PopStyleVar(3);

        const ImGuiID dockspaceId = ImGui::GetID("D3D12LookDevPTMainDockSpaceV3");
        constexpr ImGuiDockNodeFlags DockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;

        static bool s_defaultDockLayoutInitialized = false;
        if (!s_defaultDockLayoutInitialized)
        {
            s_defaultDockLayoutInitialized = true;
            if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
            {
                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace | DockspaceFlags);
                ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
                ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

                ImGuiID centerId = dockspaceId;
                ImGuiID leftId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.28f, nullptr, &centerId);
                ImGuiID rightId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.32f, nullptr, &centerId);

                ImGuiID leftBottomId = ImGui::DockBuilderSplitNode(leftId, ImGuiDir_Down, 0.38f, nullptr, &leftId);
                ImGuiID leftMiddleId = ImGui::DockBuilderSplitNode(leftId, ImGuiDir_Down, 0.48f, nullptr, &leftId);

                ImGuiID rightBottomId = ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Down, 0.34f, nullptr, &rightId);
                ImGuiID rightMiddleId = ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Down, 0.50f, nullptr, &rightId);

                ImGui::DockBuilderDockWindow("Viewport", leftId);
                ImGui::DockBuilderDockWindow("Scene", leftMiddleId);
                ImGui::DockBuilderDockWindow("Material", leftBottomId);
                ImGui::DockBuilderDockWindow("Lighting", leftBottomId);
                ImGui::DockBuilderDockWindow("Path Tracing", rightId);
                ImGui::DockBuilderDockWindow("ReSTIR", rightMiddleId);
                ImGui::DockBuilderDockWindow("Denoise", rightBottomId);
                ImGui::DockBuilderDockWindow("Diagnostics / Stats", rightBottomId);
                ImGui::DockBuilderDockWindow("MCP Server", rightBottomId);
                ImGui::DockBuilderFinish(dockspaceId);
            }
        }

        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), DockspaceFlags);
        ImGui::End();
    }

    constexpr UINT ImGuiDescriptorCount = 64;
    constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT AccumulationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr UINT RestirReservoirStride = sizeof(XMFLOAT4) * 2;
    const wchar_t* RayGenShaderName = L"RayGen";
    const wchar_t* MissShaderName = L"Miss";
    const wchar_t* ShadowMissShaderName = L"ShadowMiss";
    const wchar_t* ClosestHitShaderName = L"ClosestHit";
    const wchar_t* AnyHitShaderName = L"AnyHit";
    const wchar_t* ShadowAnyHitShaderName = L"ShadowAnyHit";
    const wchar_t* HitGroupName = L"HitGroup";
    const wchar_t* ShadowHitGroupName = L"ShadowHitGroup";

    XMFLOAT3 NormalizeFloat3(const float values[3])
    {
        XMVECTOR v = XMVector3Normalize(XMVectorSet(values[0], values[1], values[2], 0.0f));
        XMFLOAT3 result;
        XMStoreFloat3(&result, v);
        return result;
    }

    std::wstring PathtracingTierName(D3D12_RAYTRACING_TIER tier)
    {
        switch (static_cast<int>(tier))
        {
        case 10: return L"1.0";
        case 11: return L"1.1";
        case 12: return L"1.2";
        default:
            return tier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED ? L"Not supported" : L"Unknown";
        }
    }

    bool UsesRestirReuse(PathTracingMode mode)
    {
        return mode == PathTracingMode::ReSTIR || mode == PathTracingMode::ReSTIRDI || mode == PathTracingMode::ReSTIRCombined;
    }

    bool UsesRestirGI(PathTracingMode mode)
    {
        return mode == PathTracingMode::ReSTIR || mode == PathTracingMode::ReSTIRCombined;
    }

    bool UsesRestirDI(PathTracingMode mode)
    {
        return mode == PathTracingMode::ReSTIRDI || mode == PathTracingMode::ReSTIRCombined;
    }

    float Halton(uint32_t index, uint32_t base)
    {
        float f = 1.0f;
        float result = 0.0f;
        while (index > 0)
        {
            f /= static_cast<float>(base);
            result += f * static_cast<float>(index % base);
            index /= base;
        }
        return result;
    }

    const char* PathtracingModeName(PathTracingMode mode)
    {
        switch (mode)
        {
        case PathTracingMode::ReSTIR:
            return "ReSTIR GI";
        case PathTracingMode::ReSTIRDI:
            return "ReSTIR DI";
        case PathTracingMode::ReSTIRCombined:
            return "ReSTIR GI + DI";
        default:
            return "Path Tracing";
        }
    }

    PathTracingMode PathtracingModeFromName(const std::string& name, PathTracingMode fallback)
    {
        if (name == "Baseline PT" || name == "Path Tracing" || name == "path_tracing" || name == "baseline")
        {
            return PathTracingMode::Pathtracing;
        }
        if (name == "ReSTIR GI" || name == "restir_gi" || name == "restir")
        {
            return PathTracingMode::ReSTIR;
        }
        if (name == "ReSTIR DI" || name == "restir_di")
        {
            return PathTracingMode::ReSTIRDI;
        }
        if (name == "ReSTIR GI + DI" || name == "restir_gi_di" || name == "combined")
        {
            return PathTracingMode::ReSTIRCombined;
        }
        return fallback;
    }

    const char* NoisePresetName(D3D12PathTracingBackend::NoisePreset preset)
    {
        switch (preset)
        {
        case D3D12PathTracingBackend::NoisePreset::SharpPreview:
            return "sharp_preview";
        case D3D12PathTracingBackend::NoisePreset::StillCapture:
            return "still_capture";
        default:
            return "interactive_stable";
        }
    }

    const char* NoisePresetDisplayName(D3D12PathTracingBackend::NoisePreset preset)
    {
        switch (preset)
        {
        case D3D12PathTracingBackend::NoisePreset::SharpPreview:
            return "Sharp Preview";
        case D3D12PathTracingBackend::NoisePreset::StillCapture:
            return "Still Capture";
        default:
            return "Interactive Stable";
        }
    }

    bool TryParseNoisePreset(const std::string& name, D3D12PathTracingBackend::NoisePreset& preset)
    {
        if (name.empty() || name == "interactive_stable" || name == "Interactive Stable")
        {
            preset = D3D12PathTracingBackend::NoisePreset::InteractiveStable;
            return true;
        }
        if (name == "sharp_preview" || name == "Sharp Preview")
        {
            preset = D3D12PathTracingBackend::NoisePreset::SharpPreview;
            return true;
        }
        if (name == "still_capture" || name == "Still Capture")
        {
            preset = D3D12PathTracingBackend::NoisePreset::StillCapture;
            return true;
        }
        return false;
    }

    const char* JitterModeName(D3D12PathTracingBackend::JitterMode mode)
    {
        switch (mode)
        {
        case D3D12PathTracingBackend::JitterMode::Halton:
            return "halton";
        case D3D12PathTracingBackend::JitterMode::Off:
            return "off";
        default:
            return "stable16";
        }
    }

    const char* JitterModeDisplayName(D3D12PathTracingBackend::JitterMode mode)
    {
        switch (mode)
        {
        case D3D12PathTracingBackend::JitterMode::Halton:
            return "Halton";
        case D3D12PathTracingBackend::JitterMode::Off:
            return "Off";
        default:
            return "Stable16";
        }
    }

    bool TryParseJitterMode(const std::string& name, D3D12PathTracingBackend::JitterMode& mode)
    {
        if (name.empty() || name == "stable16" || name == "Stable16")
        {
            mode = D3D12PathTracingBackend::JitterMode::Stable16;
            return true;
        }
        if (name == "halton" || name == "Halton")
        {
            mode = D3D12PathTracingBackend::JitterMode::Halton;
            return true;
        }
        if (name == "off" || name == "Off" || name == "none")
        {
            mode = D3D12PathTracingBackend::JitterMode::Off;
            return true;
        }
        return false;
    }

    std::array<float, 4> Float4ToArray(const XMFLOAT4& value)
    {
        return { value.x, value.y, value.z, value.w };
    }

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return {};
        }
        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0)
        {
            return std::wstring(text.begin(), text.end());
        }
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
        return result;
    }

    std::string Base64Encode(const uint8_t* data, size_t size)
    {
        static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        output.reserve(((size + 2) / 3) * 4);
        for (size_t i = 0; i < size; i += 3)
        {
            const uint32_t a = data[i];
            const uint32_t b = (i + 1 < size) ? data[i + 1] : 0;
            const uint32_t c = (i + 2 < size) ? data[i + 2] : 0;
            const uint32_t triple = (a << 16) | (b << 8) | c;
            output.push_back(Alphabet[(triple >> 18) & 0x3f]);
            output.push_back(Alphabet[(triple >> 12) & 0x3f]);
            output.push_back(i + 1 < size ? Alphabet[(triple >> 6) & 0x3f] : '=');
            output.push_back(i + 2 < size ? Alphabet[triple & 0x3f] : '=');
        }
        return output;
    }

    std::wstring McpSettingsPath()
    {
        std::array<wchar_t, 32768> appData = {};
        size_t length = 0;
        if (_wgetenv_s(&length, appData.data(), appData.size(), L"APPDATA") != 0 || length == 0)
        {
            std::array<wchar_t, MAX_PATH> tempPath = {};
            const DWORD tempLength = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
            if (tempLength > 0 && tempLength < tempPath.size())
            {
                return (std::filesystem::path(tempPath.data()) / L"D3D12LookDevPT" / L"settings.json").wstring();
            }
            return L"settings.json";
        }
        return (std::filesystem::path(appData.data()) / L"D3D12LookDevPT" / L"settings.json").wstring();
    }

    bool AllFinite(std::initializer_list<float> values)
    {
        for (float value : values)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
        }
        return true;
    }

    void AppendJsonFloat3(std::ostringstream& out, const float values[3])
    {
        out << "[" << values[0] << "," << values[1] << "," << values[2] << "]";
    }

    void AppendJsonFloat3(std::ostringstream& out, const XMFLOAT3& value)
    {
        out << "[" << value.x << "," << value.y << "," << value.z << "]";
    }

    void AppendJsonFloat4(std::ostringstream& out, const XMFLOAT4& value)
    {
        out << "[" << value.x << "," << value.y << "," << value.z << "," << value.w << "]";
    }

    void LogDiagnostic(const std::string& message)
    {
        OutputDebugStringA((message + "\n").c_str());

        std::array<wchar_t, MAX_PATH> tempPath = {};
        const DWORD length = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
        if (length == 0 || length >= tempPath.size())
        {
            return;
        }

        std::ofstream file(std::filesystem::path(tempPath.data()) / L"D3D12LookDevPT.log", std::ios::app | std::ios::binary);
        if (file)
        {
            file << message << "\n";
        }
    }

    void LogDiagnostic(const std::wstring& message)
    {
        LogDiagnostic(WideToUtf8(message));
    }

    std::wstring OpenPathDialog(const wchar_t* filter)
    {
        std::vector<wchar_t> fileName(32768, L'\0');
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = Win32Application::GetHwnd();
        ofn.lpstrFile = fileName.data();
        ofn.nMaxFile = static_cast<DWORD>(fileName.size());
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        return GetOpenFileNameW(&ofn) ? std::wstring(fileName.data()) : std::wstring();
    }

    std::wstring SavePathDialog(const wchar_t* filter, const wchar_t* defaultExtension)
    {
        std::vector<wchar_t> fileName(32768, L'\0');
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = Win32Application::GetHwnd();
        ofn.lpstrFile = fileName.data();
        ofn.nMaxFile = static_cast<DWORD>(fileName.size());
        ofn.lpstrFilter = filter;
        ofn.lpstrDefExt = defaultExtension;
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&ofn) ? std::wstring(fileName.data()) : std::wstring();
    }

    void ExpandBounds(Bistro::Scene& scene, const XMFLOAT3& p)
    {
        scene.boundsMin.x = (std::min)(scene.boundsMin.x, p.x);
        scene.boundsMin.y = (std::min)(scene.boundsMin.y, p.y);
        scene.boundsMin.z = (std::min)(scene.boundsMin.z, p.z);
        scene.boundsMax.x = (std::max)(scene.boundsMax.x, p.x);
        scene.boundsMax.y = (std::max)(scene.boundsMax.y, p.y);
        scene.boundsMax.z = (std::max)(scene.boundsMax.z, p.z);
    }

    Bistro::Scene MakePreviewScene()
    {
        Bistro::Scene scene;
        scene.assetRoot = L".";
        scene.boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        scene.boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        Bistro::Material material;
        material.name = L"Preview Material";
        material.baseColorFactor = XMFLOAT4(0.72f, 0.76f, 0.82f, 1.0f);
        material.roughnessFactor = 0.42f;
        material.metallicFactor = 0.0f;
        scene.materials.push_back(material);

        struct Face
        {
            XMFLOAT3 normal;
            XMFLOAT3 a;
            XMFLOAT3 b;
            XMFLOAT3 c;
            XMFLOAT3 d;
        };
        const Face faces[] =
        {
            { XMFLOAT3(0, 0, -1), XMFLOAT3(-1, -1, -1), XMFLOAT3(1, -1, -1), XMFLOAT3(1, 1, -1), XMFLOAT3(-1, 1, -1) },
            { XMFLOAT3(0, 0, 1), XMFLOAT3(1, -1, 1), XMFLOAT3(-1, -1, 1), XMFLOAT3(-1, 1, 1), XMFLOAT3(1, 1, 1) },
            { XMFLOAT3(-1, 0, 0), XMFLOAT3(-1, -1, 1), XMFLOAT3(-1, -1, -1), XMFLOAT3(-1, 1, -1), XMFLOAT3(-1, 1, 1) },
            { XMFLOAT3(1, 0, 0), XMFLOAT3(1, -1, -1), XMFLOAT3(1, -1, 1), XMFLOAT3(1, 1, 1), XMFLOAT3(1, 1, -1) },
            { XMFLOAT3(0, 1, 0), XMFLOAT3(-1, 1, -1), XMFLOAT3(1, 1, -1), XMFLOAT3(1, 1, 1), XMFLOAT3(-1, 1, 1) },
            { XMFLOAT3(0, -1, 0), XMFLOAT3(-1, -1, 1), XMFLOAT3(1, -1, 1), XMFLOAT3(1, -1, -1), XMFLOAT3(-1, -1, -1) },
        };

        for (const Face& face : faces)
        {
            const uint32_t base = static_cast<uint32_t>(scene.vertices.size());
            const XMFLOAT2 uv[] = { XMFLOAT2(0, 1), XMFLOAT2(1, 1), XMFLOAT2(1, 0), XMFLOAT2(0, 0) };
            const XMFLOAT3 positions[] = { face.a, face.b, face.c, face.d };
            for (uint32_t i = 0; i < 4; ++i)
            {
                Bistro::Vertex vertex;
                vertex.position = positions[i];
                vertex.normal = face.normal;
                vertex.tangent = XMFLOAT4(1, 0, 0, 1);
                vertex.texcoord = uv[i];
                scene.vertices.push_back(vertex);
                ExpandBounds(scene, vertex.position);
            }
            scene.indices.insert(scene.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }

        Bistro::DrawItem draw;
        draw.indexCount = static_cast<uint32_t>(scene.indices.size());
        draw.startIndex = 0;
        draw.materialIndex = 0;
        scene.draws.push_back(draw);
        return scene;
    }

    Bistro::Scene ConvertImportedScene(const rb::ImportedScene& imported)
    {
        Bistro::Scene scene;
        scene.assetRoot = std::filesystem::path(imported.path).parent_path().wstring();
        scene.boundsMin = imported.boundsMin;
        scene.boundsMax = imported.boundsMax;
        scene.vertices.reserve(imported.vertices.size());
        for (const rb::SceneVertex& src : imported.vertices)
        {
            Bistro::Vertex vertex;
            vertex.position = src.position;
            vertex.normal = src.normal;
            vertex.tangent = src.tangent;
            vertex.texcoord = src.texcoord;
            scene.vertices.push_back(vertex);
        }
        scene.indices = imported.indices;
        scene.draws.reserve(imported.draws.size());
        for (const rb::SceneDraw& src : imported.draws)
        {
            Bistro::DrawItem draw;
            draw.indexCount = src.indexCount;
            draw.startIndex = src.startIndex;
            draw.baseVertex = src.baseVertex;
            draw.materialIndex = src.materialIndex;
            scene.draws.push_back(draw);
        }
        scene.materials.reserve(imported.materials.size());
        for (const rb::SceneMaterial& src : imported.materials)
        {
            Bistro::Material material;
            material.name = Utf8ToWide(src.assignment.materialName);
            material.textures[Bistro::TextureSlotBaseColor] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::BaseColor)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::BaseColor)] : src.baseColorTexturePath;
            material.textures[Bistro::TextureSlotNormal] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Normal)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Normal)] : src.normalTexturePath;
            material.textures[Bistro::TextureSlotRoughness] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Roughness)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Roughness)] : src.roughnessTexturePath;
            material.textures[Bistro::TextureSlotMetallic] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Metallic)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Metallic)] : src.metallicTexturePath;
            material.textures[Bistro::TextureSlotOcclusion] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Occlusion)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Occlusion)] : src.occlusionTexturePath;
            material.textures[Bistro::TextureSlotEmissive] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Emissive)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Emissive)] : src.emissiveTexturePath;
            material.baseColorFactor = XMFLOAT4(src.assignment.baseColorFactor[0], src.assignment.baseColorFactor[1], src.assignment.baseColorFactor[2], src.assignment.baseColorFactor[3]);
            material.emissiveFactor = XMFLOAT4(src.assignment.emissiveFactor[0], src.assignment.emissiveFactor[1], src.assignment.emissiveFactor[2], src.assignment.emissiveFactor[3]);
            material.roughnessFactor = src.assignment.roughnessFactor;
            material.metallicFactor = src.assignment.metallicFactor;
            material.occlusionStrength = src.assignment.occlusionStrength;
            material.normalStrength = src.assignment.normalStrength;
            material.alphaCutoff = src.assignment.alphaCutoff;
            material.alphaMasked = src.assignment.alphaMode == rb::AlphaMode::Mask || src.assignment.baseColorFactor[3] < 0.99f;
            material.packedOcclusionRoughnessMetallic = src.assignment.packedOcclusionRoughnessMetallic;
            scene.materials.push_back(material);
        }
        if (scene.materials.empty())
        {
            scene.materials.push_back(Bistro::Material{});
        }
        return scene;
    }
}

D3D12PathTracingBackend::D3D12PathTracingBackend(UINT width, UINT height, std::wstring name, PathTracingMode mode) :
    DXSample(width, height, name),
    m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
    m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
    m_mode(mode)
{
}

void D3D12PathTracingBackend::OnInit()
{
    LoadPipeline();
    LoadAssets();
    LoadMcpUserSettings();
    if (m_startupMcpPort > 0 || !m_startupMcpToken.empty() || m_hasStartupMcpAccessMode)
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        if (m_startupMcpPort > 0)
        {
            m_mcpSettings.port = static_cast<uint16_t>(std::clamp<UINT>(m_startupMcpPort, 1, 65535));
        }
        if (!m_startupMcpToken.empty())
        {
            m_mcpSettings.token = m_startupMcpToken;
        }
        if (m_hasStartupMcpAccessMode)
        {
            m_mcpSettings.accessMode = m_startupMcpAccessMode;
        }
    }
    if (m_startupMcpServer)
    {
        StartMcpServer();
    }
    UpdateMcpSnapshots();
    m_lastUpdate = std::chrono::steady_clock::now();
}

void D3D12PathTracingBackend::LoadPipeline()
{
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5)))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        {
            m_tearingSupported = allowTearing == TRUE;
        }
    }

    ComPtr<IDXGIAdapter1> hardwareAdapter;
    GetHardwareAdapter(factory.Get(), &hardwareAdapter);
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    if (hardwareAdapter && SUCCEEDED(hardwareAdapter->GetDesc1(&adapterDesc)))
    {
        m_adapterDescription = adapterDesc.Description;
    }
    ComPtr<ID3D12Device2> baseDevice;
    ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&baseDevice)));
    ThrowIfFailed(baseDevice.As(&m_device));

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
    m_raytracingTier = options5.RaytracingTier;
    if (m_raytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    {
        throw std::runtime_error("This GPU/driver does not support DirectX Pathtracing. D3D12PathTracingBackend has no raster fallback.");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = BackBufferFormat;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(m_commandQueue.Get(), Win32Application::GetHwnd(), &swapChainDesc, nullptr, nullptr, &swapChain));
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CreateRenderTargetViews();

    D3D12_DESCRIPTOR_HEAP_DESC imguiHeapDesc = {};
    imguiHeapDesc.NumDescriptors = ImGuiDescriptorCount;
    imguiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&imguiHeapDesc, IID_PPV_ARGS(&m_imguiDescriptorHeap)));
    m_imguiDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
}

void D3D12PathTracingBackend::CreateRenderTargetViews()
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT n = 0; n < FrameCount; ++n)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
        m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, m_rtvDescriptorSize);
    }
}

void D3D12PathTracingBackend::Resize(UINT width, UINT height)
{
    if (!m_swapChain || width == 0 || height == 0)
    {
        return;
    }
    if (width == m_width && height == m_height)
    {
        return;
    }

    LogDiagnostic("Resize: " + std::to_string(width) + "x" + std::to_string(height));
    WaitForPreviousFrame();

    for (UINT n = 0; n < FrameCount; ++n)
    {
        m_renderTargets[n].Reset();
    }

    m_width = width;
    m_height = height;
    m_aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
    m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    m_scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height));

    const UINT flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ThrowIfFailed(m_swapChain->ResizeBuffers(FrameCount, m_width, m_height, BackBufferFormat, flags));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargetViews();
    CreateGpuResourcesForCurrentScene();
}

void D3D12PathTracingBackend::LoadAssets()
{
    LogDiagnostic("LoadAssets: creating preview scene.");
    m_scene = MakePreviewScene();
    if (m_scene.draws.empty())
    {
        throw std::runtime_error("Preview scene did not produce Path Tracing geometries.");
    }

    m_defaultCameraPosition = XMFLOAT3(0.0f, 0.4f, -5.0f);
    m_defaultCameraYaw = 0.0f;
    m_defaultCameraPitch = XMConvertToRadians(4.0f);
    ResetCameraView();
    ResetCameraSpeeds();

    CreateGpuResourcesForCurrentScene();
    InitializeImGui();

    if (!m_startupScenePath.empty())
    {
        std::string diagnostics;
        LogDiagnostic(L"Startup scene load: " + m_startupScenePath);
        if (!LoadScenePath(m_startupScenePath, diagnostics))
        {
            LogDiagnostic("Startup scene load failed: " + diagnostics);
            m_sceneDiagnostics = diagnostics;
        }
        else
        {
            LogDiagnostic("Startup scene load succeeded: " + diagnostics);
        }
    }
    if (!m_startupEnvironmentPath.empty())
    {
        std::string diagnostics;
        LogDiagnostic(L"Startup environment load: " + m_startupEnvironmentPath);
        if (!LoadEnvironmentPath(m_startupEnvironmentPath, diagnostics))
        {
            LogDiagnostic("Startup environment load failed: " + diagnostics);
            m_projectDiagnostics = diagnostics;
        }
    }
}

void D3D12PathTracingBackend::CreateGpuResourcesForCurrentScene()
{
    LogDiagnostic("CreateGpuResourcesForCurrentScene: begin.");
    m_geometryRecords.clear();
    m_rtMaterials.clear();
    m_materialTextureIndices.clear();
    m_textures.clear();
    m_uploadBuffers.clear();

    LogDiagnostic("CreateGpuResourcesForCurrentScene: building light list.");
    Bistro::LightBuildResult lightBuild = Bistro::BuildLightList(m_scene);
    m_lights = std::move(lightBuild.lights);
    m_activeLightCount = lightBuild.activeLightCount;
    m_emissiveTriangleLightCount = lightBuild.emissiveTriangleCount;
    m_proceduralAreaLightCount = lightBuild.proceduralAreaCount;

    m_geometryRecords.reserve(m_scene.draws.size());
    for (const Bistro::DrawItem& draw : m_scene.draws)
    {
        Bistro::RtGeometryRecord record{};
        record.indexOffset = draw.startIndex;
        record.indexCount = draw.indexCount;
        record.baseVertex = draw.baseVertex;
        record.materialIndex = draw.materialIndex;
        m_geometryRecords.push_back(record);
    }

    if (m_sceneConstantBuffer && m_mappedSceneConstants)
    {
        m_sceneConstantBuffer->Unmap(0, nullptr);
        m_mappedSceneConstants = nullptr;
    }

    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    LogDiagnostic("CreateGpuResourcesForCurrentScene: descriptors/output/root signature.");
    CreateDescriptorHeap();
    CreateOutputResources();
    CreateGlobalRootSignature();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: scene buffers.");
    CreateSceneBuffers();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: textures.");
    CreateTextures();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: pipelines.");
    CreatePathtracingStateObject();
    CreateRestirReusePipeline();
    CreateDenoisePipeline();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: acceleration structures.");
    BuildAccelerationStructures();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: shader tables.");
    CreateShaderTables();

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
    WaitForPreviousFrame();
    m_uploadBuffers.clear();
    ResetRenderingHistory();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: complete.");
}

bool D3D12PathTracingBackend::LoadScenePath(const std::wstring& path, std::string& diagnostics)
{
    LogDiagnostic(L"LoadScenePath: importing " + path);
    rb::SceneImportResult imported = m_sceneImporter.ImportScene(path);
    if (!imported.succeeded)
    {
        diagnostics = imported.diagnostics;
        LogDiagnostic("LoadScenePath: import failed: " + diagnostics);
        return false;
    }

    try
    {
        LogDiagnostic("LoadScenePath: import succeeded. Waiting for GPU.");
        WaitForPreviousFrame();
        LogDiagnostic("LoadScenePath: converting scene.");
        m_scene = ConvertImportedScene(imported.scene);
        m_scenePath = path;
        m_sceneDiagnostics = imported.diagnostics;

        const float sx = m_scene.boundsMax.x - m_scene.boundsMin.x;
        const float sy = m_scene.boundsMax.y - m_scene.boundsMin.y;
        const float sz = m_scene.boundsMax.z - m_scene.boundsMin.z;
        const float radius = (std::max)(1.0f, 0.5f * std::sqrt(sx * sx + sy * sy + sz * sz));
        const XMFLOAT3 center(
            (m_scene.boundsMin.x + m_scene.boundsMax.x) * 0.5f,
            (m_scene.boundsMin.y + m_scene.boundsMax.y) * 0.5f,
            (m_scene.boundsMin.z + m_scene.boundsMax.z) * 0.5f);
        m_defaultCameraPosition = XMFLOAT3(center.x, center.y + radius * 0.25f, center.z - radius * 2.5f);
        m_defaultCameraYaw = 0.0f;
        m_defaultCameraPitch = XMConvertToRadians(5.0f);
        ResetCameraView();
        LogDiagnostic("LoadScenePath: creating GPU resources for imported scene.");
        CreateGpuResourcesForCurrentScene();
        diagnostics = imported.diagnostics;
        m_projectDirty = true;
        LogDiagnostic("LoadScenePath: complete.");
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        LogDiagnostic("LoadScenePath: exception: " + diagnostics);
        return false;
    }
}

bool D3D12PathTracingBackend::LoadEnvironmentPath(const std::wstring& path, std::string& diagnostics)
{
    if (!std::filesystem::exists(path))
    {
        diagnostics = "Environment texture was not found.";
        return false;
    }
    try
    {
        WaitForPreviousFrame();
        m_environmentTexturePath = path;
        m_environmentMapEnabled = true;
        CreateGpuResourcesForCurrentScene();
        diagnostics = "Environment texture loaded.";
        m_projectDirty = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

bool D3D12PathTracingBackend::SaveProjectToDisk(const std::wstring& path)
{
    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
    {
        return false;
    }

    XMFLOAT3 camera = m_camera.GetPosition();
    file << "{\n";
    file << "  \"scenePath\": \"" << cld::EscapeJson(WideToUtf8(m_scenePath)) << "\",\n";
    file << "  \"environmentPath\": \"" << cld::EscapeJson(WideToUtf8(m_environmentTexturePath)) << "\",\n";
    file << "  \"mode\": \"" << PathtracingModeName(m_mode) << "\",\n";
    file << "  \"camera\": {\"position\": [" << camera.x << ", " << camera.y << ", " << camera.z << "], \"yaw\": " << m_camera.GetYawRadians() << ", \"pitch\": " << m_camera.GetPitchRadians() << "},\n";
    file << "  \"lighting\": {\"direction\": [" << m_lightDirection[0] << ", " << m_lightDirection[1] << ", " << m_lightDirection[2] << "], \"intensity\": " << m_lightIntensity << "},\n";
    file << "  \"materials\": [\n";
    for (size_t i = 0; i < m_scene.materials.size(); ++i)
    {
        const Bistro::Material& material = m_scene.materials[i];
        file << "    {\"index\": " << i
             << ", \"name\": \"" << cld::EscapeJson(WideToUtf8(material.name)) << "\""
             << ", \"baseColor\": [" << material.baseColorFactor.x << ", " << material.baseColorFactor.y << ", " << material.baseColorFactor.z << ", " << material.baseColorFactor.w << "]"
             << ", \"emissive\": [" << material.emissiveFactor.x << ", " << material.emissiveFactor.y << ", " << material.emissiveFactor.z << ", " << material.emissiveFactor.w << "]"
             << ", \"roughness\": " << material.roughnessFactor
             << ", \"metallic\": " << material.metallicFactor
             << ", \"occlusionStrength\": " << material.occlusionStrength
             << ", \"normalStrength\": " << material.normalStrength
             << ", \"alphaCutoff\": " << material.alphaCutoff
             << ", \"alphaMasked\": " << (material.alphaMasked ? "true" : "false")
             << ", \"packedORM\": " << (material.packedOcclusionRoughnessMetallic ? "true" : "false")
             << "}" << (i + 1 < m_scene.materials.size() ? "," : "") << "\n";
    }
    file << "  ],\n";
    file << "  \"pathTracing\": {\"samplesPerFrame\": " << m_giSamplesPerFrame << ", \"maxBounces\": " << m_maxPathBounces << ", \"minBounces\": " << m_minPathBounces
         << ", \"radianceClamp\": " << m_giRadianceClamp << ", \"temporalClamp\": " << m_giTemporalClampScale
         << ", \"maxAccumSamples\": " << m_maxAccumulatedFrames << ", \"adaptiveSampling\": " << (m_adaptiveSamplingEnabled ? "true" : "false")
         << ", \"maxAdaptiveSPP\": " << m_maxAdaptiveSamplesPerPixel << ", \"varianceThreshold\": " << m_adaptiveVarianceThreshold
         << ", \"disocclusionBoost\": " << m_adaptiveDisocclusionBoost << "},\n";
    file << "  \"restir\": {\"temporalReuse\": " << (m_restirTemporalReuse ? "true" : "false") << ", \"spatialReusePasses\": " << m_restirSpatialReusePasses
         << ", \"spatialRadius\": " << m_restirSpatialRadius << ", \"candidateSamples\": " << m_restirCandidateSamples << ", \"mClamp\": " << m_restirMClamp
         << ", \"diTemporalReuse\": " << (m_restirDiTemporalReuse ? "true" : "false") << ", \"diSpatialReusePasses\": " << m_restirDiSpatialReusePasses
         << ", \"diCandidateSamples\": " << m_restirDiCandidateSamples << ", \"diMClamp\": " << m_restirDiMClamp
         << ", \"reservoirReprojection\": " << (m_reservoirReprojection ? "true" : "false")
         << ", \"reservoirValidation\": " << (m_reservoirValidation ? "true" : "false")
         << ", \"giValidationRay\": " << (m_restirGiValidationRay ? "true" : "false")
         << ", \"reservoirMaxAge\": " << m_reservoirMaxAge << "},\n";
    file << "  \"denoise\": {\"preset\": \"" << NoisePresetName(m_noisePreset) << "\", \"enabled\": " << (m_denoiserEnabled ? "true" : "false")
         << ", \"splitSignalDenoise\": " << (m_splitSignalDenoise ? "true" : "false")
         << ", \"realtimeReconstruction\": " << (m_realtimeReconstruction ? "true" : "false")
         << ", \"cameraJitter\": " << (m_cameraJitter ? "true" : "false")
         << ", \"temporalStability\": " << (m_temporalStabilityEnabled ? "true" : "false")
         << ", \"jitterMode\": \"" << JitterModeName(m_jitterMode) << "\""
         << ", \"movingJitterScale\": " << m_movingJitterScale
         << ", \"maxHistoryFrames\": " << m_reconstructionMaxHistoryFrames
         << ", \"temporalAlphaMin\": " << m_temporalAlphaMin << ", \"temporalAlphaMax\": " << m_temporalAlphaMax
         << ", \"historyClampSigma\": " << m_historyClampSigma << ", \"reactiveThreshold\": " << m_reactiveThreshold
         << ", \"specularHistoryScale\": " << m_specularHistoryScale
         << ", \"spatialIterations\": " << m_denoiserSpatialIterations << ", \"atrousPasses\": " << m_atrousPassCount
         << ", \"diffuseFilterStrength\": " << m_atrousDiffuseStrength << ", \"specularFilterStrength\": " << m_atrousSpecularStrength
         << ", \"varianceScale\": " << m_atrousVarianceScale
         << ", \"normalSigma\": " << m_denoiserNormalSigma << ", \"depthSigma\": " << m_denoiserDepthSigma
         << ", \"luminanceSigma\": " << m_denoiserLuminanceSigma << ", \"albedoSigma\": " << m_denoiserAlbedoSigma
         << ", \"strength\": " << m_denoiserStrength << "},\n";
    file << "  \"view\": {\"debugView\": " << m_debugViewMode << ", \"environmentEnabled\": " << (m_environmentMapEnabled ? "true" : "false") << "}\n";
    file << "}\n";
    m_projectPath = path;
    m_projectDirty = false;
    m_projectDiagnostics = "Project saved.";
    return true;
}

bool D3D12PathTracingBackend::LoadProjectFromDisk(const std::wstring& path, std::string& diagnostics)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
    {
        diagnostics = "Project file was not found.";
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    try
    {
        cld::JsonValue root = cld::JsonParser(buffer.str()).Parse();
        if (root.type != cld::JsonValue::Type::Object)
        {
            diagnostics = "Project root must be an object.";
            return false;
        }

        const PathTracingMode loadedMode = PathtracingModeFromName(cld::JsonStringOr(root, "mode"), m_mode);
        const bool modeChanged = loadedMode != m_mode;
        m_mode = loadedMode;

        const std::wstring scenePath = Utf8ToWide(cld::JsonStringOr(root, "scenePath"));
        if (!scenePath.empty())
        {
            if (!LoadScenePath(scenePath, diagnostics))
            {
                return false;
            }
        }
        const std::wstring environmentPath = Utf8ToWide(cld::JsonStringOr(root, "environmentPath"));
        if (!environmentPath.empty())
        {
            std::string envDiagnostics;
            LoadEnvironmentPath(environmentPath, envDiagnostics);
        }

        bool materialOverridesChanged = false;
        if (const cld::JsonValue* materials = cld::FindMember(root, "materials"); materials && materials->type == cld::JsonValue::Type::Array)
        {
            for (const cld::JsonValue& overrideValue : materials->array)
            {
                if (overrideValue.type != cld::JsonValue::Type::Object)
                {
                    continue;
                }

                int materialIndex = static_cast<int>(cld::JsonNumberOr(overrideValue, "index", -1.0));
                const std::string name = cld::JsonStringOr(overrideValue, "name");
                if (materialIndex < 0 && !name.empty())
                {
                    const std::wstring materialName = Utf8ToWide(name);
                    for (size_t i = 0; i < m_scene.materials.size(); ++i)
                    {
                        if (m_scene.materials[i].name == materialName)
                        {
                            materialIndex = static_cast<int>(i);
                            break;
                        }
                    }
                }

                if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
                {
                    continue;
                }

                Bistro::Material& material = m_scene.materials[materialIndex];
                const std::array<float, 4> baseColor = cld::JsonFloat4Or(overrideValue, "baseColor", Float4ToArray(material.baseColorFactor));
                const std::array<float, 4> emissive = cld::JsonFloat4Or(overrideValue, "emissive", Float4ToArray(material.emissiveFactor));
                material.baseColorFactor = XMFLOAT4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
                material.emissiveFactor = XMFLOAT4(emissive[0], emissive[1], emissive[2], emissive[3]);
                material.roughnessFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "roughness", material.roughnessFactor)), 0.02f, 1.0f);
                material.metallicFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "metallic", material.metallicFactor)), 0.0f, 1.0f);
                material.occlusionStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "occlusionStrength", material.occlusionStrength)), 0.0f, 2.0f);
                material.normalStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "normalStrength", material.normalStrength)), 0.0f, 2.0f);
                material.alphaCutoff = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "alphaCutoff", material.alphaCutoff)), 0.0f, 1.0f);
                material.alphaMasked = cld::JsonBoolOr(overrideValue, "alphaMasked", material.alphaMasked);
                material.packedOcclusionRoughnessMetallic = cld::JsonBoolOr(overrideValue, "packedORM", material.packedOcclusionRoughnessMetallic);
                materialOverridesChanged = true;
            }
        }

        if (const cld::JsonValue* camera = cld::FindMember(root, "camera"))
        {
            const std::array<float, 3> position = cld::JsonFloat3Or(*camera, "position", { m_defaultCameraPosition.x, m_defaultCameraPosition.y, m_defaultCameraPosition.z });
            m_camera.Reset(XMFLOAT3(position[0], position[1], position[2]), static_cast<float>(cld::JsonNumberOr(*camera, "yaw", m_defaultCameraYaw)), static_cast<float>(cld::JsonNumberOr(*camera, "pitch", m_defaultCameraPitch)));
        }
        if (const cld::JsonValue* lighting = cld::FindMember(root, "lighting"))
        {
            const std::array<float, 3> direction = cld::JsonFloat3Or(*lighting, "direction", { m_lightDirection[0], m_lightDirection[1], m_lightDirection[2] });
            m_lightDirection[0] = direction[0];
            m_lightDirection[1] = direction[1];
            m_lightDirection[2] = direction[2];
            m_lightIntensity = static_cast<float>(cld::JsonNumberOr(*lighting, "intensity", m_lightIntensity));
        }
        if (const cld::JsonValue* pathTracing = cld::FindMember(root, "pathTracing"))
        {
            m_giSamplesPerFrame = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "samplesPerFrame", m_giSamplesPerFrame)), 1, 8);
            m_maxPathBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "maxBounces", m_maxPathBounces)), 1, 8);
            m_minPathBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "minBounces", m_minPathBounces)), 0, m_maxPathBounces);
            m_giRadianceClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "radianceClamp", m_giRadianceClamp)), 1.0f, 100.0f);
            m_giTemporalClampScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "temporalClamp", m_giTemporalClampScale)), 0.25f, 4.0f);
            m_maxAccumulatedFrames = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "maxAccumSamples", m_maxAccumulatedFrames)), 1, 4096);
            m_adaptiveSamplingEnabled = cld::JsonBoolOr(*pathTracing, "adaptiveSampling", m_adaptiveSamplingEnabled);
            m_maxAdaptiveSamplesPerPixel = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "maxAdaptiveSPP", m_maxAdaptiveSamplesPerPixel)), 1, 4);
            m_adaptiveVarianceThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "varianceThreshold", m_adaptiveVarianceThreshold)), 0.02f, 1.0f);
            m_adaptiveDisocclusionBoost = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "disocclusionBoost", m_adaptiveDisocclusionBoost)), 0.0f, 4.0f);
        }
        if (const cld::JsonValue* restir = cld::FindMember(root, "restir"))
        {
            m_restirTemporalReuse = cld::JsonBoolOr(*restir, "temporalReuse", m_restirTemporalReuse);
            m_restirSpatialReusePasses = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "spatialReusePasses", m_restirSpatialReusePasses)), 0, 4);
            m_restirSpatialRadius = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "spatialRadius", m_restirSpatialRadius)), 1, 64);
            m_restirCandidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "candidateSamples", m_restirCandidateSamples)), 1, 4);
            m_restirMClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(*restir, "mClamp", m_restirMClamp)), 1.0f, 64.0f);
            m_restirDiTemporalReuse = cld::JsonBoolOr(*restir, "diTemporalReuse", m_restirDiTemporalReuse);
            m_restirDiSpatialReusePasses = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "diSpatialReusePasses", m_restirDiSpatialReusePasses)), 0, 4);
            m_restirDiCandidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "diCandidateSamples", m_restirDiCandidateSamples)), 1, 4);
            m_restirDiMClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(*restir, "diMClamp", m_restirDiMClamp)), 1.0f, 64.0f);
            m_reservoirReprojection = cld::JsonBoolOr(*restir, "reservoirReprojection", m_reservoirReprojection);
            m_reservoirValidation = cld::JsonBoolOr(*restir, "reservoirValidation", m_reservoirValidation);
            m_restirGiValidationRay = cld::JsonBoolOr(*restir, "giValidationRay", m_restirGiValidationRay);
            m_reservoirMaxAge = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "reservoirMaxAge", m_reservoirMaxAge)), 1, 32);
        }
        if (const cld::JsonValue* denoise = cld::FindMember(root, "denoise"))
        {
            if (const cld::JsonValue* presetValue = cld::FindMember(*denoise, "preset"))
            {
                NoisePreset preset = m_noisePreset;
                if (presetValue->type != cld::JsonValue::Type::String || !TryParseNoisePreset(presetValue->string, preset))
                {
                    diagnostics = "Project denoise preset is invalid.";
                    return false;
                }
                ApplyNoisePreset(preset);
            }
            if (const cld::JsonValue* jitterValue = cld::FindMember(*denoise, "jitterMode"))
            {
                JitterMode jitterMode = m_jitterMode;
                if (jitterValue->type != cld::JsonValue::Type::String || !TryParseJitterMode(jitterValue->string, jitterMode))
                {
                    diagnostics = "Project denoise jitterMode is invalid.";
                    return false;
                }
                m_jitterMode = jitterMode;
            }
            m_denoiserEnabled = cld::JsonBoolOr(*denoise, "enabled", m_denoiserEnabled);
            m_splitSignalDenoise = cld::JsonBoolOr(*denoise, "splitSignalDenoise", m_splitSignalDenoise);
            m_realtimeReconstruction = cld::JsonBoolOr(*denoise, "realtimeReconstruction", m_realtimeReconstruction);
            m_cameraJitter = cld::JsonBoolOr(*denoise, "cameraJitter", m_cameraJitter);
            m_temporalStabilityEnabled = cld::JsonBoolOr(*denoise, "temporalStability", m_temporalStabilityEnabled);
            m_movingJitterScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "movingJitterScale", m_movingJitterScale)), 0.0f, 1.0f);
            m_reconstructionMaxHistoryFrames = std::clamp(static_cast<int>(cld::JsonNumberOr(*denoise, "maxHistoryFrames", m_reconstructionMaxHistoryFrames)), 1, 128);
            m_temporalAlphaMin = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "temporalAlphaMin", m_temporalAlphaMin)), 0.01f, 0.5f);
            m_temporalAlphaMax = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "temporalAlphaMax", m_temporalAlphaMax)), m_temporalAlphaMin, 0.8f);
            m_historyClampSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "historyClampSigma", m_historyClampSigma)), 0.5f, 4.0f);
            m_reactiveThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "reactiveThreshold", m_reactiveThreshold)), 0.05f, 1.0f);
            m_specularHistoryScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "specularHistoryScale", m_specularHistoryScale)), 0.0f, 1.0f);
            m_denoiserSpatialIterations = std::clamp(static_cast<int>(cld::JsonNumberOr(*denoise, "spatialIterations", m_denoiserSpatialIterations)), 0, 4);
            m_atrousPassCount = std::clamp(static_cast<int>(cld::JsonNumberOr(*denoise, "atrousPasses", m_atrousPassCount)), 0, 5);
            m_atrousDiffuseStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "diffuseFilterStrength", m_atrousDiffuseStrength)), 0.0f, 1.0f);
            m_atrousSpecularStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "specularFilterStrength", m_atrousSpecularStrength)), 0.0f, 1.0f);
            m_atrousVarianceScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "varianceScale", m_atrousVarianceScale)), 0.25f, 4.0f);
            m_denoiserNormalSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "normalSigma", m_denoiserNormalSigma)), 0.05f, 1.0f);
            m_denoiserDepthSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "depthSigma", m_denoiserDepthSigma)), 0.002f, 0.10f);
            m_denoiserLuminanceSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "luminanceSigma", m_denoiserLuminanceSigma)), 0.1f, 8.0f);
            m_denoiserAlbedoSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "albedoSigma", m_denoiserAlbedoSigma)), 0.05f, 1.0f);
            m_denoiserStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "strength", m_denoiserStrength)), 0.0f, 1.0f);
        }
        if (const cld::JsonValue* view = cld::FindMember(root, "view"))
        {
            m_debugViewMode = std::clamp(static_cast<int>(cld::JsonNumberOr(*view, "debugView", m_debugViewMode)), 0, 40);
            m_environmentMapEnabled = cld::JsonBoolOr(*view, "environmentEnabled", m_environmentMapEnabled);
        }

        if (modeChanged || materialOverridesChanged)
        {
            CreateGpuResourcesForCurrentScene();
        }
        m_projectPath = path;
        m_projectDirty = false;
        ResetRenderingHistory();
        diagnostics = "Project loaded.";
        m_projectDiagnostics = diagnostics;
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

bool D3D12PathTracingBackend::ApplyAction(const std::string& method, const cld::JsonValue& params, std::string& diagnostics, bool validateOnly)
{
    if (method == "set_scene")
    {
        std::string scenePathUtf8 = cld::JsonStringOr(params, "path");
        if (scenePathUtf8.empty())
        {
            scenePathUtf8 = cld::JsonStringOr(params, "scenePath");
        }
        if (scenePathUtf8.empty())
        {
            diagnostics = "set_scene requires path or scenePath.";
            return false;
        }

        const std::wstring scenePath = Utf8ToWide(scenePathUtf8);
        if (!std::filesystem::exists(scenePath))
        {
            diagnostics = "Scene path does not exist.";
            return false;
        }

        const std::string environmentPathUtf8 = cld::JsonStringOr(params, "environmentPath");
        const std::wstring environmentPath = Utf8ToWide(environmentPathUtf8);
        if (!environmentPath.empty() && !std::filesystem::exists(environmentPath))
        {
            diagnostics = "Environment path does not exist.";
            return false;
        }

        if (!validateOnly)
        {
            if (!LoadScenePath(scenePath, diagnostics))
            {
                return false;
            }
            if (!environmentPath.empty() && !LoadEnvironmentPath(environmentPath, diagnostics))
            {
                return false;
            }
            m_projectDirty = true;
        }
        diagnostics = "Scene settings accepted.";
        return true;
    }
    if (method == "set_camera")
    {
        const XMFLOAT3 current = m_camera.GetPosition();
        const std::array<float, 3> position = cld::JsonFloat3Or(params, "position", { current.x, current.y, current.z });
        const float yaw = static_cast<float>(cld::JsonNumberOr(params, "yaw", m_camera.GetYawRadians()));
        const float pitch = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "pitch", m_camera.GetPitchRadians())), XMConvertToRadians(-83.0f), XMConvertToRadians(83.0f));
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]) || !std::isfinite(yaw) || !std::isfinite(pitch))
        {
            diagnostics = "Camera contains non-finite values.";
            return false;
        }

        if (!validateOnly)
        {
            m_camera.Reset(XMFLOAT3(position[0], position[1], position[2]), yaw, pitch);
            ResetRenderingHistory();
            m_projectDirty = true;
        }
        diagnostics = "Camera settings accepted.";
        return true;
    }
    if (method == "set_material")
    {
        int materialIndex = static_cast<int>(cld::JsonNumberOr(params, "index", -1.0));
        const std::string materialNameUtf8 = cld::JsonStringOr(params, "name");
        if (materialIndex < 0 && !materialNameUtf8.empty())
        {
            const std::wstring materialName = Utf8ToWide(materialNameUtf8);
            for (size_t i = 0; i < m_scene.materials.size(); ++i)
            {
                if (m_scene.materials[i].name == materialName)
                {
                    materialIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
        {
            diagnostics = "Material index or name was not found.";
            return false;
        }

        const Bistro::Material& current = m_scene.materials[materialIndex];
        const std::array<float, 4> baseColor = cld::JsonFloat4Or(params, "baseColor", Float4ToArray(current.baseColorFactor));
        const std::array<float, 4> emissive = cld::JsonFloat4Or(params, "emissive", Float4ToArray(current.emissiveFactor));
        const float roughness = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "roughness", current.roughnessFactor)), 0.02f, 1.0f);
        const float metallic = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "metallic", current.metallicFactor)), 0.0f, 1.0f);
        const float occlusion = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "occlusionStrength", current.occlusionStrength)), 0.0f, 2.0f);
        const float normal = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "normalStrength", current.normalStrength)), 0.0f, 2.0f);
        const float alphaCutoff = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "alphaCutoff", current.alphaCutoff)), 0.0f, 1.0f);
        if (!std::isfinite(roughness) || !std::isfinite(metallic) || !std::isfinite(occlusion) || !std::isfinite(normal) || !std::isfinite(alphaCutoff))
        {
            diagnostics = "Material contains non-finite values.";
            return false;
        }

        if (!validateOnly)
        {
            Bistro::Material& material = m_scene.materials[materialIndex];
            material.baseColorFactor = XMFLOAT4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
            material.emissiveFactor = XMFLOAT4(emissive[0], emissive[1], emissive[2], emissive[3]);
            material.roughnessFactor = roughness;
            material.metallicFactor = metallic;
            material.occlusionStrength = occlusion;
            material.normalStrength = normal;
            material.alphaCutoff = alphaCutoff;
            material.alphaMasked = cld::JsonBoolOr(params, "alphaMasked", material.alphaMasked);
            material.packedOcclusionRoughnessMetallic = cld::JsonBoolOr(params, "packedORM", material.packedOcclusionRoughnessMetallic);
            CreateGpuResourcesForCurrentScene();
            m_projectDirty = true;
        }
        diagnostics = "Material settings accepted.";
        return true;
    }
    if (method == "set_lighting")
    {
        const std::array<float, 3> direction = cld::JsonFloat3Or(params, "direction", { m_lightDirection[0], m_lightDirection[1], m_lightDirection[2] });
        const std::array<float, 3> color = cld::JsonFloat3Or(params, "color", { m_lightColor[0], m_lightColor[1], m_lightColor[2] });
        const std::array<float, 3> skyColor = cld::JsonFloat3Or(params, "skyColor", { m_skyColor[0], m_skyColor[1], m_skyColor[2] });
        const std::array<float, 3> skyHorizonColor = cld::JsonFloat3Or(params, "skyHorizonColor", { m_skyHorizonColor[0], m_skyHorizonColor[1], m_skyHorizonColor[2] });
        const std::array<float, 3> skyZenithColor = cld::JsonFloat3Or(params, "skyZenithColor", { m_skyZenithColor[0], m_skyZenithColor[1], m_skyZenithColor[2] });
        const std::array<float, 3> skyGroundColor = cld::JsonFloat3Or(params, "skyGroundColor", { m_skyGroundColor[0], m_skyGroundColor[1], m_skyGroundColor[2] });
        const float intensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "intensity", m_lightIntensity)), 0.0f, 100.0f);
        const float rayTMin = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "rayTMin", m_rayTMin)), 0.001f, 0.25f);
        const float skyIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "skyIntensity", m_skyIntensity)), 0.0f, 10.0f);
        const float sunIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "sunIntensity", m_sunIntensity)), 0.0f, 50.0f);
        const float sunSize = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "sunSize", m_sunAngularRadius)), 0.001f, 0.08f);
        const float environmentIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "environmentIntensity", m_environmentIntensity)), 0.0f, 10.0f);
        const float environmentRotation = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "environmentRotation", m_environmentRotation)), -3.14159f, 3.14159f);
        const float emissiveIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "emissiveIntensity", m_emissiveLightIntensity)), 0.0f, 30.0f);
        const float areaLightIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "areaLightIntensity", m_proceduralLightIntensity)), 0.0f, 50.0f);
        if (!AllFinite({ direction[0], direction[1], direction[2], color[0], color[1], color[2], skyColor[0], skyColor[1], skyColor[2],
            skyHorizonColor[0], skyHorizonColor[1], skyHorizonColor[2], skyZenithColor[0], skyZenithColor[1], skyZenithColor[2],
            skyGroundColor[0], skyGroundColor[1], skyGroundColor[2], intensity, rayTMin, skyIntensity, sunIntensity, sunSize,
            environmentIntensity, environmentRotation, emissiveIntensity, areaLightIntensity }))
        {
            diagnostics = "Lighting contains non-finite values.";
            return false;
        }

        if (!validateOnly)
        {
            m_lightDirection[0] = direction[0];
            m_lightDirection[1] = direction[1];
            m_lightDirection[2] = direction[2];
            m_lightColor[0] = color[0];
            m_lightColor[1] = color[1];
            m_lightColor[2] = color[2];
            m_lightIntensity = intensity;
            m_rayTMin = rayTMin;
            m_skyEnabled = cld::JsonBoolOr(params, "skyEnabled", m_skyEnabled);
            m_skyColor[0] = skyColor[0]; m_skyColor[1] = skyColor[1]; m_skyColor[2] = skyColor[2];
            m_skyHorizonColor[0] = skyHorizonColor[0]; m_skyHorizonColor[1] = skyHorizonColor[1]; m_skyHorizonColor[2] = skyHorizonColor[2];
            m_skyZenithColor[0] = skyZenithColor[0]; m_skyZenithColor[1] = skyZenithColor[1]; m_skyZenithColor[2] = skyZenithColor[2];
            m_skyGroundColor[0] = skyGroundColor[0]; m_skyGroundColor[1] = skyGroundColor[1]; m_skyGroundColor[2] = skyGroundColor[2];
            m_skyIntensity = skyIntensity;
            m_sunIntensity = sunIntensity;
            m_sunAngularRadius = sunSize;
            m_environmentMapEnabled = cld::JsonBoolOr(params, "environmentEnabled", m_environmentMapEnabled);
            m_environmentIntensity = environmentIntensity;
            m_environmentRotation = environmentRotation;
            m_shadowEnabled = cld::JsonBoolOr(params, "sunNEE", m_shadowEnabled);
            m_skyNeeEnabled = cld::JsonBoolOr(params, "skyNEE", m_skyNeeEnabled);
            m_emissiveLightsEnabled = cld::JsonBoolOr(params, "emissiveTriangleLights", m_emissiveLightsEnabled);
            m_emissiveLightIntensity = emissiveIntensity;
            m_proceduralLightsEnabled = cld::JsonBoolOr(params, "proceduralAreaLights", m_proceduralLightsEnabled);
            m_proceduralLightIntensity = areaLightIntensity;
            ResetRenderingHistory();
            m_projectDirty = true;
        }
        diagnostics = "Lighting settings accepted.";
        return true;
    }
    if (method == "set_path_tracing")
    {
        const int samples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "samplesPerFrame", m_giSamplesPerFrame)), 1, 8);
        const int maxBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxBounces", m_maxPathBounces)), 1, 8);
        const int minBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "minBounces", m_minPathBounces)), 0, maxBounces);
        const PathTracingMode mode = PathtracingModeFromName(cld::JsonStringOr(params, "mode"), m_mode);
        const float radianceClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "radianceClamp", m_giRadianceClamp)), 1.0f, 100.0f);
        const float temporalClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "temporalClamp", m_giTemporalClampScale)), 0.25f, 4.0f);
        const int maxAccumSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxAccumSamples", m_maxAccumulatedFrames)), 1, 4096);
        const int maxAdaptiveSpp = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxAdaptiveSPP", m_maxAdaptiveSamplesPerPixel)), 1, 4);
        const float varianceThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "varianceThreshold", m_adaptiveVarianceThreshold)), 0.02f, 1.0f);
        const float disocclusionBoost = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "disocclusionBoost", m_adaptiveDisocclusionBoost)), 0.0f, 4.0f);
        if (!AllFinite({ radianceClamp, temporalClamp, varianceThreshold, disocclusionBoost }))
        {
            diagnostics = "Path tracing settings contain non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            const bool modeChanged = mode != m_mode;
            m_mode = mode;
            m_giSamplesPerFrame = samples;
            m_maxPathBounces = maxBounces;
            m_minPathBounces = minBounces;
            m_giRadianceClamp = radianceClamp;
            m_giTemporalClampScale = temporalClamp;
            m_maxAccumulatedFrames = maxAccumSamples;
            m_freezeAccumulation = cld::JsonBoolOr(params, "freezeAccumulation", m_freezeAccumulation);
            m_adaptiveSamplingEnabled = cld::JsonBoolOr(params, "adaptiveSampling", m_adaptiveSamplingEnabled);
            m_maxAdaptiveSamplesPerPixel = maxAdaptiveSpp;
            m_adaptiveVarianceThreshold = varianceThreshold;
            m_adaptiveDisocclusionBoost = disocclusionBoost;
            if (modeChanged)
            {
                CreateGpuResourcesForCurrentScene();
            }
            else
            {
                ResetRenderingHistory();
            }
            m_projectDirty = true;
        }
        diagnostics = "Path tracing settings accepted.";
        return true;
    }
    if (method == "set_restir")
    {
        const int spatialPasses = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "spatialReusePasses", m_restirSpatialReusePasses)), 0, 4);
        const int spatialRadius = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "spatialRadius", m_restirSpatialRadius)), 1, 64);
        const int candidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "candidateSamples", m_restirCandidateSamples)), 1, 4);
        const float mClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "mClamp", m_restirMClamp)), 1.0f, 64.0f);
        const int diSpatialPasses = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "diSpatialReusePasses", m_restirDiSpatialReusePasses)), 0, 4);
        const int diCandidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "diCandidateSamples", m_restirDiCandidateSamples)), 1, 4);
        const float diMClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "diMClamp", m_restirDiMClamp)), 1.0f, 64.0f);
        const int reservoirMaxAge = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "reservoirMaxAge", m_reservoirMaxAge)), 1, 32);
        if (!AllFinite({ mClamp, diMClamp }))
        {
            diagnostics = "ReSTIR settings contain non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            m_restirTemporalReuse = cld::JsonBoolOr(params, "temporalReuse", m_restirTemporalReuse);
            m_restirSpatialReusePasses = spatialPasses;
            m_restirSpatialRadius = spatialRadius;
            m_restirCandidateSamples = candidateSamples;
            m_restirMClamp = mClamp;
            m_restirDiTemporalReuse = cld::JsonBoolOr(params, "diTemporalReuse", m_restirDiTemporalReuse);
            m_restirDiSpatialReusePasses = diSpatialPasses;
            m_restirDiCandidateSamples = diCandidateSamples;
            m_restirDiMClamp = diMClamp;
            m_reservoirReprojection = cld::JsonBoolOr(params, "reservoirReprojection", m_reservoirReprojection);
            m_reservoirValidation = cld::JsonBoolOr(params, "reservoirValidation", m_reservoirValidation);
            m_restirGiValidationRay = cld::JsonBoolOr(params, "giValidationRay", m_restirGiValidationRay);
            m_reservoirMaxAge = reservoirMaxAge;
            ResetRenderingHistory();
            m_projectDirty = true;
        }
        diagnostics = "ReSTIR settings accepted.";
        return true;
    }
    if (method == "set_denoise")
    {
        NoisePreset preset = m_noisePreset;
        bool hasPreset = false;
        if (const cld::JsonValue* presetValue = cld::FindMember(params, "preset"))
        {
            if (presetValue->type != cld::JsonValue::Type::String || !TryParseNoisePreset(presetValue->string, preset))
            {
                diagnostics = "Denoise preset is invalid.";
                return false;
            }
            hasPreset = true;
        }

        JitterMode jitterMode = m_jitterMode;
        if (const cld::JsonValue* jitterValue = cld::FindMember(params, "jitterMode"))
        {
            if (jitterValue->type != cld::JsonValue::Type::String || !TryParseJitterMode(jitterValue->string, jitterMode))
            {
                diagnostics = "Denoise jitterMode is invalid.";
                return false;
            }
        }

        const float strength = static_cast<float>(cld::JsonNumberOr(params, "strength", m_denoiserStrength));
        const float temporalAlphaMin = static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMin", m_temporalAlphaMin));
        const float temporalAlphaMax = static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMax", m_temporalAlphaMax));
        const float historyClampSigma = static_cast<float>(cld::JsonNumberOr(params, "historyClampSigma", m_historyClampSigma));
        const float reactiveThreshold = static_cast<float>(cld::JsonNumberOr(params, "reactiveThreshold", m_reactiveThreshold));
        const float specularHistoryScale = static_cast<float>(cld::JsonNumberOr(params, "specularHistoryScale", m_specularHistoryScale));
        const float diffuseFilterStrength = static_cast<float>(cld::JsonNumberOr(params, "diffuseFilterStrength", m_atrousDiffuseStrength));
        const float specularFilterStrength = static_cast<float>(cld::JsonNumberOr(params, "specularFilterStrength", m_atrousSpecularStrength));
        const float varianceScale = static_cast<float>(cld::JsonNumberOr(params, "varianceScale", m_atrousVarianceScale));
        const float normalSigma = static_cast<float>(cld::JsonNumberOr(params, "normalSigma", m_denoiserNormalSigma));
        const float depthSigma = static_cast<float>(cld::JsonNumberOr(params, "depthSigma", m_denoiserDepthSigma));
        const float luminanceSigma = static_cast<float>(cld::JsonNumberOr(params, "luminanceSigma", m_denoiserLuminanceSigma));
        const float albedoSigma = static_cast<float>(cld::JsonNumberOr(params, "albedoSigma", m_denoiserAlbedoSigma));
        const float movingJitterScale = static_cast<float>(cld::JsonNumberOr(params, "movingJitterScale", m_movingJitterScale));
        const float maxHistoryFrames = static_cast<float>(cld::JsonNumberOr(params, "maxHistoryFrames", m_reconstructionMaxHistoryFrames));
        const float spatialIterations = static_cast<float>(cld::JsonNumberOr(params, "spatialIterations", m_denoiserSpatialIterations));
        const float atrousPasses = static_cast<float>(cld::JsonNumberOr(params, "atrousPasses", m_atrousPassCount));
        if (!AllFinite({ strength, temporalAlphaMin, temporalAlphaMax, historyClampSigma, reactiveThreshold, specularHistoryScale,
            diffuseFilterStrength, specularFilterStrength, varianceScale, normalSigma, depthSigma, luminanceSigma, albedoSigma,
            movingJitterScale, maxHistoryFrames, spatialIterations, atrousPasses }))
        {
            diagnostics = "Denoise settings contain non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            if (hasPreset)
            {
                ApplyNoisePreset(preset);
            }
            else
            {
                m_noisePreset = preset;
            }

            m_denoiserEnabled = cld::JsonBoolOr(params, "enabled", m_denoiserEnabled);
            m_splitSignalDenoise = cld::JsonBoolOr(params, "splitSignalDenoise", m_splitSignalDenoise);
            m_realtimeReconstruction = cld::JsonBoolOr(params, "realtimeReconstruction", m_realtimeReconstruction);
            m_cameraJitter = cld::JsonBoolOr(params, "cameraJitter", m_cameraJitter);
            m_temporalStabilityEnabled = cld::JsonBoolOr(params, "temporalStability", m_temporalStabilityEnabled);
            m_jitterMode = jitterMode;
            m_movingJitterScale = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "movingJitterScale", m_movingJitterScale)), 0.0f, 1.0f);
            m_reconstructionMaxHistoryFrames = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxHistoryFrames", m_reconstructionMaxHistoryFrames)), 1, 128);
            m_temporalAlphaMin = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMin", m_temporalAlphaMin)), 0.01f, 0.5f);
            m_temporalAlphaMax = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMax", m_temporalAlphaMax)), m_temporalAlphaMin, 0.8f);
            m_historyClampSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "historyClampSigma", m_historyClampSigma)), 0.5f, 4.0f);
            m_reactiveThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "reactiveThreshold", m_reactiveThreshold)), 0.05f, 1.0f);
            m_specularHistoryScale = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "specularHistoryScale", m_specularHistoryScale)), 0.0f, 1.0f);
            m_denoiserSpatialIterations = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "spatialIterations", m_denoiserSpatialIterations)), 0, 4);
            m_atrousPassCount = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "atrousPasses", m_atrousPassCount)), 0, 5);
            m_atrousDiffuseStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "diffuseFilterStrength", m_atrousDiffuseStrength)), 0.0f, 1.0f);
            m_atrousSpecularStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "specularFilterStrength", m_atrousSpecularStrength)), 0.0f, 1.0f);
            m_atrousVarianceScale = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "varianceScale", m_atrousVarianceScale)), 0.25f, 4.0f);
            m_denoiserNormalSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "normalSigma", m_denoiserNormalSigma)), 0.05f, 1.0f);
            m_denoiserDepthSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "depthSigma", m_denoiserDepthSigma)), 0.002f, 0.10f);
            m_denoiserLuminanceSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "luminanceSigma", m_denoiserLuminanceSigma)), 0.1f, 8.0f);
            m_denoiserAlbedoSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "albedoSigma", m_denoiserAlbedoSigma)), 0.05f, 1.0f);
            m_denoiserStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "strength", m_denoiserStrength)), 0.0f, 1.0f);
            if (cld::JsonBoolOr(params, "resetHistory", true))
            {
                ResetRenderingHistory();
            }
            else
            {
                ResetAccumulation();
            }
            m_projectDirty = true;
        }
        diagnostics = "Denoise settings accepted.";
        return true;
    }
    if (method == "set_view")
    {
        const int debugView = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "debugView", m_debugViewMode)), 0, 40);
        const bool changesEnvironment = cld::FindMember(params, "environmentEnabled") != nullptr;
        if (!validateOnly)
        {
            m_debugViewMode = debugView;
            m_debugNormalMapYFlip = cld::JsonBoolOr(params, "normalMapYFlip", m_debugNormalMapYFlip);
            m_environmentMapEnabled = cld::JsonBoolOr(params, "environmentEnabled", m_environmentMapEnabled);
            if (changesEnvironment)
            {
                ResetRenderingHistory();
            }
            else
            {
                ResetAccumulation();
            }
            m_projectDirty = true;
        }
        diagnostics = "View settings accepted.";
        return true;
    }
    diagnostics = "Unsupported action method.";
    return false;
}

mcp::ToolResult D3D12PathTracingBackend::CallMcpTool(const std::string& name, const cld::JsonValue& arguments, int timeoutMs)
{
    if (name == "lookdevpt.get_stats")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Renderer stats returned.", m_mcpStatsJson);
    }
    if (name == "lookdevpt.get_state")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Renderer state returned.", m_mcpStateJson);
    }
    if (name == "lookdevpt.list_materials")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Material list returned.", m_mcpMaterialsJson);
    }
    if (name == "lookdevpt.capture_viewport")
    {
        mcp::CommandRequest request;
        request.toolName = name;
        request.actionMethod = "__capture_viewport";
        request.params = arguments;
        request.validateOnly = false;
        request.mutation = false;
        request.summary = "Capture current viewport PNG.";
        request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        mcp::SubmitResult submitted = m_mcpDispatcher.Submit(std::move(request), false);
        if (!submitted.accepted)
        {
            return MakeMcpJsonToolResult(false, submitted.diagnostics, "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(submitted.diagnostics) + "\"}");
        }
        if (submitted.future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
        {
            m_mcpDispatcher.Cancel(submitted.id, "MCP capture timed out.");
            return MakeMcpJsonToolResult(false, "MCP capture timed out.", "{\"ok\":false,\"diagnostics\":\"MCP capture timed out.\"}");
        }
        const mcp::CommandResult result = submitted.future.get();
        mcp::ToolResult tool;
        tool.ok = result.ok;
        tool.isError = !result.ok;
        tool.text = result.diagnostics;
        tool.structuredJson = result.structuredJson;
        if (result.ok)
        {
            std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
            tool.contentJson = "[{\"type\":\"text\",\"text\":\"" + cld::EscapeJson(result.diagnostics) + "\"},{\"type\":\"image\",\"data\":\"" + m_mcpLatestCaptureBase64 + "\",\"mimeType\":\"image/png\"},{\"type\":\"resource_link\",\"uri\":\"lookdevpt://captures/latest.png\",\"name\":\"latest_capture\",\"mimeType\":\"image/png\"}]";
        }
        return tool;
    }
    if (name == "lookdevpt.validate_action")
    {
        const std::string method = cld::JsonStringOr(arguments, "method");
        const cld::JsonValue* params = cld::FindMember(arguments, "params");
        if (method.empty() || !params || params->type != cld::JsonValue::Type::Object)
        {
            return MakeMcpJsonToolResult(false, "validate_action requires method and object params.", "{\"ok\":false,\"diagnostics\":\"validate_action requires method and object params.\"}");
        }
        return SubmitMcpActionTool(name, method, *params, true, timeoutMs);
    }

    constexpr const char* prefix = "lookdevpt.";
    if (name.rfind(prefix, 0) == 0)
    {
        const std::string actionMethod = name.substr(std::char_traits<char>::length(prefix));
        if (actionMethod == "set_scene" || actionMethod == "set_camera" || actionMethod == "set_material" ||
            actionMethod == "set_lighting" || actionMethod == "set_path_tracing" || actionMethod == "set_restir" ||
            actionMethod == "set_denoise" || actionMethod == "set_view")
        {
            return SubmitMcpActionTool(name, actionMethod, arguments, false, timeoutMs);
        }
    }

    return MakeMcpJsonToolResult(false, "Unknown MCP tool.", "{\"ok\":false,\"diagnostics\":\"Unknown MCP tool.\"}");
}

mcp::ResourceResult D3D12PathTracingBackend::ReadMcpResource(const std::string& uri)
{
    mcp::ResourceResult result;
    result.uri = uri;
    if (uri == "lookdevpt://actions/schema")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = mcp::BuildActionsSchemaJson();
        return result;
    }

    std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
    if (uri == "lookdevpt://state")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpStateJson;
        return result;
    }
    if (uri == "lookdevpt://stats")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpStatsJson;
        return result;
    }
    if (uri == "lookdevpt://materials")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpMaterialsJson;
        return result;
    }
    if (uri == "lookdevpt://captures/latest.png")
    {
        if (m_mcpLatestCaptureBase64.empty())
        {
            result.error = m_mcpLastCaptureDiagnostics;
            return result;
        }
        result.ok = true;
        result.mimeType = "image/png";
        result.blob = m_mcpLatestCaptureBase64;
        return result;
    }

    result.error = "Unknown MCP resource URI.";
    return result;
}

size_t D3D12PathTracingBackend::PendingMcpCommandCount() const
{
    return m_mcpDispatcher.PendingCount();
}

void D3D12PathTracingBackend::LoadMcpUserSettings()
{
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        m_mcpSettings = mcp::ServerSettings{};
    }
    mcp::ServerSettings loadedSettings;
    bool shouldSaveSettings = false;
    const std::wstring path = McpSettingsPath();
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
    {
        shouldSaveSettings = true;
    }
    if (file)
    {
        std::stringstream buffer;
        buffer << file.rdbuf();
        try
        {
            const cld::JsonValue root = cld::JsonParser(buffer.str()).Parse();
            if (root.type == cld::JsonValue::Type::Object)
            {
                loadedSettings.port = static_cast<uint16_t>(std::clamp(static_cast<int>(cld::JsonNumberOr(root, "port", loadedSettings.port)), 1, 65535));
                loadedSettings.requestTimeoutSeconds = std::clamp(static_cast<int>(cld::JsonNumberOr(root, "requestTimeoutSeconds", loadedSettings.requestTimeoutSeconds)), 5, 300);
                loadedSettings.accessMode = mcp::AccessModeFromName(cld::JsonStringOr(root, "accessMode"), loadedSettings.accessMode);
                loadedSettings.token = cld::JsonStringOr(root, "token", loadedSettings.token);
            }
        }
        catch (const std::exception& ex)
        {
            m_mcpUiDiagnostics = std::string("MCP settings ignored: ") + ex.what();
        }
    }
    if (loadedSettings.token.empty())
    {
        loadedSettings.token = mcp::GenerateToken();
        shouldSaveSettings = true;
    }
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        m_mcpSettings = loadedSettings;
    }
    if (shouldSaveSettings)
    {
        SaveMcpUserSettings();
    }
}

void D3D12PathTracingBackend::SaveMcpUserSettings()
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        settings = m_mcpSettings;
    }
    const std::filesystem::path path(McpSettingsPath());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        m_mcpUiDiagnostics = "Failed to save MCP settings.";
        return;
    }

    file << "{\n";
    file << "  \"port\": " << settings.port << ",\n";
    file << "  \"requestTimeoutSeconds\": " << settings.requestTimeoutSeconds << ",\n";
    file << "  \"accessMode\": \"" << mcp::AccessModeName(settings.accessMode) << "\",\n";
    file << "  \"token\": \"" << cld::EscapeJson(settings.token) << "\"\n";
    file << "}\n";
}

void D3D12PathTracingBackend::StartMcpServer()
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        if (m_mcpSettings.token.empty())
        {
            m_mcpSettings.token = mcp::GenerateToken();
        }
        settings = m_mcpSettings;
    }
    SaveMcpUserSettings();
    if (m_mcpServer.Start(settings, this))
    {
        m_mcpUiDiagnostics = "MCP server started.";
    }
    else
    {
        m_mcpUiDiagnostics = m_mcpServer.GetStatus().lastError;
    }
}

void D3D12PathTracingBackend::StopMcpServer()
{
    m_mcpDispatcher.CancelAll("MCP server stopped.");
    if (m_mcpServer.IsRunning())
    {
        m_mcpServer.Stop();
        m_mcpUiDiagnostics = "MCP server stopped.";
    }
}

void D3D12PathTracingBackend::ProcessMcpCommands()
{
    m_mcpDispatcher.ProcessOne([this](const mcp::CommandRequest& request)
    {
        mcp::CommandResult result;
        try
        {
            if (request.actionMethod == "__capture_viewport")
            {
                std::string base64Png;
                std::string diagnostics;
                result.ok = CaptureViewportPng(base64Png, diagnostics);
                result.diagnostics = diagnostics;
                if (result.ok)
                {
                    {
                        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                        m_mcpLatestCaptureBase64 = std::move(base64Png);
                        m_mcpLastCaptureDiagnostics = diagnostics;
                    }
                    result.structuredJson = "{\"ok\":true,\"diagnostics\":\"" + cld::EscapeJson(diagnostics) + "\",\"resource\":\"lookdevpt://captures/latest.png\",\"mimeType\":\"image/png\"}";
                }
                else
                {
                    result.structuredJson = "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(diagnostics) + "\"}";
                }
                return result;
            }

            std::string diagnostics;
            const bool ok = ApplyAction(request.actionMethod, request.params, diagnostics, request.validateOnly);
            UpdateMcpSnapshots();
            result.ok = ok;
            result.diagnostics = diagnostics;
            result.structuredJson = "{\"ok\":" + std::string(ok ? "true" : "false") +
                ",\"action\":\"" + cld::EscapeJson(request.actionMethod) +
                "\",\"validateOnly\":" + std::string(request.validateOnly ? "true" : "false") +
                ",\"diagnostics\":\"" + cld::EscapeJson(diagnostics) +
                "\",\"stats\":" + BuildMcpStatsJson() + "}";
        }
        catch (const std::exception& ex)
        {
            result.ok = false;
            result.diagnostics = ex.what();
            result.structuredJson = "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(result.diagnostics) + "\"}";
        }
        return result;
    });
}

void D3D12PathTracingBackend::UpdateMcpSnapshots()
{
    const std::string stateJson = BuildMcpStateJson();
    const std::string statsJson = BuildMcpStatsJson();
    const std::string materialsJson = BuildMcpMaterialsJson();
    std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
    m_mcpStateJson = stateJson;
    m_mcpStatsJson = statsJson;
    m_mcpMaterialsJson = materialsJson;
}

std::string D3D12PathTracingBackend::BuildMcpStateJson() const
{
    XMFLOAT3 camera = m_camera.GetPosition();
    std::ostringstream out;
    out << "{";
    out << "\"scenePath\":\"" << cld::EscapeJson(WideToUtf8(m_scenePath)) << "\",";
    out << "\"environmentPath\":\"" << cld::EscapeJson(WideToUtf8(m_environmentTexturePath)) << "\",";
    out << "\"projectPath\":\"" << cld::EscapeJson(WideToUtf8(m_projectPath)) << "\",";
    out << "\"projectDirty\":" << (m_projectDirty ? "true" : "false") << ",";
    out << "\"camera\":{\"position\":";
    AppendJsonFloat3(out, camera);
    out << ",\"yaw\":" << m_camera.GetYawRadians() << ",\"pitch\":" << m_camera.GetPitchRadians() << "},";
    out << "\"lighting\":{\"direction\":";
    AppendJsonFloat3(out, m_lightDirection);
    out << ",\"color\":";
    AppendJsonFloat3(out, m_lightColor);
    out << ",\"intensity\":" << m_lightIntensity << ",\"rayTMin\":" << m_rayTMin
        << ",\"skyEnabled\":" << (m_skyEnabled ? "true" : "false") << ",\"skyIntensity\":" << m_skyIntensity
        << ",\"sunIntensity\":" << m_sunIntensity << ",\"sunSize\":" << m_sunAngularRadius
        << ",\"environmentEnabled\":" << (m_environmentMapEnabled ? "true" : "false")
        << ",\"environmentIntensity\":" << m_environmentIntensity << ",\"environmentRotation\":" << m_environmentRotation
        << ",\"emissiveTriangleLights\":" << (m_emissiveLightsEnabled ? "true" : "false")
        << ",\"proceduralAreaLights\":" << (m_proceduralLightsEnabled ? "true" : "false") << "},";
    out << "\"pathTracing\":{\"mode\":\"" << PathtracingModeName(m_mode) << "\",\"samplesPerFrame\":" << m_giSamplesPerFrame
        << ",\"maxBounces\":" << m_maxPathBounces << ",\"minBounces\":" << m_minPathBounces
        << ",\"radianceClamp\":" << m_giRadianceClamp << ",\"maxAccumSamples\":" << m_maxAccumulatedFrames
        << ",\"freezeAccumulation\":" << (m_freezeAccumulation ? "true" : "false")
        << ",\"adaptiveSampling\":" << (m_adaptiveSamplingEnabled ? "true" : "false") << "},";
    out << "\"restir\":{\"temporalReuse\":" << (m_restirTemporalReuse ? "true" : "false")
        << ",\"spatialReusePasses\":" << m_restirSpatialReusePasses << ",\"spatialRadius\":" << m_restirSpatialRadius
        << ",\"candidateSamples\":" << m_restirCandidateSamples << ",\"mClamp\":" << m_restirMClamp
        << ",\"diTemporalReuse\":" << (m_restirDiTemporalReuse ? "true" : "false")
        << ",\"diSpatialReusePasses\":" << m_restirDiSpatialReusePasses
        << ",\"diCandidateSamples\":" << m_restirDiCandidateSamples << ",\"diMClamp\":" << m_restirDiMClamp
        << ",\"reservoirReprojection\":" << (m_reservoirReprojection ? "true" : "false")
        << ",\"reservoirValidation\":" << (m_reservoirValidation ? "true" : "false")
        << ",\"giValidationRay\":" << (m_restirGiValidationRay ? "true" : "false")
        << ",\"reservoirMaxAge\":" << m_reservoirMaxAge << "},";
    out << "\"denoise\":{\"preset\":\"" << NoisePresetName(m_noisePreset) << "\",\"presetLabel\":\"" << NoisePresetDisplayName(m_noisePreset)
        << "\",\"enabled\":" << (m_denoiserEnabled ? "true" : "false")
        << ",\"splitSignalDenoise\":" << (m_splitSignalDenoise ? "true" : "false")
        << ",\"realtimeReconstruction\":" << (m_realtimeReconstruction ? "true" : "false")
        << ",\"cameraJitter\":" << (m_cameraJitter ? "true" : "false")
        << ",\"temporalStability\":" << (m_temporalStabilityEnabled ? "true" : "false")
        << ",\"jitterMode\":\"" << JitterModeName(m_jitterMode) << "\""
        << ",\"movingJitterScale\":" << m_movingJitterScale
        << ",\"currentJitterStrength\":" << m_currentJitterStrength
        << ",\"cameraMotionAmount\":" << m_cameraMotionAmount
        << ",\"temporalHistoryValid\":" << (m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested ? "true" : "false")
        << ",\"maxHistoryFrames\":" << m_reconstructionMaxHistoryFrames
        << ",\"historyClampSigma\":" << m_historyClampSigma << ",\"reactiveThreshold\":" << m_reactiveThreshold
        << ",\"spatialIterations\":" << m_denoiserSpatialIterations << ",\"atrousPasses\":" << m_atrousPassCount
        << ",\"strength\":" << m_denoiserStrength << "},";
    out << "\"view\":{\"debugView\":" << m_debugViewMode << ",\"normalMapYFlip\":" << (m_debugNormalMapYFlip ? "true" : "false") << "}";
    out << "}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpStatsJson() const
{
    uint64_t primitiveCount = 0;
    uint64_t submittedIndexCount = 0;
    for (const Bistro::DrawItem& draw : m_scene.draws)
    {
        submittedIndexCount += draw.indexCount;
        primitiveCount += draw.indexCount / 3;
    }

    std::ostringstream out;
    out << "{";
    out << "\"api\":\"Direct3D 12 DXR\",";
    out << "\"adapter\":\"" << cld::EscapeJson(WideToUtf8(m_adapterDescription)) << "\",";
    out << "\"dxrTier\":\"" << cld::EscapeJson(WideToUtf8(PathtracingTierName(m_raytracingTier))) << "\",";
    out << "\"resolution\":{\"width\":" << m_width << ",\"height\":" << m_height << "},";
    out << "\"materials\":" << m_scene.materials.size() << ",";
    out << "\"textures\":" << m_textures.size() << ",";
    out << "\"vertices\":" << m_scene.vertices.size() << ",";
    out << "\"indices\":" << m_scene.indices.size() << ",";
    out << "\"submittedIndices\":" << submittedIndexCount << ",";
    out << "\"primitives\":" << primitiveCount << ",";
    out << "\"blasGeometries\":" << m_geometryRecords.size() << ",";
    out << "\"tlasInstances\":1,";
    out << "\"lights\":" << m_activeLightCount << ",";
    out << "\"emissiveTriangleLights\":" << m_emissiveTriangleLightCount << ",";
    out << "\"proceduralAreaLights\":" << m_proceduralAreaLightCount << ",";
    out << "\"samplesAccumulated\":" << m_accumulatedFrames << ",";
    out << "\"activeMode\":\"" << PathtracingModeName(m_mode) << "\",";
    out << "\"reservoirCount\":" << m_restirReservoirElementCount << ",";
    out << "\"denoiser\":{\"preset\":\"" << NoisePresetName(m_noisePreset) << "\",\"enabled\":" << (m_denoiserEnabled ? "true" : "false")
        << ",\"temporalStability\":" << (m_temporalStabilityEnabled ? "true" : "false")
        << ",\"historyValid\":" << (m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested ? "true" : "false")
        << ",\"jitterMode\":\"" << JitterModeName(m_jitterMode) << "\",\"jitterStrength\":" << m_currentJitterStrength
        << ",\"spatialIterations\":" << m_denoiserSpatialIterations << ",\"atrousPasses\":" << m_atrousPassCount << "},";
    out << "\"mcp\":{\"running\":" << (m_mcpServer.IsRunning() ? "true" : "false") << ",\"pendingCommands\":" << m_mcpDispatcher.PendingCount() << "}";
    out << "}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpMaterialsJson() const
{
    std::ostringstream out;
    out << "{\"materials\":[";
    for (size_t i = 0; i < m_scene.materials.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        const Bistro::Material& material = m_scene.materials[i];
        out << "{\"index\":" << i << ",\"name\":\"" << cld::EscapeJson(WideToUtf8(material.name)) << "\",";
        out << "\"baseColor\":";
        AppendJsonFloat4(out, material.baseColorFactor);
        out << ",\"emissive\":";
        AppendJsonFloat4(out, material.emissiveFactor);
        out << ",\"roughness\":" << material.roughnessFactor << ",\"metallic\":" << material.metallicFactor;
        out << ",\"occlusionStrength\":" << material.occlusionStrength << ",\"normalStrength\":" << material.normalStrength;
        out << ",\"alphaCutoff\":" << material.alphaCutoff << ",\"alphaMasked\":" << (material.alphaMasked ? "true" : "false");
        out << ",\"packedORM\":" << (material.packedOcclusionRoughnessMetallic ? "true" : "false") << "}";
    }
    out << "]}";
    return out.str();
}

mcp::ToolResult D3D12PathTracingBackend::SubmitMcpActionTool(const std::string& toolName, const std::string& actionMethod, const cld::JsonValue& params, bool validateOnly, int timeoutMs)
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        settings = m_mcpSettings;
    }

    const bool mutation = !validateOnly;
    if (mutation && settings.accessMode == mcp::AccessMode::ReadOnly)
    {
        return MakeMcpJsonToolResult(false, "MCP mutation rejected because access mode is Read Only.", "{\"ok\":false,\"diagnostics\":\"MCP mutation rejected because access mode is Read Only.\"}");
    }

    mcp::CommandRequest request;
    request.toolName = toolName;
    request.actionMethod = actionMethod;
    request.params = params;
    request.validateOnly = validateOnly;
    request.mutation = mutation;
    request.summary = (validateOnly ? "Validate " : "Apply ") + actionMethod + " " + cld::JsonValueToJson(params);
    request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    const bool requiresApproval = mutation && settings.accessMode == mcp::AccessMode::ConfirmMutations;
    mcp::SubmitResult submitted = m_mcpDispatcher.Submit(std::move(request), requiresApproval);
    if (!submitted.accepted)
    {
        return MakeMcpJsonToolResult(false, submitted.diagnostics, "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(submitted.diagnostics) + "\"}");
    }

    if (submitted.future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
    {
        m_mcpDispatcher.Cancel(submitted.id, "MCP request timed out.");
        return MakeMcpJsonToolResult(false, "MCP request timed out.", "{\"ok\":false,\"diagnostics\":\"MCP request timed out.\"}");
    }

    const mcp::CommandResult result = submitted.future.get();
    return MakeMcpJsonToolResult(result.ok, result.diagnostics, result.structuredJson);
}

mcp::ToolResult D3D12PathTracingBackend::MakeMcpJsonToolResult(bool ok, const std::string& text, const std::string& structuredJson) const
{
    mcp::ToolResult result;
    result.ok = ok;
    result.isError = !ok;
    result.text = text;
    result.structuredJson = structuredJson.empty() ? "{}" : structuredJson;
    return result;
}

bool D3D12PathTracingBackend::CaptureViewportPng(std::string& base64Png, std::string& diagnostics)
{
    if (!m_PathtracingOutput || !m_commandQueue || !m_device)
    {
        diagnostics = "Path tracing output is not ready.";
        return false;
    }

    try
    {
        WaitForPreviousFrame();
        const D3D12_RESOURCE_DESC sourceDesc = m_PathtracingOutput->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &layout, &rowCount, &rowSizeInBytes, &totalBytes);
        (void)rowCount;
        (void)rowSizeInBytes;

        ComPtr<ID3D12Resource> readback;
        const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&readback)));

        ThrowIfFailed(m_commandAllocator->Reset());
        ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));
        auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &toCopySource);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = m_PathtracingOutput.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        auto backToUav = CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &backToUav);
        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* commandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
        WaitForPreviousFrame();

        void* mapped = nullptr;
        const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalBytes) };
        ThrowIfFailed(readback->Map(0, &readRange, &mapped));
        const uint8_t* sourceBytes = static_cast<const uint8_t*>(mapped) + layout.Offset;
        const size_t width = static_cast<size_t>(sourceDesc.Width);
        const size_t height = static_cast<size_t>(sourceDesc.Height);
        std::vector<uint8_t> pixels(width * height * 4);
        for (size_t y = 0; y < height; ++y)
        {
            const float* sourceRow = reinterpret_cast<const float*>(sourceBytes + y * layout.Footprint.RowPitch);
            uint8_t* destinationRow = pixels.data() + y * width * 4;
            for (size_t x = 0; x < width; ++x)
            {
                const float r = std::clamp(sourceRow[x * 4 + 0], 0.0f, 1.0f);
                const float g = std::clamp(sourceRow[x * 4 + 1], 0.0f, 1.0f);
                const float b = std::clamp(sourceRow[x * 4 + 2], 0.0f, 1.0f);
                const float a = std::clamp(sourceRow[x * 4 + 3], 0.0f, 1.0f);
                destinationRow[x * 4 + 0] = static_cast<uint8_t>(std::lround(r * 255.0f));
                destinationRow[x * 4 + 1] = static_cast<uint8_t>(std::lround(g * 255.0f));
                destinationRow[x * 4 + 2] = static_cast<uint8_t>(std::lround(b * 255.0f));
                destinationRow[x * 4 + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
            }
        }
        const D3D12_RANGE writtenRange = { 0, 0 };
        readback->Unmap(0, &writtenRange);

        DirectX::Image pngImage = {};
        pngImage.width = width;
        pngImage.height = height;
        pngImage.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        pngImage.rowPitch = width * 4;
        pngImage.slicePitch = pixels.size();
        pngImage.pixels = pixels.data();

        DirectX::Blob pngBlob;
        ThrowIfFailed(DirectX::SaveToWICMemory(pngImage, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, pngBlob));
        base64Png = Base64Encode(pngBlob.GetConstBufferPointer(), pngBlob.GetBufferSize());
        diagnostics = "Viewport captured.";
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

void D3D12PathTracingBackend::CreateDescriptorHeap()
{
    m_descriptorCount = DescriptorTextureBase + static_cast<UINT>(m_scene.materials.size()) * TextureSlotCount + 1u;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = m_descriptorCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap)));
}

void D3D12PathTracingBackend::CreateOutputResources()
{
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto createDefaultTexture = [&](const D3D12_RESOURCE_DESC& desc, ID3D12Resource** resource)
    {
        ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(resource)));
    };

    D3D12_RESOURCE_DESC outputDesc = CD3DX12_RESOURCE_DESC::Tex2D(BackBufferFormat, m_width, m_height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    createDefaultTexture(outputDesc, &m_PathtracingOutput);
    m_PathtracingOutput->SetName(L"PathtracingOutput");

    D3D12_RESOURCE_DESC accumulationDesc = CD3DX12_RESOURCE_DESC::Tex2D(AccumulationFormat, m_width, m_height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    createDefaultTexture(accumulationDesc, &m_accumulationOutput);
    m_accumulationOutput->SetName(L"PathtracingAccumulation");
    createDefaultTexture(accumulationDesc, &m_denoiseAov0);
    m_denoiseAov0->SetName(L"PathtracingDenoiseAov0");
    createDefaultTexture(accumulationDesc, &m_denoiseAov1);
    m_denoiseAov1->SetName(L"PathtracingDenoiseAov1");
    createDefaultTexture(accumulationDesc, &m_denoiseAov2);
    m_denoiseAov2->SetName(L"PathtracingDenoiseAov2");
    createDefaultTexture(accumulationDesc, &m_reconstructionHistoryRadiance);
    m_reconstructionHistoryRadiance->SetName(L"PathtracingReconstructionHistoryRadiance");
    createDefaultTexture(accumulationDesc, &m_reconstructionHistoryMoments);
    m_reconstructionHistoryMoments->SetName(L"PathtracingReconstructionHistoryMoments");
    createDefaultTexture(accumulationDesc, &m_reconstructionHistoryLength);
    m_reconstructionHistoryLength->SetName(L"PathtracingReconstructionHistoryLength");
    createDefaultTexture(accumulationDesc, &m_previousDenoiseAov0);
    m_previousDenoiseAov0->SetName(L"PathtracingPreviousDenoiseAov0");
    createDefaultTexture(accumulationDesc, &m_previousDenoiseAov1);
    m_previousDenoiseAov1->SetName(L"PathtracingPreviousDenoiseAov1");
    createDefaultTexture(accumulationDesc, &m_previousDenoiseAov2);
    m_previousDenoiseAov2->SetName(L"PathtracingPreviousDenoiseAov2");
    createDefaultTexture(accumulationDesc, &m_signalCurrentRadiance);
    m_signalCurrentRadiance->SetName(L"PathtracingSignalCurrentRadiance");
    createDefaultTexture(accumulationDesc, &m_signalDirect);
    m_signalDirect->SetName(L"PathtracingSignalDirect");
    createDefaultTexture(accumulationDesc, &m_signalIndirect);
    m_signalIndirect->SetName(L"PathtracingSignalIndirect");
    createDefaultTexture(accumulationDesc, &m_signalResidual);
    m_signalResidual->SetName(L"PathtracingSignalResidual");
    createDefaultTexture(accumulationDesc, &m_denoisePing);
    m_denoisePing->SetName(L"PathtracingDenoisePing");
    createDefaultTexture(accumulationDesc, &m_denoisePong);
    m_denoisePong->SetName(L"PathtracingDenoisePong");

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav = {};
    outputUav.Format = BackBufferFormat;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_PathtracingOutput.Get(), nullptr, &outputUav, CpuDescriptor(DescriptorOutputUav));

    D3D12_UNORDERED_ACCESS_VIEW_DESC accumulationUav = {};
    accumulationUav.Format = AccumulationFormat;
    accumulationUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_accumulationOutput.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorAccumulationUav));
    m_device->CreateUnorderedAccessView(m_denoiseAov0.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorDenoiseAov0Uav));
    m_device->CreateUnorderedAccessView(m_denoiseAov1.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorDenoiseAov1Uav));
    m_device->CreateUnorderedAccessView(m_denoiseAov2.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorDenoiseAov2Uav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryRadiance.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorReconstructionHistoryRadianceUav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryMoments.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorReconstructionHistoryMomentsUav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryLength.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorReconstructionHistoryLengthUav));
    m_device->CreateUnorderedAccessView(m_previousDenoiseAov0.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorPreviousDenoiseAov0Uav));
    m_device->CreateUnorderedAccessView(m_previousDenoiseAov1.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorPreviousDenoiseAov1Uav));
    m_device->CreateUnorderedAccessView(m_previousDenoiseAov2.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorPreviousDenoiseAov2Uav));
    m_device->CreateUnorderedAccessView(m_signalCurrentRadiance.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorSignalCurrentRadianceUav));
    m_device->CreateUnorderedAccessView(m_signalDirect.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorSignalDirectUav));
    m_device->CreateUnorderedAccessView(m_signalIndirect.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorSignalIndirectUav));
    m_device->CreateUnorderedAccessView(m_signalResidual.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorSignalResidualUav));
    m_device->CreateUnorderedAccessView(m_denoisePing.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorDenoisePingUav));
    m_device->CreateUnorderedAccessView(m_denoisePong.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorDenoisePongUav));

    m_restirReservoirElementCount = (std::max)(1u, m_width * m_height);
    m_restirReservoirBufferSize = static_cast<UINT64>(m_restirReservoirElementCount) * RestirReservoirStride;
    m_restirReservoirCurrent = CreateUavBuffer(m_restirReservoirBufferSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RestirReservoirCurrent");
    m_restirReservoirHistory = CreateUavBuffer(m_restirReservoirBufferSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RestirReservoirHistory");
    m_restirReservoirSpatial = CreateUavBuffer(m_restirReservoirBufferSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RestirReservoirSpatial");
    m_restirDiReservoirCurrent = CreateUavBuffer(m_restirReservoirBufferSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RestirDiReservoirCurrent");
    m_restirDiReservoirHistory = CreateUavBuffer(m_restirReservoirBufferSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RestirDiReservoirHistory");
    m_restirDiReservoirSpatial = CreateUavBuffer(m_restirReservoirBufferSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RestirDiReservoirSpatial");

    D3D12_UNORDERED_ACCESS_VIEW_DESC reservoirUav = {};
    reservoirUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    reservoirUav.Buffer.NumElements = m_restirReservoirElementCount;
    reservoirUav.Buffer.StructureByteStride = RestirReservoirStride;
    m_device->CreateUnorderedAccessView(m_restirReservoirCurrent.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirCurrentUav));
    m_device->CreateUnorderedAccessView(m_restirReservoirHistory.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirHistoryUav));
    m_device->CreateUnorderedAccessView(m_restirReservoirSpatial.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirSpatialUav));
    m_device->CreateUnorderedAccessView(m_restirDiReservoirCurrent.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirDiCurrentUav));
    m_device->CreateUnorderedAccessView(m_restirDiReservoirHistory.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirDiHistoryUav));
    m_device->CreateUnorderedAccessView(m_restirDiReservoirSpatial.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirDiSpatialUav));
}

void D3D12PathTracingBackend::CreateGlobalRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 23, 0, 0);
    CD3DX12_DESCRIPTOR_RANGE sceneBufferRange;
    sceneBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE textureRange;
    textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<UINT>(m_scene.materials.size()) * TextureSlotCount + 1u, 0, 1);

    CD3DX12_ROOT_PARAMETER rootParameters[RootParameterCount];
    rootParameters[RootOutputTable].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootAccelerationStructure].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootSceneConstants].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootSceneBuffers].InitAsDescriptorTable(1, &sceneBufferRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootTextureTable].InitAsDescriptorTable(1, &textureRange, D3D12_SHADER_VISIBILITY_ALL);

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 8;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(_countof(rootParameters), rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr) && error)
    {
        OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
    }
    ThrowIfFailed(hr);
    ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSignature)));
}

void D3D12PathTracingBackend::CreateSceneBuffers()
{
    m_vertexBuffer = CreateDefaultBuffer(m_scene.vertices.data(), m_scene.vertices.size() * sizeof(Bistro::Vertex), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtVertices");
    m_indexBuffer = CreateDefaultBuffer(m_scene.indices.data(), m_scene.indices.size() * sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtIndices");
    m_geometryBuffer = CreateDefaultBuffer(m_geometryRecords.data(), m_geometryRecords.size() * sizeof(Bistro::RtGeometryRecord), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtGeometryRecords");
    m_lightBuffer = CreateDefaultBuffer(m_lights.data(), m_lights.size() * sizeof(Bistro::RtLight), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtLights");

    D3D12_SHADER_RESOURCE_VIEW_DESC vertexSrv = {};
    vertexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    vertexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vertexSrv.Buffer.NumElements = static_cast<UINT>(m_scene.vertices.size());
    vertexSrv.Buffer.StructureByteStride = sizeof(Bistro::Vertex);
    m_device->CreateShaderResourceView(m_vertexBuffer.Get(), &vertexSrv, CpuDescriptor(DescriptorVertexBuffer));

    D3D12_SHADER_RESOURCE_VIEW_DESC indexSrv = {};
    indexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    indexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    indexSrv.Buffer.NumElements = static_cast<UINT>(m_scene.indices.size());
    indexSrv.Buffer.StructureByteStride = sizeof(uint32_t);
    m_device->CreateShaderResourceView(m_indexBuffer.Get(), &indexSrv, CpuDescriptor(DescriptorIndexBuffer));

    D3D12_SHADER_RESOURCE_VIEW_DESC geometrySrv = {};
    geometrySrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    geometrySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    geometrySrv.Buffer.NumElements = static_cast<UINT>(m_geometryRecords.size());
    geometrySrv.Buffer.StructureByteStride = sizeof(Bistro::RtGeometryRecord);
    m_device->CreateShaderResourceView(m_geometryBuffer.Get(), &geometrySrv, CpuDescriptor(DescriptorGeometryBuffer));

    D3D12_SHADER_RESOURCE_VIEW_DESC lightSrv = {};
    lightSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    lightSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lightSrv.Buffer.NumElements = static_cast<UINT>(m_lights.size());
    lightSrv.Buffer.StructureByteStride = sizeof(Bistro::RtLight);
    m_device->CreateShaderResourceView(m_lightBuffer.Get(), &lightSrv, CpuDescriptor(DescriptorLightBuffer));

    const UINT constantSize = CalculateConstantBufferByteSize(sizeof(SceneConstantBuffer));
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto constantDesc = CD3DX12_RESOURCE_DESC::Buffer(constantSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &constantDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_sceneConstantBuffer)));
    ThrowIfFailed(m_sceneConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedSceneConstants)));
}

UINT D3D12PathTracingBackend::CreateTextureResource(const std::wstring& path, bool srgb, const uint8_t fallback[4], std::map<std::wstring, UINT>& cache)
{
    std::wstring key = path.empty() ? (std::wstring(L"fallback:") + std::to_wstring(fallback[0]) + L"," + std::to_wstring(fallback[1]) + L"," + std::to_wstring(fallback[2]) + L"," + std::to_wstring(fallback[3]) + (srgb ? L":srgb" : L":linear")) : path + (srgb ? L":srgb" : L":linear");
    auto found = cache.find(key);
    if (found != cache.end())
    {
        return found->second;
    }

    Bistro::TextureData image = Bistro::LoadTextureD3D12(path, srgb, fallback);
    GpuTexture texture;
    texture.path = path;
    texture.format = image.format;
    texture.width = image.width;
    texture.height = image.height;
    texture.mipLevels = image.mipLevels;
    texture.fallback = image.fallback;
    if (image.mipLevels > 0xffffu)
    {
        throw std::runtime_error("Texture has too many mip levels for a D3D12 Texture2D resource.");
    }
    const UINT16 textureMipLevels = static_cast<UINT16>(image.mipLevels);
    D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(image.format, image.width, image.height, 1, textureMipLevels);
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource)));
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.resource.Get(), 0, image.mipLevels);
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture.upload)));

    std::vector<D3D12_SUBRESOURCE_DATA> subresources(image.mipLevels);
    for (uint32_t mipIndex = 0; mipIndex < image.mipLevels; ++mipIndex)
    {
        const Bistro::TextureMip& mip = image.mips[mipIndex];
        subresources[mipIndex].pData = image.pixels.data() + mip.offset;
        subresources[mipIndex].RowPitch = static_cast<LONG_PTR>(mip.rowPitch);
        subresources[mipIndex].SlicePitch = static_cast<LONG_PTR>(mip.slicePitch);
    }
    UpdateSubresources(m_commandList.Get(), texture.resource.Get(), texture.upload.Get(), 0, 0, image.mipLevels, subresources.data());
    auto textureReadyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_commandList->ResourceBarrier(1, &textureReadyBarrier);

    const UINT index = static_cast<UINT>(m_textures.size());
    m_textures.push_back(texture);
    cache[key] = index;
    return index;
}

void D3D12PathTracingBackend::CreateTextures()
{
    const uint8_t white[] = { 255, 255, 255, 255 };
    const uint8_t normal[] = { 128, 128, 255, 255 };
    const uint8_t roughness[] = { 122, 122, 122, 255 };
    const uint8_t metallic[] = { 0, 0, 0, 255 };
    const uint8_t black[] = { 0, 0, 0, 255 };
    const uint8_t environmentFallback[] = { 35, 68, 110, 255 };
    std::map<std::wstring, UINT> cache;
    m_materialTextureIndices.resize(m_scene.materials.size());
    m_rtMaterials.resize(m_scene.materials.size());

    for (size_t materialIndex = 0; materialIndex < m_scene.materials.size(); ++materialIndex)
    {
        const Bistro::Material& material = m_scene.materials[materialIndex];
        auto& indices = m_materialTextureIndices[materialIndex];
        indices[Bistro::TextureSlotBaseColor] = CreateTextureResource(material.textures[Bistro::TextureSlotBaseColor], true, white, cache);
        indices[Bistro::TextureSlotNormal] = CreateTextureResource(material.textures[Bistro::TextureSlotNormal], false, normal, cache);
        indices[Bistro::TextureSlotRoughness] = CreateTextureResource(material.textures[Bistro::TextureSlotRoughness], false, roughness, cache);
        indices[Bistro::TextureSlotMetallic] = CreateTextureResource(material.textures[Bistro::TextureSlotMetallic], false, metallic, cache);
        indices[Bistro::TextureSlotOcclusion] = CreateTextureResource(material.textures[Bistro::TextureSlotOcclusion], false, white, cache);
        indices[Bistro::TextureSlotEmissive] = CreateTextureResource(material.textures[Bistro::TextureSlotEmissive], true, black, cache);

        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            const GpuTexture& texture = m_textures[indices[slot]];
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = texture.format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = texture.mipLevels;
            srvDesc.Texture2D.MostDetailedMip = 0;
            m_device->CreateShaderResourceView(texture.resource.Get(), &srvDesc, CpuDescriptor(DescriptorTextureBase + static_cast<UINT>(materialIndex) * TextureSlotCount + slot));
        }

        Bistro::RtMaterial rtMaterial{};
        rtMaterial.baseColorFactor = material.baseColorFactor;
        rtMaterial.emissiveFactor = material.emissiveFactor;
        rtMaterial.textureBaseIndex = static_cast<uint32_t>(materialIndex * TextureSlotCount);
        rtMaterial.alphaMasked = material.alphaMasked ? 1u : 0u;
        rtMaterial.alphaCutoff = material.alphaCutoff;
        rtMaterial.normalStrength = material.normalStrength;
        rtMaterial.roughnessFactor = material.roughnessFactor;
        rtMaterial.metallicFactor = material.metallicFactor;
        rtMaterial.occlusionStrength = material.occlusionStrength;
        rtMaterial.packedOcclusionRoughnessMetallic = material.packedOcclusionRoughnessMetallic ? 1u : 0u;
        m_rtMaterials[materialIndex] = rtMaterial;
    }

    const UINT environmentTexture = CreateTextureResource(m_environmentTexturePath, false, environmentFallback, cache);
    m_environmentDescriptorIndex = static_cast<UINT>(m_scene.materials.size()) * TextureSlotCount;
    const GpuTexture& environment = m_textures[environmentTexture];
    D3D12_SHADER_RESOURCE_VIEW_DESC environmentSrv = {};
    environmentSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    environmentSrv.Format = environment.format;
    environmentSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    environmentSrv.Texture2D.MipLevels = environment.mipLevels;
    environmentSrv.Texture2D.MostDetailedMip = 0;
    m_device->CreateShaderResourceView(environment.resource.Get(), &environmentSrv, CpuDescriptor(DescriptorTextureBase + m_environmentDescriptorIndex));

    m_materialBuffer = CreateDefaultBuffer(m_rtMaterials.data(), m_rtMaterials.size() * sizeof(Bistro::RtMaterial), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtMaterials");
    D3D12_SHADER_RESOURCE_VIEW_DESC materialSrv = {};
    materialSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    materialSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    materialSrv.Buffer.NumElements = static_cast<UINT>(m_rtMaterials.size());
    materialSrv.Buffer.StructureByteStride = sizeof(Bistro::RtMaterial);
    m_device->CreateShaderResourceView(m_materialBuffer.Get(), &materialSrv, CpuDescriptor(DescriptorMaterialBuffer));
}

void D3D12PathTracingBackend::CreatePathtracingStateObject()
{
    const std::wstring exeDir = GetAssetFullPath(L"");
    byte* shaderData = nullptr;
    UINT shaderSize = 0;
    ThrowIfFailed(ReadDataFromFile((exeDir + ShaderFileName()).c_str(), &shaderData, &shaderSize));
    std::vector<UINT8> shader(shaderData, shaderData + shaderSize);
    free(shaderData);

    CD3DX12_STATE_OBJECT_DESC pipeline(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
    auto library = pipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE shaderBytecode = CD3DX12_SHADER_BYTECODE(shader.data(), shader.size());
    library->SetDXILLibrary(&shaderBytecode);
    library->DefineExport(RayGenShaderName);
    library->DefineExport(MissShaderName);
    library->DefineExport(ShadowMissShaderName);
    library->DefineExport(ClosestHitShaderName);
    library->DefineExport(AnyHitShaderName);
    library->DefineExport(ShadowAnyHitShaderName);

    auto hitGroup = pipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetHitGroupExport(HitGroupName);
    hitGroup->SetClosestHitShaderImport(ClosestHitShaderName);
    hitGroup->SetAnyHitShaderImport(AnyHitShaderName);
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    auto shadowHitGroup = pipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    shadowHitGroup->SetHitGroupExport(ShadowHitGroupName);
    shadowHitGroup->SetAnyHitShaderImport(ShadowAnyHitShaderName);
    shadowHitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    auto shaderConfig = pipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    shaderConfig->Config(192, sizeof(XMFLOAT2));

    auto globalRootSignature = pipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_globalRootSignature.Get());

    auto pipelineConfig = pipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipelineConfig->Config(MaxTraceRecursionDepth());

    ThrowIfFailed(m_device->CreateStateObject(pipeline, IID_PPV_ARGS(&m_stateObject)));
    ThrowIfFailed(m_stateObject.As(&m_stateObjectProperties));
}

void D3D12PathTracingBackend::CreateRestirReusePipeline()
{
    if (!UsesRestirReuse(m_mode))
    {
        return;
    }

    const std::wstring exeDir = GetAssetFullPath(L"");
    byte* shaderData = nullptr;
    UINT shaderSize = 0;
    ThrowIfFailed(ReadDataFromFile((exeDir + RestirReuseShaderFileName()).c_str(), &shaderData, &shaderSize));
    std::vector<UINT8> shader(shaderData, shaderData + shaderSize);
    free(shaderData);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_globalRootSignature.Get();
    desc.CS = CD3DX12_SHADER_BYTECODE(shader.data(), shader.size());
    ThrowIfFailed(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_restirReusePipeline)));

    if (m_mode == PathTracingMode::ReSTIRCombined)
    {
        shaderData = nullptr;
        shaderSize = 0;
        ThrowIfFailed(ReadDataFromFile((exeDir + RestirDiReuseShaderFileName()).c_str(), &shaderData, &shaderSize));
        std::vector<UINT8> diShader(shaderData, shaderData + shaderSize);
        free(shaderData);
        desc.CS = CD3DX12_SHADER_BYTECODE(diShader.data(), diShader.size());
        ThrowIfFailed(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_restirDiReusePipeline)));
    }
}

void D3D12PathTracingBackend::CreateDenoisePipeline()
{
    const std::wstring exeDir = GetAssetFullPath(L"");
    auto createPipeline = [&](const std::wstring& fileName, ComPtr<ID3D12PipelineState>& pipeline)
    {
        byte* shaderData = nullptr;
        UINT shaderSize = 0;
        ThrowIfFailed(ReadDataFromFile((exeDir + fileName).c_str(), &shaderData, &shaderSize));
        std::vector<UINT8> shader(shaderData, shaderData + shaderSize);
        free(shaderData);

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_globalRootSignature.Get();
        desc.CS = CD3DX12_SHADER_BYTECODE(shader.data(), shader.size());
        ThrowIfFailed(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };

    createPipeline(L"PathTracingDenoiseTemporal.cso", m_denoiseTemporalPipeline);
    for (UINT i = 0; i < DenoiseAtrousPipelineCount; ++i)
    {
        createPipeline(L"PathTracingDenoiseAtrous" + std::to_wstring(i) + L".cso", m_denoiseAtrousPipelines[i]);
    }
    createPipeline(L"PathTracingDenoiseComposite.cso", m_denoiseCompositePipeline);
}

void D3D12PathTracingBackend::BuildAccelerationStructures()
{
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    geometryDescs.reserve(m_scene.draws.size());
    for (const Bistro::DrawItem& draw : m_scene.draws)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        desc.Triangles.VertexBuffer.StartAddress = m_vertexBuffer->GetGPUVirtualAddress();
        desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Bistro::Vertex);
        desc.Triangles.VertexCount = static_cast<UINT>(m_scene.vertices.size());
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress() + draw.startIndex * sizeof(uint32_t);
        desc.Triangles.IndexCount = draw.indexCount;
        desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        desc.Flags = m_scene.materials[draw.materialIndex].alphaMasked ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometryDescs.push_back(desc);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomInputs = {};
    bottomInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottomInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottomInputs.NumDescs = static_cast<UINT>(geometryDescs.size());
    bottomInputs.pGeometryDescs = geometryDescs.data();
    bottomInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&bottomInputs, &bottomInfo);
    if (bottomInfo.ResultDataMaxSizeInBytes == 0)
    {
        throw std::runtime_error("Failed to query BLAS size.");
    }

    m_bottomLevelAs.scratch = CreateUavBuffer(bottomInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, L"BLAS Scratch");
    m_bottomLevelAs.result = CreateUavBuffer(bottomInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"BLAS");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomBuild = {};
    bottomBuild.Inputs = bottomInputs;
    bottomBuild.ScratchAccelerationStructureData = m_bottomLevelAs.scratch->GetGPUVirtualAddress();
    bottomBuild.DestAccelerationStructureData = m_bottomLevelAs.result->GetGPUVirtualAddress();
    m_commandList->BuildRaytracingAccelerationStructure(&bottomBuild, 0, nullptr);
    auto bottomAsBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_bottomLevelAs.result.Get());
    m_commandList->ResourceBarrier(1, &bottomAsBarrier);

    D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
    instanceDesc.Transform[0][0] = 1.0f;
    instanceDesc.Transform[1][1] = 1.0f;
    instanceDesc.Transform[2][2] = 1.0f;
    instanceDesc.InstanceMask = 0xff;
    instanceDesc.AccelerationStructure = m_bottomLevelAs.result->GetGPUVirtualAddress();
    m_topLevelAs.instanceDesc = CreateUploadBuffer(&instanceDesc, sizeof(instanceDesc), L"TLAS Instance");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInputs = {};
    topInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topInputs.NumDescs = 1;
    topInputs.InstanceDescs = m_topLevelAs.instanceDesc->GetGPUVirtualAddress();
    topInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&topInputs, &topInfo);
    if (topInfo.ResultDataMaxSizeInBytes == 0)
    {
        throw std::runtime_error("Failed to query TLAS size.");
    }
    m_topLevelAs.scratch = CreateUavBuffer(topInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, L"TLAS Scratch");
    m_topLevelAs.result = CreateUavBuffer(topInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"TLAS");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild = {};
    topBuild.Inputs = topInputs;
    topBuild.ScratchAccelerationStructureData = m_topLevelAs.scratch->GetGPUVirtualAddress();
    topBuild.DestAccelerationStructureData = m_topLevelAs.result->GetGPUVirtualAddress();
    m_commandList->BuildRaytracingAccelerationStructure(&topBuild, 0, nullptr);
    auto topAsBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_topLevelAs.result.Get());
    m_commandList->ResourceBarrier(1, &topAsBarrier);
}

void D3D12PathTracingBackend::CreateShaderTables()
{
    const UINT shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    const UINT recordSize = Align(shaderIdentifierSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

    auto createTable = [&](const wchar_t* name, std::initializer_list<const wchar_t*> exports, ShaderTableInfo& table)
    {
        table.recordSize = recordSize;
        table.recordCount = static_cast<UINT>(exports.size());
        const UINT64 bufferSize = static_cast<UINT64>(recordSize) * table.recordCount;
        table.resource = CreateUploadBuffer(nullptr, bufferSize, name);
        UINT8* mapped = nullptr;
        ThrowIfFailed(table.resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        UINT index = 0;
        for (const wchar_t* exportName : exports)
        {
            void* identifier = m_stateObjectProperties->GetShaderIdentifier(exportName);
            if (!identifier)
            {
                throw std::runtime_error("Failed to resolve a Path Tracing shader identifier.");
            }
            memcpy(mapped + index * recordSize, identifier, shaderIdentifierSize);
            ++index;
        }
        table.resource->Unmap(0, nullptr);
    };

    createTable(L"RayGen Shader Table", { RayGenShaderName }, m_rayGenTable);
    createTable(L"Miss Shader Table", { MissShaderName, ShadowMissShaderName }, m_missTable);
    createTable(L"HitGroup Shader Table", { HitGroupName, ShadowHitGroupName }, m_hitGroupTable);
}

void D3D12PathTracingBackend::OnUpdate()
{
    if (m_resizePending)
    {
        m_resizePending = false;
        Resize(m_pendingResizeWidth, m_pendingResizeHeight);
    }

    if (m_pendingFileDialog != PendingFileDialog::None)
    {
        const PendingFileDialog dialog = m_pendingFileDialog;
        m_pendingFileDialog = PendingFileDialog::None;

        if (dialog == PendingFileDialog::OpenScene)
        {
            LogDiagnostic("Opening scene file dialog.");
            const std::wstring path = OpenPathDialog(L"Scene Files\0*.gltf;*.glb;*.fbx;*.obj\0All Files\0*.*\0");
            if (!path.empty())
            {
                m_pendingScenePath = path;
                m_sceneDiagnostics = "Scene load queued.";
            }
        }
        else if (dialog == PendingFileDialog::OpenEnvironment)
        {
            LogDiagnostic("Opening environment file dialog.");
            const std::wstring path = OpenPathDialog(L"Environment Images\0*.hdr;*.dds;*.png;*.jpg;*.jpeg;*.tga\0All Files\0*.*\0");
            if (!path.empty())
            {
                m_pendingEnvironmentPath = path;
                m_projectDiagnostics = "Environment load queued.";
            }
        }
        else if (dialog == PendingFileDialog::OpenProject)
        {
            LogDiagnostic("Opening project file dialog.");
            const std::wstring path = OpenPathDialog(L"LookDevPT Project\0*.lookdevpt.json\0JSON\0*.json\0All Files\0*.*\0");
            if (!path.empty())
            {
                m_pendingProjectPath = path;
                m_projectDiagnostics = "Project load queued.";
            }
        }
        else if (dialog == PendingFileDialog::SaveProjectAs)
        {
            LogDiagnostic("Opening project save dialog.");
            const std::wstring path = SavePathDialog(L"LookDevPT Project\0*.lookdevpt.json\0JSON\0*.json\0All Files\0*.*\0", L"lookdevpt.json");
            if (!path.empty())
            {
                SaveProjectToDisk(path);
            }
        }
    }

    if (!m_pendingProjectPath.empty())
    {
        const std::wstring path = std::move(m_pendingProjectPath);
        m_pendingProjectPath.clear();
        std::string diagnostics;
        LogDiagnostic(L"Pending project load: " + path);
        if (!LoadProjectFromDisk(path, diagnostics))
        {
            m_projectDiagnostics = diagnostics;
            LogDiagnostic("Pending project load failed: " + diagnostics);
        }
        else
        {
            LogDiagnostic("Pending project load succeeded: " + diagnostics);
        }
    }
    if (!m_pendingScenePath.empty())
    {
        const std::wstring path = std::move(m_pendingScenePath);
        m_pendingScenePath.clear();
        std::string diagnostics;
        LogDiagnostic(L"Pending scene load: " + path);
        if (!LoadScenePath(path, diagnostics))
        {
            m_sceneDiagnostics = diagnostics;
            LogDiagnostic("Pending scene load failed: " + diagnostics);
        }
        else
        {
            LogDiagnostic("Pending scene load succeeded: " + diagnostics);
        }
    }
    if (!m_pendingEnvironmentPath.empty())
    {
        const std::wstring path = std::move(m_pendingEnvironmentPath);
        m_pendingEnvironmentPath.clear();
        std::string diagnostics;
        LogDiagnostic(L"Pending environment load: " + path);
        if (!LoadEnvironmentPath(path, diagnostics))
        {
            m_projectDiagnostics = diagnostics;
            LogDiagnostic("Pending environment load failed: " + diagnostics);
        }
    }
    if (m_pendingGpuResourceRefresh)
    {
        m_pendingGpuResourceRefresh = false;
        LogDiagnostic("Pending GPU resource refresh.");
        WaitForPreviousFrame();
        CreateGpuResourcesForCurrentScene();
        m_projectDirty = true;
    }

    ProcessMcpCommands();

    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = std::chrono::duration<float>(now - m_lastUpdate).count();
    m_lastUpdate = now;
    m_camera.SetActive(GetForegroundWindow() == Win32Application::GetHwnd());
    m_camera.Update(deltaSeconds);
    UpdateConstantBuffer(deltaSeconds);
    UpdateMcpSnapshots();
}

void D3D12PathTracingBackend::UpdateConstantBuffer(float)
{
    if (HasAccumulationStateChanged())
    {
        ResetAccumulation();
    }

    const float aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
    XMMATRIX view = m_camera.GetViewMatrix();
    XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspectRatio, 0.1f, 10000.0f);
    XMMATRIX viewProjection = view * projection;
    XMMATRIX inverseViewProjection = XMMatrixInverse(nullptr, viewProjection);

    UpdateCameraMotionState();

    const XMFLOAT4X4 previousViewProjection = m_previousViewProjection;
    float jitterStrength = 1.0f;
    if (m_temporalStabilityEnabled)
    {
        const float stopBlend = std::clamp(static_cast<float>(m_framesSinceCameraMotion) / 4.0f, 0.0f, 1.0f);
        jitterStrength = m_cameraMotionAmount > 0.001f ? m_movingJitterScale : std::lerp(m_movingJitterScale, 1.0f, stopBlend);
    }
    if (!m_cameraJitter || m_jitterMode == JitterMode::Off)
    {
        jitterStrength = 0.0f;
    }
    m_currentJitterStrength = std::clamp(jitterStrength, 0.0f, 1.0f);

    uint32_t jitterIndex = (m_frameCounter & 1023u) + 1u;
    if (m_jitterMode == JitterMode::Stable16)
    {
        jitterIndex = (m_frameCounter & 15u) + 1u;
    }
    const XMFLOAT2 baseJitter(Halton(jitterIndex, 2) - 0.5f, Halton(jitterIndex, 3) - 0.5f);
    m_currentJitter = m_currentJitterStrength > 0.0f
        ? XMFLOAT2(baseJitter.x * m_currentJitterStrength, baseJitter.y * m_currentJitterStrength)
        : XMFLOAT2(0.0f, 0.0f);
    const bool temporalHistoryValid = m_temporalStabilityEnabled && m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested;

    SceneConstantBuffer constants{};
    XMStoreFloat4x4(&constants.inverseViewProjection, inverseViewProjection);
    XMStoreFloat4x4(&constants.viewProjection, viewProjection);
    constants.previousViewProjection = m_hasPreviousViewProjection ? previousViewProjection : constants.viewProjection;
    XMFLOAT3 cameraPosition = m_camera.GetPosition();
    constants.cameraPosition = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
    XMFLOAT3 lightDirection = NormalizeFloat3(m_lightDirection);
    constants.lightDirection = XMFLOAT4(lightDirection.x, lightDirection.y, lightDirection.z, 0.0f);
    constants.lightColor = XMFLOAT4(m_lightColor[0], m_lightColor[1], m_lightColor[2], m_lightIntensity);
    constants.debugOptions = XMFLOAT4(static_cast<float>(m_debugViewMode), m_debugNormalMapYFlip ? 1.0f : 0.0f, m_shadowEnabled ? 1.0f : 0.0f, m_skyNeeEnabled ? 1.0f : 0.0f);
    constants.skyColor = XMFLOAT4(m_skyColor[0], m_skyColor[1], m_skyColor[2], m_skyIntensity);
    constants.skyHorizonColor = XMFLOAT4(m_skyHorizonColor[0], m_skyHorizonColor[1], m_skyHorizonColor[2], 0.0f);
    constants.skyZenithColor = XMFLOAT4(m_skyZenithColor[0], m_skyZenithColor[1], m_skyZenithColor[2], 0.0f);
    constants.skyGroundColor = XMFLOAT4(m_skyGroundColor[0], m_skyGroundColor[1], m_skyGroundColor[2], 0.0f);
    constants.skyOptions = XMFLOAT4(m_sunIntensity, m_sunAngularRadius, m_skyGroundBlend, m_skyEnabled ? 1.0f : 0.0f);
    constants.rayOptions = XMFLOAT4(m_rayTMin, m_rayTMax, static_cast<float>(m_width), static_cast<float>(m_height));
    constants.frameOptions = XMFLOAT4(static_cast<float>(m_accumulatedFrames), static_cast<float>(m_maxAccumulatedFrames), m_freezeAccumulation ? 1.0f : 0.0f, static_cast<float>(m_frameCounter));
    constants.giOptions = XMFLOAT4(static_cast<float>(m_giSamplesPerFrame), m_giRadianceClamp, m_giTemporalClampScale, m_giTemporalClampMin);
    constants.pathOptions = XMFLOAT4(static_cast<float>(m_maxPathBounces), static_cast<float>(m_minPathBounces), static_cast<float>(m_restirCandidateSamples), UsesRestirReuse(m_mode) ? 1.0f : 0.0f);
    constants.restirOptions = XMFLOAT4(m_restirTemporalReuse ? 1.0f : 0.0f, static_cast<float>(m_restirSpatialReusePasses), static_cast<float>(m_restirSpatialRadius), m_restirMClamp);
    const bool combinedRestir = m_mode == PathTracingMode::ReSTIRCombined;
    constants.restirDiOptions = XMFLOAT4(
        (combinedRestir ? m_restirDiTemporalReuse : m_restirTemporalReuse) ? 1.0f : 0.0f,
        static_cast<float>(combinedRestir ? m_restirDiSpatialReusePasses : m_restirSpatialReusePasses),
        static_cast<float>(combinedRestir ? m_restirDiCandidateSamples : m_restirCandidateSamples),
        combinedRestir ? m_restirDiMClamp : m_restirMClamp);
    constants.lightOptions = XMFLOAT4(static_cast<float>(m_activeLightCount), m_emissiveLightsEnabled ? m_emissiveLightIntensity : 0.0f, m_proceduralLightsEnabled ? m_proceduralLightIntensity : 0.0f, static_cast<float>(m_environmentDescriptorIndex));
    constants.environmentOptions = XMFLOAT4(m_environmentMapEnabled ? 1.0f : 0.0f, m_environmentIntensity, m_environmentRotation, 0.0f);
    constants.denoiseOptions = XMFLOAT4(m_denoiserEnabled ? 1.0f : 0.0f, static_cast<float>(m_denoiserSpatialIterations), m_denoiserNormalSigma, m_denoiserDepthSigma);
    constants.denoiseOptions2 = XMFLOAT4(m_denoiserLuminanceSigma, m_denoiserAlbedoSigma, m_denoiserStrength, 0.0f);
    constants.jitterOptions = XMFLOAT4(m_currentJitter.x, m_currentJitter.y, m_previousJitter.x, m_previousJitter.y);
    constants.reconstructionOptions = XMFLOAT4(m_realtimeReconstruction ? 1.0f : 0.0f, static_cast<float>(m_reconstructionMaxHistoryFrames), m_temporalAlphaMin, m_temporalAlphaMax);
    constants.validationOptions = XMFLOAT4(m_validationNormalDotThreshold, m_validationDepthRelativeThreshold, m_validationAlbedoThreshold, m_validationRoughnessThreshold);
    constants.atrousOptions = XMFLOAT4(static_cast<float>(m_atrousPassCount), m_atrousDiffuseStrength, m_atrousSpecularStrength, m_atrousVarianceScale);
    constants.adaptiveOptions = XMFLOAT4(m_adaptiveSamplingEnabled ? 1.0f : 0.0f, static_cast<float>(m_maxAdaptiveSamplesPerPixel), m_adaptiveVarianceThreshold, m_adaptiveDisocclusionBoost);
    constants.restirStabilityOptions = XMFLOAT4(m_reservoirReprojection ? 1.0f : 0.0f, m_reservoirValidation ? 1.0f : 0.0f, m_restirGiValidationRay ? 1.0f : 0.0f, static_cast<float>(m_reservoirMaxAge));
    constants.signalDenoiseOptions = XMFLOAT4(m_splitSignalDenoise ? 1.0f : 0.0f, m_historyClampSigma, m_reactiveThreshold, m_specularHistoryScale);
    constants.denoisePassOptions = XMFLOAT4(0.0f, static_cast<float>(m_atrousPassCount), 0.0f, 0.0f);
    constants.stabilityOptions = XMFLOAT4(m_temporalStabilityEnabled ? 1.0f : 0.0f, m_cameraMotionAmount, m_currentJitterStrength, temporalHistoryValid ? 1.0f : 0.0f);
    memcpy(m_mappedSceneConstants, &constants, sizeof(constants));

    XMStoreFloat4x4(&m_previousViewProjection, viewProjection);
    m_hasPreviousViewProjection = true;
    m_previousJitter = m_currentJitter;
    m_resetDenoiseHistoryRequested = false;
    m_denoiseHistoryValid = true;

    if (!m_freezeAccumulation)
    {
        m_accumulatedFrames = (std::min)(m_accumulatedFrames + 1u, static_cast<uint32_t>((std::max)(m_maxAccumulatedFrames, 1)));
        ++m_frameCounter;
    }
}

void D3D12PathTracingBackend::OnRender()
{
    if (m_minimized)
    {
        return;
    }

    BuildUI();
    PopulateCommandList();
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    const UINT syncInterval = m_vsyncEnabled ? 1 : 0;
    const UINT presentFlags = (!m_vsyncEnabled && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    ThrowIfFailed(m_swapChain->Present(syncInterval, presentFlags));
    WaitForPreviousFrame();
}

void D3D12PathTracingBackend::PopulateCommandList()
{
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    DispatchRays();
    RunRestirReusePass();
    RunDenoisePass();
    CopyOutputToBackBuffer();

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
    auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &toRenderTarget);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    ID3D12DescriptorHeap* imguiHeaps[] = { m_imguiDescriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(imguiHeaps), imguiHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());

    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &toPresent);
    ThrowIfFailed(m_commandList->Close());
}

void D3D12PathTracingBackend::DispatchRays()
{
    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, GpuDescriptor(DescriptorOutputUav));
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
    m_commandList->SetPipelineState1(m_stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenTable.resource->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenTable.recordSize;
    dispatchDesc.MissShaderTable.StartAddress = m_missTable.resource->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = m_missTable.recordSize * m_missTable.recordCount;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missTable.recordSize;
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupTable.resource->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupTable.recordSize * m_hitGroupTable.recordCount;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupTable.recordSize;
    dispatchDesc.Width = m_width;
    dispatchDesc.Height = m_height;
    dispatchDesc.Depth = 1;
    m_commandList->DispatchRays(&dispatchDesc);
    D3D12_RESOURCE_BARRIER uavBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_accumulationOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_denoiseAov0.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_denoiseAov1.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_denoiseAov2.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalCurrentRadiance.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalResidual.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_restirReservoirCurrent.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_restirDiReservoirCurrent.Get())
    };
    m_commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);
}

void D3D12PathTracingBackend::RunRestirReusePass()
{
    if (!UsesRestirReuse(m_mode) || !m_restirReusePipeline)
    {
        return;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, GpuDescriptor(DescriptorOutputUav));
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
    m_commandList->SetPipelineState(m_restirReusePipeline.Get());
    m_commandList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    auto restirSpatialBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_restirReservoirSpatial.Get());
    m_commandList->ResourceBarrier(1, &restirSpatialBarrier);
    D3D12_RESOURCE_BARRIER toCopy[] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(m_restirReservoirSpatial.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_restirReservoirHistory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)
    };
    m_commandList->ResourceBarrier(_countof(toCopy), toCopy);
    m_commandList->CopyBufferRegion(m_restirReservoirHistory.Get(), 0, m_restirReservoirSpatial.Get(), 0, m_restirReservoirBufferSize);

    D3D12_RESOURCE_BARRIER afterCopy[] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(m_restirReservoirSpatial.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(m_restirReservoirHistory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    m_commandList->ResourceBarrier(_countof(afterCopy), afterCopy);

    if (m_mode == PathTracingMode::ReSTIRCombined && m_restirDiReusePipeline)
    {
        m_commandList->SetPipelineState(m_restirDiReusePipeline.Get());
        m_commandList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

        auto restirDiSpatialBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_restirDiReservoirSpatial.Get());
        m_commandList->ResourceBarrier(1, &restirDiSpatialBarrier);
        D3D12_RESOURCE_BARRIER diToCopy[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(m_restirDiReservoirSpatial.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_restirDiReservoirHistory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)
        };
        m_commandList->ResourceBarrier(_countof(diToCopy), diToCopy);
        m_commandList->CopyBufferRegion(m_restirDiReservoirHistory.Get(), 0, m_restirDiReservoirSpatial.Get(), 0, m_restirReservoirBufferSize);

        D3D12_RESOURCE_BARRIER diAfterCopy[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(m_restirDiReservoirSpatial.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(m_restirDiReservoirHistory.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        m_commandList->ResourceBarrier(_countof(diAfterCopy), diAfterCopy);
    }
}

void D3D12PathTracingBackend::RunDenoisePass()
{
    if (!m_denoiseTemporalPipeline || !m_denoiseCompositePipeline || (!m_denoiserEnabled && m_debugViewMode < 16))
    {
        return;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, GpuDescriptor(DescriptorOutputUav));
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));

    const UINT dispatchX = (m_width + 7) / 8;
    const UINT dispatchY = (m_height + 7) / 8;
    m_commandList->SetPipelineState(m_denoiseTemporalPipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);
    D3D12_RESOURCE_BARRIER temporalBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryRadiance.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryMoments.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryLength.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePing.Get())
    };
    m_commandList->ResourceBarrier(_countof(temporalBarriers), temporalBarriers);

    const UINT atrousPasses = (std::min)(static_cast<UINT>((std::max)(m_atrousPassCount, 0)), DenoiseAtrousPipelineCount);
    for (UINT pass = 0; pass < atrousPasses; ++pass)
    {
        if (!m_denoiseAtrousPipelines[pass])
        {
            break;
        }
        m_commandList->SetPipelineState(m_denoiseAtrousPipelines[pass].Get());
        m_commandList->Dispatch(dispatchX, dispatchY, 1);
        D3D12_RESOURCE_BARRIER atrousBarriers[] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePing.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePong.Get())
        };
        m_commandList->ResourceBarrier(_countof(atrousBarriers), atrousBarriers);
    }

    m_commandList->SetPipelineState(m_denoiseCompositePipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_previousDenoiseAov0.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_previousDenoiseAov1.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_previousDenoiseAov2.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePing.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePong.Get())
    };
    m_commandList->ResourceBarrier(_countof(barriers), barriers);
}

void D3D12PathTracingBackend::CopyOutputToBackBuffer()
{
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
    };
    m_commandList->ResourceBarrier(_countof(barriers), barriers);
    m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_PathtracingOutput.Get());
    auto outputBackToUav = CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_commandList->ResourceBarrier(1, &outputBackToUav);
}

void D3D12PathTracingBackend::BuildUI()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Project"))
        {
            if (ImGui::MenuItem("Open Scene..."))
            {
                m_pendingFileDialog = PendingFileDialog::OpenScene;
            }
            if (ImGui::MenuItem("Open Environment..."))
            {
                m_pendingFileDialog = PendingFileDialog::OpenEnvironment;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open Project..."))
            {
                m_pendingFileDialog = PendingFileDialog::OpenProject;
            }
            if (ImGui::MenuItem("Save Project", nullptr, false, !m_projectPath.empty()))
            {
                SaveProjectToDisk(m_projectPath);
            }
            if (ImGui::MenuItem("Save Project As..."))
            {
                m_pendingFileDialog = PendingFileDialog::SaveProjectAs;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    SubmitMainDockSpace();

    const char* modeItems[] = { "Baseline PT", "ReSTIR GI", "ReSTIR DI", "ReSTIR GI + DI" };
    const char* debugModes[] =
    {
        "Final", "Base Color", "World Normal", "Normal Texture", "Roughness", "Metallic", "Emissive",
        "Hit Distance", "Direct NEE", "Indirect", "Bounce Count", "Accumulation Samples", "Sky",
        "Reservoir Weight", "Temporal Reuse", "Spatial Reuse", "Current Radiance", "Temporal Output",
        "History Length", "Variance", "Motion Vector", "Disocclusion Mask", "Denoised Indirect",
        "Denoise Delta", "Reservoir Age", "Reservoir Validity", "GI Reservoir Weight",
        "DI Reservoir Weight", "GI Temporal Reuse", "DI Temporal Reuse", "GI Spatial Reuse",
        "DI Spatial Reuse", "Direct Signal", "Indirect Signal", "Residual Signal", "Temporal Input",
        "Temporal Output Detail", "A-Trous Output", "Reactive Mask", "History Match", "History Confidence"
    };

    ImGui::SetNextWindowPos(ImVec2(24.0f, 48.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 340.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Viewport");
    ImGui::PushItemWidth(250.0f);
    ImGui::TextUnformatted("D3D12LookDevPT");
    if (!m_scenePath.empty())
    {
        ImGui::TextWrapped("Scene: %s", WideToUtf8(std::filesystem::path(m_scenePath).filename().wstring()).c_str());
    }
    else
    {
        ImGui::TextUnformatted("Scene: Preview cube");
    }
    ImGui::TextWrapped("%s", m_sceneDiagnostics.c_str());
    if (!m_projectDiagnostics.empty())
    {
        ImGui::TextWrapped("%s%s", m_projectDiagnostics.c_str(), m_projectDirty ? " *" : "");
    }
    if (ImGui::Button("Open Scene"))
    {
        m_pendingFileDialog = PendingFileDialog::OpenScene;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open HDRI"))
    {
        m_pendingFileDialog = PendingFileDialog::OpenEnvironment;
    }
    int modeIndex = static_cast<int>(m_mode);
    if (ImGui::Combo("Render Mode", &modeIndex, modeItems, _countof(modeItems)))
    {
        m_mode = static_cast<PathTracingMode>(std::clamp(modeIndex, 0, 3));
        m_pendingGpuResourceRefresh = true;
        m_projectDirty = true;
    }
    static int displayResolutionPreset = 1;
    const char* displayResolutionItems[] = { "720p (1280 x 720)", "1080p (1920 x 1080)", "4K (3840 x 2160)" };
    if (ImGui::Combo("Display Resolution", &displayResolutionPreset, displayResolutionItems, static_cast<int>(sizeof(displayResolutionItems) / sizeof(displayResolutionItems[0]))))
    {
        ApplyBistroDisplayResolution(Win32Application::GetHwnd(), displayResolutionPreset);
    }
    if (ImGui::Combo("Debug View", &m_debugViewMode, debugModes, _countof(debugModes))) ResetAccumulation();
    if (ImGui::Checkbox("Normal Map Y Flip", &m_debugNormalMapYFlip)) ResetAccumulation();
    if (ImGui::Button("Reset Accumulation")) ResetAccumulation();
    ImGui::PopItemWidth();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(400.0f, 48.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 380.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene");
    ImGui::PushItemWidth(240.0f);
    ImGui::Text("Meshes: %zu", m_scene.draws.size());
    ImGui::Text("Materials: %zu", m_scene.materials.size());
    ImGui::Text("Triangles: %zu", m_scene.indices.size() / 3);
    ImGui::Text("Bounds Min: %.2f %.2f %.2f", m_scene.boundsMin.x, m_scene.boundsMin.y, m_scene.boundsMin.z);
    ImGui::Text("Bounds Max: %.2f %.2f %.2f", m_scene.boundsMax.x, m_scene.boundsMax.y, m_scene.boundsMax.z);
    ImGui::Separator();
    XMFLOAT3 cameraPosition = m_camera.GetPosition();
    float cameraAngles[] =
    {
        XMConvertToDegrees(m_camera.GetYawRadians()),
        XMConvertToDegrees(m_camera.GetPitchRadians())
    };
    bool cameraChanged = false;
    cameraChanged |= ImGui::DragFloat3("Position", &cameraPosition.x, 0.1f, -10000.0f, 10000.0f, "%.2f");
    if (ImGui::DragFloat2("Yaw / Pitch", cameraAngles, 0.25f, -360.0f, 360.0f, "%.1f deg"))
    {
        cameraAngles[1] = std::clamp(cameraAngles[1], -83.0f, 83.0f);
        cameraChanged = true;
    }
    if (cameraChanged)
    {
        m_camera.Reset(cameraPosition, XMConvertToRadians(cameraAngles[0]), XMConvertToRadians(cameraAngles[1]));
        ResetAccumulation();
        m_projectDirty = true;
    }
    if (ImGui::Button("Reset Camera View")) { ResetCameraView(); ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderFloat("Move Speed", &m_baseMoveSpeed, 0.1f, 50.0f, "%.1f")) m_camera.SetMoveSpeeds(m_baseMoveSpeed, m_fastMoveSpeed);
    if (ImGui::SliderFloat("Fast Speed", &m_fastMoveSpeed, 0.1f, 100.0f, "%.1f")) m_camera.SetMoveSpeeds(m_baseMoveSpeed, m_fastMoveSpeed);
    if (ImGui::Button("Reset Camera Speed")) ResetCameraSpeeds();
    ImGui::PopItemWidth();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(24.0f, 340.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 380.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Material");
    ImGui::PushItemWidth(250.0f);
    static int selectedMaterial = 0;
    if (m_scene.materials.empty())
    {
        ImGui::TextUnformatted("No materials.");
    }
    else
    {
        selectedMaterial = std::clamp(selectedMaterial, 0, static_cast<int>(m_scene.materials.size()) - 1);
        std::string currentName = WideToUtf8(m_scene.materials[selectedMaterial].name);
        if (currentName.empty())
        {
            currentName = "Material " + std::to_string(selectedMaterial);
        }
        if (ImGui::BeginCombo("Material", currentName.c_str()))
        {
            for (size_t i = 0; i < m_scene.materials.size(); ++i)
            {
                std::string label = WideToUtf8(m_scene.materials[i].name);
                if (label.empty())
                {
                    label = "Material " + std::to_string(i);
                }
                label += "##material" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), selectedMaterial == static_cast<int>(i)))
                {
                    selectedMaterial = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }

        Bistro::Material& material = m_scene.materials[selectedMaterial];
        bool materialChanged = false;
        float baseColor[] = { material.baseColorFactor.x, material.baseColorFactor.y, material.baseColorFactor.z, material.baseColorFactor.w };
        float emissive[] = { material.emissiveFactor.x, material.emissiveFactor.y, material.emissiveFactor.z, material.emissiveFactor.w };
        if (ImGui::ColorEdit4("Base Color", baseColor))
        {
            material.baseColorFactor = XMFLOAT4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
            materialChanged = true;
        }
        if (ImGui::ColorEdit4("Emissive", emissive))
        {
            material.emissiveFactor = XMFLOAT4(emissive[0], emissive[1], emissive[2], emissive[3]);
            materialChanged = true;
        }
        materialChanged |= ImGui::SliderFloat("Roughness", &material.roughnessFactor, 0.02f, 1.0f, "%.2f");
        materialChanged |= ImGui::SliderFloat("Metallic", &material.metallicFactor, 0.0f, 1.0f, "%.2f");
        materialChanged |= ImGui::SliderFloat("Occlusion", &material.occlusionStrength, 0.0f, 2.0f, "%.2f");
        materialChanged |= ImGui::SliderFloat("Normal Strength", &material.normalStrength, 0.0f, 2.0f, "%.2f");
        materialChanged |= ImGui::Checkbox("Alpha Mask", &material.alphaMasked);
        materialChanged |= ImGui::SliderFloat("Alpha Cutoff", &material.alphaCutoff, 0.0f, 1.0f, "%.2f");
        materialChanged |= ImGui::Checkbox("Packed ORM", &material.packedOcclusionRoughnessMetallic);
        if (materialChanged)
        {
            m_pendingGpuResourceRefresh = true;
            m_projectDirty = true;
        }
    }
    ImGui::PopItemWidth();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(400.0f, 370.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 430.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Lighting");
    ImGui::PushItemWidth(240.0f);
    auto resetLightingSettings = [&]()
    {
        ResetRenderingHistory();
        m_projectDirty = true;
    };
    if (ImGui::SliderFloat3("Light Direction", m_lightDirection, -1.0f, 1.0f)) resetLightingSettings();
    if (ImGui::ColorEdit3("Light Color", m_lightColor)) resetLightingSettings();
    if (ImGui::SliderFloat("Light Intensity", &m_lightIntensity, 0.0f, 20.0f, "%.2f")) resetLightingSettings();
    if (ImGui::Button("Reset Light")) { ResetLight(); resetLightingSettings(); }
    ImGui::Separator();
    if (ImGui::SliderFloat("Ray Bias / TMin", &m_rayTMin, 0.001f, 0.25f, "%.3f")) resetLightingSettings();
    if (ImGui::Checkbox("Sky Enabled", &m_skyEnabled)) resetLightingSettings();
    if (ImGui::ColorEdit3("Sky Color", m_skyColor)) resetLightingSettings();
    if (ImGui::ColorEdit3("Sky Horizon Color", m_skyHorizonColor)) resetLightingSettings();
    if (ImGui::ColorEdit3("Sky Zenith Color", m_skyZenithColor)) resetLightingSettings();
    if (ImGui::ColorEdit3("Sky Ground Color", m_skyGroundColor)) resetLightingSettings();
    if (ImGui::SliderFloat("Sky Intensity", &m_skyIntensity, 0.0f, 10.0f, "%.2f")) resetLightingSettings();
    if (ImGui::SliderFloat("Sun Intensity", &m_sunIntensity, 0.0f, 50.0f, "%.2f")) resetLightingSettings();
    if (ImGui::SliderFloat("Sun Size", &m_sunAngularRadius, 0.001f, 0.08f, "%.3f")) resetLightingSettings();
    if (ImGui::Checkbox("Environment Map", &m_environmentMapEnabled)) resetLightingSettings();
    if (ImGui::SliderFloat("Environment Intensity", &m_environmentIntensity, 0.0f, 10.0f, "%.2f")) resetLightingSettings();
    if (ImGui::SliderFloat("Environment Rotation", &m_environmentRotation, -3.14159f, 3.14159f, "%.2f")) resetLightingSettings();
    if (ImGui::Checkbox("Sun NEE", &m_shadowEnabled)) resetLightingSettings();
    if (ImGui::Checkbox("Sky NEE", &m_skyNeeEnabled)) resetLightingSettings();
    if (ImGui::Checkbox("Emissive Triangle Lights", &m_emissiveLightsEnabled)) resetLightingSettings();
    if (ImGui::SliderFloat("Emissive Intensity", &m_emissiveLightIntensity, 0.0f, 30.0f, "%.2f")) resetLightingSettings();
    if (ImGui::Checkbox("Procedural Area Lights", &m_proceduralLightsEnabled)) resetLightingSettings();
    if (ImGui::SliderFloat("Area Light Intensity", &m_proceduralLightIntensity, 0.0f, 50.0f, "%.2f")) resetLightingSettings();
    ImGui::PopItemWidth();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(760.0f, 48.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 380.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Path Tracing");
    ImGui::PushItemWidth(240.0f);
    if (ImGui::SliderInt("Samples / Frame", &m_giSamplesPerFrame, 1, 8)) { ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderInt("Max Bounces", &m_maxPathBounces, 1, 8)) { ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderInt("Min Bounces", &m_minPathBounces, 0, 4)) { ResetRenderingHistory(); m_projectDirty = true; }
    if (m_minPathBounces > m_maxPathBounces) m_minPathBounces = m_maxPathBounces;
    if (ImGui::SliderFloat("Radiance Clamp", &m_giRadianceClamp, 1.0f, 100.0f, "%.1f")) { ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderFloat("Temporal Clamp", &m_giTemporalClampScale, 0.25f, 4.0f, "%.2f")) { ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderInt("Max Accum Samples", &m_maxAccumulatedFrames, 1, 4096)) { ResetRenderingHistory(); m_projectDirty = true; }
    ImGui::Checkbox("Freeze Accumulation", &m_freezeAccumulation);
    ImGui::Separator();
    if (ImGui::Checkbox("Adaptive Samples", &m_adaptiveSamplingEnabled)) { ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderInt("Max Adaptive SPP", &m_maxAdaptiveSamplesPerPixel, 1, 4)) { ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderFloat("Variance Threshold", &m_adaptiveVarianceThreshold, 0.02f, 1.0f, "%.2f")) { ResetRenderingHistory(); m_projectDirty = true; }
    if (ImGui::SliderFloat("Disocclusion Boost", &m_adaptiveDisocclusionBoost, 0.0f, 4.0f, "%.2f")) { ResetRenderingHistory(); m_projectDirty = true; }
    ImGui::PopItemWidth();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(760.0f, 370.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 460.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Denoise");
    ImGui::PushItemWidth(240.0f);
    auto resetDenoiseSettings = [&]()
    {
        ResetRenderingHistory();
        m_projectDirty = true;
    };
    const char* noisePresetItems[] = { "Interactive Stable", "Sharp Preview", "Still Capture" };
    int noisePresetIndex = m_noisePreset == NoisePreset::SharpPreview ? 1 : m_noisePreset == NoisePreset::StillCapture ? 2 : 0;
    if (ImGui::Combo("Noise Preset", &noisePresetIndex, noisePresetItems, _countof(noisePresetItems)))
    {
        ApplyNoisePreset(noisePresetIndex == 1 ? NoisePreset::SharpPreview : noisePresetIndex == 2 ? NoisePreset::StillCapture : NoisePreset::InteractiveStable);
        resetDenoiseSettings();
    }
    const char* jitterModeItems[] = { "Stable16", "Halton", "Off" };
    int jitterModeIndex = m_jitterMode == JitterMode::Halton ? 1 : m_jitterMode == JitterMode::Off ? 2 : 0;
    if (ImGui::Combo("Jitter Mode", &jitterModeIndex, jitterModeItems, _countof(jitterModeItems)))
    {
        m_jitterMode = jitterModeIndex == 1 ? JitterMode::Halton : jitterModeIndex == 2 ? JitterMode::Off : JitterMode::Stable16;
        resetDenoiseSettings();
    }
    if (ImGui::Button("Reset Denoise History")) ResetDenoiseHistory();
    if (ImGui::Checkbox("Denoiser Enabled", &m_denoiserEnabled)) resetDenoiseSettings();
    if (ImGui::Checkbox("Split Signal Denoise", &m_splitSignalDenoise)) resetDenoiseSettings();
    if (ImGui::Checkbox("Realtime Reconstruction", &m_realtimeReconstruction)) resetDenoiseSettings();
    if (ImGui::Checkbox("Temporal Stability", &m_temporalStabilityEnabled)) resetDenoiseSettings();
    if (ImGui::Checkbox("Camera Jitter", &m_cameraJitter)) resetDenoiseSettings();
    if (ImGui::SliderFloat("Moving Jitter Scale", &m_movingJitterScale, 0.0f, 1.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderInt("Max History Frames", &m_reconstructionMaxHistoryFrames, 1, 128)) resetDenoiseSettings();
    if (ImGui::SliderFloat("Temporal Alpha Min", &m_temporalAlphaMin, 0.01f, 0.5f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Temporal Alpha Max", &m_temporalAlphaMax, 0.02f, 0.8f, "%.2f")) resetDenoiseSettings();
    if (m_temporalAlphaMin > m_temporalAlphaMax) m_temporalAlphaMin = m_temporalAlphaMax;
    if (ImGui::SliderFloat("History Clamp Sigma", &m_historyClampSigma, 0.5f, 4.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Reactive Threshold", &m_reactiveThreshold, 0.05f, 1.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Specular History Scale", &m_specularHistoryScale, 0.0f, 1.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderInt("Spatial Iterations", &m_denoiserSpatialIterations, 0, 4)) resetDenoiseSettings();
    if (ImGui::SliderInt("A-Trous Passes", &m_atrousPassCount, 0, 5)) resetDenoiseSettings();
    if (ImGui::SliderFloat("Diffuse Filter Strength", &m_atrousDiffuseStrength, 0.0f, 1.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Specular Filter Strength", &m_atrousSpecularStrength, 0.0f, 1.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Variance Scale", &m_atrousVarianceScale, 0.25f, 4.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Normal Sigma", &m_denoiserNormalSigma, 0.05f, 1.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Depth Sigma", &m_denoiserDepthSigma, 0.002f, 0.10f, "%.3f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Luminance Sigma", &m_denoiserLuminanceSigma, 0.1f, 8.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Albedo Sigma", &m_denoiserAlbedoSigma, 0.05f, 1.0f, "%.2f")) resetDenoiseSettings();
    if (ImGui::SliderFloat("Denoiser Strength", &m_denoiserStrength, 0.0f, 1.0f, "%.2f")) resetDenoiseSettings();
    ImGui::PopItemWidth();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(1120.0f, 48.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 400.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("ReSTIR");
    ImGui::PushItemWidth(240.0f);
    if (UsesRestirReuse(m_mode))
    {
        ImGui::TextUnformatted(m_mode == PathTracingMode::ReSTIRCombined ? "ReSTIR Combined" : (m_mode == PathTracingMode::ReSTIRDI ? "ReSTIR DI" : "ReSTIR GI"));
        const char* temporalLabel = m_mode == PathTracingMode::ReSTIRCombined ? "GI Temporal Reuse" : "Temporal Reuse";
        const char* spatialLabel = m_mode == PathTracingMode::ReSTIRCombined ? "GI Spatial Reuse Passes" : "Spatial Reuse Passes";
        const char* candidateLabel = m_mode == PathTracingMode::ReSTIRCombined ? "GI Candidate Samples / Pixel" : "Candidate Samples / Pixel";
        const char* clampLabel = m_mode == PathTracingMode::ReSTIRCombined ? "GI Reservoir M Clamp" : "Reservoir M Clamp";
        if (ImGui::Checkbox(temporalLabel, &m_restirTemporalReuse)) { ResetRenderingHistory(); m_projectDirty = true; }
        if (ImGui::SliderInt(spatialLabel, &m_restirSpatialReusePasses, 0, 4)) { ResetRenderingHistory(); m_projectDirty = true; }
        if (ImGui::SliderInt("Spatial Radius", &m_restirSpatialRadius, 1, 64)) { ResetRenderingHistory(); m_projectDirty = true; }
        if (ImGui::SliderInt(candidateLabel, &m_restirCandidateSamples, 1, 4)) { ResetRenderingHistory(); m_projectDirty = true; }
        if (ImGui::SliderFloat(clampLabel, &m_restirMClamp, 1.0f, 64.0f, "%.1f")) { ResetRenderingHistory(); m_projectDirty = true; }
        if (m_mode == PathTracingMode::ReSTIRCombined)
        {
            if (ImGui::Checkbox("DI Temporal Reuse", &m_restirDiTemporalReuse)) { ResetRenderingHistory(); m_projectDirty = true; }
            if (ImGui::SliderInt("DI Spatial Reuse Passes", &m_restirDiSpatialReusePasses, 0, 4)) { ResetRenderingHistory(); m_projectDirty = true; }
            if (ImGui::SliderInt("DI Candidate Samples / Pixel", &m_restirDiCandidateSamples, 1, 4)) { ResetRenderingHistory(); m_projectDirty = true; }
            if (ImGui::SliderFloat("DI Reservoir M Clamp", &m_restirDiMClamp, 1.0f, 64.0f, "%.1f")) { ResetRenderingHistory(); m_projectDirty = true; }
        }
        if (ImGui::Checkbox("Reservoir Reprojection", &m_reservoirReprojection)) { ResetRenderingHistory(); m_projectDirty = true; }
        if (ImGui::Checkbox("Reservoir Validation", &m_reservoirValidation)) { ResetRenderingHistory(); m_projectDirty = true; }
        if (UsesRestirGI(m_mode))
        {
            if (ImGui::Checkbox("GI Validation Ray", &m_restirGiValidationRay)) { ResetRenderingHistory(); m_projectDirty = true; }
        }
        if (ImGui::SliderInt("Reservoir Max Age", &m_reservoirMaxAge, 1, 32)) { ResetRenderingHistory(); m_projectDirty = true; }
        if (ImGui::Button("Reset Reservoirs")) ResetRenderingHistory();
    }
    else
    {
        ImGui::TextUnformatted("Inactive for Baseline PT.");
    }
    ImGui::PopItemWidth();
    ImGui::End();

    BuildRendererStatsUI();
    BuildMcpServerUI();
    ImGui::Render();
}

void D3D12PathTracingBackend::BuildRendererStatsUI()
{
    uint64_t primitiveCount = 0;
    uint64_t submittedIndexCount = 0;
    for (const Bistro::DrawItem& draw : m_scene.draws)
    {
        submittedIndexCount += draw.indexCount;
        primitiveCount += draw.indexCount / 3;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float fps = io.Framerate;
    const float frameTimeMs = fps > 0.0f ? 1000.0f / fps : 0.0f;

    ImGui::SetNextWindowPos(ImVec2(1120.0f, 390.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440.0f, 400.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Diagnostics / Stats");
    ImGui::Text("API: Direct3D 12 DXR");
    ImGui::Text("DXR Tier: %ls", PathtracingTierName(m_raytracingTier).c_str());
    ImGui::Text("FPS: %.1f", fps);
    ImGui::Text("Frame Time: %.3f ms", frameTimeMs);
    ImGui::Checkbox("VSync", &m_vsyncEnabled);
    ImGui::Text("Tearing: %s", m_tearingSupported ? "Supported" : "Unsupported");
    ImGui::Separator();
    ImGui::Text("Materials: %zu", m_scene.materials.size());
    ImGui::Text("Textures: %zu", m_textures.size());
    ImGui::Text("Vertices: %zu", m_scene.vertices.size());
    ImGui::Text("Indices: %zu", m_scene.indices.size());
    ImGui::Text("Submitted Indices: %llu", static_cast<unsigned long long>(submittedIndexCount));
    ImGui::Text("Primitives: %llu", static_cast<unsigned long long>(primitiveCount));
    ImGui::Text("BLAS Geometries: %zu", m_geometryRecords.size());
    ImGui::Text("TLAS Instances: 1");
    ImGui::Text("SBT Records: %u", m_rayGenTable.recordCount + m_missTable.recordCount + m_hitGroupTable.recordCount);
    ImGui::Text("Light List: %u", m_activeLightCount);
    ImGui::Text("Emissive Tri Lights: %u", m_emissiveTriangleLightCount);
    ImGui::Text("Procedural Area Lights: %u", m_proceduralAreaLightCount);
    ImGui::Text("Environment: %s", m_environmentTexturePath.empty() ? "Procedural Sky" : "Texture");
    ImGui::Text("Output: %ux%u", m_width, m_height);
    ImGui::Text("Accumulated Samples: %u", m_accumulatedFrames);
    ImGui::Text("Mode: %s", PathtracingModeName(m_mode));
    ImGui::Text("Denoiser: %s (%d pass%s)", m_denoiserEnabled ? "On" : "Off", m_denoiserSpatialIterations, m_denoiserSpatialIterations == 1 ? "" : "es");
    ImGui::Text("Noise Preset: %s", NoisePresetDisplayName(m_noisePreset));
    ImGui::Text("Reconstruction: %s (%d history)", m_realtimeReconstruction ? "Realtime" : "Progressive", m_reconstructionMaxHistoryFrames);
    ImGui::Text("Temporal Stability: %s / %s", m_temporalStabilityEnabled ? "On" : "Off", m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested ? "History Valid" : "History Reset");
    ImGui::Text("Jitter: %s strength %.2f", JitterModeDisplayName(m_jitterMode), m_currentJitterStrength);
    ImGui::Text("Camera Motion: %.3f", m_cameraMotionAmount);
    ImGui::Text("Signal Split: %s", m_splitSignalDenoise ? "On" : "Off");
    ImGui::Text("A-Trous Passes: %d", m_atrousPassCount);
    ImGui::Text("Adaptive SPP: %s / max %d", m_adaptiveSamplingEnabled ? "On" : "Off", m_maxAdaptiveSamplesPerPixel);
    ImGui::Text("History Clamp Sigma: %.2f", m_historyClampSigma);
    ImGui::Text("Reactive Threshold: %.2f", m_reactiveThreshold);
    ImGui::TextUnformatted("History Valid: see History Match / Confidence views");
    ImGui::TextUnformatted("Disocclusion: see Disocclusion Mask view");
    ImGui::End();
}

void D3D12PathTracingBackend::BuildMcpServerUI()
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        settings = m_mcpSettings;
    }

    const mcp::ServerStatus status = m_mcpServer.GetStatus();
    ImGui::SetNextWindowPos(ImVec2(1120.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 430.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("MCP Server");

    ImGui::Text("Status: %s", status.running ? "Running" : "Stopped");
    ImGui::TextWrapped("Endpoint: %s", status.endpoint.c_str());
    ImGui::Text("Sessions: %zu", status.activeSessions);
    ImGui::Text("Active Requests: %zu", status.activeRequests);
    ImGui::Text("Pending Commands: %zu", m_mcpDispatcher.PendingCount());
    if (!status.lastError.empty())
    {
        ImGui::TextWrapped("Last Error: %s", status.lastError.c_str());
    }
    if (!m_mcpUiDiagnostics.empty())
    {
        ImGui::TextWrapped("%s", m_mcpUiDiagnostics.c_str());
    }

    if (status.running)
    {
        if (ImGui::Button("Stop Server"))
        {
            StopMcpServer();
        }
    }
    else
    {
        if (ImGui::Button("Start Server"))
        {
            StartMcpServer();
        }
    }

    ImGui::Separator();
    int port = settings.port;
    if (status.running)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputInt("Port", &port))
    {
        port = std::clamp(port, 1, 65535);
        {
            std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
            m_mcpSettings.port = static_cast<uint16_t>(port);
        }
        SaveMcpUserSettings();
    }
    if (status.running)
    {
        ImGui::EndDisabled();
    }

    int timeout = settings.requestTimeoutSeconds;
    if (ImGui::SliderInt("Request Timeout", &timeout, 5, 300, "%d s"))
    {
        {
            std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
            m_mcpSettings.requestTimeoutSeconds = timeout;
        }
        SaveMcpUserSettings();
    }

    const char* accessItems[] = { "Read Only", "Confirm Mutations", "Allow Mutations" };
    int accessIndex = settings.accessMode == mcp::AccessMode::ReadOnly ? 0 : settings.accessMode == mcp::AccessMode::AllowMutations ? 2 : 1;
    if (ImGui::Combo("Access Mode", &accessIndex, accessItems, _countof(accessItems)))
    {
        mcp::AccessMode mode = mcp::AccessMode::ConfirmMutations;
        if (accessIndex == 0)
        {
            mode = mcp::AccessMode::ReadOnly;
        }
        else if (accessIndex == 2)
        {
            mode = mcp::AccessMode::AllowMutations;
        }
        {
            std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
            m_mcpSettings.accessMode = mode;
        }
        SaveMcpUserSettings();
    }

    ImGui::TextWrapped("Bearer Token: %s", settings.token.c_str());
    if (ImGui::Button("Copy Token"))
    {
        ImGui::SetClipboardText(settings.token.c_str());
        m_mcpUiDiagnostics = "MCP token copied.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Regenerate Token"))
    {
        const bool wasRunning = m_mcpServer.IsRunning();
        if (wasRunning)
        {
            StopMcpServer();
        }
        {
            std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
            m_mcpSettings.token = mcp::GenerateToken();
        }
        SaveMcpUserSettings();
        if (wasRunning)
        {
            StartMcpServer();
        }
        else
        {
            m_mcpUiDiagnostics = "MCP token regenerated.";
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Pending Approvals");
    const std::vector<mcp::PendingApproval> approvals = m_mcpDispatcher.PendingApprovals();
    if (approvals.empty())
    {
        ImGui::TextUnformatted("None");
    }
    for (const mcp::PendingApproval& approval : approvals)
    {
        ImGui::PushID(static_cast<int>(approval.id));
        ImGui::TextWrapped("#%llu %s", static_cast<unsigned long long>(approval.id), approval.toolName.c_str());
        ImGui::TextWrapped("%s", approval.summary.c_str());
        ImGui::Text("Timeout: %d s", (std::max)(approval.secondsRemaining, 0));
        if (ImGui::Button("Approve"))
        {
            m_mcpDispatcher.Approve(approval.id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reject"))
        {
            m_mcpDispatcher.Reject(approval.id, "MCP request was rejected in the UI.");
        }
        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::TextUnformatted("Recent Requests");
    for (const std::string& entry : status.recentRequests)
    {
        ImGui::BulletText("%s", entry.c_str());
    }
    ImGui::End();
}

void D3D12PathTracingBackend::OnKeyDown(UINT8 key)
{
    if (GetForegroundWindow() != Win32Application::GetHwnd())
    {
        return;
    }

    if (key == VK_SPACE)
    {
        ResetCameraView();
        ResetRenderingHistory();
    }
}

void D3D12PathTracingBackend::OnWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_SETFOCUS)
    {
        m_camera.SetActive(true);
        return;
    }
    if (message == WM_KILLFOCUS)
    {
        m_camera.SetActive(false);
        return;
    }

    if (message == WM_SIZE)
    {
        m_minimized = wParam == SIZE_MINIMIZED;
        if (!m_minimized)
        {
            const UINT width = static_cast<UINT>(LOWORD(lParam));
            const UINT height = static_cast<UINT>(HIWORD(lParam));
            if (width > 0 && height > 0 && (width != m_width || height != m_height))
            {
                m_pendingResizeWidth = width;
                m_pendingResizeHeight = height;
                m_resizePending = true;
            }
        }
        return;
    }

    if (GetForegroundWindow() != Win32Application::GetHwnd())
    {
        m_camera.SetActive(false);
        return;
    }

    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
    {
        m_camera.OnMouseButton(message, wParam);
    }
    if (message == WM_MOUSEMOVE)
    {
        m_camera.OnMouseMove(lParam);
    }
}

void D3D12PathTracingBackend::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
    DXSample::ParseCommandLineArgs(argv, argc);

    auto matchesPathArgument = [](const std::wstring& argument, const wchar_t* prefix)
    {
        const size_t prefixLength = wcslen(prefix);
        return _wcsnicmp(argument.c_str(), prefix, prefixLength) == 0 &&
            (argument.size() == prefixLength || argument[prefixLength] == L'=');
    };

    auto consumePathArgument = [&](int& index, const wchar_t* prefix) -> std::wstring
    {
        const std::wstring argument = argv[index];
        const size_t prefixLength = wcslen(prefix);
        if (argument.size() > prefixLength && argument[prefixLength] == L'=')
        {
            return argument.substr(prefixLength + 1);
        }
        if (index + 1 < argc)
        {
            ++index;
            return argv[index];
        }
        return {};
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring argument = argv[i];
        if (matchesPathArgument(argument, L"--scene") || matchesPathArgument(argument, L"-scene") || matchesPathArgument(argument, L"/scene"))
        {
            const wchar_t* prefix = argument.starts_with(L"--scene") ? L"--scene" : argument.starts_with(L"/scene") ? L"/scene" : L"-scene";
            m_startupScenePath = consumePathArgument(i, prefix);
        }
        else if (matchesPathArgument(argument, L"--environment") || matchesPathArgument(argument, L"-environment") || matchesPathArgument(argument, L"/environment"))
        {
            const wchar_t* prefix = argument.starts_with(L"--environment") ? L"--environment" : argument.starts_with(L"/environment") ? L"/environment" : L"-environment";
            m_startupEnvironmentPath = consumePathArgument(i, prefix);
        }
        else if (_wcsicmp(argument.c_str(), L"--mcp-server") == 0 || _wcsicmp(argument.c_str(), L"-mcp-server") == 0 || _wcsicmp(argument.c_str(), L"/mcp-server") == 0)
        {
            m_startupMcpServer = true;
        }
        else if (matchesPathArgument(argument, L"--mcp-port") || matchesPathArgument(argument, L"-mcp-port") || matchesPathArgument(argument, L"/mcp-port"))
        {
            const wchar_t* prefix = argument.starts_with(L"--mcp-port") ? L"--mcp-port" : argument.starts_with(L"/mcp-port") ? L"/mcp-port" : L"-mcp-port";
            const std::wstring portText = consumePathArgument(i, prefix);
            if (!portText.empty())
            {
                m_startupMcpPort = static_cast<UINT>(std::clamp(_wtoi(portText.c_str()), 1, 65535));
            }
        }
        else if (matchesPathArgument(argument, L"--mcp-token") || matchesPathArgument(argument, L"-mcp-token") || matchesPathArgument(argument, L"/mcp-token"))
        {
            const wchar_t* prefix = argument.starts_with(L"--mcp-token") ? L"--mcp-token" : argument.starts_with(L"/mcp-token") ? L"/mcp-token" : L"-mcp-token";
            m_startupMcpToken = WideToUtf8(consumePathArgument(i, prefix));
        }
        else if (matchesPathArgument(argument, L"--mcp-access") || matchesPathArgument(argument, L"-mcp-access") || matchesPathArgument(argument, L"/mcp-access"))
        {
            const wchar_t* prefix = argument.starts_with(L"--mcp-access") ? L"--mcp-access" : argument.starts_with(L"/mcp-access") ? L"/mcp-access" : L"-mcp-access";
            m_startupMcpAccessMode = mcp::AccessModeFromName(WideToUtf8(consumePathArgument(i, prefix)), mcp::AccessMode::ConfirmMutations);
            m_hasStartupMcpAccessMode = true;
        }
    }
}

void D3D12PathTracingBackend::OnDestroy()
{
    StopMcpServer();
    m_mcpDispatcher.CancelAll("Application is shutting down.");
    ShutdownImGui();
    WaitForPreviousFrame();
    if (m_sceneConstantBuffer && m_mappedSceneConstants)
    {
        m_sceneConstantBuffer->Unmap(0, nullptr);
        m_mappedSceneConstants = nullptr;
    }
    CloseHandle(m_fenceEvent);
}

void D3D12PathTracingBackend::WaitForPreviousFrame()
{
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    ++m_fenceValue;
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12PathTracingBackend::InitializeImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 18.0f;
    style.ScaleAllSizes(1.2f);
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 18.0f;
    io.Fonts->AddFontDefault(&fontConfig);
    ImGui_ImplWin32_Init(Win32Application::GetHwnd());

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_commandQueue.Get();
    initInfo.NumFramesInFlight = FrameCount;
    initInfo.RTVFormat = BackBufferFormat;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = m_imguiDescriptorHeap.Get();
    initInfo.SrvDescriptorAllocFn = AllocateImGuiDescriptor;
    initInfo.SrvDescriptorFreeFn = FreeImGuiDescriptor;
    initInfo.UserData = this;
    ImGui_ImplDX12_Init(&initInfo);
}

void D3D12PathTracingBackend::ShutdownImGui()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void D3D12PathTracingBackend::ResetLight()
{
    m_lightDirection[0] = -0.35f;
    m_lightDirection[1] = -0.8f;
    m_lightDirection[2] = 0.45f;
    m_lightColor[0] = 1.0f;
    m_lightColor[1] = 0.96f;
    m_lightColor[2] = 0.88f;
    m_lightIntensity = 4.0f;
}

void D3D12PathTracingBackend::ResetCameraView()
{
    m_camera.Reset(m_defaultCameraPosition, m_defaultCameraYaw, m_defaultCameraPitch);
}

void D3D12PathTracingBackend::ResetCameraSpeeds()
{
    m_baseMoveSpeed = 17.0f;
    m_fastMoveSpeed = 58.2f;
    m_camera.SetMoveSpeeds(m_baseMoveSpeed, m_fastMoveSpeed);
}

void D3D12PathTracingBackend::ResetAccumulation()
{
    m_accumulatedFrames = 0;
    m_resetAccumulationRequested = true;
}

void D3D12PathTracingBackend::ResetDenoiseHistory()
{
    m_denoiseHistoryValid = false;
    m_resetDenoiseHistoryRequested = true;
    m_hasPreviousViewProjection = false;
    m_previousJitter = XMFLOAT2(0.0f, 0.0f);
    m_cameraMotionTrackingInitialized = false;
}

void D3D12PathTracingBackend::ResetRenderingHistory()
{
    ResetAccumulation();
    ResetDenoiseHistory();
}

void D3D12PathTracingBackend::ApplyNoisePreset(NoisePreset preset)
{
    m_noisePreset = preset;
    m_denoiserEnabled = true;
    m_splitSignalDenoise = true;
    m_realtimeReconstruction = true;
    m_cameraJitter = preset != NoisePreset::StillCapture;
    m_temporalStabilityEnabled = true;
    m_jitterMode = JitterMode::Stable16;

    switch (preset)
    {
    case NoisePreset::SharpPreview:
        m_movingJitterScale = 0.35f;
        m_reconstructionMaxHistoryFrames = 18;
        m_temporalAlphaMin = 0.08f;
        m_temporalAlphaMax = 0.32f;
        m_historyClampSigma = 1.10f;
        m_reactiveThreshold = 0.28f;
        m_specularHistoryScale = 0.35f;
        m_atrousPassCount = 2;
        m_atrousDiffuseStrength = 0.70f;
        m_atrousSpecularStrength = 0.22f;
        m_atrousVarianceScale = 1.00f;
        m_denoiserStrength = 0.65f;
        break;
    case NoisePreset::StillCapture:
        m_cameraJitter = true;
        m_movingJitterScale = 0.50f;
        m_reconstructionMaxHistoryFrames = 96;
        m_temporalAlphaMin = 0.02f;
        m_temporalAlphaMax = 0.12f;
        m_historyClampSigma = 2.00f;
        m_reactiveThreshold = 0.45f;
        m_specularHistoryScale = 0.60f;
        m_atrousPassCount = 5;
        m_atrousDiffuseStrength = 0.95f;
        m_atrousSpecularStrength = 0.45f;
        m_atrousVarianceScale = 1.60f;
        m_denoiserStrength = 0.95f;
        break;
    default:
        m_movingJitterScale = 0.25f;
        m_reconstructionMaxHistoryFrames = 32;
        m_temporalAlphaMin = 0.04f;
        m_temporalAlphaMax = 0.22f;
        m_historyClampSigma = 1.50f;
        m_reactiveThreshold = 0.35f;
        m_specularHistoryScale = 0.45f;
        m_atrousPassCount = 3;
        m_atrousDiffuseStrength = 0.85f;
        m_atrousSpecularStrength = 0.35f;
        m_atrousVarianceScale = 1.25f;
        m_denoiserStrength = 0.85f;
        break;
    }
}

void D3D12PathTracingBackend::UpdateCameraMotionState()
{
    const XMFLOAT3 cameraPosition = m_camera.GetPosition();
    const float yaw = m_camera.GetYawRadians();
    const float pitch = m_camera.GetPitchRadians();

    if (!m_cameraMotionTrackingInitialized)
    {
        m_previousCameraMotionState = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, yaw);
        m_previousCameraMotionPitch = pitch;
        m_cameraMotionAmount = 0.0f;
        m_framesSinceCameraMotion = 4;
        m_cameraMotionTrackingInitialized = true;
        return;
    }

    const float dx = cameraPosition.x - m_previousCameraMotionState.x;
    const float dy = cameraPosition.y - m_previousCameraMotionState.y;
    const float dz = cameraPosition.z - m_previousCameraMotionState.z;
    const float positionDelta = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float angleDelta = std::abs(yaw - m_previousCameraMotionState.w) + std::abs(pitch - m_previousCameraMotionPitch);
    m_cameraMotionAmount = std::clamp(positionDelta * 0.20f + angleDelta * 2.0f, 0.0f, 1.0f);

    if (positionDelta > 5.0f || angleDelta > XMConvertToRadians(30.0f))
    {
        ResetDenoiseHistory();
    }

    if (m_cameraMotionAmount > 0.001f)
    {
        m_framesSinceCameraMotion = 0;
    }
    else
    {
        m_framesSinceCameraMotion = (std::min)(m_framesSinceCameraMotion + 1u, 4u);
    }

    m_previousCameraMotionState = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, yaw);
    m_previousCameraMotionPitch = pitch;
    m_cameraMotionTrackingInitialized = true;
}

bool D3D12PathTracingBackend::HasAccumulationStateChanged()
{
    XMFLOAT3 cameraPosition = m_camera.GetPosition();
    XMFLOAT4 cameraAndYaw(cameraPosition.x, cameraPosition.y, cameraPosition.z, m_camera.GetYawRadians() + m_camera.GetPitchRadians());
    XMFLOAT4 lighting(m_lightDirection[0], m_lightDirection[1], m_lightDirection[2], m_lightIntensity + static_cast<float>(m_debugViewMode) + (m_shadowEnabled ? 1.0f : 0.0f) + (m_skyNeeEnabled ? 2.0f : 0.0f));
    XMFLOAT4 giOptions(static_cast<float>(m_giSamplesPerFrame), m_giRadianceClamp, m_giTemporalClampScale, m_giTemporalClampMin);
    XMFLOAT4 pathOptions(static_cast<float>(m_maxPathBounces), static_cast<float>(m_minPathBounces), static_cast<float>(m_restirCandidateSamples), UsesRestirReuse(m_mode) ? 1.0f : 0.0f);
    XMFLOAT4 restirOptions(m_restirTemporalReuse ? 1.0f : 0.0f, static_cast<float>(m_restirSpatialReusePasses), static_cast<float>(m_restirSpatialRadius), m_restirMClamp);
    const bool combinedRestir = m_mode == PathTracingMode::ReSTIRCombined;
    XMFLOAT4 restirDiOptions(
        (combinedRestir ? m_restirDiTemporalReuse : m_restirTemporalReuse) ? 1.0f : 0.0f,
        static_cast<float>(combinedRestir ? m_restirDiSpatialReusePasses : m_restirSpatialReusePasses),
        static_cast<float>(combinedRestir ? m_restirDiCandidateSamples : m_restirCandidateSamples),
        combinedRestir ? m_restirDiMClamp : m_restirMClamp);
    XMFLOAT4 lightSystemOptions(
        (m_emissiveLightsEnabled ? m_emissiveLightIntensity : 0.0f) + (m_proceduralLightsEnabled ? m_proceduralLightIntensity : 0.0f),
        m_environmentMapEnabled ? m_environmentIntensity : 0.0f,
        m_environmentRotation,
        static_cast<float>(m_activeLightCount));
    XMFLOAT4 signalDenoiseOptions(m_splitSignalDenoise ? 1.0f : 0.0f, m_historyClampSigma, m_reactiveThreshold, m_specularHistoryScale);
    const bool changed =
        m_resetAccumulationRequested ||
        memcmp(&cameraAndYaw, &m_lastCameraAndYaw, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&lighting, &m_lastLighting, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&giOptions, &m_lastGiOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&pathOptions, &m_lastPathOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&restirOptions, &m_lastRestirOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&restirDiOptions, &m_lastRestirDiOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&lightSystemOptions, &m_lastLightSystemOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&signalDenoiseOptions, &m_lastSignalDenoiseOptions, sizeof(XMFLOAT4)) != 0;
    m_lastCameraAndYaw = cameraAndYaw;
    m_lastLighting = lighting;
    m_lastGiOptions = giOptions;
    m_lastPathOptions = pathOptions;
    m_lastRestirOptions = restirOptions;
    m_lastRestirDiOptions = restirDiOptions;
    m_lastLightSystemOptions = lightSystemOptions;
    m_lastSignalDenoiseOptions = signalDenoiseOptions;
    m_resetAccumulationRequested = false;
    return changed;
}

ComPtr<ID3D12Resource> D3D12PathTracingBackend::CreateDefaultBuffer(const void* data, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES finalState, const wchar_t* name)
{
    ComPtr<ID3D12Resource> resource;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    resource->SetName(name);
    if (data && size > 0)
    {
        ComPtr<ID3D12Resource> upload = CreateUploadBuffer(data, size, L"UploadBuffer");
        m_uploadBuffers.push_back(upload);
        m_commandList->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, size);
        auto uploadReadyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        m_commandList->ResourceBarrier(1, &uploadReadyBarrier);
    }
    return resource;
}

ComPtr<ID3D12Resource> D3D12PathTracingBackend::CreateUploadBuffer(const void* data, UINT64 size, const wchar_t* name)
{
    ComPtr<ID3D12Resource> resource;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)));
    resource->SetName(name);
    if (data && size > 0)
    {
        void* mapped = nullptr;
        ThrowIfFailed(resource->Map(0, nullptr, &mapped));
        memcpy(mapped, data, size);
        resource->Unmap(0, nullptr);
    }
    return resource;
}

ComPtr<ID3D12Resource> D3D12PathTracingBackend::CreateUavBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState, const wchar_t* name)
{
    ComPtr<ID3D12Resource> resource;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource)));
    resource->SetName(name);
    return resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12PathTracingBackend::CpuDescriptor(UINT index) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), index, m_descriptorSize);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12PathTracingBackend::GpuDescriptor(UINT index) const
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), index, m_descriptorSize);
}

std::wstring D3D12PathTracingBackend::ShaderFileName() const
{
    if (m_mode == PathTracingMode::ReSTIR)
    {
        return L"PathTracingReSTIR.lib.cso";
    }
    if (m_mode == PathTracingMode::ReSTIRDI)
    {
        return L"PathTracingReSTIRDI.lib.cso";
    }
    if (m_mode == PathTracingMode::ReSTIRCombined)
    {
        return L"PathTracingReSTIRCombined.lib.cso";
    }
    return L"PathTracing.lib.cso";
}

std::wstring D3D12PathTracingBackend::RestirReuseShaderFileName() const
{
    return L"ReSTIRResolve.cso";
}

std::wstring D3D12PathTracingBackend::RestirDiReuseShaderFileName() const
{
    return L"ReSTIRResolveDI.cso";
}

std::wstring D3D12PathTracingBackend::DenoiseShaderFileName() const
{
    return L"PathTracingDenoise.cso";
}

UINT D3D12PathTracingBackend::MaxTraceRecursionDepth() const
{
    return 1u;
}

UINT D3D12PathTracingBackend::Align(UINT value, UINT alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void D3D12PathTracingBackend::AllocateImGuiDescriptor(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    auto* sample = static_cast<D3D12PathTracingBackend*>(info->UserData);
    const UINT descriptorIndex = sample->m_imguiDescriptorCursor++;
    if (descriptorIndex >= ImGuiDescriptorCount)
    {
        ThrowIfFailed(E_OUTOFMEMORY);
    }

    *outCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(sample->m_imguiDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), descriptorIndex, sample->m_imguiDescriptorSize);
    *outGpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(sample->m_imguiDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, sample->m_imguiDescriptorSize);
}

void D3D12PathTracingBackend::FreeImGuiDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
{
}
