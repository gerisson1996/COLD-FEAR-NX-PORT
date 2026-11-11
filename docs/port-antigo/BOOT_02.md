# Segundo teste — build 0.1.1

Log e ELF correspondente preservados em `analysis/boot-02/`.

## Avanço confirmado

- Entrada do guest em 0,171 s, sem repetir o crash do dispatch.
- Vulkan/NVK inicializado; swapchain 1280x720.
- Primeiro `VK-FLIP` em 0,969 s; depois nenhum novo quadro até o watchdog.
- Não há exceção de CPU neste log. O watchdog aponta espera da GPU com GET = PUT = `0x004438F4`.

## Espera e ligação ausente

A pilha contém retorno `0x00267E80` (chamada de `sub_00267E70` para `sub_00267B60`) e `0x00261C9B`. `sub_00267B60` lê o ponteiro global do device em `0x00273398`, a sequência enviada em `device+0x2C` e o endereço do semáforo em `device+0x30`. A espera em `0x00267C97` depende do valor escrito nesse semáforo. O registrador ESI do snapshot é `0x002733A0`, compatível com o device observado nesse caminho.

O port não havia registrado essa ligação. O executor usa `s_sem_va` para o método `0x1D70`; seu fallback herdado consulta o endereço de MKSM `0x003F0C08`, inadequado para Cold Fear.

O build 0.1.2 registra `xbox_Nv2aMirrorFence(0x00273398, 0x2C, 0x30)` antes de iniciar o guest. Com `RECOMP_PB_EXEC=1`, a rotina apenas descobre e registra o destino; o executor continua responsável por escrever a conclusão real. Não foi removida a espera nem forçada uma conclusão antecipada.

## Validação e limites

`tools/test_gpu_fence.py` compila as rotinas reais de registro e release extraídas do runtime, com memória guest de teste. Passou: device ainda ausente, semáforo ainda ausente, descoberta do destino, ausência de conclusão antecipada, release GPU e mudança do destino do semáforo. Teste de host, sem execução do renderer ou do console.

É necessária uma nova execução no Switch para confirmar que esta ligação resolve a espera observada. Procurar `NV2A fence mirror: device at 0x00273398` e `[D3D] GPU semaphore registered` no log do novo build.

Pendências separadas: ICALLs para `0x002FF6E0`, `0x002FFF1E`, `0x00301D3A`, `0x00028C80` e instruções de I/O/SSE não implementadas. Não foram substituídas por stubs adicionais neste build. Ainda não há menu ou gameplay validado.
