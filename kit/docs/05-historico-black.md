# 5. O port do Black: o histórico de cada problema

Um exemplo real do ciclo "testa no console, lê o log, corrige". Cada etapa mostra o que apareceu no log, como a causa foi achada e o que mudou. Serve de modelo para os próximos ports.

---

## Ponto de partida

- **Jogo:** Black (Xbox, Criterion 2006), xbe americano (SHA-1 `c5bfed25…`). O xbe europeu da tradução PT-BR da Tokyo Games RGH (SHA-1 `796ad836…`) é convertido no americano pelo `europe.patch`, no PC (`tools/europe_to_us.py`) e no console (`game_files.cpp`).
- **Base:** [Black-Android](https://github.com/RanggaCasper/Black-Android) v1.5.0 (MIT). É o port de Android feito com o [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) (commit `99078b1`). Já tinha o D3D8 e o DirectSound reescritos em Vulkan e no áudio do sistema, e o jogo inteiro rodando em ARM64.
- **O que foi feito:** trocar a camada do Android (`native/`, SDL) por uma do Switch (`host-switch/`, libnx) e adaptar o SDK ao Switch.
- **Recompilação:** 64 partes, 15.042 funções, 107 MB de C++.

O primeiro `.nro` tinha 29,8 MB.

---

## Etapa 1: fechava logo no começo

**Log:** terminava em `InitKernelData`, sem `crash:`.

**Investigação.** Pus um `std::set_terminate` que relança a exceção e registra o tipo. Apareceu `uncaught std::system_error 88`. 88 é ENOSYS, "função não implementada". O runtime cria threads auxiliares com `std::thread(...).detach()`, e o libnx não tem `pthread_detach`.

**Correção:** `__syscall_thread_detach` devolvendo 0 (`stubs.cpp`). [Armadilha 5](04-armadilhas-do-switch.md#5-stdthreaddetach-derruba-o-programa).

Também entraram no log: sondas de criação de thread (`ProbeThread` em `boot.cpp`) e o `abort`/`assert` próprios.

---

## Etapa 2: fechava em silêncio no Vulkan

**Log:** parava no meio da inicialização do renderizador, sem mensagem.

**Investigação.** Pus linhas numeradas em cada passo do `Init` e do `CreateFixedImages` (`vulkan: image lut / 2d / 3d / cube...`). Parou na imagem 3D. Com o `__assert_func` próprio apareceu `assert image->image_type == VK_IMAGE_TYPE_3D`: a imagem era criada 2D com view 3D.

**Correção:** `imageType` 3D quando a view é 3D (`vulkan.cpp`). [Armadilha 6](04-armadilhas-do-switch.md#6-nvk-imagem-2d-com-view-3d).

---

## Etapa 3: crash no guest em `FFFFFFEC` (o som)

**Log:** `crash: guest address FFFFFFEC` na thread do jogo, pilha em `sub_84F90`.

**Investigação.** `FFFFFFEC` = 0 − 0x14: um campo lido de um ponteiro **nulo**. Segui a pilha:

1. `sub_84F90` usava o dispositivo de saída de som, que era nulo;
2. quem cria o dispositivo: `sub_8C090` → `sub_830D0` → a inicialização em `0x8C6C0` → `sub_8C570`, que carrega `Sound/DSP/BkXboxFx.bin`;
3. com traces em `fopen`/`fseek`/`ftell`/`fread` do jogo (`switch/trace.cpp`), o `ftell` devolvia **−1**: o jogo não conseguia ler o tamanho do arquivo, desistia dos efeitos de som e não criava o dispositivo.

**Primeira tentativa:** buffer intermediário para o IPC de arquivos ([armadilha 2](04-armadilhas-do-switch.md#2-leitura-e-escrita-de-arquivo-direto-na-memória-do-xbox-falham)). Era necessário, mas não resolveu.

**Segunda tentativa:** `stat()` no lugar de `std::filesystem::file_size`. Também falhou.

**Causa real:** o runtime abria tudo com `"r+b"`, e com o arquivo aberto para escrita o SD recusa abri-lo de novo, inclusive o `stat` por caminho.

**Correção:** abrir com `"rb"` quando o jogo não pede escrita, e o tamanho por `fstat` no arquivo já aberto (`files.cpp`). [Armadilha 4](04-armadilhas-do-switch.md#4-o-sd-não-abre-o-mesmo-arquivo-duas-vezes-se-uma-das-aberturas-for-de-escrita).

**Resultado:** `ftell -> 5CE4`, `dsound: effects image from memory, 22 effects`, dispositivo criado. Depois disso carregaram as fontes, os textos, o `GlobData`, o save, os menus, a música e as streams de som.

---

## Etapa 4: crash no primeiro vídeo

**Log:** o jogo abria `D:\videos\LO_N_UK.xmv` e caía com `guest address F22A7900` na thread 14. O `pc` estava na seção XMV do xbe (cerca de `0x24056B`).

**Investigação.** `F22A7900` está na janela write-combined `0xF0000000`, que não existe no Switch. O ponteiro vinha do D3D do jogo: `Lock2DSurface` (`0x26D910`) faz `or ecx, 0xF0000000` em `0x26D95C` e devolve isso como `pBits` para o decodificador escrever o quadro do vídeo.

**Correção:** gancho no `Lock2DSurface` que chama a original e converte o `pBits` para `0x80000000` (`src/d3d/d3d8.cpp`). [Armadilha 3](04-armadilhas-do-switch.md#3-a-janela-0xf0000000-write-combined-não-existe).

**Resultado:** **jogo rodando inteiro, com vídeos, menus, som e gameplay, a até 60 fps.**

---

## O que ficou em aberto

- **`FEAudio.awd`:** o `fread` do jogo nesse arquivo aparecia devolvendo 0 no trace, com a abertura feita em outra thread. Pode ser só a leitura assíncrona do jogo. Se faltar som nos menus, é o primeiro suspeito.
- **Diagnósticos:** os traces do `switch/trace.cpp` ficam desligados por padrão (`#define BLACK_TRACE` liga). Continuam ligados, por serem baratos e úteis num crash: `ProbeThread` no `boot.cpp` (3 linhas no boot), `app: alive` (1 linha a cada 5 s) e `fflush` por linha no log ([armadilha 10](04-armadilhas-do-switch.md#10-log-no-sd)).

---

## Lições

1. **Primeiro faça o log contar tudo:** handler de exceção, `abort`, `assert`, `terminate` e `stderr` num arquivo. Sem isso, metade dos crashes parece "fechou sozinho".
2. **Ponteiro nulo no guest** (`0000xxxx`, `FFFFFFxx`): siga para trás até quem deveria ter criado o objeto. Quase sempre é um arquivo que não carregou ou uma inicialização que falhou em silêncio.
3. **Trace nas funções de arquivo do jogo** mostra na hora o que ele tentou ler e o que voltou.
4. **Uma correção que "deveria funcionar" e não funcionou** é sinal de que a causa é outra. No Black, o `stat` também falhava porque o problema era o modo de abertura, não a função de tamanho.
5. **Guarde o `.elf` de cada build** que vai para o console.
