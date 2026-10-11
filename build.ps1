param([string]$BuildDir = "$PSScriptRoot\build", [string]$Configuration = 'Release', [switch]$Direct)
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $vsRoot) { throw 'MSVC x64 build tools are required' }
$msvcVersion = (Get-Content -LiteralPath "$vsRoot\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt").Trim()
$msvcRoot = "$vsRoot\VC\Tools\MSVC\$msvcVersion"
$sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVersion = '10.0.22621.0'
if (-not (Test-Path -LiteralPath "$sdkRoot\Include\$sdkVersion\um\Windows.h")) {
    $installedSdk = Get-ChildItem -LiteralPath "$sdkRoot\Include" -Directory |
        Where-Object { $_.Name -match '^10\.0\.\d+\.0$' -and
            (Test-Path -LiteralPath "$($_.FullName)\um\Windows.h") -and
            (Test-Path -LiteralPath "$($_.FullName)\ucrt\stddef.h") -and
            (Test-Path -LiteralPath "$sdkRoot\Lib\$($_.Name)\um\x64\user32.lib") } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $installedSdk -or [version]$installedSdk.Name -lt [version]'10.0.22621.0') {
        throw 'Windows SDK 10.0.22621.0 or newer is required'
    }
    $sdkVersion = $installedSdk.Name
}
Write-Output "Building with Windows SDK $sdkVersion"

$cmake = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$env:PATH = "$msvcRoot\bin\Hostx64\x64;$sdkRoot\bin\$sdkVersion\x64;$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
$env:INCLUDE = "$msvcRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\winrt;$sdkRoot\Include\$sdkVersion\cppwinrt"
$env:LIB = "$msvcRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
if ($Direct) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $sources = @('main','common','indicator','capture','uia','desktop','server','foreground') | ForEach-Object { "$PSScriptRoot\src\$_.cpp" }
    & "$msvcRoot\bin\Hostx64\x64\cl.exe" /nologo /std:c++latest /EHsc /W4 /utf-8 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 "/I$PSScriptRoot\include" "/I$PSScriptRoot\third_party" "/Fo$BuildDir\" "/Fe$BuildDir\desktop-control.exe" @sources /link /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot\resources\app.manifest" windowsapp.lib d3d11.lib dxgi.lib user32.lib gdi32.lib dwmapi.lib ole32.lib oleaut32.lib shell32.lib propsys.lib shlwapi.lib windowscodecs.lib uiautomationcore.lib d2d1.lib dwrite.lib advapi32.lib
    if ($LASTEXITCODE) { throw 'Direct build failed' }
    & "$msvcRoot\bin\Hostx64\x64\cl.exe" /nologo /std:c++latest /EHsc /W4 /utf-8 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 "/I$PSScriptRoot\include" "/I$PSScriptRoot\third_party" "/Fo$BuildDir\foreground_helper.obj" "/Fe$BuildDir\desktop-control-foreground.exe" "$PSScriptRoot\src\foreground_helper.cpp" /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot\resources\app.manifest" user32.lib gdi32.lib
    if ($LASTEXITCODE) { throw 'Foreground helper build failed' }
    & "$msvcRoot\bin\Hostx64\x64\cl.exe" /nologo /std:c++latest /EHsc /utf-8 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX "/Fo$BuildDir\fixture.obj" "/Fe$BuildDir\desktop-control-fixture.exe" "$PSScriptRoot\tests\fixture.cpp" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot\resources\app.manifest" user32.lib gdi32.lib
    if ($LASTEXITCODE) { throw 'Fixture build failed' }
    if (Test-Path -LiteralPath "$PSScriptRoot\tests\integration.cpp") {
        & "$msvcRoot\bin\Hostx64\x64\cl.exe" /nologo /std:c++latest /EHsc /utf-8 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX "/I$PSScriptRoot\third_party" "/I$PSScriptRoot\include" "/Fo$BuildDir\integration.obj" "/Fe$BuildDir\desktop-control-integration.exe" "$PSScriptRoot\tests\integration.cpp" /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot\resources\app.manifest" user32.lib ole32.lib
        if ($LASTEXITCODE) { throw 'Integration harness build failed' }
    }

    $timeoutObjects = @('common','desktop','foreground','uia','capture','indicator','server') | ForEach-Object { "$BuildDir\$_.obj" }
    & "$msvcRoot\bin\Hostx64\x64\cl.exe" /nologo /std:c++latest /EHsc /utf-8 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 "/I$PSScriptRoot\include" "/I$PSScriptRoot\third_party" "/Fo$BuildDir\timeout-test.obj" "/Fe$BuildDir\desktop-control-timeout-tests.exe" "$PSScriptRoot\tests\test_timeout.cpp" @timeoutObjects /link windowsapp.lib d3d11.lib dxgi.lib user32.lib gdi32.lib dwmapi.lib ole32.lib oleaut32.lib shell32.lib propsys.lib shlwapi.lib windowscodecs.lib uiautomationcore.lib d2d1.lib dwrite.lib advapi32.lib
    if ($LASTEXITCODE) { throw 'Timeout test build failed' }
    & "$BuildDir\desktop-control-timeout-tests.exe"
    if ($LASTEXITCODE) { throw 'Timeout behavior tests failed' }

    python "$PSScriptRoot\tests\test_protocol.py" "$BuildDir\desktop-control.exe"
    if ($LASTEXITCODE) { throw 'Protocol tests failed' }
    return
}
& $cmake -S $PSScriptRoot -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $cmake --build $BuildDir --parallel 2
if ($LASTEXITCODE) { throw 'Build failed' }
& "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
