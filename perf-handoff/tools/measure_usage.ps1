# Samples a running kyty_emulator.exe: GPU 3D usage, dedicated/shared video memory, CPU usage and
# RAM, every 2 s for the given seconds, and prints averages.
# Usage: powershell -File measure_usage.ps1 <seconds> <out.csv>
param([int]$Seconds = 40, [string]$Out = "usage.csv")
$p = Get-Process kyty_emulator -ErrorAction Stop | Select-Object -First 1
$id = $p.Id
$cores = [Environment]::ProcessorCount
$rows = @()
$lastCpu = $p.TotalProcessorTime.TotalMilliseconds
$lastT = Get-Date
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
	$c = Get-Counter -Counter "\GPU Engine(pid_$($id)_*engtype_3D)\Utilization Percentage", "\GPU Process Memory(pid_$($id)_*)\Dedicated Usage", "\GPU Process Memory(pid_$($id)_*)\Shared Usage" -ErrorAction SilentlyContinue
	$gpu = ($c.CounterSamples | Where-Object { $_.Path -match 'utilization' } | Measure-Object CookedValue -Sum).Sum
	$ded = ($c.CounterSamples | Where-Object { $_.Path -match 'dedicated' } | Measure-Object CookedValue -Sum).Sum / 1MB
	$sha = ($c.CounterSamples | Where-Object { $_.Path -match 'shared' } | Measure-Object CookedValue -Sum).Sum / 1MB
	$p.Refresh()
	$now = Get-Date
	$cpu = ($p.TotalProcessorTime.TotalMilliseconds - $lastCpu) / ($now - $lastT).TotalMilliseconds * 100 / $cores
	$lastCpu = $p.TotalProcessorTime.TotalMilliseconds; $lastT = $now
	$rows += [pscustomobject]@{ gpu3d = [math]::Round($gpu, 1); vram_mb = [int]$ded; shared_mb = [int]$sha; cpu_pct = [math]::Round($cpu, 1); ram_ws_mb = [int]($p.WorkingSet64 / 1MB) }
	Start-Sleep -Milliseconds 1500
}
$rows | Export-Csv -NoTypeInformation $Out
$avg = { param($n) [math]::Round(($rows | Measure-Object $n -Average).Average, 1) }
"GPU 3D {0}% | VRAM {1} MB (shared {2} MB) | CPU {3}% of all cores | RAM {4} MB" -f (& $avg gpu3d), (& $avg vram_mb), (& $avg shared_mb), (& $avg cpu_pct), (& $avg ram_ws_mb)
