[Português (BR)](README.md) | [English](README.en.md) | **Español**

# Cold Fear (Xbox) para Nintendo Switch

Port experimental por recompilación estática: el ejecutable x86 de Xbox se convierte en C y se compila para ARM64 con el toolkit [xboxrecomp](xboxrecomp/) y un renderizador Vulkan (NVK). No es emulación: el código del juego corre de forma nativa, y solo se reimplementa el hardware de Xbox (GPU NV2A, audio, kernel).

Versión **0.0.3** — autor RECOMP-GUTO.

**Este repositorio no contiene el juego.** Los archivos del disco, el XBE y el código C generado a partir de él no se distribuyen. Para compilar o jugar necesitas tu propia copia de Cold Fear (Xbox).

## Apoya el proyecto

El port es gratis y de código abierto. Si quieres apoyar el tiempo de desarrollo y las pruebas en hardware real, puedes donar en Ko-fi:

[![Ko-fi](https://img.shields.io/badge/Ko--fi-apoyar-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/gerissoncassiano)

Desde Brasil también puedes usar **PIX** (clave de correo): `pocketneogames@gmail.com` — código QR en el [README en portugués](README.md#apoie-o-projeto).

Las donaciones son opcionales: nadie tiene que donar para jugar, y el proyecto no incluye ni vende el juego.

## Versión del juego usada

El port se hizo y se probó con esta versión. Otras regiones o revisiones pueden no funcionar, porque el código depende del ejecutable exacto.

| Campo | Valor |
| --- | --- |
| Juego | Cold Fear (Xbox, Ubisoft, 2005) |
| Región | NTSC-U (EE. UU.) |
| Title ID | `0x5553004B` (US-075) |
| Versión del certificado | 1 |
| XBE compilado | 06/02/2005 (XDK 5849) |
| `default.xbe` | 3.624.960 bytes |
| SHA-256 de `default.xbe` | `7371e422ecd0ac5c95ebaa343a6a9c3d504f2f261a53ed8acf212f258ab377ef` |

Para comprobar tu copia en PowerShell:

```powershell
Get-FileHash .\game\default.xbe -Algorithm SHA256
```

## Estado

Jugable en una Switch real, desde el menú hasta el gameplay en el barco.

- Audio: efectos, voces y música (streams de FMOD reproducidos en el host).
- Video: Vulkan/NVK con los shaders de vértices y de píxeles de la NV2A traducidos a GLSL, niebla, stencil, mapas de sombra de la lámpara, texturas de volumen, polygon offset, la rampa de gamma del juego.
- Iluminación fija de la NV2A (luces por vértice, generación de coordenadas de textura y matrices de textura) en el shader de vértices.
- Brillo ajustable en el juego: mantén **−** y pulsa la cruceta **arriba/abajo** (0 a 10, se guarda en la SD).
- Rendimiento: unos 22–30 FPS en el barco sin overclock (el overclock opcional suma ~5).
- Resolución interna 960×720 por defecto (`RECOMP_GL_SCALE`), más nítida en la pantalla de 720p.

Problemas conocidos:

- El brillo especular de la iluminación fija aún no se calcula.
- Los logs de diagnóstico siguen compilados.

## Próximas correcciones

- **Rendimiento:** llegar a 30 FPS estables, reduciendo el costo de las llamadas entre funciones del juego (registros guardados en cada llamada), de las lecturas de TLS y del cambio entre los hilos del juego.

## Jugar (release lista)

1. Descarga `coldfear.nro` en [Releases](https://github.com/gerisson1996/COLD-FEAR-NX-PORT/releases).
2. Cópialo a `sdmc:/switch/coldfear/`.
3. Copia los archivos de tu disco a `sdmc:/switch/coldfear/game/`.
4. Ábrelo con title takeover (mantén R al abrir un juego instalado) para tener toda la memoria.

```text
switch/coldfear/
  coldfear.nro
  coldfear_env.txt   (opcional)
  game/
    default.xbe
    bigfile.bin
    data/
```

El log se guarda en `sdmc:/switch/coldfear/coldfear_log.txt`.

## Compilar

Necesitas:

- devkitPro con devkitA64, libnx y SDL2 (MSYS2).
- El SDK de NVK en `nvk_sdk/` (Mesa NVK para Switch; no se incluye aquí por su tamaño).
- Python con `capstone`, para generar el código C.
- Los archivos de tu disco de Cold Fear en `game/`.

Generar el código C a partir de tu `default.xbe`:

```powershell
.\tools\regen.ps1 -Python 'C:\ruta\python.exe'
```

Compilar (MSYS2/devkitPro, en esta carpeta):

```bash
bash switch/build.sh
```

Preparar la carpeta de la tarjeta SD en `switch_sd/`:

```powershell
.\tools\package.ps1
```

Para comprobar los archivos del disco contra una ISO:

```powershell
python .\tools\verify_disc.py 'C:\ruta\Cold Fear.iso'
```

## Opciones (`coldfear_env.txt`)

Una por línea, `CLAVE=valor`; las líneas que empiezan con `#` se ignoran.

| Opción | Efecto |
| --- | --- |
| `COLDFEAR_LOG=0` | sin log |
| `NV2A_BRIGHTNESS=n` | brillo inicial (0–10) si aún no existe `brightness.txt` |
| `RECOMP_GL_SCALE=n` | resolución interna: 1 = 640×480, 1.5 = 960×720 (por defecto), 2 = 1280×960 |
| `RECOMP_VK_INSTANCE=0` | desactiva la agrupación de dibujos repetidos (activada por defecto) |
| `NV2A_SHADOW_BIAS=n` | tolerancia de la comparación de sombra de las lámparas (128 por defecto) |
| `COLDFEAR_CPU_MHZ` / `COLDFEAR_GPU_MHZ` / `COLDFEAR_MEM_MHZ` | overclock opcional |

## Licencias y origen

Consulta `docs/PROVENANCE.md` para el origen de cada componente. El toolkit xboxrecomp conserva sus licencias en `xboxrecomp/LICENSE`, `xboxrecomp/NOTICE` y `xboxrecomp/LICENSES/`; el resto sigue `LICENSE` y `NOTICE` en la raíz. No distribuyas los archivos del juego, la ISO ni el código generado.
