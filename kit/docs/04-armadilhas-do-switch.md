# 4. Armadilhas do Switch (e onde cada uma foi resolvida)

Tudo o que quebrou no port do Black por ser **Switch** (libnx, newlib, NVK, o cartão SD), e não por causa do jogo. Já está corrigido no kit e vale para qualquer port. Se um jogo novo mostrar algo parecido, comece por aqui.

Formato: **sintoma → causa → correção → arquivo**.

---

## 1. Memória do Xbox: sem `mmap`, sem duas visões

**Causa.** O runtime reserva 4 GB e põe a memória do Xbox em endereços fixos (`g_base + endereço do Xbox`). No PC isso usa `mmap`/`VirtualAlloc`. O Switch não tem `mmap`, e o kernel recusa mapear a mesma memória duas vezes.

**Correção.** Reserva de 4 GB com `virtmemFindAslr`. A memória é feita de blocos do heap mostrados no endereço certo por *code memory* (`svcCreateCodeMemory`), 1 MB por vez.

**Arquivo:** `sdk/XboxXBE-Recomp/runtime/kernel/memory.cpp` (blocos `#ifdef __SWITCH__`).

**Consequências:** as armadilhas 2 e 3.

---

## 2. Leitura e escrita de arquivo direto na memória do Xbox falham

**Sintoma.** `fread`/`fwrite` com destino na memória do Xbox devolvem 0 ou erro, mas para buffers normais funcionam.

**Causa.** A memória do Xbox é code memory, e o serviço de arquivos do Switch (IPC) recusa esse tipo de memória como buffer.

**Correção.** `Transfer` em `files.cpp` passa por um buffer intermediário normal (`thread_local std::vector`, até 1 MB por pedaço), nas leituras e escritas de arquivo e de disco (`.iso`).

**Arquivo:** `sdk/.../kernel/files.cpp` (`Transfer`).

---

## 3. A janela `0xF0000000` (write-combined) não existe

**Sintoma.** `crash: guest address F0xxxxxx`. No Black foi o decodificador de vídeo XMV escrevendo em `F22A7900`.

**Causa.** No Xbox, a RAM física aparece em `0x80000000` (normal) e de novo em `0xF0000000` (write-combined, para a GPU). No Switch não dá para mapear duas visões (armadilha 1), então só existe a de `0x80000000`.

**Correção.** Achar quem produz o ponteiro `0xF...` e trocar pelo mesmo endereço em `0x80000000`. No Black foi o D3D do próprio jogo: `Lock2DSurface` (em `0x26D910`) faz `or ecx, 0xF0000000`. O gancho chama a original e reescreve o `pBits` devolvido no `D3DLOCKED_RECT`.

**Arquivo:** `exemplos/black/src/d3d/d3d8.cpp` (bloco `#ifdef __SWITCH__` no `Lock2DSurface`).

**Em outros jogos:** outros lugares do D3D do Black também calculam ponteiros `0xF...` (`0x26C067`, `0x26C13B`, `0x27327B`), mas o jogo não chegou a usá-los. Outro jogo pode usar. A correção é igual: gancho na função, conversão do ponteiro. A conta é `ptr - kWriteCombineBase + kContiguousBase`.

---

## 4. O SD não abre o mesmo arquivo duas vezes se uma das aberturas for de escrita

**Sintoma.** O tamanho do arquivo não é lido: `ftell` devolve −1, `std::filesystem::file_size` e `stat()` falham, e aparece `NtQueryInformationFile: no size for ...` no log. No Black, o som não iniciava, porque `BkXboxFx.bin` "tinha tamanho −1".

**Causa.** O runtime abria todo arquivo com `"r+b"` (leitura e escrita). Com um arquivo aberto para escrita, o SD recusa abri-lo de novo, e o `stat` por caminho abre o arquivo por baixo dos panos.

**Correção.**

1. Abrir com `"rb"` quando o jogo não pediu escrita: a máscara de acesso `0x520D0116` cobre os bits de escrita do NT.
2. Tamanho pelo arquivo já aberto (`fstat(fileno(fp))`, função `OpenSize`), nunca pelo caminho.
3. Mudança de tamanho com `OpenSize` + `truncate`.

**Arquivos:** `sdk/.../kernel/files.cpp` (`NtCreateFile`, `NtOpenFile`, `NtQueryInformationFile`, `NtSetInformationFile`, `OpenSize`) e `host-switch/game_files.cpp` (`stat` no lugar de `fs::file_size`).

**Regra:** no Switch, não use `std::filesystem::file_size`; use `stat` ou, melhor, `fstat` no arquivo aberto.

---

## 5. `std::thread::detach` derruba o programa

**Sintoma.** `crash: uncaught std::system_error 88` logo no começo (`InitKernelData`).

**Causa.** O libnx não implementa `pthread_detach`; a chamada devolve ENOSYS (88) e o `std::thread` lança exceção.

**Correção.** `__syscall_thread_detach` que devolve 0. A thread "solta" só libera a pilha quando o programa fecha.

**Arquivo:** `host-switch/stubs.cpp`.

---

## 6. NVK: imagem 2D com view 3D

**Sintoma.** O programa fecha sem `crash:` em `CreateFixedImages`; com o handler, aparece `crash: assert image->image_type == VK_IMAGE_TYPE_3D`.

**Causa.** O backend criava imagens com `imageType` 2D e view 3D. Drivers de PC aceitam; o NVK tem `assert`.

**Correção.** `ci.imageType = d > 1 || vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;`

**Arquivo:** `host-switch/vulkan.cpp` (`CreateImage`).

**Regra:** o NVK é rígido. Qualquer coisa "fora da especificação" que funciona no PC vira `assert` no Switch. O texto do assert (no log e no `stderr.log`) diz o que corrigir.

---

## 7. O driver fecha o programa em silêncio

**Causa.** O Mesa chama `abort()` e `assert()`, e no libnx isso fecha o processo sem exceção de CPU, então o handler de crash não roda.

**Correção.** `abort()` e `__assert_func` próprios, que escrevem no log antes de sair. `stderr` vai para `logs/stderr.log`. O `std::set_terminate` registra exceções C++ não tratadas.

**Arquivo:** `host-switch/main.cpp`.

---

## 8. O que a newlib e o libnx não têm

| Falta | Onde foi resolvido |
|---|---|
| `pread` | `host-switch/stubs.cpp` (seek + read sob um lock) |
| `pthread_setname_np` | `sdk/.../kernel/threads.cpp` (ignorado) |
| Relançar o executável (troca de xbe) | `sdk/.../kernel/boot.cpp` (desligado) |
| `fork`/`exec` (ImGui abre links) | `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS` no CMake |
| SDL (o código do Android usa duas funções) | `host-switch/compat/SDL3/SDL.h` |
| Toque, giroscópio, teclado e mouse | `host-switch/stubs.cpp` (respostas neutras) |

---

## 9. Compilação

| Problema | Correção |
|---|---|
| GCC do devkitA64 com erro interno (`aarch64_function_arg_alignment`) nos tipos `aligned(1)` | Tipos naturais com `may_alias` no Switch: o ARM do Switch aceita acesso desalinhado. `sdk/.../cpu/x86_ops.h` |
| `char` sem sinal no ARM (com sinal no x86) | `-fsigned-char` em tudo |
| Cabeçalho pré-compilado do glslang com caminho MSYS (`/c/...`) que o GCC não abre | `CMAKE_DISABLE_PRECOMPILE_HEADERS ON` |
| glslang do NVK sem HLSL | O glslang de `third_party` com HLSL, e `--allow-multiple-definition` na linkagem |
| Recompilador falha em caminho com acento | Kit em `C:\XboxSwitchPortKit` (o `recompilar.ps1` confere) |
| CMake do recompilador pega o `llvm-rc` do devkitPro | `CMAKE_RC_COMPILER` apontando para o do Visual Studio (`recompilar.ps1`) |

---

## 10. Log no SD

**Causa.** Um crash mata o processo sem chance de gravar o que ficou no buffer.

**Correção.** No Switch, toda linha do log recebe `fflush` e o modo verboso fica sempre ligado.

**Arquivos:** `sdk/.../core/log.cpp`, `core/log.h`.

**Custo:** com muito log (traces), o jogo engasga. No build final, tire os traces. Se precisar, volte o `fflush` só para avisos e erros (o `#else` do `log.cpp`).
