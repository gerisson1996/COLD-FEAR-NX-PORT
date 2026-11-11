# Build experimental - 15/10/2026

Build atual: 0.1.3 boot-trace. Compilacao ARM64/libnx/Vulkan/NVK e empacotamento concluidos.

- NRO: `switch_sd/switch/coldfear/coldfear.nro`
- Tamanho: 46500037 bytes.
- SHA-256: `4EE76806F88EC9537CCF5BBADF9F3F7A3675599A6420FB99C8178A59E947FBDC`.
- ELF: `build-switch-coldfear/coldfear_recomp.elf`.
- Log do build: `analysis/build-switch.log`.

O teste no Switch da versao 0.1.2 confirmou registro do semaforo e primeiro quadro Vulkan; depois houve um crash na inicializacao grafica ao ler `[edi+0xC]`, com EDI invalido. Ver `BOOT_03.md` e arquivos preservados em `analysis/boot-03/`.

A versao 0.1.3 adiciona diagnostico limitado a 2048 chamadas, em 12 funcoes. O teste de host `tools/test_boot_trace.py` passou (aninhamento, preservacao de resultados, delta de pilha, limite e idempotencia). Ainda exige execucao no console para localizar a origem do crash.

Os testes anteriores do dispatch e do semaforo estao documentados em BOOT_01.md e BOOT_02.md. Os 110 arquivos do disco foram verificados contra a ISO. Permanecem funcoes nao resolvidas e instrucoes nao implementadas; menu, controles, audio, saves e gameplay nao validados. Nao classificar como port jogavel.

Para o proximo teste, substituir somente `switch/coldfear/coldfear.nro` no SD, iniciar em Title Takeover e recuperar `switch/coldfear/coldfear_log.txt`.
