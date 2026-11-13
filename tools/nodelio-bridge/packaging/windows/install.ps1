$ErrorActionPreference = 'Stop'
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\NodeLioBridge'
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
$target = Join-Path $installDir 'nodelio-bridge.exe'
# A running executable is not killed or replaced implicitly. Exit the bridge before upgrading.
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'nodelio-bridge.exe') -Destination $target -Force
$routerSrc = Join-Path $PSScriptRoot 'meshcall-router.exe'
if (Test-Path -LiteralPath $routerSrc) {
	Copy-Item -LiteralPath $routerSrc -Destination (Join-Path $installDir 'meshcall-router.exe') -Force
}
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Programs')) 'NodeLio Bridge.lnk'))
$shortcut.TargetPath = $target
$shortcut.WorkingDirectory = $installDir
$shortcut.Save()
Start-Process -FilePath $target -WorkingDirectory $installDir -WindowStyle Hidden
Write-Host '已安装到当前用户。登录自启动请在配置页自行选择。'
