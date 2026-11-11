# Cold Fear no XboxSwitchPortKit — estado para continuar

Kit: `C:\XboxSwitchPortKit` (HLE D3D8, backend Vulkan/NVK no Switch). Este jogo: `exemplos\coldfear`.
Port antigo (recomp com emulação NV2A, referência de comportamento): a raiz deste repositório.

## Compilar e empacotar
- Versão: `exemplos/coldfear/port.cmake`, `PORT_VERSION` (aumentar a cada build; o log mostra `build switch-X`). Atual: 0.1.37.
- Build: `/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/XboxSwitchPortKit && JOBS=8 ./ferramentas/build_switch.sh exemplos/coldfear > analise/coldfear_build.log 2>&1'`
- Pacote: `powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\empacotar.ps1 C:\XboxSwitchPortKit\exemplos\coldfear`
  → `C:\XboxSwitchPortKit\sd\switch\coldfear\coldfear.nro` (no cartão: `sdmc:/switch/coldfear/`, logs em `logs/`).
- Crash: `./ferramentas/endereco_crash.sh analise/coldfear-XXXX.elf <log>`.
- Código recompilado do jogo (disassembly comentado): `exemplos/coldfear/generated/game/x86_recomp.*.cpp`.

## Build de PC (para achar bugs sem o Switch)
Mesmo código do jogo e mesmo renderer Vulkan (`host-switch/vulkan.cpp`), em Windows (`host-pc/`). Log completo como no Switch.
- Build: `cd /c/XboxSwitchPortKit && cmd //c "ferramentas\\build_pc.bat exemplos\\coldfear" > analise/coldfear_pc_build.log 2>&1`
  → `build/coldfear-pc/coldfear_pc.exe`.
- Rodar com roteiro: `powershell -ExecutionPolicy Bypass -File ferramentas/pc_roteiro.ps1 -Exe "C:\XboxSwitchPortKit\build\coldfear-pc\coldfear_pc.exe" -Game "<pasta game>" -Script <roteiro>`.
  Linhas do roteiro: `<seg> cmd BOTOES [quadros]` (ex. `START`, `DOWN+A 10`, `LS`/`LSL`), `<seg> shot`, `<seg> trace` (um quadro de draws no log), `<seg> end`.
  `analise/pc_r3.txt` chega ao gameplay (Rear Deck) em ~70 s; START em 12/21/26 s chega ao menu principal.
- Shots: `build/coldfear-pc/shots/NNNN.bmp` → `python ferramentas/bmp2png.py <pasta>`. Logs: `build/coldfear-pc/logs/`.
- Teclado: setas = D-pad, WASD = analógico, Enter = A, Tab = Start, F10 = Minus + R3.

## Arquivos principais
- `src/d3d/d3d8.cpp` — HLE do D3D8 (viewport no device +0xEE0, espelho D3D__RenderState, gamma/brilho, overlay XMV, diagnóstico `NoteBand`).
- `host-switch/vulkan.cpp` — backend (afeta todos os jogos do kit): alvos de profundidade por tamanho, depth bias nos passes LEQUAL/EQUAL com blend (corrigiu a lanterna).
- `src/game/shadow.cpp` — shader de sombra (bias + 4 amostras).
- `src/game/menu_bands.cpp` — zera o letterbox das páginas do menu (`[menu+0x50300]`, sub_0022E8C0); o do gameplay (sub_0018E100, câmera 0x368CE0) continua.
- `src/game/sound.cpp` — hooks de DSOUND: GetCaps (B003E) devolve 192/64 vozes (corrigiu o som), B0061 ignorado.
- `src/game/coldfear.cpp` — Cold Fear não recebe a flag 480p (`app::g_video_480p = false`, igual ao port antigo).
- `src/common/menu.cpp` — painel do kit (Minus + R3), contador de FPS via ImGui, esquerda/direita mudam o valor da linha.
- `src/common/game_info.cpp` — combo Minus + R3, opções salvas.

## Resolvido
Espinhos de vértices, cena escura, HUD, lanterna piscando, brilho, painel de opções, som do menu/jogo (0.1.24), FPS na tela (0.1.27),
faixas pretas no menu (0.1.33): cada página do menu tem um valor de letterbox nos dados (0.08 no menu principal); a faixa
vai da borda da área segura de TV (36 px) mais 8% da altura segura = 68 px. Na CRT o overscan escondia quase tudo.

## Em aberto
1. Sombra/iluminação: os passes REVERSE_SUBTRACT das lâmpadas, desenhados na 0.1.32, deixam cunhas escuras de bordas retas
   sobre a geometria distante (PC, Rear Deck). Na 0.1.33 voltaram a ser pulados por padrão (`skip_revsub` = true, igual ao
   port antigo); Graphics > Quality > SKIP LAMP SUBTRACT PASS desligado os desenha. Falta saber o que o usuário viu como bug
   e se o volume de sombra precisa de stencil/clip para ficar certo.
2. Opções do kit dentro do menu do próprio jogo (hoje só Minus + R3).
3. Remover os logs de diagnóstico (viewport, band, dsound buffer/play, trace do d3d8.cpp) quando terminar.

Git: só a parte Switch vai para o repositório; Android fica fora do commit.

## Giroscopio (0.1.34)

- `src/game/gyro.cpp`: envolve `sub_0017EBE0` (update da mira; controller do jogador em `[esp+4]`). Antes da chamada soma
  o giro (radianos) em `controller+0xAD0` (giro do corpo pedido no tick, positivo = esquerda, consumido pela chamada) e
  `controller+0xAD4` (pitch da mira, persistente, positivo = cima no Switch real, limite 0.407; gyro.cpp usa `pitch - dy`). Resposta 1:1, sem a rampa do
  analogico (`sub_0017FD60` aplica zona morta + 0,5 s de aceleracao em `+0x17A0/+0x17A4`).
- O giro vem de `input::TakeMouse` (host-switch/gyro.cpp), so com ZL segurado: `PORT_GYRO_MODE 3` no port.cmake e o
  padrao antes do jogador escolher (novo no host; 0 nos outros jogos). Menu: GYROSCOPE (OFF / WHILE AIMING (ZL)),
  sensibilidade ate 10, vertical, eixo, steadiness, drift, inverter X/Y.
- Nao testado no Switch real (o PC nao tem giroscopio). Se o vertical vier invertido: GYRO INVERT Y, ou trocar o sinal
  de `dy` em gyro.cpp.
- host-pc: o roteiro aceita RS, RSD, RSL, RSR (analogico direito).

## Sombras das lampadas e OLED (0.1.35)

- Causa das cunhas escuras: o mapa de sombra da lampada (tex0, LIN_D16, endereco BORDER, borda 0000FFFF = plano
  distante) tinha a borda lida como cor sem alfa -> TRANSPARENT_BLACK -> profundidade 0 -> tudo fora do mapa em sombra.
  `host-switch/vulkan.cpp` (StageSampler, ramo shadow) agora usa OPAQUE_WHITE quando a borda >= 0x8000. Vale para todos
  os jogos do kit. `skip_revsub` passou a false (sombras desenhadas); menu: SKIP LAMP SHADOWS.
- Conferido so no PC (`COLDFEAR_REVSUB=1` forca desenhar; capturas em analise/pc_border). Falta o Switch real.
- OLED: `src/game/oled.cpp` (`game::OledBoost`), opcao `oled_screen` (0 AUTO = modelo Aula fora do dock, 1 OFF, 2 ON),
  menu Graphics > OLED SCREEN BOOST. Multiplica o expoente da rampa de brilho por 0.75 (SendGamma no d3d8.cpp).
- Trace (PC): agora lista as texturas de cada estagio (formato, endereco, borda, shadowfunc).

## Desempenho (0.1.36)

- Log do Switch na 0.1.35 (render 120%, cap 30): 30 fps cravados em gameplay; quedas para 23-26 fps (media de 300
  quadros) coincidem com carregamentos e shaders novos. A escala dinamica caiu de 1.8 para 1.5 e nao voltou.
- 0.1.36: a linha `d3d: Swap N` (a cada 300 quadros) agora traz fps, quadro mais longo, quadros acima de 40 ms e a
  media por parte: game (codigo do jogo), present, gpu wait (alto = GPU e o limite), pace (ocioso). Usar o proximo log
  do Switch para decidir o que otimizar.
- Removidos: NoteBand e o log de viewport (d3d8.cpp). O aviso `wait: thread blocked` (kernel/objects.cpp) agora sai
  em 2 s e a cada dobro do tempo, nao a cada 2 s.

## Capa e medicao (0.1.37)

- Capa: `exemplos/coldfear/icon.jpg` (256x256, copiada de `cold fear 1/artwork/coldfear-icon-256.jpg`), embutida no NRO.
- Log do Switch 0.1.36 (portatil, render 100% = escala 1.5, aniso 8): `game` 15-29 ms por quadro (CPU do jogo e o
  maior custo); nas areas pesadas (66-85 s do log) `gpu wait` 20-22 ms e pace 0 -> 23-24 fps, e a escala dinamica so
  caiu (1.5 -> 1.35) uns 20 s depois. Ideias: escala dinamica reagir mais rapido / piso menor; depois olhar a CPU.
