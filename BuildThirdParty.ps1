param(
    [Parameter(Mandatory = $true)][string]$AssimpRoot,
    [Parameter(Mandatory = $true)][string]$AssimpBuildRoot,
    [Parameter(Mandatory = $true)][string]$AssimpLibDir,
    [Parameter(Mandatory = $true)][string]$AssimpZlibDir,
    [Parameter(Mandatory = $true)][string]$AssimpLibName,
    [Parameter(Mandatory = $true)][string]$AssimpZlibName,
    [Parameter(Mandatory = $true)][string]$DirectXTexProject,
    [Parameter(Mandatory = $true)][string]$DirectXTexLibDir,
    [Parameter(Mandatory = $true)][string]$MSBuildPath,
    [Parameter(Mandatory = $true)][string]$Configuration,
    [string]$VisualStudioVersion = ''
)

$ErrorActionPreference = 'Stop'

function Invoke-WithMutex {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    $mutex = [System.Threading.Mutex]::new($false, $Name)
    try {
        [void]$mutex.WaitOne()
        & $Body
    }
    finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

function Copy-FirstMatch {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string[]]$Patterns,
        [Parameter(Mandatory = $true)][string]$DestinationName
    )

    foreach ($pattern in $Patterns) {
        $candidate = Get-ChildItem -Path $Directory -Filter $pattern -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate) {
            $destination = Join-Path $Directory $DestinationName
            Copy-Item -LiteralPath $candidate.FullName -Destination $destination -Force
            return
        }
    }
    throw "No library matching '$($Patterns -join ', ')' was found in $Directory."
}

function Get-CMakeGeneratorArgs {
    $help = cmake --help 2>$null
    if ($VisualStudioVersion -like '18.*' -and $help -match 'Visual Studio 18 2026') {
        return @('-G', 'Visual Studio 18 2026', '-A', 'x64')
    }
    if ($VisualStudioVersion -like '17.*' -and $help -match 'Visual Studio 17 2022') {
        return @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    }
    if ($help -match 'Visual Studio 18 2026') {
        return @('-G', 'Visual Studio 18 2026', '-A', 'x64')
    }
    return @('-G', 'Visual Studio 17 2022', '-A', 'x64')
}

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\', '/'))
}

function Test-CMakeCacheMatchesSource {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$SourceRoot
    )

    if (!(Test-Path -LiteralPath $CachePath)) {
        return $true
    }

    $prefix = 'CMAKE_HOME_DIRECTORY:INTERNAL='
    $line = Get-Content -LiteralPath $CachePath | Where-Object { $_.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
    if (!$line) {
        return $false
    }

    $cachedSource = $line.Substring($prefix.Length)
    return (Get-NormalizedPath $cachedSource) -ieq (Get-NormalizedPath $SourceRoot)
}

function Reset-AssimpBuildRootIfStale {
    param(
        [Parameter(Mandatory = $true)][string]$BuildRoot,
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$CachePath
    )

    if (Test-CMakeCacheMatchesSource -CachePath $CachePath -SourceRoot $SourceRoot) {
        return
    }

    $normalizedBuildRoot = Get-NormalizedPath $BuildRoot
    $normalizedSourceRoot = Get-NormalizedPath $SourceRoot
    $comparison = [System.StringComparison]::OrdinalIgnoreCase
    if (!$normalizedBuildRoot.StartsWith($normalizedSourceRoot + [System.IO.Path]::DirectorySeparatorChar, $comparison)) {
        throw "Refusing to delete stale Assimp build root outside the Assimp source tree: $normalizedBuildRoot"
    }

    Remove-Item -LiteralPath $normalizedBuildRoot -Recurse -Force
}

$cmakeGeneratorArgs = Get-CMakeGeneratorArgs

Invoke-WithMutex -Name "Local\D3D12LookDevPT-DirectXTex-$Configuration" -Body {
    $directXTexLib = Join-Path $DirectXTexLibDir 'DirectXTex.lib'
    if (!(Test-Path $directXTexLib)) {
        & $MSBuildPath $DirectXTexProject /p:Platform=x64 /p:Configuration=$Configuration /v:minimal
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
}

Invoke-WithMutex -Name "Local\D3D12LookDevPT-Assimp-$Configuration" -Body {
    $cachePath = Join-Path $AssimpBuildRoot 'CMakeCache.txt'
    $solutionPath = Join-Path $AssimpBuildRoot 'Assimp.sln'
    $assimpNormalized = Join-Path $AssimpLibDir $AssimpLibName
    $zlibNormalized = Join-Path $AssimpZlibDir $AssimpZlibName

    Reset-AssimpBuildRootIfStale -BuildRoot $AssimpBuildRoot -SourceRoot $AssimpRoot -CachePath $cachePath

    if (!(Test-Path $cachePath) -or !(Test-Path $solutionPath)) {
        cmake -S $AssimpRoot -B $AssimpBuildRoot @cmakeGeneratorArgs `
            -DASSIMP_BUILD_TESTS=OFF `
            -DASSIMP_BUILD_ASSIMP_TOOLS=OFF `
            -DBUILD_SHARED_LIBS=OFF `
            -DASSIMP_INSTALL=OFF `
            -DASSIMP_WARNINGS_AS_ERRORS=OFF
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }

    if (!(Test-Path $assimpNormalized) -or !(Test-Path $zlibNormalized)) {
        cmake --build $AssimpBuildRoot --config $Configuration --target assimp --parallel
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        New-Item -ItemType Directory -Force -Path $AssimpLibDir | Out-Null
        New-Item -ItemType Directory -Force -Path $AssimpZlibDir | Out-Null
        if ($Configuration -eq 'Debug') {
            Copy-FirstMatch -Directory $AssimpLibDir -Patterns @('assimp-vc*-mtd.lib', 'assimp*.lib') -DestinationName $AssimpLibName
            Copy-FirstMatch -Directory $AssimpZlibDir -Patterns @('zlibstaticd.lib', 'zlib*.lib') -DestinationName $AssimpZlibName
        }
        else {
            Copy-FirstMatch -Directory $AssimpLibDir -Patterns @('assimp-vc*-mt.lib', 'assimp*.lib') -DestinationName $AssimpLibName
            Copy-FirstMatch -Directory $AssimpZlibDir -Patterns @('zlibstatic.lib', 'zlib*.lib') -DestinationName $AssimpZlibName
        }
    }
}
