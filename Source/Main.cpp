#include "stdafx.h"
#include "D3D12PathTracingBackend.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    D3D12PathTracingBackend app(1920, 1080, L"D3D12LookDevPT", PathTracingMode::ReSTIRCombined);
    return Win32Application::Run(&app, instance, showCommand);
}
