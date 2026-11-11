<div align="center">

<img src="artwork/cold-fear-cover.png" alt="Cold Fear" width="280">

# Cold Fear (Xbox) para Nintendo Switch

Port por recompilação estática do Cold Fear do Xbox original, rodando nativo no Switch.

![version](https://img.shields.io/badge/version-0.1.37-blue) ![platform](https://img.shields.io/badge/platform-Nintendo%20Switch-E60012?logo=nintendoswitch&logoColor=white) ![license](https://img.shields.io/badge/license-MIT-green) [![Ko-fi](https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/gerissoncassiano)

**Português (BR)** · [English](README.en.md) · [Español](README.es.md)

[Recursos](#estado) · [Jogar](#jogar) · [Compilar](#compilar-port-novo-pasta-kit) · [Histórico](CHANGELOG.md) · [Apoiar](#apoie-o-projeto)

</div>

---

Port experimental por recompilação estática: o executável x86 do Xbox vira C++, compilado para ARM64, com o [XboxSwitchPortKit](kit/) e um renderizador Vulkan (NVK). Não é emulação: o código do jogo roda nativo, e as bibliotecas do Xbox (Direct3D 8, DirectSound, kernel) são reescritas para o Switch.

> **Este repositório não contém o jogo.** Os arquivos do disco, o XBE e o código C gerado a partir dele não são distribuídos. Para compilar ou jogar você precisa da sua própria cópia de Cold Fear (Xbox).

## Apoie o projeto

O port é grátis e o código é aberto. Se quiser ajudar no tempo de desenvolvimento e nos testes no Switch real, você pode doar pelo Ko-fi:

[![Ko-fi](https://img.shields.io/badge/Ko--fi-apoiar-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/gerissoncassiano)

Ou por **PIX** (chave e-mail): `pocketneogames@gmail.com`

<img src=".github/pix-qr.png" alt="QR Code PIX" width="200">

PIX copia e cola:

```text
00020126460014br.gov.bcb.pix0124pocketneogames@gmail.com5204000053039865802BR5911RECOMP GUTO6006BRASIL62070503***63048D6B
```

Doação é opcional: ninguém precisa doar para jogar, e o projeto não inclui nem vende o jogo.

## Estado

Jogável no Switch real, do menu ao gameplay, a 30 FPS na maior parte do tempo.

A partir da versão 0.1 o port foi refeito sobre o **XboxSwitchPortKit** (pasta [`kit/`](kit/)): em vez de emular a GPU NV2A, o Direct3D 8 e o DirectSound do Xbox são reescritos direto em Vulkan (NVK) e no áudio do Switch. O port antigo (0.0.x, com emulação da NV2A) continua na raiz do repositório como referência.

O que a 0.1.37 tem:

- **Giroscópio na mira:** segure **ZL** (mirar) e mova o console. O giro entra direto nos ângulos de mira do jogo, 1:1, sem a zona morta nem a aceleração do analógico. Opções: sensibilidade, velocidade vertical, eixo (girar / inclinar / os dois), estabilidade (filtra o tremor da mão), correção de drift e inverter X/Y.
- **Sombras das lâmpadas** corrigidas: o jogador e os objetos projetam sombra, sem as áreas escuras de borda reta que apareciam antes.
- **Tela OLED:** opção OLED SCREEN BOOST (AUTO / OFF / ON) que clareia os tons escuros. No AUTO liga sozinha num Switch OLED em modo portátil e desliga no dock.
- **Menu do port:** aperte **L3** e **R3** juntos (os dois analógicos). Resolução de renderização, filtro de tela, brilho, V-Sync, limite de FPS, contador de FPS, filtro anisotrópico, sombras, zonas mortas e sensibilidade dos analógicos, saída de áudio e as opções de giroscópio.
- Faixas pretas só no gameplay (como no original); os menus ocupam a tela inteira.
- Áudio: efeitos, vozes e música.
- Capa no menu do Switch.
- Sem overclock.

Desempenho medido no Switch (portátil, resolução 100%): 30 FPS na maior parte do jogo; em áreas pesadas cai para 23–26 FPS por alguns segundos, e também em carregamentos e na primeira vez que um efeito aparece. O log grava a cada 300 quadros o FPS e para onde foi o tempo (código do jogo, GPU, ocioso).

Problemas conhecidos:

- Quedas de FPS nas áreas mais pesadas: a resolução dinâmica demora a reagir.
- As opções do port ficam só no menu **L3 + R3**, ainda não dentro do menu do próprio jogo.

## Capturas

Capturas da build de teste para PC, que usa o mesmo código do jogo e o mesmo renderizador do Switch.

<p align="center"><img src="docs/img/gameplay-1.png" width="48%"> <img src="docs/img/gameplay-2.png" width="48%"></p>

## Controles

Os controles são os do jogo original. O port acrescenta:

| Botão | Função |
| --- | --- |
| Segurar **ZL** | Mirar; o giroscópio move a mira enquanto estiver segurado |
| **L3** + **R3** | Abrir o menu do port |
| Esquerda / direita no menu | Mudar o valor da opção |

## Versão do jogo usada

O port foi feito e testado com esta versão. Outras regiões ou revisões podem não funcionar, porque o código depende do executável exato.

| Campo | Valor |
| --- | --- |
| Jogo | Cold Fear (Xbox, Ubisoft, 2005) |
| Região | NTSC-U (EUA) |
| Title ID | `0x5553004B` (US-075) |
| Versão do certificado | 1 |
| XBE compilado em | 06/02/2005 (XDK 5849) |
| `default.xbe` | 3.624.960 bytes |
| SHA-256 do `default.xbe` | `7371e422ecd0ac5c95ebaa343a6a9c3d504f2f261a53ed8acf212f258ab377ef` |

Para conferir a sua cópia no PowerShell:

```powershell
Get-FileHash .\game\default.xbe -Algorithm SHA256
```

## Jogar

Você precisa da sua própria cópia de Cold Fear (Xbox, NTSC-U). Baixe o `coldfear.nro` em [Releases](https://github.com/gerisson1996/COLD-FEAR-NX-PORT/releases/latest) e monte o cartão assim:

```text
sdmc:/switch/coldfear/
  coldfear.nro
  game/              <- os arquivos do disco (default.xbe, bigfile.bin, DATA/...) ou um .iso
  logs/              <- criado pelo port
```

Abra pelo hbmenu **segurando R ao abrir um jogo instalado** (modo título, com a memória inteira).

Controles: os do jogo original. **ZL** mira (e ativa o giroscópio), **L3 + R3** abre o menu do port.

## Compilar (port novo, pasta `kit/`)

A pasta `kit/` tem o código do port e do kit, sem o jogo e sem os binários grandes:

| Pasta | O que é |
| --- | --- |
| `kit/exemplos/coldfear/` | O port: HLE do Direct3D 8 e do DirectSound, giroscópio, sombras, OLED, menu (`src/`), `port.cmake`, `config/game.toml`, capa. `HANDOFF.md` tem as notas de desenvolvimento. |
| `kit/host-switch/` | Camada do Switch: Vulkan/NVK, áudio, controles, giroscópio, ImGui. |
| `kit/host-pc/` | Build de teste para Windows, com o mesmo código do jogo e o mesmo renderizador, para achar bugs sem o console. |
| `kit/sdk/XboxXBE-Recomp/` | O recompilador e o runtime ([TGP482/XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), MIT), com as mudanças para o Switch. |
| `kit/ferramentas/`, `kit/docs/` | Scripts de recompilar, compilar e empacotar, e a documentação do kit. |

Não estão no repositório, por tamanho ou por serem do jogo: `nvk/` (driver Mesa NVK compilado para o Switch), `third_party/` (glslang, Dear ImGui, toml++), `exemplos/coldfear/assets/default.xbe` e `exemplos/coldfear/generated/` (o código C++ gerado do seu xbe).

Passos (detalhes em [`kit/LEIA-ME.md`](kit/LEIA-ME.md) e [`kit/docs/`](kit/docs/)):

1. Copie `kit/` para um caminho sem acento nem espaço, por exemplo `C:\XboxSwitchPortKit`, e complete com `nvk/` e `third_party/`.
2. Instale o Visual Studio 2022 Build Tools com Clang e o devkitPro com `switch-dev` ([requisitos](kit/docs/01-requisitos.md)).
3. Coloque o seu `default.xbe` em `exemplos\coldfear\assets\`.
4. Recompile, compile e empacote:

```powershell
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\recompilar.ps1 C:\XboxSwitchPortKit\exemplos\coldfear
```

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/XboxSwitchPortKit && JOBS=8 ./ferramentas/build_switch.sh exemplos/coldfear'
```

```powershell
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\empacotar.ps1 C:\XboxSwitchPortKit\exemplos\coldfear
```

O resultado fica em `sd\switch\coldfear\coldfear.nro`.

## Estrutura do repositório

| Pasta | Conteúdo |
| --- | --- |
| `kit/` | Port atual (0.1.x) e o kit usado para compilá-lo |
| `src/`, `xboxrecomp/`, `switch/`, `tools/`, `config/` | Port antigo (0.0.x), com emulação da NV2A |
| `docs/` | Origem dos componentes (`PROVENANCE.md`) e as notas do port antigo (`port-antigo/`) |
| `artwork/` | Capa e ícone |
| `CHANGELOG.md` | Histórico de versões |

## Histórico de versões

Veja [CHANGELOG.md](CHANGELOG.md).

## Port antigo (0.0.x)

O código na raiz (`src/`, `xboxrecomp/`, `switch/`, `tools/`) é o primeiro port, com emulação da GPU NV2A, parado na 0.0.3 (22–30 FPS). As instruções dele estão em `docs/port-antigo/BUILD_STATUS.md` e no histórico deste README.

## Licenças e origem

- Port novo: XboxXBE-Recomp (MIT), Black-Android, base de `kit/exemplos/coldfear/src/common` (MIT, `kit/exemplos/coldfear/LICENSE-Black-Android`), Mesa/NVK (MIT), glslang, Dear ImGui e toml++ nas licenças deles.
- Port antigo: veja `docs/PROVENANCE.md`; o toolkit xboxrecomp mantém suas licenças em `xboxrecomp/LICENSE`, `xboxrecomp/NOTICE` e `xboxrecomp/LICENSES/`; o restante segue `LICENSE` e `NOTICE` na raiz.
- Não distribua os arquivos do jogo, a ISO nem o código gerado.
