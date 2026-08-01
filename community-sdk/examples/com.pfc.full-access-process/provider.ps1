$ErrorActionPreference = "Stop"
[Console]::InputEncoding = [Text.Encoding]::UTF8
[Console]::OutputEncoding = [Text.Encoding]::UTF8

while (($line = [Console]::In.ReadLine()) -ne $null) {
    $message = $line | ConvertFrom-Json
    if ($message.type -eq "shutdown") {
        break
    }
    if ($message.type -eq "action" -and $message.action -eq "openTaskManager") {
        Start-Process taskmgr.exe
        continue
    }
    if ($message.type -in @("initialize", "instancesChanged")) {
        foreach ($instance in $message.instances) {
            @{
                type = "snapshot"
                instanceId = $instance.instanceId
                data = @{
                    processCount = @(Get-Process).Count
                    updatedAt = [DateTimeOffset]::Now.ToString("O")
                }
            } | ConvertTo-Json -Compress -Depth 8 | Write-Output
        }
    }
}
