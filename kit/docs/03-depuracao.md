# 3. Depuração: logs, crashes e traces

No Switch não há depurador. Tudo passa pelo **log**, e o kit escreve nele o máximo possível antes de qualquer saída.

---

## Os arquivos

Em `sdmc:/switch/<nome>/logs/`:

| Arquivo | O que tem |
|---|---|
| `<nome>_NNN.log` | Um por execução, numerado (o maior é o mais novo). Tudo o que o runtime, o jogo e o host registram. |
| `stderr.log` | O que o driver (Mesa/NVK) e a libc imprimem: asserts do Vulkan e erros do glslang. Sobrescrito a cada execução. |

Sempre peça os dois.

---

## Lendo um log

### Começo: o boot

```
Black (default.xbe), ..., build switch-0.1.0, Switch
Game: default.xbe in sdmc:/switch/black/game, sha1 ..., Europe release
app: ImGui / app: controllers / app: booting the image
vulkan: ... (passos do renderizador)
```

Se parar aqui, o problema é de inicialização: memória, threads ou Vulkan.

### Meio: o jogo rodando

Arquivos abertos, sons, vídeos (`.xmv`, `.bik`), saves. A **última coisa** antes do crash quase sempre aponta o subsistema culpado.

### `app: alive`

Linha escrita a cada 5 segundos pela thread principal.

- O log termina com `app: alive` ainda aparecendo, mas o jogo parou: é **trava** (deadlock, espera infinita), não crash.
- O log termina sem `crash:` e sem `alive`: o processo foi morto de fora (o sistema, falta de memória).

---

## Linhas `crash:`

### Exceção de CPU (acesso inválido, instrução ilegal)

Exemplo real (Black, decodificador de vídeo):

```
crash: exception 257 at pc 000000368FF1C384 lr 000000368FF20008 sp 0000002561294A10, address 000000452C669900
crash: main at 0x368f69b060; guest base 0x443a3c2000
crash: guest address F22A7900
crash: x0  0000002561294DC0 000000443A3C2000 00000000F22A7900 FF000000FF000000   (registradores ARM)
crash: on guest thread 14
  eax 00000010 ecx 0065AF00 edx 0066DB00 ebx 00240225 esp D005F8E0 ebp D005F8EC esi 0060FF00 edi F22A7900
  [esp+010] 0024056B        <- endereços de retorno na pilha: quem chamou quem, no xbe
  [esp+124] 0024F0DC
```

- **`guest address`**: o endereço do Xbox que o código tentou acessar. Exemplos:
  - `0000xxxx` ou perto de `FFFFFFxx`: ponteiro nulo (um objeto que não foi criado, `FFFFFFEC` = nulo − 0x14);
  - `F0xxxxxx`: a janela write-combined do Xbox, que não existe no Switch ([armadilhas](04-armadilhas-do-switch.md#3-a-janela-0xf0000000-write-combined-não-existe));
  - `8xxxxxxx`: RAM física pelo alias contíguo (válido).
- **`on guest thread` + linhas `[esp+...]`**: o crash foi no código do jogo. Nesse exemplo, `edi = F22A7900` é o ponteiro inválido, e o `0024056B` cai na seção XMV (vídeo) do xbe. Os endereços da pilha são do xbe e mostram a cadeia de chamadas.
- **`pc`/`lr`**: onde no programa ARM. Para traduzir em nome de função:

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/XboxSwitchPortKit && ./ferramentas/endereco_crash.sh analise/black-0C2BFA28.elf /c/caminho/black_006.log'
```

Use o `.elf` **do mesmo build** que gerou o log: o `empacotar.ps1` guarda cada um em `analise/` com o começo do SHA-256 no nome. A saída dá algo como `sub_0024056B` (código do jogo, endereço no xbe) ou `vkCreateImage`/`kernel::NtReadFile` (host).

### `abort()` e asserts

```
crash: assert image->image_type == VK_IMAGE_TYPE_3D failed in nvk_..., ../src/nouveau/...:123
crash: abort() called from 0x80001234 (main at 0x8000...)
```

Vêm quase sempre do **driver**: o Mesa usa `assert`/`abort` em vez de devolver erro. O texto do assert diz o que o driver não aceitou. Veja também o `stderr.log`.

### Exceção C++ não tratada

```
crash: uncaught std::system_error 88: ...
```

Um `std::thread`, `std::filesystem` ou similar falhou. O número é o `errno`: 88 = ENOSYS (o Switch não tem a chamada), 2 = ENOENT (arquivo não existe), 13 = EACCES.

---

## Traces: ver o que o jogo faz

Quando o log não basta, intercepte as funções do jogo e registre os argumentos. O exemplo completo é `exemplos/black/switch/trace.cpp`: ele rastreia `fopen`, `fread`, `fseek` e `ftell` do jogo, a criação de objetos e o alocador. Foi assim que se achou o `ftell` devolvendo −1 no Black. Ele fica desligado por padrão: descomente `#define BLACK_TRACE` no topo para ligar. Use o mesmo esquema nos seus traces.

Arquivos `.cpp` na pasta `switch/` do jogo entram no build sozinhos.

```cpp
std::atomic<int> g_lines{0};
bool More() { return g_lines.fetch_add(1) < 400; }   // limita: o SD é lento, log demais trava o jogo

X86_EXTERN_FUNC(__imp__sub_00091870);
X86_FUNC(sub_00091870) {                             // fread do jogo
    uint32_t ret = R32(ctx.esp), buf = R32(ctx.esp + 4), size = R32(ctx.esp + 8), n = R32(ctx.esp + 12);
    __imp__sub_00091870(ctx, base);
    if (More()) LOGI("trace: fread(%08X, %u, %u) de %08X -> %u", buf, size, n, ret, ctx.eax);
}
```

**Como achar o endereço de uma função:**

1. pela pilha do guest num crash (os endereços de retorno estão dentro das funções que chamaram);
2. abrindo o xbe no Ghidra: `python sdk/XboxXBE-Recomp/tools/symbols/xbe2pe.py exemplos/<jogo>/config/game.toml jogo.exe` gera um PE com as funções exportadas por nome;
3. procurando strings: um texto do jogo (`"Sound/DSP/BkXboxFx.bin"`) leva à função que o usa.

Para registrar só as primeiras vezes, há o `LOG_FIRST(n, ...)` do runtime.

**Desligue os traces no build final**: eles escrevem no SD e deixam o jogo mais lento. Para conferir, `aarch64-none-elf-nm build/<jogo>/<nome>.elf | grep " T sub_"` lista os ganchos que entraram.

---

## Outras técnicas usadas no Black

- **Passos numerados no log**: quando o crash é "silencioso" (sem `crash:`), ponha `LOGI("vulkan: passo X")` entre os passos suspeitos. A última linha mostra onde parou. Foi assim que se achou o assert do NVK em `CreateFixedImages`.
- **Sondas**: `kernel/boot.cpp` tem `ProbeThread`, que cria uma thread pelo libnx direto e registra memória livre e heap. Separa "o sistema não deixa criar threads" de "o std::thread falhou".
- **Comparar com o PC/Android**: se o jogo tem port para PC com o mesmo SDK, rode lá com o mesmo trace. A diferença mostra o que é do Switch.
- **Guardar cada `.elf`**: sem o `.elf` exato, um endereço de crash não diz nada. O `empacotar.ps1` já guarda.
