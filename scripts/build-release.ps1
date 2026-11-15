# SSCL release build (single project, no tests, embedded build secrets).
#
# Purpose:
#   - Build Release|x64 only, for the launcher project (SSCL.vcxproj).
#     SSCL.Tests is NOT part of this build graph, so no test binary is produced.
#   - Read build-secrets.txt and embed BOTH the Microsoft Client ID and the
#     CurseForge API key into the generated build_secrets.hpp.
#     kGeneratedPublishBuild is set to false on purpose, so build_config.cpp
#     uses the embedded values at runtime.
#   - Minimize output size: LTCG, COMDAT folding, unreferenced code removal,
#     string pooling, function-level linking, no debug information/PDB.
#   - Copy the resulting EXE to SSCL\dist\SSCL.exe and write a SHA-256 file.
#
# WARNING: this build contains the CurseForge API key. Do NOT publish it
# publicly. The three warnings at the end are intentional and mirror
# scripts\build.ps1.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File E:\Cpp_Project\SSCL\scripts\build-release.ps1
#
# ASCII-only content for Windows PowerShell 5.1 compatibility.

param(
    [string]$MsBuild = 'D:\Software\Visual_Studio\2026\MSBuild\Current\Bin\MSBuild.exe',
    [string]$Proxy = 'http://127.0.0.1:23333',
    # Optional: favor code size over speed (/O1). Default keeps /O2 for UI responsiveness;
    # size is already reduced by LTCG + COMDAT folding + no PDB.
    [switch]$MinimizeSize
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root 'SSCL\SSCL.vcxproj'
$secretsFile = Join-Path $root 'build-secrets.txt'
$secretsHeader = Join-Path $root 'SSCL\src\core\config\build_secrets.hpp'
$dist = Join-Path $root 'SSCL\dist'

function Fail-Secrets([string]$message) {
    [Console]::Error.WriteLine("BUILD SECRETS ERROR: $message")
    [Console]::Error.WriteLine("Required file: <root>\build-secrets.txt")
    [Console]::Error.WriteLine("Format (comment/blank lines allowed):")
    [Console]::Error.WriteLine("  SSCL_MS_CLIENT_ID=<non-empty Microsoft app client id>")
    [Console]::Error.WriteLine("  SSCL_CF_API_KEY=<CurseForge API key; may be empty>")
    exit 1
}

if (-not (Test-Path -LiteralPath $project)) {
    Write-Output "project not found: $project"
    exit 1
}

# ---------------------------------------------------------------- MSBuild
if (-not (Test-Path -LiteralPath $MsBuild)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($found) { $MsBuild = $found }
    }
}
if (-not (Test-Path -LiteralPath $MsBuild)) {
    Write-Output "MSBuild not found: $MsBuild"
    exit 1
}

# ---------------------------------------------------------------- secrets
if (-not (Test-Path -LiteralPath $secretsFile)) {
    Fail-Secrets "build-secrets.txt not found - build aborted."
}

$allowedKeys = @('SSCL_MS_CLIENT_ID', 'SSCL_CF_API_KEY')
$secrets = @{}
$lineNo = 0
foreach ($line in [System.IO.File]::ReadAllLines($secretsFile)) {
    $lineNo++
    $text = $line.Trim()
    if ($text -eq '' -or $text.StartsWith('#')) { continue }
    $eq = $text.IndexOf('=')
    if ($eq -lt 1) {
        Fail-Secrets "line $lineNo is not KEY=VALUE."
    }
    $name = $text.Substring(0, $eq).Trim()
    $value = $text.Substring($eq + 1).Trim()
    if ($allowedKeys -notcontains $name) {
        Fail-Secrets "line $lineNo has unknown key '$name'."
    }
    if ($secrets.ContainsKey($name)) {
        Fail-Secrets "line $lineNo duplicates key '$name'."
    }
    $secrets[$name] = $value
}
if (-not $secrets.ContainsKey('SSCL_MS_CLIENT_ID') -or
    [string]::IsNullOrWhiteSpace($secrets['SSCL_MS_CLIENT_ID'])) {
    Fail-Secrets "SSCL_MS_CLIENT_ID must be present and non-empty."
}
$clientId = $secrets['SSCL_MS_CLIENT_ID']
$cfKey = if ($secrets.ContainsKey('SSCL_CF_API_KEY')) { $secrets['SSCL_CF_API_KEY'] } else { '' }
$cfKeyProvided = -not [string]::IsNullOrWhiteSpace($cfKey)
if (-not $cfKeyProvided) {
    Write-Output 'build secrets: SSCL_CF_API_KEY empty - official CF API disabled; MCIM mirror or user-supplied key only.'
}

# Guard: never allow a plaintext credential in tracked source files.
$plainValues = @($clientId)
if ($cfKeyProvided) { $plainValues += $cfKey }
$sourceDir = Join-Path $root 'SSCL\src'
$checkFiles = @(Get-ChildItem -Path $sourceDir -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.hpp') -and $_.Name -ne 'build_secrets.hpp' })
$checkFiles += @(Get-ChildItem -Path (Join-Path $root 'SSCL') -File -Filter '*.vcxproj')
$checkFiles += @(Get-ChildItem -Path $PSScriptRoot -File -Filter '*.ps1')
foreach ($file in $checkFiles) {
    $content = [System.IO.File]::ReadAllText($file.FullName)
    foreach ($plain in $plainValues) {
        if ($plain -ne '' -and $content.Contains($plain)) {
            Fail-Secrets "plaintext credential detected in source file '$($file.FullName)' - build aborted."
        }
    }
}
Write-Output 'build secrets: format OK; source plaintext check OK'

function ConvertTo-CppString([string]$value) {
    if (-not $value) { return '' }
    return $value.Replace('\', '\\').Replace('"', '\"')
}

$headerLines = @(
    '#pragma once'
    '// GENERATED by scripts/build-release.ps1 from build-secrets.txt. DO NOT EDIT OR COMMIT.'
    'namespace sscl::core::detail {'
    'inline constexpr bool kGeneratedPublishBuild = false;'
    ('inline constexpr char kGeneratedClientId[] = "' + (ConvertTo-CppString $clientId) + '";')
    ('inline constexpr char kGeneratedCurseForgeKey[] = "' + (ConvertTo-CppString $cfKey) + '";')
    '}'
    ''
)
$header = [string]::Join("`n", $headerLines)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$existing = ''
if (Test-Path -LiteralPath $secretsHeader) {
    $existing = [System.IO.File]::ReadAllText($secretsHeader).Replace("`r`n", "`n")
}
if ($existing -eq $header) {
    Write-Output 'build secrets header unchanged'
} else {
    [System.IO.File]::WriteAllText($secretsHeader, $header, $utf8NoBom)
    Write-Output 'build secrets header updated (values not logged)'
}

# ---------------------------------------------------------------- vcpkg env
if ($Proxy -ne '') {
    $env:http_proxy = $Proxy
    $env:https_proxy = $Proxy
    $env:HTTP_PROXY = $Proxy
    $env:HTTPS_PROXY = $Proxy
}
$env:VCPKG_DOWNLOADS = Join-Path $root 'SSCL\vcpkg_downloads'
$fetch = Join-Path $PSScriptRoot 'vcpkg_fetch.ps1'
if (Test-Path -LiteralPath $fetch) {
    $env:X_VCPKG_ASSET_SOURCES = "x-script,C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fetch -Url {url} -Dst {dst} -Sha512 {sha512}"
}

# ---------------------------------------------------------------- build
# Size flags: /GL + /LTCG, /OPT:REF, /OPT:ICF, /Gy, /GF, no PDB (/DEBUG off).
# /O2 is kept for UI responsiveness; size is reduced via link-time folding.
$optimization = if ($MinimizeSize) { 'MinSpace' } else { 'MaxSpeed' }
Write-Output "=== release build (SSCL.vcxproj only, no tests, optimization=$optimization) ==="
& $MsBuild $project /m /t:Rebuild `
    /p:Configuration=Release /p:Platform=x64 `
    /p:Optimization=$optimization `
    /p:WholeProgramOptimization=true `
    /p:LinkTimeCodeGeneration=UseLinkTimeCodeGeneration `
    /p:FunctionLevelLinking=true `
    /p:IntrinsicFunctions=true `
    /p:StringPooling=true `
    /p:OptimizeReferences=true `
    /p:EnableCOMDATFolding=true `
    /p:GenerateDebugInformation=false `
    /p:LinkIncremental=false
if ($LASTEXITCODE -ne 0) {
    Write-Output "BUILD FAILED (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

$exe = Join-Path $root 'SSCL\x64\Release\SSCL.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Output "missing executable: $exe"
    exit 1
}

# ---------------------------------------------------------------- package
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$target = Join-Path $dist 'SSCL.exe'
Copy-Item -LiteralPath $exe -Destination $target -Force
$size = (Get-Item -LiteralPath $target).Length
Write-Output ("packaged: {0} ({1:N2} MB)" -f $target, ($size / 1MB))

$hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLower()
$shaFile = Join-Path $dist 'SSCL.exe.sha256'
Set-Content -LiteralPath $shaFile -Value ("{0}  SSCL.exe" -f $hash) -Encoding ascii
Write-Output "sha256: $hash"
Write-Output 'RELEASE BUILD OK (tests were not built)'

# Mirror scripts\build.ps1: three warnings when the CF key is embedded.
if ($cfKeyProvided) {
    Write-Warning 'WARNING 1/3: THIS EXE CONTAINS SECRETS (SSCL_MS_CLIENT_ID and SSCL_CF_API_KEY embedded at build time).'
    Write-Warning 'WARNING 2/3: Any user can extract it via strings / memory dump / TLS-intercepting proxy. Do NOT distribute this build publicly.'
    Write-Warning 'WARNING 3/3: Treat this key as public: rotate it, keep it low-privilege, or rebuild with an empty SSCL_CF_API_KEY.'
}
exit 0
