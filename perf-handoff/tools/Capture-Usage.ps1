param([int]$TargetProcessId = 0, [int]$Samples = 30, [int]$IntervalSeconds = 2)
$ErrorActionPreference = 'Stop'
$outDir = $PSScriptRoot
if ($TargetProcessId -eq 0) {
    $targets = @(Get-Process kyty_emulator -ErrorAction Stop)
    if ($targets.Count -ne 1) { throw 'Specify -TargetProcessId when more than one emulator is running.' }
    $TargetProcessId = $targets[0].Id
}
$logical = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
$meta = [ordered]@{
    CapturedAt = (Get-Date).ToString('o'); LogicalProcessors = $logical
    Process = Get-CimInstance Win32_Process -Filter "ProcessId=$TargetProcessId" | Select-Object ProcessId,ExecutablePath,CommandLine,CreationDate
    CPU = Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors
    GPU = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM)
    RAMBytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    OS = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,TotalVirtualMemorySize,FreeVirtualMemory,FreePhysicalMemory
    Pagefile = @(Get-CimInstance Win32_PageFileUsage | Select-Object Name,AllocatedBaseSize,CurrentUsage,PeakUsage)
    ReportedFPS = 16; FPSMethod = 'User reported 16 FPS; capture also samples rounded emulator window-title FPS and cumulative frame counter'
}
$meta | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$outDir\system-and-launch.json" -Encoding utf8
$initial = Get-Process -Id $TargetProcessId
$initialThreads = @{}
foreach ($t in $initial.Threads) { try { $initialThreads[$t.Id] = $t.TotalProcessorTime.TotalSeconds } catch {} }
$startCPU = $initial.TotalProcessorTime.TotalSeconds
$start = Get-Date
$paths = @('\GPU Engine(*)\Utilization Percentage', '\GPU Process Memory(*)\Dedicated Usage', '\GPU Process Memory(*)\Shared Usage')
$rows = [System.Collections.Generic.List[object]]::new()
$raw = [System.Collections.Generic.List[object]]::new()
$lastTime = $start
$lastCPU = $startCPU
Get-Counter -Counter $paths -SampleInterval $IntervalSeconds -MaxSamples $Samples -ErrorAction Continue | ForEach-Object {
    $now = Get-Date
    $proc = Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
    if ($null -eq $proc) { throw 'Target process exited during capture.' }
    $cpu = $proc.TotalProcessorTime.TotalSeconds
    $seconds = ($now - $lastTime).TotalSeconds
    $values = @($_.CounterSamples | Where-Object { $_.Status -eq 0 -and ($_.Path -notmatch '\\gpu ' -or $_.InstanceName -like "pid_${TargetProcessId}_*") })
    foreach ($v in $values) { $raw.Add([pscustomobject]@{Timestamp=$now.ToString('o');Path=$v.Path;Instance=$v.InstanceName;Value=$v.CookedValue}) }
    function ValueOf($suffix) { ($values | Where-Object Path -like "*$suffix" | Select-Object -First 1).CookedValue }
    $engines = @($values | Where-Object Path -like '*\gpu engine(*)\utilization percentage')
    $gpuPeak = ($engines | Measure-Object CookedValue -Maximum).Maximum
    $gpu3d = ($engines | Where-Object InstanceName -like '*engtype_3d' | Measure-Object CookedValue -Sum).Sum
    $fps = $null; $frameCount = $null
    if ($proc.MainWindowTitle -match 'frame: (\d+), fps: (\d+)') { $frameCount = [long]$Matches[1]; $fps = [int]$Matches[2] }
    $row = [pscustomobject]@{
        Timestamp=$now.ToString('o');ElapsedSeconds=[math]::Round(($now-$start).TotalSeconds,3)
        ProcessCPUPercent=[math]::Round(($cpu-$lastCPU)/$seconds/$logical*100,3)
        ProcessCPUCores=[math]::Round(($cpu-$lastCPU)/$seconds,3)
        WorkingSetMiB=$proc.WorkingSet64/1MB;PrivateBytesMiB=$proc.PrivateMemorySize64/1MB
        ProcessGPU3DPercent=$gpu3d;ProcessGPUBusiestEnginePercent=$gpuPeak
        ProcessGPUDedicatedMiB=(($values | Where-Object Path -like '*\gpu process memory(*)\dedicated usage' | Measure-Object CookedValue -Sum).Sum)/1MB
        ProcessGPUSharedMiB=(($values | Where-Object Path -like '*\gpu process memory(*)\shared usage' | Measure-Object CookedValue -Sum).Sum)/1MB
        WindowTitle=$proc.MainWindowTitle;WindowReportedFPS=$fps;FrameCount=$frameCount
    }
    $rows.Add($row)
    $rows | Export-Csv -LiteralPath "$outDir\usage-samples.csv" -NoTypeInformation
    $lastCPU=$cpu; $lastTime=$now
}
$raw | Export-Csv -LiteralPath "$outDir\raw-counters.csv" -NoTypeInformation
$end=Get-Date
$final=Get-Process -Id $TargetProcessId
$threadRows=foreach ($t in $final.Threads) {
    try {
        if ($initialThreads.ContainsKey($t.Id)) {
            $delta=$t.TotalProcessorTime.TotalSeconds-$initialThreads[$t.Id]
            [pscustomobject]@{ThreadId=$t.Id;CPUSeconds=$delta;PercentOfOneLogicalCPU=100*$delta/($end-$start).TotalSeconds}
        }
    } catch {}
}
$threadRows | Sort-Object CPUSeconds -Descending | Export-Csv -LiteralPath "$outDir\thread-cpu.csv" -NoTypeInformation
[pscustomobject]@{DurationSeconds=($end-$start).TotalSeconds;Samples=$rows.Count;ProcessCPUSeconds=$final.TotalProcessorTime.TotalSeconds-$startCPU;AverageProcessCPUPercent=100*($final.TotalProcessorTime.TotalSeconds-$startCPU)/($end-$start).TotalSeconds/$logical} | ConvertTo-Json | Set-Content -LiteralPath "$outDir\capture-summary.json" -Encoding utf8
Write-Output "Captured $($rows.Count) samples in $outDir"
