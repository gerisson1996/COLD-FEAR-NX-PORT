param(
    [string]$ToolkitPath = $env:XBOXRECOMP_DIR,
    [string]$Python = "python"
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $ToolkitPath) { $ToolkitPath = Join-Path $Root "xboxrecomp" }
$Xbe = Join-Path $Root "game\default.xbe"
$Analysis = Join-Path $Root "game\coldfear_analysis.json"
$Disasm = Join-Path $Root "analysis\disasm"
$FuncId = Join-Path $Root "analysis\func_id"
$Abi = Join-Path $Root "analysis\abi"
$Recomp = Join-Path $Root "analysis\recomp"
$Gen = Join-Path $Root "gen"

if (-not $ToolkitPath -or -not (Test-Path -LiteralPath (Join-Path $ToolkitPath "tools\recomp\__main__.py"))) {
    throw "Passe -ToolkitPath ou defina XBOXRECOMP_DIR para o diretório xboxrecomp."
}
if (-not (Test-Path -LiteralPath $Xbe)) {
    throw "XBE não encontrado: $Xbe"
}

& $Python -c "import capstone"
if ($LASTEXITCODE -ne 0) { throw "Python precisa do capstone. Use -Python com o interpretador configurado." }

New-Item -ItemType Directory -Path $Disasm,$FuncId,$Abi,$Recomp,$Gen -Force | Out-Null
Push-Location $ToolkitPath
try {
    & $Python -m tools.xbe_parser $Xbe --json $Analysis *> (Join-Path $Root "analysis\xbe_parser.log")
    if ($LASTEXITCODE -ne 0) { throw "tools.xbe_parser falhou (exit $LASTEXITCODE)." }

    $Seeds = Join-Path $Root "config\seed_functions.json"
    $SeedArg = if (Test-Path -LiteralPath $Seeds) { @("--seed-functions", $Seeds) } else { @() }
    & $Python -m tools.disasm $Xbe --analysis-json $Analysis --output $Disasm --force @SeedArg *> (Join-Path $Root "analysis\disasm.log")
    if ($LASTEXITCODE -ne 0) { throw "tools.disasm falhou (exit $LASTEXITCODE)." }

    & $Python -m tools.func_id $Xbe `
        --functions (Join-Path $Disasm "functions.json") `
        --strings (Join-Path $Disasm "strings.json") `
        --xrefs (Join-Path $Disasm "xrefs.json") `
        --output $FuncId *> (Join-Path $Root "analysis\func_id.log")
    if ($LASTEXITCODE -ne 0) { throw "tools.func_id falhou (exit $LASTEXITCODE)." }

    & $Python -m tools.abi_analysis $Xbe `
        --disasm-dir $Disasm `
        --func-id-dir $FuncId `
        --output-dir $Abi *> (Join-Path $Root "analysis\abi.log")
    if ($LASTEXITCODE -ne 0) { throw "tools.abi_analysis falhou (exit $LASTEXITCODE)." }

    & $Python -m tools.recomp $Xbe --game-name "Cold Fear" --all --split 250 `
        --gen-dir $Gen --output-dir $Recomp `
        --disasm-dir $Disasm --func-id-dir $FuncId --abi-dir $Abi `
        --mmio-sections DSOUND,XPP *> (Join-Path $Root "analysis\recomp.log")
    if ($LASTEXITCODE -ne 0) { throw "tools.recomp falhou (exit $LASTEXITCODE)." }
    # Relative paths from the project root: the devkitPro (MSYS2) Python
    # cannot open C:\ or C:/ paths.
    Push-Location $Root
    & $Python 'tools/fix_generated.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao aplicar as correcoes verificadas desta edicao.' }
    & $Python 'tools/fix_cmpltss.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao traduzir os cmpltss restantes.' }
    & $Python 'tools/fix_comiss_nan.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao corrigir comiss com NaN.' }
    & $Python 'tools/fix_flags_cpuid.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar pushfd/popfd/cpuid ao runtime.' }
    & $Python 'tools/fix_movsx_bp.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao corrigir movsx de bp.' }
    & $Python 'tools/fix_fmod_hle.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o tocador de streams do FMOD.' }
    & $Python 'tools/fix_ptank_flag.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao corrigir o formato das particulas.' }
    & $Python 'tools/fix_ptank_float_uv.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao desligar a UV comprimida das particulas.' }
    & $Python 'tools/fix_dswait.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar a espera de voz do DirectSound.' }
    & $Python 'tools/fix_game_log.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o log do jogo.' }
    & $Python 'tools/fix_entity_dbg.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o log de entidades.' }
    & $Python 'tools/fix_bank_dbg.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o log de bancos.' }
    & $Python 'tools/fix_flip_hook.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o gancho de flip.' }
    & $Python 'tools/fix_dsp_ack.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao aplicar o ack do DSP.' }
    & $Python 'tools/fix_memmove.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao trocar o memmove.' }
    & $Python 'tools/fix_overlay_stop.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao corrigir a parada do overlay.' }
    & $Python 'tools/fix_entnum_dbg.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o log de numero de entidade.' }
    & $Python 'tools/fix_gamma_ramp.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar a rampa de gama.' }
    & $Python 'tools/instrument_boot.py'
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao instrumentar o diagnostico de boot.' }
    Pop-Location
}
finally {
    Pop-Location
}

Write-Host "Código gerado em $Gen"

