param(
    [Parameter(Mandatory = $true)]
    [string]$VcpkgRoot,

    [Parameter(Mandatory = $true)]
    [string]$HamlibRoot,

    [Parameter(Mandatory = $true)]
    [string]$QtRoot,

    [string]$BuildDirectory = "build-windows-hamlib",
    [string]$PackageDirectory = "package-windows-hamlib"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$qtBin = Join-Path $QtRoot "bin"
$hamlibRootPath = (Resolve-Path $HamlibRoot).Path
$triplet = "x64-windows"
$vcpkgBin = Join-Path $VcpkgRoot "installed\$triplet\bin"
$buildPath = Join-Path $repoRoot $BuildDirectory
$packagePath = Join-Path $repoRoot $PackageDirectory

foreach ($requiredPath in @($vcpkgExe, $toolchain, $qtBin, $hamlibRootPath)) {
    if (-not (Test-Path $requiredPath)) {
        throw "Required build path does not exist: $requiredPath"
    }
}

$rigHeader = Get-ChildItem -Path $hamlibRootPath -Recurse -File -Filter "rig.h" |
    Where-Object { $_.Directory.Name -eq "hamlib" -and $_.Directory.Parent.Name -eq "include" } |
    Select-Object -First 1
$hamlibDefinition = Get-ChildItem -Path $hamlibRootPath -Recurse -File -Filter "*hamlib*.def" |
    Select-Object -First 1
$hamlibDll = Get-ChildItem -Path $hamlibRootPath -Recurse -File -Filter "*hamlib*.dll" |
    Where-Object { $_.Name -match "^(lib)?hamlib.*\.dll$" } |
    Select-Object -First 1
if (-not $rigHeader -or -not $hamlibDefinition -or -not $hamlibDll) {
    throw "HamlibRoot must contain include\hamlib\rig.h, a Hamlib .def file, and its main DLL."
}
$hamlibInclude = $rigHeader.Directory.Parent.FullName
$hamlibBin = $hamlibDll.Directory.FullName
$hamlibImportDirectory = Join-Path $buildPath "hamlib-import"
$hamlibImportLibrary = Join-Path $hamlibImportDirectory "hamlib.lib"
$hamlibDllName = [System.IO.Path]::GetFileNameWithoutExtension($hamlibDll.Name)
New-Item -ItemType Directory -Force -Path $hamlibImportDirectory | Out-Null

& lib.exe "/def:$($hamlibDefinition.FullName)" "/machine:x64" `
    "/name:$hamlibDllName" "/out:$hamlibImportLibrary"
if ($LASTEXITCODE -ne 0) {
    throw "Could not create the x64 MSVC Hamlib import library from its .def file."
}

$env:VCPKG_ROOT = (Resolve-Path $VcpkgRoot).Path
$env:PATH = "$qtBin;$vcpkgBin;$hamlibBin;$env:PATH"

& $vcpkgExe install yaml-cpp --triplet $triplet
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg dependency installation failed with exit code $LASTEXITCODE."
}

cmake -S $repoRoot -B $buildPath -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DVCPKG_TARGET_TRIPLET=$triplet" `
    "-DCMAKE_PREFIX_PATH=$QtRoot" `
    "-DHAMLIB_INCLUDE_DIR=$hamlibInclude" `
    "-DHAMLIB_LIBRARY=$hamlibImportLibrary" `
    "-DHAMLIB_RUNTIME_DIR=$hamlibBin" `
    -DLOGGER_ENABLE_HAMLIB=ON `
    -DLOGGER_REQUIRE_HAMLIB=ON `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

cmake --build $buildPath --config Release
if ($LASTEXITCODE -ne 0) {
    throw "Windows build failed with exit code $LASTEXITCODE."
}

ctest --test-dir $buildPath -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw "Windows tests failed with exit code $LASTEXITCODE."
}

if (Test-Path $packagePath) {
    throw "Package directory already exists; move or remove it first: $packagePath"
}
cmake --install $buildPath --config Release --prefix $packagePath
if ($LASTEXITCODE -ne 0) {
    throw "Installation failed with exit code $LASTEXITCODE."
}

$executable = Join-Path $packagePath "bin\contest_logger.exe"
& (Join-Path $qtBin "windeployqt.exe") --release --compiler-runtime $executable
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE."
}

$packagedHamlibDll = Get-ChildItem (Join-Path $packagePath "bin") -Filter "*hamlib*.dll" |
    Where-Object { $_.Name -match "^(lib)?hamlib.*\.dll$" } |
    Select-Object -First 1
if (-not $packagedHamlibDll) {
    throw "The installed application package does not contain a Hamlib runtime DLL."
}

$archive = Join-Path $repoRoot "ContestLogger-windows-x64-hamlib.zip"
if (Test-Path $archive) {
    throw "Package archive already exists; move or remove it first: $archive"
}
Compress-Archive -Path (Join-Path $packagePath "*") -DestinationPath $archive
Write-Host "Created Hamlib-enabled Windows package: $archive"
