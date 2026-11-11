# Recompila o default.xbe de um jogo para C++ (a parte que roda no PC, antes do build do Switch).
#
#   powershell -ExecutionPolicy Bypass -File ferramentas\recompilar.ps1 exemplos\black
#
# Na pasta do jogo: assets\default.xbe -> generated\game (o C++ que o build do Switch compila).
# Sem config\game.toml, gera um primeiro com o xbe_symbols (acha as funcoes do XDK pelo xbe).
#
# Precisa do Visual Studio 2022 Build Tools com "Desenvolvimento para desktop com C++" e o componente
# "Ferramentas do C++ Clang" (clang-cl). Na primeira vez baixa o Zydis e o toml++ pelo git (precisa de internet).
#
# O caminho do kit e do jogo nao pode ter acento: o recompilador abre os arquivos com caminhos ANSI, e
# "Area de Trabalho" com acento quebra. Por isso o kit fica em C:\XboxSwitchPortKit.
param(
    [Parameter(Mandatory = $true)][string]$Jogo,
    [string]$VsDir = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
)
$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent $PSScriptRoot
$game = (Resolve-Path -LiteralPath $Jogo).Path
if ($kit -match '[^\x00-\x7F]' -or $game -match '[^\x00-\x7F]') {
    throw "caminho com acento ou caractere especial: $kit / $game. Mova para uma pasta como C:\XboxSwitchPortKit."
}
if (-not (Test-Path -LiteralPath "$game\assets\default.xbe")) { throw "falta $game\assets\default.xbe" }
if (-not (Test-Path -LiteralPath $VsDir)) { throw "Visual Studio Build Tools nao encontrado em $VsDir (use -VsDir)" }

$tools = "$kit\ferramentas\recompilador"
$build = "$tools\build"
$xdk = "$kit\sdk\XboxXBE-Recomp\xdk"
$cmake = "$VsDir\Common7\IDE\CommonExtensions\Microsoft\CMake"
$clang = "$VsDir\VC\Tools\Llvm\x64\bin\clang-cl.exe"
# o llvm-rc do Visual Studio: com o devkitPro no PATH, o CMake acharia o do msys2, que nao abre os caminhos do Windows
$rc = "$VsDir\VC\Tools\Llvm\x64\bin\llvm-rc.exe" -replace '\\', '/'  # com \ o CMake le "\P" como escape
$vsenv = "call `"$VsDir\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 >nul" +
         " && set PATH=$cmake\CMake\bin;$cmake\Ninja;%PATH%"

# 1. o recompilador (xbe_recomp) e o xbe_symbols, compilados uma vez
$cmd = "$vsenv && cmake -S `"$tools`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release" +
       " `"-DCMAKE_C_COMPILER=$clang`" `"-DCMAKE_CXX_COMPILER=$clang`" `"-DCMAKE_RC_COMPILER=$rc`"" +
       " && cmake --build `"$build`""
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "o build do recompilador falhou ($LASTEXITCODE)" }

# 2. config\game.toml: o que ja existe, ou um novo feito pelo xbe_symbols
if (-not (Test-Path -LiteralPath "$game\config\game.toml")) {
    New-Item -ItemType Directory -Path "$game\config" -Force | Out-Null
    & cmd.exe /c "cd /d `"$game`" && `"$build\xbe_symbols.exe`" assets\default.xbe `"$xdk`" --out config\game.toml"
    if ($LASTEXITCODE -ne 0) { throw "xbe_symbols falhou ($LASTEXITCODE): veja acima as funcoes do XDK que faltaram" }
    Write-Host "config\game.toml criado: confira os nomes que o xbe_symbols disse que faltaram."
}

# 3. a recompilacao: generated\game
& cmd.exe /c "cd /d `"$game`" && `"$build\xbe_recomp.exe`" config\game.toml --xdk `"$xdk`""
if ($LASTEXITCODE -ne 0) { throw "a recompilacao falhou ($LASTEXITCODE)" }
Write-Host "pronto: $game\generated\game"
