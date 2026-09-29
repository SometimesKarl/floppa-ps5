# Samples dedicated (VRAM) and shared GPU memory of the running kyty_emulator process,
# plus its RAM working set, every $IntervalS seconds for $Count samples. Output: CSV to stdout.
param([int]$Count = 12, [int]$IntervalS = 5, [int]$WaitForProcessS = 300)

$deadline = (Get-Date).AddSeconds($WaitForProcessS)
$proc = $null
while (-not $proc -and (Get-Date) -lt $deadline) {
    $proc = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) { Start-Sleep -Seconds 2 }
}
if (-not $proc) { Write-Output "no kyty_emulator process"; exit 1 }
$emuPid = $proc.Id
Write-Output "t_s,dedicated_mib,shared_mib,working_set_mib,private_mib"
$start = Get-Date
for ($i = 0; $i -lt $Count; $i++) {
    try {
        $ded = (Get-Counter "\GPU Process Memory(pid_$($emuPid)_*)\Dedicated Usage" -ErrorAction Stop).CounterSamples |
            Measure-Object -Property CookedValue -Sum
        $sh = (Get-Counter "\GPU Process Memory(pid_$($emuPid)_*)\Shared Usage" -ErrorAction Stop).CounterSamples |
            Measure-Object -Property CookedValue -Sum
        $p = Get-Process -Id $emuPid -ErrorAction Stop
        $t = [int]((Get-Date) - $start).TotalSeconds
        Write-Output ("{0},{1:N0},{2:N0},{3:N0},{4:N0}" -f $t, ($ded.Sum / 1MB), ($sh.Sum / 1MB),
            ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB))
    } catch { Write-Output "sample failed: $_" }
    Start-Sleep -Seconds $IntervalS
}
