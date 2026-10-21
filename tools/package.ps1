param([string]$BuildDir = 'build-switch-coldfear')
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Nro = Join-Path (Join-Path $Root $BuildDir) 'coldfear_recomp.nro'
$Game = Join-Path $Root 'game'
if (-not (Test-Path -LiteralPath $Nro)) { throw "Build ausente: $Nro" }
if (-not (Test-Path -LiteralPath (Join-Path $Game 'default.xbe'))) { throw 'default.xbe ausente' }
$Header = [System.IO.File]::ReadAllBytes($Nro)
if ($Header.Length -lt 128 -or [System.Text.Encoding]::ASCII.GetString($Header,16,4) -ne 'NRO0') {
    throw 'O executavel nao possui um cabecalho NRO valido.'
}
$ImageSize = [BitConverter]::ToUInt32($Header,24)
if ($ImageSize -lt 128 -or $ImageSize -gt $Header.Length) { throw 'Tamanho NRO invalido.' }
$Dest = Join-Path $Root 'switch_sd\switch\coldfear'
New-Item -ItemType Directory -Path $Dest -Force | Out-Null
Copy-Item -LiteralPath $Nro -Destination (Join-Path $Dest 'coldfear.nro')
if (-not (Test-Path -LiteralPath (Join-Path $Dest 'coldfear_env.txt'))) {
    Copy-Item -LiteralPath (Join-Path $Root 'config\coldfear_env.txt') -Destination $Dest
}
& robocopy $Game (Join-Path $Dest 'game') /E /XF '*_analysis.json' /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { throw "Falha na copia do jogo: $LASTEXITCODE" }
$Manifest = [ordered]@{
    title = 'Cold Fear'
    port_version = '0.1.3 boot-trace'
    status = 'experimental; boot and gameplay not validated'
    renderer = 'Vulkan/NVK'
    nro_sha256 = (Get-FileHash -LiteralPath $Nro -Algorithm SHA256).Hash
    nro_bytes = $Header.Length
    xbe_sha256 = (Get-FileHash -LiteralPath (Join-Path $Game 'default.xbe') -Algorithm SHA256).Hash
}
$Manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Dest 'build-info.json') -Encoding utf8
Write-Host "Pacote experimental: $Dest"
Write-Host 'Copie switch_sd/switch para a raiz do SD. Boot e gameplay ainda precisam ser validados.'
