# Clear CI_COMMIT_MESSAGE and CI_COMMIT_DESCRIPTION for please meson
# when the commit message is complicated
$env:CI_COMMIT_MESSAGE=""
$env:CI_COMMIT_DESCRIPTION=""

# force the CA cert cache to be rebuilt, in case Meson tries to access anything
Write-Host "Refreshing Windows TLS CA cache"
(New-Object System.Net.WebClient).DownloadString("https://github.com") >$null

$env:PYTHONUTF8=1

Get-Date
Write-Host "Compiling Mesa"
$builddir = New-Item -Force -ItemType Directory -Name "_build"
$installdir = New-Item -Force -ItemType Directory -Name "_install"
$builddir=$builddir.FullName
$installdir=$installdir.FullName
$sourcedir=$PWD

Remove-Item -Recurse -Force $builddir
Remove-Item -Recurse -Force $installdir
New-Item -ItemType Directory -Path $builddir
New-Item -ItemType Directory -Path $installdir

Write-Output "*" > $builddir\.gitignore
Write-Output "*" > $installdir\.gitignore

Write-Output builddir:$builddir
Write-Output installdir:$installdir
Write-Output sourcedir:$sourcedir

$MyPath = $MyInvocation.MyCommand.Path | Split-Path -Parent
. "$MyPath\mesa_vs_init.ps1"

$depsInstallPath="C:\mesa-deps"

Push-Location $builddir

meson `
--default-library=shared `
--buildtype=release `
--wrap-mode=nodownload `
-Db_ndebug=false `
-Db_vscrt=mt `
--cmake-prefix-path="$depsInstallPath" `
--pkg-config-path="$depsInstallPath\lib\pkgconfig;$depsInstallPath\share\pkgconfig" `
--prefix="$installdir" `
-Dllvm=enabled `
-Dshared-llvm=disabled `
-Dvulkan-drivers="swrast,amd,microsoft-experimental" `
-Dgallium-drivers="swrast,d3d12,zink" `
-Dgallium-va=true `
-Dvideo-codecs="h264dec,h264enc,h265dec,h265enc,vc1dec" `
-Dshared-glapi=enabled `
-Dgles1=enabled `
-Dgles2=enabled `
-Dgallium-opencl=icd `
-Dgallium-rusticl=false `
-Dopencl-spirv=true `
-Dmicrosoft-clc=enabled `
-Dstatic-libclc=all `
-Dspirv-to-dxil=true `
-Dbuild-tests=true `
-Dwerror=true `
-Dwarning_level=2 `
$sourcedir && `
meson install && `
meson test --num-processes 32 --print-errorlogs

$buildstatus = $?
Pop-Location

Get-Date

if (!$buildstatus) {
  Write-Host "Mesa build or test failed"
  Exit 1
}

Copy-Item ".\.gitlab-ci\windows\piglit_run.ps1" -Destination $installdir

Copy-Item ".\.gitlab-ci\windows\spirv2dxil_check.ps1" -Destination $installdir
Copy-Item ".\.gitlab-ci\windows\spirv2dxil_run.ps1" -Destination $installdir

Copy-Item ".\.gitlab-ci\windows\deqp_runner_run.ps1" -Destination $installdir
Copy-Item ".\src\microsoft\ci\deqp-dozen.toml" -Destination $installdir

Get-ChildItem -Recurse -Filter "ci" | Get-ChildItem -Filter "*.txt" | Copy-Item -Destination $installdir
