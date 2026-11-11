# 1. Requisitos e instalação

## No PC (Windows)

### Visual Studio 2022 Build Tools (recompilador)

O `xbe_recomp` roda no Windows e é compilado com o `clang-cl`.

1. Baixe o **Build Tools for Visual Studio 2022** (o instalador gratuito da Microsoft).
2. Marque a carga de trabalho **"Desenvolvimento para desktop com C++"**.
3. Nos componentes individuais, marque **"Ferramentas do C++ Clang para Windows"**.
4. Pasta padrão: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`. Se instalar em outra, passe `-VsDir "<pasta>"` ao `recompilar.ps1`.

O CMake e o Ninja já vêm no Build Tools. O `git` precisa estar no PATH: na primeira vez o CMake baixa o Zydis e o toml++ do GitHub.

### devkitPro (build do Switch)

1. Instale o **devkitPro** pelo instalador do Windows (pasta padrão `C:\devkitPro`), marcando **Switch Development**.
2. Abra o bash dele (`C:\devkitPro\msys2\usr\bin\bash.exe -l`) e instale o que o build usa:

```bash
pacman -S --needed switch-dev switch-cmake switch-libzstd switch-libexpat switch-zlib cmake ninja
```

O kit foi testado com o devkitA64 (GCC 15/16) e o CMake 4.4 do msys2.

### Python 3

Opcional: só para scripts como o `europe_to_us.py` do Black.

## Caminho do kit: sem acento

O kit precisa ficar num caminho **só com letras sem acento e sem espaço**, como `C:\XboxSwitchPortKit`. O `xbe_recomp` abre os arquivos com caminhos ANSI, então "Área de Trabalho" quebra. O `recompilar.ps1` confere isso e avisa.

## Memória do PC

O código recompilado tem mais de 100 MB de C++ em 64 arquivos grandes. Cada processo do GCC pode usar de 1 a 2 GB de RAM:

- 16 GB de RAM: `JOBS=6` (o padrão);
- 32 GB ou mais: `JOBS=8` a `JOBS=12`.

Se o build morrer sem mensagem ("Killed", "out of memory"), baixe o `JOBS`.

## No Switch

- **Atmosphère** (CFW) com o **hbmenu**.
- Abra os ports em **modo título**: segure **R** ao abrir qualquer jogo instalado e escolha o `.nro` no hbmenu. Pelo álbum (modo applet) o homebrew recebe bem menos memória, e o jogo pode faltar memória ou nem iniciar.
- Espaço no SD: o tamanho do jogo (o Black tem uns 4 GB) mais uns 30 MB do `.nro`.

## Pastas no SD (por port)

```
sdmc:/switch/<nome>/
├── <nome>.nro          o port
├── <nome>.toml         configurações (criado na primeira vez)
├── game/               arquivos do disco: default.xbe + pastas, ou um .iso
├── logs/               <nome>_NNN.log (um por execução) e stderr.log
├── hdd/                o "HD" do Xbox: saves (E:), cache (Z:)
└── shadercache/        shaders já compilados (a 1ª vez de cada cena é mais lenta)
```
