# Histórico de versões

As versões 0.1.x são o port novo (pasta `kit/`, Direct3D 8 reescrito em Vulkan). As 0.0.x são o port antigo (raiz, emulação da NV2A).

## 0.1.37 — 2026-11-11

- Capa do jogo no menu do Switch.

## 0.1.36 — 2026-11-11

- Log: a cada 300 quadros, FPS, quadro mais longo e tempo médio por parte (jogo, GPU, ocioso).
- Removidos logs de diagnóstico que rodavam a cada desenho.

## 0.1.35 — 2026-11-11

- Sombras das lâmpadas corrigidas e ligadas por padrão (a borda do mapa de sombra era lida como "em sombra").
- OLED SCREEN BOOST (AUTO / OFF / ON): clareia os tons escuros na tela OLED.
- Giroscópio: eixo vertical correto por padrão.

## 0.1.34 — 2026-11-11

- Mira por giroscópio enquanto ZL está segurado, com sensibilidade, velocidade vertical, eixo, estabilidade, correção de drift e inverter X/Y.

## 0.1.33 — 2026-11-11

- Menus sem faixas pretas; as faixas ficam só no gameplay.

## 0.1.0 a 0.1.32

- Port refeito sobre o XboxSwitchPortKit: geometria, HUD, lanterna, brilho, som do menu e do jogo, contador de FPS, menu do port (L3 + R3).

## 0.0.3 — 2026-10-22

- Port antigo: compilação com -O3, desenhos instanciados, menu de mods (− + R3).

## 0.0.2 — 2026-10-21

- Port antigo: corrigidos os quadrados escuros do passe de sombra das lâmpadas; vídeo em tela cheia.

## 0.0.1 — 2026-10-21

- Primeira versão pública do port antigo.
