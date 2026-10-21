# Terceiro teste: build 0.1.2

Log, ELF e manifesto correspondentes preservados em `analysis/boot-03/`.

O semaforo foi registrado em `0x80441000` (valor inicial 3). Houve primeiro
VK-FLIP em 0,860 s e configuracao do modo de video em 1,658 s. Em 1,663 s,
ocorreu uma excecao por acesso ao endereco guest `0x6E65657E`.

O PC `0x4d26eb9de4`, relativo ao switch_boot `0x4d25a49e30` e ao ELF arquivado,
aponta para `sub_002415A0`, instrucao guest `0x0024178F`: `fild [edi+0xC]`.
EDI era `0x6E656572`; ESI era `0x570A3A20`. Os bytes parecem fragmentos de texto,
mas o log nao demonstra onde foram carregados nem se houve sobrescrita da pilha.
Nao se deve substituir o ponteiro por um valor presumido.

## Build 0.1.3 boot-trace

Build de diagnostico, sem alegar correcao da causa. Instrumenta 152 chamadas
diretas em 12 funcoes da inicializacao grafica, Clear/Present e formatacao de
mensagens. Cada par `[CF-CALL n] >` / `<` mostra alvo, registradores, ESP, delta
da pilha e valor do slot de retorno. O identificador permite parear chamadas
aninhadas. A leitura do slot e limitada a faixa de pilha principal observada.
O limite global e 2048 chamadas; depois disso as chamadas continuam sem log.

`tools/instrument_boot.py` aplica a instrumentacao por endereco de funcao e
e executado por `tools/regen.ps1`. Nao modifica instrucoes do XBE, retornos,
registradores ou resultados das funcoes. `tools/test_boot_trace.py` verifica
chamadas aninhadas, preservacao dos resultados reais (inclusive ponteiro ruim),
delta de pilha, limite e idempotencia da instrumentacao.

As quatro funcoes nao resolvidas do log anterior continuam pendentes. Este
teste deve localizar a primeira chamada que retorna com EDI/ESI ou ESP incorreto,
reduzindo a busca antes de alterar convencoes de chamada ou traducao.

No Switch, substituir somente `switch/coldfear/coldfear.nro`, executar por
Title Takeover e recuperar o novo `switch/coldfear/coldfear_log.txt`.
Menu, audio, controles, saves e gameplay continuam sem validacao.
