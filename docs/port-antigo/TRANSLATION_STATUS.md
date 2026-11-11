# Pendencias da primeira traducao

## Correcao aplicada durante o build

`sub_00211062` era um falso positivo de `gap_prologue`: `RET` em `0x211061`, alinhamento `8B FF` em `0x211062`, quatro ponteiros de 32 bits em `0x211064..0x211073` e preenchimento `CC` ate `0x21107F`. Os destinos sao `0x210F50`, `0x210F28`, `0x210F3A` e `0x210F6B`, todos pertencentes a `sub_00210EE0`; o switch gerado mantem esses quatro caminhos.

`tools/fix_generated.py` remove apenas a funcao falsa e sua entrada no dispatch, condicionado ao SHA-256 exato do XBE. Os dados do jogo nao mudam. A segunda execucao do script nao faz alteracoes. Isso reduz as funcoes reais incluidas de 13.259 para 13.258; os numeros abaixo preservam o inventario original.

Este inventario vem do tradutor, nao de uma execucao do jogo. Enderecos devem ser confirmados no XBE antes de criar overrides.

| Instrucao | Ocorrencias |
| --- | ---: |
| outsd | 225 |
| insb | 112 |
| outsb | 64 |
| arpl | 37 |
| aas | 35 |
| cmpltss | 34 |
| insd | 21 |
| sldt | 20 |
| str | 18 |
| in | 15 |
| bound | 14 |
| cmpsb | 14 |
| les | 9 |
| out | 9 |
| lcall | 8 |
| pushfd | 7 |
| cli | 5 |
| sti | 4 |
| hlt | 4 |
| popfd | 4 |
| das | 3 |
| int | 3 |
| lds | 3 |
| into | 3 |
| daa | 3 |
| ljmp | 3 |
| int1 | 3 |
| aaa | 2 |
| ltr | 2 |
| scasb | 2 |
| cpuid | 2 |
| iretd | 2 |
| scasd | 2 |
| verw | 2 |
| verr | 1 |
| ldmxcsr | 1 |
| wbinvd | 1 |
| sgdt | 1 |
| stmxcsr | 1 |

Total: 699 ocorrencias. Funcoes traduzidas: 13259. Stubs nao resolvidos: 102.

Prioridade: reproduzir o primeiro bloqueio no console, relacionar o log ao guest VA e verificar a funcao no disassembly. Nao converter automaticamente todas estas ocorrencias em no-ops nem reutilizar offsets de outros jogos.
