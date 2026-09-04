# AI-authored developer fixture; deploy with Satsuma as SYSTEM before a test reboot.
$ErrorActionPreference = 'Stop'
$context = Get-Content (Join-Path $PSScriptRoot 'context.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$root = [IO.Path]::GetFullPath($context.test_root)
if ((Split-Path $root -Parent) -ne [Environment]::GetFolderPath('CommonApplicationData') -or
    (Split-Path $root -Leaf) -notmatch '^SatsumaAcceptance-[a-f0-9]{12}$' -or
    $context.task_name -cne (Split-Path $root -Leaf) -or
    $context.confirm -cne 'DISPOSABLE_VM_INSTALLER_TEST' -or
    (Get-CimInstance Win32_ComputerSystemProduct).UUID -ine $context.hardware_id -or
    -not [Security.Principal.WindowsIdentity]::GetCurrent().IsSystem) { throw 'Invalid fixture scope' }
if (Test-Path $root) { throw 'Fixture directory already exists' }
if (Get-ScheduledTask -TaskName $context.task_name -ErrorAction SilentlyContinue) { throw 'Fixture task already exists' }
$directory = New-Item -ItemType Directory -Path $root
$acl = New-Object Security.AccessControl.DirectorySecurity
$acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)')
Set-Acl -LiteralPath $directory.FullName -AclObject $acl
foreach ($file in @('context.json','fixtures.json','SatsumaVM.exe','same.exe','older.exe','newer.exe','probe.exe','real_vmware_installer_guest.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $root $file)
}
$script = Join-Path $root 'real_vmware_installer_guest.ps1'
$action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$script+'"')
$principal = New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 15)
Register-ScheduledTask -TaskName $context.task_name -Action $action -Principal $principal -Trigger $trigger -Settings $settings | Out-Null
Write-Output 'Prepared one-shot installer acceptance for the next Guest boot.'
