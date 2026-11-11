# 2. Como fazer um port novo, passo a passo

Resumo: **copie o exemplo do Black, troque o xbe e a configuração, apague os ganchos específicos do Black, compile e teste no console, lendo o log a cada rodada.** O primeiro boot quase nunca funciona; o trabalho de verdade é o ciclo log → correção → build, descrito em [03-depuracao.md](03-depuracao.md).

---

## 0. Antes de começar: o jogo serve?

O SDK precisa conhecer a **versão do XDK** com que o jogo foi compilado. Hoje o kit tem só a **5849** (`sdk/XboxXBE-Recomp/xdk/5849`), que é a do Black e de muitos jogos de 2004 a 2006.

Para ver a versão de um jogo, rode o passo 2 abaixo: o `xbe_symbols` imprime as versões das bibliotecas do xbe (D3D8, DSOUND, XAPILIB…).

- **5849**: o caminho deste guia.
- **Outra versão**: falta `xdk/<versão>/layout.toml` (posições dos campos das estruturas do D3D) e `signatures.toml` (para achar as funções). É um trabalho grande de engenharia reversa; veja o README do SDK, seção "A new XDK build". Prefira começar por um jogo 5849.

Outra coisa que ajuda: o jogo já ter port para PC ou Android feito com o XboxXBE-Recomp (como o Black-Android). Os ganchos e correções dele servem quase direto.

---

## 1. Crie a pasta do jogo

Copie o exemplo e limpe o que é do Black:

```powershell
$k = 'C:\XboxSwitchPortKit'
robocopy "$k\exemplos\black" "$k\exemplos\meujogo" /E /XD generated assets build /XF port.cmake icon.jpg
Remove-Item "$k\exemplos\meujogo\config\game.toml", "$k\exemplos\meujogo\src\common\europe.patch", `
            "$k\exemplos\meujogo\switch\trace.cpp", "$k\exemplos\meujogo\tools\europe_to_us.py"
New-Item -ItemType Directory "$k\exemplos\meujogo\assets"
```

Ponha o xbe do jogo em `exemplos/meujogo/assets/default.xbe`.

### O que fica e o que sai

| Pasta | Genérico? | O que fazer |
|---|---|---|
| `src/d3d/` | Sim | Direct3D 8 em cima do renderizador Vulkan. Mantenha. Tem a correção do `Lock2DSurface` para o Switch. |
| `src/sound/`, `src/audio/` | Sim | DirectSound do Xbox (mixer, streams, reverb). Mantenha. |
| `src/common/` | Quase | `game_info.cpp` (o descritor do jogo) **muda**; `options.h`/`menu.cpp`/`translations.*` têm opções do Black (FOV, trapaças) que podem ficar inertes no começo. |
| `src/game/` | **Não** | Ganchos do Black por endereço (`sub_001E5FA0`…). **Apague tudo** e escreva os do seu jogo quando precisar. |
| `switch/` (do jogo) | Não | Código só do Switch para este jogo (traces, contornos). Opcional. |

---

## 2. `port.cmake`

Crie `exemplos/meujogo/port.cmake`:

```cmake
set(PORT_NAME meujogo)            # meujogo.nro, na pasta sdmc:/switch/meujogo (sem espaço nem acento)
set(PORT_TITLE "Meu Jogo")        # nome no hbmenu e nas mensagens de erro
set(PORT_AUTHOR "Seu nome")
set(PORT_VERSION "0.1.0")
# opcional: aceitar o xbe de outra região, convertido no console por um patch (veja o Black)
# set(PORT_XBE_PATCH ${GAME_DIR}/src/common/outra_regiao.patch)
# set(PORT_XBE_PATCH_SHA1 "<sha1 do xbe que o patch converte>")
```

Um `icon.jpg` de 256×256 na pasta do jogo vira o ícone do hbmenu.

---

## 3. Gere a configuração e recompile

Sem `config/game.toml`, o `recompilar.ps1` cria um com o `xbe_symbols` e depois recompila:

```bash
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\recompilar.ps1 C:\XboxSwitchPortKit\exemplos\meujogo
```

Leia a saída:

- **versões das bibliotecas**: confirme que é XDK 5849;
- **funções do XDK que faltaram ou ficaram ambíguas**: ficam `= 0` no `game.toml`. Muitas são funções que o jogo não usa, e tudo bem. Se o jogo travar numa delas depois, o endereço tem que ser achado à mão (Ghidra/IDA) e escrito no `game.toml`;
- **`unimplemented ...`**: instruções x86 raras que o recompilador não traduz (o Black tem algumas, de código que nunca roda). Só importa se o crash cair numa delas.

O resultado fica em `generated/game` (`partitions` no `game.toml` diz em quantos arquivos ele é dividido).

---

## 4. `game_info.cpp`

Em `src/common/game_info.cpp`, preencha o `app::kGame`:

```cpp
const app::GameInfo app::kGame = {
    .title = L"Meu Jogo",
    .short_title = L"Meu Jogo",
    .name = L"MeuJogo",
    .log = "meujogo",                  // logs/meujogo_001.log
    .settings = "meujogo.toml",
    .splash = nullptr,
    .missing = L"...",                 // não usado no Switch
    .images = {{"default.xbe", "MeuJogo"}},
    .options = {/* opções salvas no .toml: {"nome", &bool, nullptr} ou {"nome", nullptr, &uint32} */},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,                // depois de ler as configurações, antes do boot
    .loaded = nullptr,                 // xbe na memória, antes de rodar
    .window = nullptr,
    .message = nullptr,
    .ui = app::DrawUi,                 // menu de opções (ImGui)
    .pad = game::Pad,                  // mexer no controle antes do jogo ver
    .rumble = game::Rumble,
};
```

Tire das `options` as que dependiam de `src/game` (trapaças, desbloqueios).

---

## 5. Compile primeiro só o host

Com `src/game` apagado, várias coisas de `src/common` vão reclamar de nomes que sumiram. O alvo `host_check` compila só o host e o runtime, sem os 100 MB de código do jogo, e acha esses erros em 1 ou 2 minutos:

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/XboxSwitchPortKit && TARGET=host_check ./ferramentas/build_switch.sh exemplos/meujogo'
```

Para cada erro: apague a referência, ou crie uma versão vazia (por exemplo `bool LanguageAvailable(...) { return false; }`). Repita até passar.

Um exemplo do que vai aparecer: `src/common/menu.cpp` chama `game::LanguageAvailable`, que no Black fica em `src/game/language.cpp`. Copie de volta só o mínimo necessário, ou troque a chamada.

---

## 6. Build completo, empacotar e testar

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/XboxSwitchPortKit && JOBS=8 ./ferramentas/build_switch.sh exemplos/meujogo'
```
```bash
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\empacotar.ps1 C:\XboxSwitchPortKit\exemplos\meujogo
```

Copie `sd\switch\meujogo\meujogo.nro` para `sdmc:/switch/meujogo/` e os arquivos do disco para `sdmc:/switch/meujogo/game/`. Rode e pegue o log em `sdmc:/switch/meujogo/logs/`.

Depois do primeiro build, só o que mudou é recompilado: mexer no host leva 1 ou 2 minutos.

---

## 7. O ciclo

1. Rodou e parou: abra o log mais novo e veja a última coisa que o jogo fez antes do `crash:`.
2. Ache a causa ([03-depuracao.md](03-depuracao.md)). Antes de investigar, veja se não é uma das [armadilhas já conhecidas](04-armadilhas-do-switch.md).
3. Corrija no lugar certo:
   - comportamento do Switch (arquivos, memória, threads): `host-switch/` ou `sdk/`, e a correção vale para todos os ports;
   - função do jogo: um gancho `X86_FUNC(sub_XXXXXXXX)` em `src/game/` ou em `switch/` do jogo.
4. Build, empacotar, testar, repetir.

O [histórico do Black](05-historico-black.md) mostra esse ciclo inteiro, crash por crash.

---

## Ganchos: substituir uma função do jogo

Toda função recompilada é "fraca": se você define outra com o mesmo nome, a sua vale. A original continua acessível por `__imp__`:

```cpp
#include "xdk/hle.h"
using namespace kernel;

X86_EXTERN_FUNC(__imp__sub_0008AEB0);       // a original
X86_FUNC(sub_0008AEB0) {                    // a sua, chamada no lugar dela
    uint32_t ret = R32(ctx.esp);            // endereço de retorno (quem chamou)
    uint32_t a0  = R32(ctx.esp + 4);        // 1º argumento (stdcall/cdecl: na pilha)
    // leia os argumentos ANTES de chamar a original: ela mexe na pilha
    __imp__sub_0008AEB0(ctx, base);
    LOGI("fopen(%s) de %08X -> %08X", reinterpret_cast<const char*>(base + a0), ret, ctx.eax);  // eax = retorno
}
```

- `R32/W32/R8...`: lêem e escrevem a memória do Xbox (endereços de 32 bits).
- `ctx.eax`, `ctx.ecx`...: os registradores x86.
- Para funções do XDK com nome (do `game.toml`), use `HLE_WRAP`/`GUEST(...)` como em `src/d3d/d3d8.cpp`.
