@echo off
chcp 65001 >nul
set MSYSTEM=MSYS
set CHERE_INVOKING=1
set "HERE=%~dp0"
cd /d "%HERE%"
C:\devkitPro\msys2\usr\bin\bash.exe -lc "export DEVKITPRO=/opt/devkitpro; export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH; cd \"$(cygpath -u \"$HERE\")\" && ./switch/build.sh; echo EXIT=$? > analysis/build-switch.exit"
echo.
echo Build terminado. Pode fechar esta janela.
pause
