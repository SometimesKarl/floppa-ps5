# Copies the current clangcl-release build to builds\<sha>-clangcl with its manifest.
# Usage: powershell -File stage_build.ps1 <short-sha>
param([Parameter(Mandatory = $true)][string]$Sha)
$pe  = 'C:\Users\himav\Desktop\kyty ps5\Performance Experiments'
$src = 'C:\Users\himav\Desktop\kyty ps5-src\_Build'
$d   = "$pe\builds\$Sha-clangcl"
New-Item -ItemType Directory -Force $d | Out-Null
Copy-Item "$src\clangcl-release\kyty_emulator.exe", "$src\clangcl-release\kyty_emulator.pdb" $d
Copy-Item "$pe\builds\f149b3d-clangcl\libwinpthread-1.dll" $d
$log = Get-Content "$src\build-$Sha.log"
$i = ($log | Select-String -Pattern '^source_commit:' | Select-Object -First 1).LineNumber
$log[($i - 1)..($log.Count - 1)] | Set-Content "$d\build-manifest.txt" -Encoding utf8
"$Sha staged: " + (Get-FileHash "$d\kyty_emulator.exe").Hash.Substring(0, 16)
