[Português (BR)](README.md) | [English](README.en.md) | **Español**

# Cold Fear (Xbox) para Nintendo Switch

Port experimental por recompilación estática: el ejecutable x86 de Xbox se convierte en C++ y se compila para ARM64 con el [XboxSwitchPortKit](kit/) y un renderizador Vulkan (NVK). No es emulación: el código del juego corre de forma nativa, y las bibliotecas de Xbox (Direct3D 8, DirectSound, kernel) se reescriben para la Switch.

Versión **0.1.37** — autor RECOMP-GUTO.

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

Jugable en una Switch real, desde el menú hasta el gameplay, a 30 FPS la mayor parte del tiempo.

Desde la versión 0.1 el port se rehízo sobre el **XboxSwitchPortKit** (carpeta [`kit/`](kit/)): en vez de emular la GPU NV2A, el Direct3D 8 y el DirectSound de la Xbox se reescriben directamente sobre Vulkan (NVK) y el audio de la Switch. El port antiguo (0.0.x, con emulación de la NV2A) sigue en la raíz del repositorio como referencia.

Lo que tiene la 0.1.37:

- **Giroscopio al apuntar:** mantén **ZL** (apuntar) y mueve la consola. El giro entra directo en los ángulos de puntería del juego, 1:1, sin la zona muerta ni la aceleración del stick. Opciones: sensibilidad, velocidad vertical, eje (girar / inclinar / ambos), estabilidad (filtra el temblor de la mano), corrección de deriva e invertir X/Y.
- **Sombras de las lámparas** corregidas: el jugador y los objetos proyectan sombra, sin las zonas oscuras de bordes rectos de antes.
- **Pantalla OLED:** opción OLED SCREEN BOOST (AUTO / OFF / ON) que aclara los tonos oscuros. En AUTO se activa sola en una Switch OLED en modo portátil y se apaga en el dock.
- **Menú del port:** mantén **−** y pulsa **R3**. Resolución de renderizado, filtro de pantalla, brillo, V-Sync, límite de FPS, contador de FPS, filtro anisotrópico, sombras, zonas muertas y sensibilidad de los sticks, salida de audio y las opciones de giroscopio.
- Franjas negras solo en el gameplay (como en el original); los menús ocupan toda la pantalla.
- Audio: efectos, voces y música.
- Carátula en el menú de la Switch.
- Sin overclock.

Rendimiento medido en una Switch (portátil, resolución 100%): 30 FPS en la mayor parte del juego; en zonas pesadas baja a 23–26 FPS unos segundos, igual que en las cargas y la primera vez que aparece un efecto. Cada 300 cuadros el log registra los FPS y a dónde fue el tiempo (código del juego, GPU, inactivo).

Problemas conocidos:

- Caídas de FPS en las zonas más pesadas: la resolución dinámica reacciona tarde.
- Las opciones del port están solo en el menú **− + R3**, aún no dentro del menú del propio juego.

## Jugar

Necesitas tu propia copia de Cold Fear (Xbox, NTSC-U). **Desde la 0.1 el `.nro` no se distribuye aquí**, porque contiene el código del juego recompilado: compila el tuyo (sección siguiente).

```text
sdmc:/switch/coldfear/
  coldfear.nro
  game/              <- los archivos del disco (default.xbe, bigfile.bin, DATA/...) o un .iso
  logs/              <- creado por el port
```

Ábrelo desde hbmenu **manteniendo R al abrir un juego instalado** (modo título, con toda la memoria).

Controles: los del juego original. **ZL** apunta (y activa el giroscopio), **− + R3** abre el menú del port.

## Compilar (port nuevo, carpeta `kit/`)

`kit/` tiene el código del port y del kit, sin el juego y sin los binarios grandes:

| Carpeta | Qué es |
| --- | --- |
| `kit/exemplos/coldfear/` | El port: HLE de Direct3D 8 y DirectSound, giroscopio, sombras, OLED, menú (`src/`), `port.cmake`, `config/game.toml`, carátula. `HANDOFF.md` tiene las notas de desarrollo (en portugués). |
| `kit/host-switch/` | Capa de la Switch: Vulkan/NVK, audio, mandos, giroscopio, ImGui. |
| `kit/host-pc/` | Build de prueba para Windows con el mismo código del juego y el mismo renderizador, para encontrar errores sin la consola. |
| `kit/sdk/XboxXBE-Recomp/` | El recompilador y el runtime ([TGP482/XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), MIT), con los cambios para la Switch. |
| `kit/ferramentas/`, `kit/docs/` | Scripts para recompilar, compilar y empaquetar, y la documentación del kit (en portugués). |

No están en el repositorio, por tamaño o por venir del juego: `nvk/` (el driver Mesa NVK compilado para la Switch), `third_party/` (glslang, Dear ImGui, toml++), `exemplos/coldfear/assets/default.xbe` y `exemplos/coldfear/generated/` (el C++ generado de tu xbe).

Pasos (detalles en [`kit/LEIA-ME.md`](kit/LEIA-ME.md) y [`kit/docs/`](kit/docs/)):

1. Copia `kit/` a una ruta sin acentos ni espacios, por ejemplo `C:\XboxSwitchPortKit`, y añade `nvk/` y `third_party/`.
2. Instala Visual Studio 2022 Build Tools con Clang y devkitPro con `switch-dev` ([requisitos](kit/docs/01-requisitos.md)).
3. Pon tu `default.xbe` en `exemplos\coldfear\assets\`.
4. Recompila, compila y empaqueta:

```powershell
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\recompilar.ps1 C:\XboxSwitchPortKit\exemplos\coldfear
```

```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/XboxSwitchPortKit && JOBS=8 ./ferramentas/build_switch.sh exemplos/coldfear'
```

```powershell
powershell -ExecutionPolicy Bypass -File C:\XboxSwitchPortKit\ferramentas\empacotar.ps1 C:\XboxSwitchPortKit\exemplos\coldfear
```

El resultado queda en `sd\switch\coldfear\coldfear.nro`.

## Port antiguo (0.0.x)

El código de la raíz (`src/`, `xboxrecomp/`, `switch/`, `tools/`) es el primer port, con emulación de la GPU NV2A, detenido en la 0.0.3 (22–30 FPS). Sus instrucciones están en `docs/BUILD_STATUS.md` y en el historial de este README.

## Licencias y origen

- Port nuevo: XboxXBE-Recomp (MIT), Black-Android, base de `kit/exemplos/coldfear/src/common` (MIT, `kit/exemplos/coldfear/LICENSE-Black-Android`), Mesa/NVK (MIT), glslang, Dear ImGui y toml++ bajo sus propias licencias.
- Port antiguo: ver `docs/PROVENANCE.md`; el toolkit xboxrecomp mantiene sus licencias en `xboxrecomp/LICENSE`, `xboxrecomp/NOTICE` y `xboxrecomp/LICENSES/`; el resto sigue `LICENSE` y `NOTICE` en la raíz.
- No distribuyas los archivos del juego, la ISO, el código generado ni un `.nro` compilado.
