**Português (BR)** | [English](README.en.md) | [Español](README.es.md)

# Cold Fear (Xbox) para Nintendo Switch

Port experimental por recompilação estática: o executável x86 do Xbox vira C, compilado para ARM64, com o toolkit [xboxrecomp](xboxrecomp/) e um renderizador Vulkan (NVK). Não é emulação: o código do jogo roda nativo, e só o hardware do Xbox (GPU NV2A, áudio, kernel) é reimplementado.

Versão **0.0.1** — autor RECOMP-GUTO.

**Este repositório não contém o jogo.** Os arquivos do disco, o XBE e o código C gerado a partir dele não são distribuídos. Para compilar ou jogar você precisa da sua própria cópia de Cold Fear (Xbox).

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

## Próximas correções

- **Quadrados escuros (passada `[REVSUB]`):** a causa foi encontrada. A comparação do mapa de sombra das lâmpadas estava invertida, e elas escureciam toda a área iluminada. A correção está em teste no Switch e entra na próxima versão.
- **Desempenho:** subir dos ~17–24 FPS atuais no navio, reduzindo o custo da troca entre as threads do jogo, das leituras de TLS e das alocações do driver NVK.

## Jogar (release pronta)

1. Baixe `coldfear.nro` em [Releases](https://github.com/gerisson1996/COLD-FEAR-NX-PORT/releases).
2. Copie para `sdmc:/switch/coldfear/`.
3. Copie os arquivos do seu disco para `sdmc:/switch/coldfear/game/`.
4. Abra pelo Title Takeover (segure R ao abrir um jogo instalado), para ter a memória inteira.

```text
switch/coldfear/
  coldfear.nro
  coldfear_env.txt   (opcional)
  game/
    default.xbe
    bigfile.bin
    data/
```

O log fica em `sdmc:/switch/coldfear/coldfear_log.txt`.

## Compilar

Você precisa de:

- devkitPro com devkitA64, libnx e SDL2 (MSYS2).
- O SDK NVK em `nvk_sdk/` (Mesa NVK para Switch; não incluído aqui por tamanho).
- Python com `capstone`, para gerar o código C.
- Os arquivos do seu disco de Cold Fear em `game/`.

Gerar o código C a partir do seu `default.xbe`:

```powershell
.\tools\regen.ps1 -Python 'C:\caminho\python.exe'
```

Compilar (MSYS2/devkitPro, nesta pasta):

```bash
bash switch/build.sh
```

Montar a pasta do cartão SD em `switch_sd/`:

```powershell
.\tools\package.ps1
```

Para conferir os arquivos do disco contra uma ISO:

```powershell
python .\tools\verify_disc.py 'C:\caminho\Cold Fear.iso'
```

## Opções (`coldfear_env.txt`)

Uma por linha, `CHAVE=valor`; linhas com `#` são ignoradas.

| Opção | Efeito |
| --- | --- |
| `COLDFEAR_LOG=0` | sem log |
| `NV2A_BRIGHTNESS=n` | brilho inicial (0–10) se ainda não houver `brightness.txt` |
| `NV2A_SKIP_REVSUB=1` | esconde a passada que desenha os quadrados escuros |
| `COLDFEAR_CPU_MHZ` / `COLDFEAR_GPU_MHZ` / `COLDFEAR_MEM_MHZ` | overclock opcional |

## Licenças e origem

Veja `docs/PROVENANCE.md` para a origem dos componentes. O toolkit xboxrecomp mantém suas licenças em `xboxrecomp/LICENSE`, `xboxrecomp/NOTICE` e `xboxrecomp/LICENSES/`; o restante segue `LICENSE` e `NOTICE` na raiz. Não distribua os arquivos do jogo, a ISO ou o código gerado.
