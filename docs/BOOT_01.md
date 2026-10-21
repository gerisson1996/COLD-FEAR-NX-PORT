# Primeiro teste de boot e correção 0.1.1

O log enviado registra inicialização do runtime, áudio e resolução dos 118 imports de kernel. O crash ocorre em 0,210 s, antes de `Starting Cold Fear guest` e antes de qualquer ICALL.

Com o ELF preservado em `analysis/boot-01/coldfear_recomp.elf`:

- `switch_boot` no ELF: `0x9BE20`; endereço em execução: `0x141B24CE20`.
- Base calculada do módulo: `0x141B1B1000`.
- PC do crash: `0x141CC1B1D8`; offset ELF: `0x1A6A1D8`.
- `addr2line`: `recomp_dispatch_init`, antiga linha 13320 de `gen/recomp_dispatch.c`.

A remoção da falsa função `sub_00211062` deixou 13.258 entradas, mas o contador literal permaneceu em 13.259. A inicialização leu uma entrada além do array e a usou como índice para escrita. O campo “guest VA” do tratador é uma conversão do endereço da falha; não demonstra que o jogo chegou a executar um acesso a esse endereço.

Correção: derivar `g_recomp_table_size` com `sizeof(array) / sizeof(array[0])` no código gerado, no pós-processamento e no gerador do toolkit local. Dados da ISO não foram alterados.

Regressão: `tools/test_dispatch.py` compila o dispatch real com funções guest inertes. Antes da correção falhou na contagem, e o compilador também apontou acesso fora do array. Depois passou: 13.258 entradas, ordem, busca binária, tabela direta, endereços ausentes, limites e inicialização repetida. Esse teste não executa o jogo.

O novo log identifica `0.1.1 dispatch-size-fix`. Reteste no Switch pendente; espelhamento de memória e demais avisos do runtime não foram alterados sem evidência de que causaram este crash.
