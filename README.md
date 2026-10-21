# Cold Fear (Xbox) para Nintendo Switch

Port experimental por recompilacao estatica: x86 -> C -> ARM64, com xboxrecomp e Vulkan/NVK. O build 0.1.2 registrou o semaforo da GPU e chegou a configuracao de video, mas sofreu um crash com ponteiro invalido. Menu e gameplay ainda nao foram validados.

## Estado

- Build atual: **0.1.3 boot-trace**, de diagnostico. Registra chamadas da inicializacao grafica para localizar onde os registradores ou a pilha se corrompem. Nao representa uma correcao confirmada do crash. Detalhes em `docs/BOOT_03.md`.
- Build Vulkan/NVK e teste do diagnostico concluidos. NRO: `switch_sd/switch/coldfear/coldfear.nro` (46.500.037 bytes). Basta substituir esse arquivo no SD e recuperar o novo log.
- Pacote de teste pronto em `switch_sd/switch/coldfear/`, com NRO, configuração, manifesto de hashes e os 110 arquivos do disco.
- XBE identificado como Cold Fear, Title ID `0x5553004B`, entrada `0x0001BA4E`.
- Os 110 arquivos em `game/` foram comparados com a ISO fornecida, por tamanho e SHA-256. Relatório: `analysis/disc-verification.json`.
- Trabalho inicial recuperado de `Cold Fear/ColdFearPort`: 13.259 funções traduzidas, 102 stubs não resolvidos e 699 ocorrências de instruções não implementadas. Algumas podem corresponder a dados interpretados como código; execução e análise são necessárias para distinguir os casos.
- Runtime e SDK NVK copiados do MKSM. Build selecionado: Vulkan. OpenGL tem um erro de compilação herdado e não foi corrigido.
- Sem patches de endereços de MKSM/Burnout aplicados ao Cold Fear.
- Corrigido um falso positivo do detector em `0x00211062`, adjacente a uma tabela de saltos; correção reproduzível por `tools/fix_generated.py`. Detalhes em `docs/TRANSLATION_STATUS.md`.

## Compilar

No MSYS2/devkitPro, dentro desta pasta:

```bash
bash switch/build.sh
```

Requer devkitA64, libnx, SDL2 e dependências do runtime instaladas no devkitPro. O SDK NVK está em `nvk_sdk/`. O script usa o toolkit local e gera `build-switch-coldfear/coldfear_recomp.nro`. Log: `analysis/build-switch.log`.

## Preparar cartão SD

Depois de compilar, no PowerShell:

```powershell
.\tools\package.ps1
```

Copie o conteúdo de `switch_sd/switch/` para `sdmc:/switch/`:

```text
switch/coldfear/
  coldfear.nro
  coldfear_env.txt
  game/
    default.xbe
    bigfile.bin
    data/
```

Inicie por Title Takeover (segure R ao abrir um jogo instalado). Este é um build de diagnóstico: travamentos ou tela preta precisam ser investigados. O log esperado é `sdmc:/switch/coldfear/coldfear_log.txt`. Guarde também o relato do último quadro exibido e se os controles responderam. Não há promessa de FPS ou de jogabilidade nesta etapa.

## Regenerar C

```powershell
.\tools\regen.ps1 -Python 'C:\caminho\python.exe'
```

O Python escolhido precisa de `capstone`. A análise fica em `analysis/`; a tradução fica em `gen/`. Regenerar não resolve automaticamente os stubs ou as instruções pendentes.

Para verificar novamente os arquivos da ISO:

```powershell
python .\tools\verify_disc.py 'C:\caminho\Cold Fear.iso'
```

## Referências e próximos marcos

Veja `docs/PROVENANCE.md` para a origem dos componentes. MKSM e Burnout orientam o runtime de Xbox original; NFS Most Wanted usa Xbox 360/ReXGlue, portanto é referência de organização, não código reaproveitável do jogo.

Próximos marcos: obter log do primeiro boot, resolver o primeiro bloqueio real, validar menu/controles, renderização, áudio, saves e gameplay. Não distribuir os arquivos do jogo, a ISO ou o código gerado. Licenças dos componentes permanecem em `LICENSE`, `NOTICE` e `xboxrecomp/LICENSES/`.


