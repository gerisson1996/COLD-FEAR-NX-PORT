# Runs the PC test build with a script of presses and screenshots, then closes it.
# pc_roteiro.ps1 <exe> <game folder> <script file> [extra args]
# script lines: "<seconds> cmd <BUTTONS> [frames]" | "<seconds> shot" | "<seconds> trace" (one frame of draws in the log) | "<seconds> end"  (seconds from start)
param([string]$Exe, [string]$Game, [string]$Script, [string]$Extra = "")
$dir = Split-Path $Exe
Remove-Item -Force -ErrorAction SilentlyContinue "$dir\cmd", "$dir\shot"
$argv = @("`"$Game`"")
if ($Extra) { $argv += $Extra }
$p = Start-Process -FilePath $Exe -ArgumentList $argv -WorkingDirectory $dir -PassThru
$start = Get-Date
foreach ($line in Get-Content $Script) {
    if ($line -match '^\s*#' -or $line.Trim() -eq '') { continue }
    $parts = $line.Trim() -split '\s+'
    $at = [double]$parts[0]
    while (((Get-Date) - $start).TotalSeconds -lt $at) {
        if ($p.HasExited) { Write-Output "exited early ($($p.ExitCode))"; exit 1 }
        Start-Sleep -Milliseconds 100
    }
    switch ($parts[1]) {
        'cmd' {
            while (Test-Path "$dir\cmd") { Start-Sleep -Milliseconds 50 }
            ($parts[2..($parts.Length - 1)] -join ' ') | Set-Content -NoNewline -Encoding ascii "$dir\cmd"
        }
        'shot' { '' | Set-Content -Encoding ascii "$dir\shot" }
        'trace' { '' | Set-Content -Encoding ascii "$dir\trace" }
        'end' { if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }; Write-Output "ended at $at s"; exit 0 }
    }
}
