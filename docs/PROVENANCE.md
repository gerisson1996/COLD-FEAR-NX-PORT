# Origem da base

- `src/`, `gen/`, `analysis/` e `game/`: trabalho inicial encontrado em `Cold Fear/ColdFearPort` ao lado da ISO fornecida pelo usuário. A origem foi preservada.
- `xboxrecomp/`: snapshot local de `mksm-sw/xboxrecomp`, sem caches nem análises de MKSM. Licenças e avisos originais preservados.
- `nvk_sdk/`: SDK local de `mksm-sw/nvk_sdk`, utilizado pelo backend Vulkan para Switch.
- `burnout3-sw`: referência de arquitetura Xbox → C → ARM64, GPU NV2A e organização do SD.
- `nfsmw-nx-ref`: referência de distribuição para Switch/Vulkan. Usa Xbox 360/ReXGlue e não fornece código recompilado compatível com este XBE.

XBE: Cold Fear, Title ID `0x5553004B`, entry point `0x0001BA4E`.
SHA-256: `7371e422ecd0ac5c95ebaa343a6a9c3d504f2f261a53ed8acf212f258ab377ef`.

Os arquivos gerados são da análise inicial; não equivalem a validação de execução. A compilação OpenGL encontrou chamada `gl_flip()` incompatível no runtime herdado. O build selecionado usa Vulkan/NVK. Nenhuma correção de renderização OpenGL foi aplicada.
