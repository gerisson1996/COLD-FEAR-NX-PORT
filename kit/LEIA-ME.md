# Xbox → Switch Port Kit

Kit para portar jogos do **Xbox original** para o **Nintendo Switch** como homebrew (`.nro`), por **recompilação estática**: o código x86 do jogo vira C++ no PC e é compilado para ARM64. Não é um emulador. O jogo roda nativo, com o Direct3D 8 e o DirectSound do Xbox reescritos em Vulkan (NVK) e no áudio do Switch.

Este kit saiu do port do **Black** (Criterion, 2006), com a tradução PT-BR da Tokyo Games RGH, que roda completo no Switch a até 60 fps. Ele vem pronto como exemplo em `exemplos/black`.

---

## Como funciona

```
 PC (Windows)                                   PC (devkitPro)                   Switch
┌───────────────┐   recompilar.ps1   ┌──────────────────┐   build_switch.sh   ┌──────────────┐
│ default.xbe   │ ─────────────────▶ │ generated/game   │ ──────────────────▶ │ jogo.nro     │
│ (x86, Xbox)   │   xbe_recomp       │ (C++, 100+ MB)   │   GCC ARM64 + NVK   │ + arquivos   │
└───────────────┘                    └──────────────────┘   + host-switch     │   do disco   │
                                                                              └──────────────┘
```

1. **Recompilação** (`ferramentas/recompilar.ps1`): o `xbe_recomp` do SDK lê o `default.xbe` e escreve cada função do jogo como C++ (`sub_XXXXXXXX`, o endereço no xbe).
2. **Build** (`ferramentas/build_switch.sh`): o GCC do devkitPro compila esse C++ com o runtime do SDK (kernel do Xbox, memória, threads, arquivos), o código do jogo (`src/`) e a camada do Switch (`host-switch/`), e gera o `.nro`.
3. **No console**: o `.nro` lê os arquivos do disco do jogo (pasta extraída ou `.iso`) em `sdmc:/switch/<nome>/game`.

---

## Pastas

| Pasta | O que é |
|---|---|
| `sdk/XboxXBE-Recomp` | O SDK de recompilação ([TGP482/XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), commit `99078b1`, MIT), **com as mudanças para o Switch** (memória, arquivos, threads). Ver [docs/04-armadilhas-do-switch.md](docs/04-armadilhas-do-switch.md). |
| `host-switch/` | A camada do Switch: `main.cpp` (applet, logs, crash handler), `vulkan.cpp` (renderizador NVK), `audio.cpp` (audren), `input.cpp` (HID/Joy-Con), `gyro.cpp` (mira por giroscópio, que entra no mouse do jogo), `game_files.cpp` (acha o jogo no SD), `stubs.cpp` (o que o Switch não tem), `CMakeLists.txt`. É genérica: cada jogo é configurado pelo seu `port.cmake`. |
| `nvk/` | O driver Vulkan NVK (Mesa para Switch) já compilado, linkado estaticamente. Só os arquivos que o build usa. |
| `third_party/` | glslang (compila os shaders HLSL no console), Dear ImGui (menu de opções), toml++ (configurações). |
| `ferramentas/` | Scripts: `recompilar.ps1`, `build_switch.sh`, `empacotar.ps1`, `endereco_crash.sh`, e `recompilador/` (CMake do `xbe_recomp`). |
| `exemplos/black/` | O port do Black inteiro, funcionando: modelo para os próximos. |
| `docs/` | A documentação, em ordem de leitura. |

Pastas criadas pelos scripts (podem ser apagadas, voltam no próximo build): `build/`, `sd/`, `analise/`, `ferramentas/recompilador/build/`.

---

## Começo rápido: compilar o Black

Requisitos instalados (ver [docs/01-requisitos.md](docs/01-requisitos.md)): Visual Studio 2022 Build Tools com Clang e devkitPro com `switch-dev`.

**1. O xbe.** O kit precisa do `default.xbe` **americano** do Black em `exemplos/black/assets/default.xbe`. Se você tem o europeu (o da tradução PT-BR), converta:

```bash
python exemplos/black/tools/europe_to_us.py "<pasta do jogo>/default.xbe" exemplos/black/src/common/europe.patch exemplos/black/assets/default.xbe
```

**2. Recompilar** (PowerShell, uns 2 minutos):

```bash
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\recompilar.ps1 C:\XboxSwitchPortKit\exemplos\black
```

**3. Compilar o `.nro`** (bash do devkitPro, de 20 a 40 minutos na primeira vez):

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/XboxSwitchPortKit && JOBS=8 ./ferramentas/build_switch.sh exemplos/black'
```

**4. Empacotar:**

```bash
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\empacotar.ps1 C:\XboxSwitchPortKit\exemplos\black
```

**5. No cartão SD:**

```
sdmc:/switch/black/black.nro        <- de C:\XboxSwitchPortKit\sd\switch\black\black.nro
sdmc:/switch/black/game/            <- os arquivos do disco (default.xbe + pastas) ou um .iso
```

Abra pelo hbmenu, **de preferência segurando R ao abrir um jogo** (modo título, com a memória toda). Os logs ficam em `sdmc:/switch/black/logs/`.

---

## Documentação

1. [Requisitos e instalação](docs/01-requisitos.md)
2. [Como fazer um port novo, passo a passo](docs/02-novo-port.md)
3. [Depuração: logs, crashes e traces](docs/03-depuracao.md)
4. [Armadilhas do Switch (e onde cada uma foi resolvida)](docs/04-armadilhas-do-switch.md)
5. [O port do Black: o histórico de cada problema](docs/05-historico-black.md)

---

## Licenças e arquivos do jogo

- XboxXBE-Recomp: MIT. Black-Android (base de `exemplos/black/src`): MIT (`exemplos/black/LICENSE-Black-Android`). Mesa/NVK: MIT. glslang, Dear ImGui, toml++: as licenças estão nas pastas deles.
- **Nenhum arquivo do jogo pode ser distribuído.** `exemplos/*/assets/default.xbe` e `exemplos/*/generated/` vêm da sua cópia do jogo: não publique essas pastas nem um `.nro` (que contém o código do jogo recompilado). Para compartilhar o kit, apague-as antes.
