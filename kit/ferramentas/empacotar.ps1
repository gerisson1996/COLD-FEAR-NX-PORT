# Copia o .nro compilado para sd\switch\<nome>, a pasta que espelha o cartao SD, e mostra o SHA-256 (para
# saber qual build esta no console). Guarda tambem o .elf em analise\<nome>-<SHA8>.elf: e ele que traduz o
# endereco de um crash do log (ferramentas/endereco_crash.sh).
#
#   powershell -ExecutionPolicy Bypass -File ferramentas\empacotar.ps1 exemplos\black
param([Parameter(Mandatory = $true)][string]$Jogo)
$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent $PSScriptRoot
$game = (Resolve-Path -LiteralPath $Jogo).Path
$line = Get-Content -LiteralPath "$game\port.cmake" | Where-Object { $_ -match '^\s*set\(PORT_NAME\s' } | Select-Object -First 1
$name = $line -replace '^\s*set\(PORT_NAME\s+"?([^")\s]+)"?.*$', '$1'
if (-not $name) { throw "PORT_NAME nao encontrado em $game\port.cmake" }
$build = Join-Path $kit ('build\' + (Split-Path -Leaf $game))
$nro = "$build\$name.nro"
if (-not (Test-Path -LiteralPath $nro)) { throw "sem build: $nro (rode ferramentas/build_switch.sh)" }
$hash = (Get-FileHash -LiteralPath $nro -Algorithm SHA256).Hash
$dest = "$kit\sd\switch\$name"
New-Item -ItemType Directory -Path $dest, "$kit\analise" -Force | Out-Null
Copy-Item -LiteralPath $nro -Destination "$dest\$name.nro"
Copy-Item -LiteralPath "$build\$name.elf" -Destination ("$kit\analise\$name-" + $hash.Substring(0, 8) + '.elf')
Write-Host "$name.nro -> $dest"
Write-Host "SHA-256 $hash"
Write-Host "No cartao: sdmc:/switch/$name/$name.nro, o jogo em sdmc:/switch/$name/game, os logs em sdmc:/switch/$name/logs."
