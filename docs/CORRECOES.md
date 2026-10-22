# Correções do port Cold Fear (Xbox → Switch)

Registro de tudo o que foi corrigido para o Cold Fear rodar no Switch, com a causa de cada problema e onde está a correção. Serve de guia para os próximos ports com o mesmo toolkit (xboxrecomp + renderizador Vulkan/NVK), porque muitos desses erros são do toolkit ou do XDK do Xbox, e não deste jogo.

Convenções:

- `gen/` é o código C gerado a partir do XBE. Ele não é editado à mão: cada correção nele é um script `tools/fix_*.py`, idempotente e registrado em `tools/regen.ps1`. Assim, uma nova geração reaplica tudo.
- O toolkit fica em `xboxrecomp/`; o código próprio do jogo, em `src/`.
- "Guest" é o código do jogo (x86 traduzido); "host" é o código nativo do port.

---

## 1. Tradução de instruções (lifter)

| Problema | Sintoma | Correção |
| --- | --- | --- |
| `jb`/`jbe` depois de `comiss`/`ucomiss` não pulavam com NaN (no x86 uma comparação com NaN liga CF) | partículas com UV NaN passavam em todos os testes e viravam "explosões" de vértices | `tools/fix_comiss_nan.py` |
| `pushfd`/`popfd`/`cpuid` eram no-op | a pilha ficava desalinhada em um slot; a checagem de CPU do FMOD devolvia lixo e o `FSOUND_Init` falhava (sem som) | `tools/fix_flags_cpuid.py` liga essas instruções às funções do runtime |
| `movsx r32, bp` virava extensão sem sinal | o decodificador IMA ADPCM do FMOD cortava toda amostra negativa em +32767: música e efeitos viravam chiado | `tools/fix_movsx_bp.py` (o lifter também foi corrigido) |
| `cmpltss` não traduzido | a normalização segura do jogo devolvia vetor zero | `tools/fix_cmpltss.py` |
| O memmove do CRT (`sub_0003BB00`) usa tabelas de salto dentro do código, e o desmontador cortou a função no meio | cópias perdiam de 1 a 3 bytes finais | `tools/fix_memmove.py` troca por um memmove nativo |
| Falso positivo de prólogo em `0x00211062`, ao lado de uma tabela de salto | função quebrada | `tools/fix_generated.py` |

Verificação final: o código gerado tem 652 pontos `RECOMP_UNIMPL`, quase todos dados lidos como código. O runtime registra no log cada um que é executado (`[UNIMPL] ... REACHED`). Durante o jogo só rodaram `cli`/`sti`, `in`/`out` em portas de vídeo, `wbinvd` e `stmxcsr`/`ldmxcsr`, todos inofensivos como no-op. **Para outro jogo:** jogue um pouco e faça `grep REACHED` no log antes de procurar bugs no lifter.

## 2. Kernel, threads e hardware simulado

| Problema | Sintoma | Correção |
| --- | --- | --- |
| Overrides manuais não empilhavam o endereço de retorno | crash no DeadBody | `src/recomp_manual.c` |
| O DirectSound espera o firmware do DSP limpar o mailbox, e o DSP do runtime é um stub | travava no boot, dentro do DirectSound | `tools/fix_dsp_ack.py` |
| O D3D desliga o overlay de vídeo e espera o hardware zerar `NV_PVIDEO_BUFFER` | travava depois do vídeo da Ubisoft | `tools/fix_overlay_stop.py` |
| O handler de vblank do D3D não avisava o executor dos flips concluídos | o executor esperava flips para sempre | `tools/fix_flip_hook.py` |
| O APU lia páginas do heap do FMOD como memória contígua | áudio corrompido | `xbox_PhysIsHeapPage` em `xboxrecomp/src/kernel/xbox_devbus.c` |
| Streams de música e ambiente do FMOD saíam como estática | música com chiado | `src/fmod_hle.c` toca os `.fsb` no host; `tools/fix_fmod_hle.py` desvia as funções |
| `fence_mirrors_tick` dava o fence como concluído sempre que o DMA ficava parado | o pool de vertex buffers do RenderWare reusava buffers ainda em uso: "espinhos" e partículas explodindo no navio | só força o fence depois de 100 ms parado (`RECOMP_FENCE_MIRROR_MS`), em `xbox_memory_layout.c` |
| `RtlEnterCriticalSection` liberava o GIL até quando o lock estava livre | ~15% da thread principal só em trocas de GIL | caminho rápido `kernel_cs_enter_fast`; `RtlLeaveCriticalSection` mantém o GIL (`kernel_bridge.c`) |
| Espera de parada de voz do DirectSound com timeout de 0,5 s, consultada ~200 mil vezes por segundo | CPU gasta à toa | `tools/fix_dswait.py` + `coldfear_dswait_timeout` (1 ms) |
| O log do próprio jogo ia para lugar nenhum | erros escondidos antes de crashes | `tools/fix_game_log.py` (linhas `[GAME:n]`) |

## 3. Renderizador Vulkan (NV2A → Vulkan)

Quase tudo está em `xboxrecomp/src/nv2a_vk/nv2a_vk.c` e nos tradutores de shader `xboxrecomp/src/nv2a_gl/gl_vsh.c` e `gl_psh.c`.

### Geometria e estado

- **Z-cull / near-far:** `SET_ZMIN_MAX_CONTROL` (0x1D78) pede para cortar o que está entre o olho e o plano near, e não apenas prender a profundidade. Foi feito com `gl_ClipDistance` (`u_zcull`).
- **Stencil:** no NV2A o valor de op `0` é `ZERO`, e `KEEP` é `0x1E00`. O backend lia 0 como KEEP. Agora os registradores são iniciados com os valores de reset do hardware.
- **Polygon offset:** `0x338`, `0x384` e `0x388` não eram implementados, o que causava acne de sombra. Viraram depth bias dinâmico, com as unidades multiplicadas por 256 em buffers de 16 bits.
- **Névoa:** o shader de vértice passava `oFog.x` direto como fator. Agora existe a unidade de névoa do NV2A (`nv2a_fog`, fórmulas do xemu; registradores 0x29C, 0x9C0, 0x9C4 e 0x2A4). Sem ela, os objetos ficavam azulados.

### Pipeline fixo (fixed function), a maior causa de "jogo escuro"

- **Iluminação por vértice** (`SET_LIGHTING_ENABLE`), usada em ~1/3 dos desenhos, não era calculada. Agora é: `nv2a_light` em `gl_vsh.c`, com ambiente, emissão, até 8 luzes infinitas, locais e spot, atenuação e alcance, seguindo o xemu. O D3D do XDK pré-multiplica o material nas cores das luzes. Opção: `NV2A_FFP_LIGHT=0`.
- **Normais invertidas:** o teto do navio tem normais apontando para fora da sala e ficava preto. O shader ilumina o lado que a câmera vê (`NV2A_FFP_FLIP=0` desliga).
- **Geração de coordenadas de textura e matrizes de textura** (`0x3C0`, `0x420`, `0x6C0`, `0x840`): `nv2a_texgen`.

### Mapas de sombra (shadow buffers)

- **Passes só de profundidade** (`color_offset` 0) eram descartados. Agora recebem uma superfície de cor descartável e um depth buffer amostrável.
- **Mesmo endereço, vários tamanhos:** o jogo desenha o mapa de sombra de cada lâmpada no mesmo endereço em 1024, 512 e 256. A busca agora é por tamanho e pela última escrita (`depth_find`).
- **Ordem da comparação:** é `stored FUNC r`, com 1 = iluminado. A prova foi uma passada com `REV_SUBTRACT` que usa um mux no resultado. A ordem invertida escurecia toda a área de cada lâmpada (os "quadrados escuros").
- **PCF 2×2 e bias 128:** o NV2A filtra a comparação com filtro LINEAR. Uma amostra só criava blocos (`NV2A_SHADOW_PCF`, `NV2A_SHADOW_BIAS`).
- **Fora da área do mapa = iluminado.** O jogo nunca limpa o mapa, então o port limpa no primeiro desenho de sombra do quadro (`NV2A_SHADOW_CLEAR=0` desliga).
- **Texturas 3D** (volume AY8, 256×256×32) agora são amostradas como `sampler3D`, com o swizzle 3D.

### Imagem final

- **Rampa de gama do jogo:** o D3D grava no DAC; agora vai para uma passada LUT antes do present (`tools/fix_gamma_ramp.py`, `NV2A_GAMMA=0`).
- **Menu de mods** (− + R3, `nv2a_menu.h` + `overlays` em `nv2a_vk.c`, itens do port em `switch_nx.c`): brilho, resolução, contador de FPS, sombras das lâmpadas em 5 níveis (escala `u_out_scale` só nas passadas `REV_SUBTRACT`), névoa e overclock ao vivo. Desligar o overclock fecha a sessão do clkrst; mantida aberta no clock de fábrica, o sistema derrubava a CPU a cada segundo depois que o overclock era religado. As escolhas ficam em `mods.txt`, lido antes do `coldfear_env.txt`.
- **Brilho do jogador:** **−** + direcional cima/baixo, níveis 0–10, salvo em `brightness.txt`, com painel "BRILHO" na tela.
- **Resolução interna:** `RECOMP_GL_SCALE=1.5` dá 960×720 sem custo visível de FPS, porque o gargalo é a CPU. O overlay de vídeo (FMV) passa por uma imagem intermediária e é esticado, senão ficava num canto.

## 4. Desempenho

Medidas no Switch sem overclock: de 13–21 FPS no começo para ~22–30 FPS (v0.0.3).

| Achado | Ganho / correção |
| --- | --- |
| GIL trocado em toda critical section | caminho rápido (seção 2) |
| Espera de voz do DirectSound | timeout de 1 ms |
| Cache de shaders e pipelines gravado na pasta de outro port (`sdmc:/switch/nfsu2x/`) | `RECOMP_CACHE_DIR`, ligado em `switch_nx.c`; o cache e o brilho passam a persistir |
| Tradução de endereço com 2 comparações em todo acesso à memória | caminho rápido "RAM base primeiro" em `recomp_fold_tiled` |
| Marcação do profiler em todas as 13.310 funções | removida (`tools/strip_diag_hooks.py`) |
| Decodificação de textura com laço bit a bit por pixel (12,6% da thread gráfica) | tabelas de swizzle por linha e coluna |
| Superfície de desenho guardada só por endereço: os mapas de sombra eram recriados várias vezes por quadro (~12% da thread gráfica em alocação no driver) | cache por endereço **e** largura |
| `vkCmdBindVertexBuffers` com 16 bindings: o Mesa faz `malloc` acima de 8 (7% da thread gráfica) | duas chamadas de 8 |
| Acessos à memória sem `volatile` | **sem ganho**; mantido o `volatile` (seguro) |
| Interrupção `NOP(9)` do D3D (~270/s): a thread gráfica parava até o jogo rodar o handler, que só copia um registrador da GPU para outro (`nv[nv[0x401A88]] = nv[0x40186C]`) | feita direto no executor (`nv2a_pb_exec.c`, `NV2A_NOP9_HLE=0` volta ao trap); a espera da thread do jogo caiu de ~15% para ~2–13% |
| Desenhos repetidos um por um | instancing (`RECOMP_VK_INSTANCE`, agora padrão): thread gráfica de ~70% para ~55% |
| Código do jogo compilado com `-O2` | `-O3` (`switch/build.sh`, `GEN_OPT`): ~+5 FPS de média no teste; NRO maior (95 MB) |
| Overclock (CPU 1785 / memória 1600) | +~5 FPS, mas fica opcional (`COLDFEAR_CPU_MHZ`, `COLDFEAR_MEM_MHZ`) |

**Gargalo atual (v0.0.3):** a thread do jogo, em ~70–80% de um núcleo. O maior custo restante é o código traduzido salvar e recarregar os 7 registradores x86 em toda chamada de função, mais as leituras de TLS e a troca do GIL. O próximo passo é mudar o lifter para salvar só os registradores usados.

## 5. Ferramentas de diagnóstico

- **Log:** `sdmc:/switch/coldfear/coldfear_log.txt`, confirmado no SD a cada 2 s. Sem esse fsync, fechar o jogo pelo HOME perdia tudo depois dos primeiros segundos. Confira sempre a linha `BUILD` antes de analisar.
- **Profiler nativo:** `RECOMP_NX_PROFILE=1` e `RECOMP_NX_PROFILE_AFTER=60` gravam `prof.bin`. Para ler: `python tools/prof_report.py prof.bin syms.txt`, com os símbolos do ELF exato: `aarch64-none-elf-nm -n --defined-only`. Se não souber de qual build veio o `prof.bin`, confira se os `lr` caem logo depois de um `bl`: só o ELF certo bate 100%.
- **Linhas úteis do log:** `[perf] ... fps`, `% of a core per thread`, `main thread waits by caller`, `[VKSTAT]`, `[hitch]`, `[UNIMPL] ... REACHED`.
- **Interruptores de teste do renderizador**, para isolar efeitos: `NV2A_SKIP_REVSUB`, `NV2A_SKIP_STENCIL_COLOR`, `NV2A_SKIP_MULT`, `NV2A_SKIP_ALPHA=ffp|prog|z`, `NV2A_SHADOW_FORCE`, `NV2A_VOL_WHITE`, `NV2A_FOG=0`, `NV2A_STENCIL=0`, `NV2A_TEXGEN=0`.

## 6. Lições para os próximos ports

1. **Jogo escuro raramente é um bug de gama.** Primeiro veja quantos desenhos usam o pipeline fixo com iluminação (`[VKSTAT] by transform`). Se o backend não calcula a iluminação fixa, metade do cenário fica preta.
2. **Um efeito estranho? Desligue uma classe de desenhos por vez** (subtração, multiplicação, stencil, transparência) e veja o que some. Isso achou os quadrados em duas rodadas.
3. **Decodifique o combiner** da passada suspeita (`[REVSUB]` imprime ICW/OCW): a matemática do próprio jogo diz qual ordem ou sinal é o correto.
4. **Caches por endereço não bastam**: o mesmo endereço pode ter vários tamanhos ao longo do quadro (sombras, alvos de pós-processamento).
5. **Meça antes de otimizar:** o profiler mostrou que o jogo passava metade do tempo esperando, e não calculando. Tirar o `volatile` (o palpite óbvio) não ganhou nada.
6. **Nunca grave em pasta fixa de outro port:** cache, saves e configurações devem usar a pasta do jogo atual.
7. **Hardware que o jogo espera e não existe** (DSP, overlay, vblank) costuma travar o jogo num laço de espera. Procure o laço no log e responda como o hardware real faria.

## Pendências

- Legenda some por alguns segundos durante uma conversa no main deck (a voz toca normalmente). A suspeita é um efeito de tela cheia cobrindo o texto.
- Brilho especular da iluminação fixa ainda não calculado.
- Lifter: salvar só os registradores usados em cada chamada (desempenho).
- Remover os logs de diagnóstico ainda compilados.
