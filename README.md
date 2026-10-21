# Cold Fear (Xbox) para Nintendo Switch

Port experimental por recompilação estática: o executável x86 do Xbox vira C, compilado para ARM64, com o toolkit [xboxrecomp](xboxrecomp/) e um renderizador Vulkan (NVK). Não é emulação: o código do jogo roda nativo, e só o hardware do Xbox (GPU NV2A, áudio, kernel) é reimplementado.

Versão **0.0.1** — autor RECOMP-GUTO.

**Este repositório não contém o jogo.** Os arquivos do disco, o XBE e o código C gerado a partir dele não são distribuídos. Para compilar você precisa da sua própria cópia de Cold Fear (Xbox).

## Estado

Jogável no Switch real, do menu ao gameplay no navio.

- Áudio: efeitos, vozes e música (streams do FMOD tocados no host).
- Vídeo: Vulkan/NVK com shaders de vértice e de pixel do NV2A traduzidos para GLSL, névoa, stencil, mapas de sombra da lâmpada, texturas de volume, polygon offset, rampa de gama do jogo.
- Iluminação fixa do NV2A (luzes por vértice, geração de coordenadas de textura e matrizes de textura) implementada no shader de vértice.
- Brilho ajustável no jogo: segure **−** e use o direcional **cima/baixo** (0 a 10, salvo no SD).
- Desempenho: cerca de 17–24 FPS no navio, com overclock opcional.

Problemas conhecidos:

- Quadrados escuros de uma passada de sombra por subtração em algumas áreas (`NV2A_SKIP_REVSUB=1` no `coldfear_env.txt` esconde a passada enquanto não há correção).
- Brilho especular da iluminação fixa ainda não calculado.
- Logs de diagnóstico ainda compilados.

## O que você precisa

- devkitPro com devkitA64, libnx e SDL2 (MSYS2).
- O SDK NVK em `nvk_sdk/` (Mesa NVK para Switch; não incluído aqui por tamanho).
- Python com `capstone`, para gerar o código C.
- Os arquivos do seu disco de Cold Fear em `game/` (`default.xbe`, `bigfile.bin`, `data/`...).

## Gerar o código C

```powershell
.\tools\regen.ps1 -Python 'C:\caminho\python.exe'
```

Analisa `game/default.xbe`, traduz para `gen/` e aplica as correções em `tools/fix_*.py`. Para conferir os arquivos do disco contra uma ISO:

```powershell
python .\tools\verify_disc.py 'C:\caminho\Cold Fear.iso'
```

## Compilar

No MSYS2/devkitPro, dentro desta pasta:

```bash
bash switch/build.sh
```

Gera `build-switch-coldfear/coldfear_recomp.nro`.

## Preparar o cartão SD

```powershell
.\tools\package.ps1
```

Copie `switch_sd/switch/` para `sdmc:/switch/`:

```text
switch/coldfear/
  coldfear.nro
  coldfear_env.txt
  game/
    default.xbe
    bigfile.bin
    data/
```

Abra pelo Title Takeover (segure R ao abrir um jogo instalado), para ter a memória inteira. O log fica em `sdmc:/switch/coldfear/coldfear_log.txt`.

## Opções (`coldfear_env.txt`)

Uma por linha, `CHAVE=valor`; linhas com `#` são ignoradas.

| Opção | Efeito |
| --- | --- |
| `COLDFEAR_LOG=0` | sem log |
| `NV2A_BRIGHTNESS=n` | brilho inicial (0–10) se ainda não houver `brightness.txt` |
| `NV2A_SKIP_REVSUB=1` | esconde a passada que desenha os quadrados escuros |
| `COLDFEAR_CPU_MHZ` / `COLDFEAR_GPU_MHZ` / `COLDFEAR_MEM_MHZ` | overclock opcional |

## Apoie o projeto

O port é grátis e o código é aberto. Se quiser ajudar no tempo de desenvolvimento e nos testes no Switch real, você pode doar pelo Ko-fi:

[![Ko-fi](https://img.shields.io/badge/Ko--fi-apoiar-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/gerissoncassiano)

Doação é opcional: ninguém precisa doar para jogar, e o projeto não inclui nem vende o jogo.

## Licenças e origem

Veja `docs/PROVENANCE.md` para a origem dos componentes. O toolkit xboxrecomp mantém suas licenças em `xboxrecomp/LICENSE`, `xboxrecomp/NOTICE` e `xboxrecomp/LICENSES/`; o restante segue `LICENSE` e `NOTICE` na raiz. Não distribua os arquivos do jogo, a ISO ou o código gerado.
