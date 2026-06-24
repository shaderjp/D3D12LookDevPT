[CmdletBinding()]
param(
    [string]$Root = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateSet("x64")]
    [string]$Platform = "x64",
    [switch]$CheckAssets,
    [switch]$CheckDLSS,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

if (-not $Root) {
    $scriptRoot = $PSScriptRoot
    if (-not $scriptRoot) {
        $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    $Root = (Resolve-Path (Join-Path $scriptRoot "..")).Path
}

$results = New-Object System.Collections.Generic.List[object]

function Add-Result {
    param(
        [ValidateSet("OK", "WARN", "FAIL")]
        [string]$Status,
        [string]$Check,
        [string]$Message,
        [string]$Fix = ""
    )

    $results.Add([pscustomobject]@{
        Status = $Status
        Check = $Check
        Message = $Message
        Fix = $Fix
    })
}

function Test-File {
    param(
        [string]$RelativePath,
        [string]$Check,
        [string]$Fix,
        [switch]$WarnOnly
    )

    $path = Join-Path $Root $RelativePath
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Add-Result -Status "OK" -Check $Check -Message "$RelativePath found."
        return
    }

    if ($WarnOnly) {
        Add-Result -Status "WARN" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    } else {
        Add-Result -Status "FAIL" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    }
}

function Test-Directory {
    param(
        [string]$RelativePath,
        [string]$Check,
        [string]$Fix,
        [switch]$WarnOnly
    )

    $path = Join-Path $Root $RelativePath
    if (Test-Path -LiteralPath $path -PathType Container) {
        Add-Result -Status "OK" -Check $Check -Message "$RelativePath found."
        return
    }

    if ($WarnOnly) {
        Add-Result -Status "WARN" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    } else {
        Add-Result -Status "FAIL" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    }
}

function Find-MSBuild {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    return ""
}

try {
    $Root = (Resolve-Path -LiteralPath $Root).Path
} catch {
    Add-Result -Status "FAIL" -Check "Root" -Message "Root path does not exist: $Root"
}

if (Test-Path -LiteralPath $Root -PathType Container) {
    Test-File -RelativePath "D3D12LookDevPT.sln" -Check "Solution" -Fix "Run this script from the repository root or pass -Root."
    Test-File -RelativePath "D3D12LookDevPT.vcxproj" -Check "Project" -Fix "The Visual Studio project is missing."
    Test-File -RelativePath ".gitmodules" -Check "Submodule config" -Fix "The repository should include .gitmodules."
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($git) {
    Add-Result -Status "OK" -Check "Git" -Message "git found: $($git.Source)"
} else {
    Add-Result -Status "FAIL" -Check "Git" -Message "git.exe was not found." -Fix "Install Git for Windows and retry."
}

$msbuild = Find-MSBuild
if ($msbuild) {
    Add-Result -Status "OK" -Check "MSBuild" -Message "MSBuild found: $msbuild"
} else {
    Add-Result -Status "FAIL" -Check "MSBuild" -Message "MSBuild was not found." -Fix "Install Visual Studio 2022 or Visual Studio 2026 with C++ desktop workload."
}

$submoduleFix = "Run: git submodule update --init --recursive --depth 1"
Test-File -RelativePath "ThirdParty/imgui/imgui.cpp" -Check "Submodule imgui" -Fix $submoduleFix
Test-File -RelativePath "ThirdParty/imgui/backends/imgui_impl_dx12.cpp" -Check "Submodule imgui backend" -Fix $submoduleFix
Test-File -RelativePath "ThirdParty/assimp/include/assimp/Importer.hpp" -Check "Submodule assimp" -Fix $submoduleFix
Test-File -RelativePath "ThirdParty/DirectXTex/DirectXTex/DirectXTex.h" -Check "Submodule DirectXTex" -Fix $submoduleFix
Test-File -RelativePath "ThirdParty/Streamline/include/sl.h" -Check "Optional DLSS Streamline submodule" -Fix $submoduleFix -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/DLSS/include/nvsdk_ngx.h" -Check "Optional DLSS SDK submodule" -Fix $submoduleFix -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll" -Check "Optional DLSS SR runtime" -Fix "Initialize ThirdParty/DLSS or build with /p:EnableDLSS=false." -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/DLSS/lib/Windows_x86_64/rel/nvngx_dlssd.dll" -Check "Optional DLSS-RR runtime" -Fix "Initialize ThirdParty/DLSS or build with /p:EnableDLSS=false." -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/Streamline/bin/x64/sl.interposer.dll" -Check "Optional Streamline runtime" -Fix "Download/build Streamline runtime and place sl.interposer.dll under ThirdParty/Streamline/bin/x64 or Bin/x64/<Config>/Streamline. Build with /p:EnableDLSS=false to skip DLSS." -WarnOnly:(!$CheckDLSS)

$nugetRoot = $env:NuGetPackageRoot
if (-not $nugetRoot) {
    $nugetRoot = Join-Path $env:USERPROFILE ".nuget\packages"
}

$dxcPath = Join-Path $nugetRoot "microsoft.direct3d.dxc\1.9.2602.17\build\native\bin\x64\dxc.exe"
$agilityCore = Join-Path $nugetRoot "microsoft.direct3d.d3d12\1.619.3\build\native\bin\x64\D3D12Core.dll"
$restoreFix = "Run: msbuild D3D12LookDevPT.sln /t:Restore /p:Configuration=$Configuration /p:Platform=$Platform"

if (Test-Path -LiteralPath $dxcPath -PathType Leaf) {
    Add-Result -Status "OK" -Check "DXC package" -Message "dxc.exe found."
} else {
    Add-Result -Status "WARN" -Check "DXC package" -Message "dxc.exe was not found in the NuGet package cache." -Fix $restoreFix
}

if (Test-Path -LiteralPath $agilityCore -PathType Leaf) {
    Add-Result -Status "OK" -Check "D3D12 Agility SDK" -Message "D3D12Core.dll found."
} else {
    Add-Result -Status "WARN" -Check "D3D12 Agility SDK" -Message "D3D12 Agility SDK runtime was not found in the NuGet package cache." -Fix $restoreFix
}

if ($CheckAssets) {
    Test-Directory -RelativePath "Bistro_v5_2" -Check "Bistro root" -Fix "Download Bistro separately and place Bistro_v5_2 next to the solution. See docs/assets.md."
    Test-File -RelativePath "Bistro_v5_2/BistroExterior.fbx" -Check "Bistro exterior" -Fix "Extract BistroExterior.fbx into Bistro_v5_2."
    Test-Directory -RelativePath "Bistro_v5_2/Textures" -Check "Bistro textures" -Fix "Keep the original Bistro Textures folder beside the FBX files."
    Test-File -RelativePath "Bistro_v5_2/BistroInterior.fbx" -Check "Bistro interior" -Fix "Optional: extract BistroInterior.fbx if you want interior testing." -WarnOnly
    Test-File -RelativePath "Bistro_v5_2/BistroInterior_Wine.fbx" -Check "Bistro wine interior" -Fix "Optional: extract BistroInterior_Wine.fbx if you want wine interior testing." -WarnOnly
    Test-File -RelativePath "Bistro_v5_2/san_giuseppe_bridge_4k.hdr" -Check "Bistro HDRI" -Fix "Optional: place an HDRI in Bistro_v5_2 or use your own environment map." -WarnOnly
} else {
    Add-Result -Status "WARN" -Check "Assets" -Message "Asset checks were skipped." -Fix "Run with -CheckAssets to verify Bistro_v5_2 placement."
}

if (-not $CheckDLSS) {
    Add-Result -Status "WARN" -Check "DLSS strict check" -Message "DLSS runtime checks were warning-only." -Fix "Run with -CheckDLSS to fail when optional DLSS files are missing."
}

$failCount = @($results | Where-Object { $_.Status -eq "FAIL" }).Count
$warnCount = @($results | Where-Object { $_.Status -eq "WARN" }).Count

if ($Json) {
    [pscustomobject]@{
        Root = $Root
        Configuration = $Configuration
        Platform = $Platform
        CheckDLSS = [bool]$CheckDLSS
        Failures = $failCount
        Warnings = $warnCount
        Results = $results
    } | ConvertTo-Json -Depth 5
} else {
    Write-Host "D3D12LookDevPT setup check"
    Write-Host "Root: $Root"
    Write-Host ""
    foreach ($result in $results) {
        $prefix = "[$($result.Status)]"
        if ($result.Status -eq "OK") {
            Write-Host "$prefix $($result.Check): $($result.Message)" -ForegroundColor Green
        } elseif ($result.Status -eq "WARN") {
            Write-Host "$prefix $($result.Check): $($result.Message)" -ForegroundColor Yellow
            if ($result.Fix) { Write-Host "       $($result.Fix)" -ForegroundColor DarkYellow }
        } else {
            Write-Host "$prefix $($result.Check): $($result.Message)" -ForegroundColor Red
            if ($result.Fix) { Write-Host "       $($result.Fix)" -ForegroundColor DarkRed }
        }
    }
    Write-Host ""
    Write-Host "Summary: $failCount failure(s), $warnCount warning(s)"
}

if ($failCount -gt 0) {
    exit 1
}
