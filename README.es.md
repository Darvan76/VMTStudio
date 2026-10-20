<div align="center">

<img src="vmtstudio-icon.png" alt="Logo de VMT-Studio" width="128" height="128" />

# VMT-Studio

**Un estudio de materiales nativo y moderno, impulsado por el renderizador de Source Engine de Valve — fidelidad pixel-perfect, directamente en tu escritorio.**

![Release](https://img.shields.io/badge/release-v0.2.0-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![C++](https://img.shields.io/badge/standard-C%2B%2B17-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey.svg)
![Qt](https://img.shields.io/badge/GUI-Qt%206.8-green.svg)
[![Motor: Source Engine](https://img.shields.io/badge/Renderizador-Source%20Engine%20(Nativo%20HLMV)-F37021.svg)](https://developer.valvesoftware.com/wiki/Half-Life_Model_Viewer)

*Leer en otros idiomas: [English](README.md)*

</div>

---

**VMT-Studio** es un estudio de materiales de alto rendimiento, inspector de parámetros y previsualizador 3D en tiempo real para materiales (`.vmt`) y texturas (`.vtf`) del motor Source de Valve. Escrito en C++17 con interfaz Qt 6, integra el renderizador real de Source Engine para ofrecer una **fidelidad visual 100% auténtica e idéntica al juego** — sin aproximaciones, sin reimplementaciones.

A diferencia de visores genéricos que intentan replicar los shaders de Source mediante pipelines personalizados en OpenGL o Vulkan, VMT-Studio carga la `materialsystem.dll` nativa de Valve y renderiza a través de una arquitectura HLMV (Half-Life Model Viewer) embebida. Cada reflejo Phong, cada bumpmap, cada cubemap, cada lightwarp — se ven exactamente igual que dentro del juego.

---

## Tabla de Contenidos

- [Características Principales](#características-principales)
- [Arquitectura](#arquitectura)
  - [Arquitectura de Doble Proceso (UI x64 + HLMV Win32)](#arquitectura-de-doble-proceso-ui-x64--hlmv-win32)
  - [Renderizado Nativo de Brushes y Modelos Studio](#renderizado-nativo-de-brushes-y-modelos-studio)
  - [Módulos del Proyecto](#módulos-del-proyecto)
- [Juegos y Motores Compatibles](#juegos-y-motores-compatibles)
- [Versión Portable (Inicio Rápido)](#versión-portable-inicio-rápido)
- [Requisitos de Compilación](#requisitos-de-compilación)
- [Guía de Compilación](#guía-de-compilación)
  - [Paso 1: Clonar e Inicializar Submódulos](#paso-1-clonar-e-inicializar-submódulos)
  - [Paso 2: Compilar VTFLib++ (32 y 64 bits)](#paso-2-compilar-vtflib-32-y-64-bits)
  - [Paso 3: Compilar el Backend HLMV de 32 bits](#paso-3-compilar-el-backend-hlmv-de-32-bits)
  - [Paso 4: Compilar el Frontend Qt 6 de 64 bits](#paso-4-compilar-el-frontend-qt-6-de-64-bits)
- [Uso y Configuración](#uso-y-configuración)
- [Visor de Modelos](#visor-de-modelos)
- [Pruebas Automatizadas](#pruebas-automatizadas)
- [Licencias y Avisos Legales](#licencias-y-avisos-legales)
- [Declaración sobre Inteligencia Artificial](#declaración-sobre-inteligencia-artificial)

---

## Características Principales

### 🎨 Renderizado 100% Nativo de Source Engine

- Carga directamente las bibliotecas Direct3D 9 del juego instalado (`materialsystem.dll`, `tier0.dll`, `vstdlib.dll`).
- Sin aproximaciones — specular highlights exactos, curvas de fresnel, sombreado half-lambert, lightwarps, modos de mezcla y reflejos cubemap originales.

### 🧊 Previsualización Dual: Modelos y Brushes

- **Modelos Studio**: Esfera, Cubo, Cilindro, Plano o modelos `.mdl` personalizados con reproducción de animaciones y control de velocidad.
- **Geometría de Brush Nativa**: Superficies de mundo reales (`brush_wall`, `brush_floor`, `brush_cube`, `brush_displacement`) construidas con `CMeshBuilder` de Source — normales auténticas, UVs de lightmap y colores de vértice para shaders `LightmappedGeneric` y `WorldVertexTransition`.

### ✏️ Edición Bidireccional en Tiempo Real

- **Inspector Visual de Parámetros** — editor dinámico basado en esquemas con deslizadores, casillas, selectores de color y explorador de texturas.
- **Editor de Código** — editor de texto con resaltado de sintaxis para VMT.
- **Sincronización Instantánea** — los cambios en cualquier editor actualizan la vista 3D en tiempo real (debounce de 250 ms).
- **Deshacer / Rehacer Completo** — historial unificado en ambos editores (`Ctrl+Z` / `Ctrl+Y`).

### 📂 Explorador de Assets (`F3` / `F4`)

- Una única ventana modeless para materiales, texturas y modelos (`.vmt`, `.vtf`, `.mdl`) en carpetas loose, archivos `.vpk` montados y carpetas añadidas por el usuario, sin extraerlos.
- Búsqueda por tokens, filtros por tipo y fuente, vistas de lista y rejilla, previews por tipo y arrastrar y soltar sobre el visor.
- Apertura instantánea desde una caché de índice por juego (`%LOCALAPPDATA%\VMTStudio\cache\<game-key>\assets-v1.tsv`); **Refresh** (`Ctrl+R`) reescanea y la reescribe.
- Las miniaturas se decodifican fuera del hilo de UI y se cachean por juego (`%LOCALAPPDATA%\VMTStudio\thumbnails\<game-key>\`).
- `F4` abre la misma ventana filtrada a modelos; el selector de texturas del inspector la usa en modo *pick*, por lo que las texturas dentro de VPK son seleccionables.

### 🔍 Validación de Esquemas y Diagnósticos (`F7`)

- Comprobación contra esquemas oficiales de shaders de Source (`data/schema`).
- Detección de shaders desconocidos, parámetros obsoletos, incompatibilidades de tipo y dependencias faltantes (ej: `$phongboost` sin `$phong`).
- **Verificación de Texturas en VFS** — consulta el `IFileSystem` del juego para verificar si las texturas VTF referenciadas existen en disco o dentro de los VPK montados.

### 💡 Estudio de Iluminación y Escena

- Luz ambiental, luz principal (dirección, acimut, elevación, color), luz de relleno y color de fondo.
- Presets de iluminación: *Studio Dark*, *Studio Light*, *Outdoor* y *Flat*.

### 🔬 Modos de Sombreado y Depuración

- Textured / Shaded, Flatshaded, Wireframe y previsualización de Normal Map / Bumpmap.

### 🎮 Detección Automática de Juegos

- Consulta el registro de Windows (`HKCU\Software\Valve\Steam`) y `STEAMPATH` para localizar Steam.
- Parsea manifiestos de bibliotecas múltiples (`libraryfolders.vdf`) en todas las unidades.
- Menú desplegable interactivo **Juegos conocidos** con selección en un clic e indicadores de estado.

### 🛡️ Validación de la Instalación del Motor

- Verificación en múltiples etapas: estructura de directorios, `gameinfo.txt`, cabeceras PE y arquitectura binaria (32 bits x86).
- Comprobación de DLLs esenciales (`tier0.dll`, `vstdlib.dll`, `filesystem_stdio.dll`, `materialsystem.dll`, `shaderapidx9.dll`).
- Validación de tablas de exportación (`CreateInterface`, `CommandLine_Tier0`) con diagnósticos accionables (`carpeta vacía`, `DLL faltante`, `arquitectura incorrecta`, `sin exportaciones`).

### ⚡ Parsing de Alto Rendimiento

- **`vmt_core`** — escaneo de tokens sin asignaciones de memoria, tablas de delimitadores ASCII, `LineTable` con búsqueda binaria y serialización AST exacta byte a byte.
- **`vtf_reader`** — escalado optimizado para RGBA8888 sin asignaciones intermedias, liberación inmediata de buffers y decodificación thread-safe con `std::scoped_lock`.

### 💾 Flujo de Trabajo Seguro y Atómico

- Guardado atómico mediante archivos `.tmp` para evitar corrupción.
- `File > Save to game folder...` guarda overrides directamente en la carpeta `materials/` del juego, sobreescribiendo el contenido de los VPK.

---

## Arquitectura

### Arquitectura de Doble Proceso (UI x64 + HLMV Win32)

Los binarios de Source Engine 2013 y juegos comerciales (Half-Life 2, CS:S, TF2, Garry's Mod) son aplicaciones heredadas de 32 bits (`x86`). Qt 6 se compila para 64 bits (`x64`). En Windows, un proceso de 64 bits no puede cargar DLLs de 32 bits.

VMT-Studio resuelve esto con una arquitectura cliente-servidor out-of-process:

```
┌─────────────────────────────────────────────────────────────┐
│                    vmtstudio.exe (x64)                      │
│                  Frontend UI (Qt 6 MSVC)                    │
│                                                             │
│  ┌───────────────────────┐       ┌───────────────────────┐  │
│  │   Inspector Visual    │       │   Editor de Código    │  │
│  └───────────┬───────────┘       └───────────┬───────────┘  │
│              │                               │              │
│              └───────────────┬───────────────┘              │
│                              ▼                              │
│                    ┌──────────────────┐                     │
│                    │ Viewport Widget  │ ◄─── (Incrusta HWND)│
│                    └─────────┬────────┘                     │
└──────────────────────────────┼──────────────────────────────┘
                               │ Win32 IPC (WM_COPYDATA)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                   hlmv_backend.exe (x86)                    │
│            Backend de Renderizado (Win32 / C++17)           │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │     HLMV Core + MX Toolkit + Viewport Direct3D 9      │  │
│  └───────────────────────────┬───────────────────────────┘  │
│                              ▼                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │     DLLs Nativas de Source Engine (materialsystem,   │  │
│  │          tier0.dll, vstdlib.dll, filesystem)          │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

1. **`vmtstudio` (Qt 6 x64)** — ejecuta la interfaz moderna, gestiona archivos, procesa esquemas de shaders, ofrece resaltado de sintaxis y aloja un contenedor Win32 nativo (`ViewportWidget`).
2. **`hlmv_backend` (Win32 x86)** — se lanza como proceso hijo con `-embed <HWND> -game <dir> -bin <dir>`. Carga las bibliotecas nativas de Source, inicializa Direct3D 9 y reubica su viewport dentro del widget de Qt.
3. **IPC de Ultra Baja Latencia** — la interfaz envía comandos (`SET_MATERIAL`, `RELOAD_MATERIAL`, `SET_SHAPE`, `SET_LIGHTING`, etc.) mediante `WM_COPYDATA` sin latencia perceptible.

### Renderizado Nativo de Brushes y Modelos Studio

La mayoría de visores solo muestran materiales en una esfera. En Source Engine, los materiales se dividen en dos categorías:

- **Modelos Studio (`VertexLitGeneric`)** — requieren huesos, cubos ambientales y pipeline de iluminación de personajes/props.
- **Brushes de Mundo (`LightmappedGeneric`, `WorldVertexTransition`)** — requieren lightmaps, planos de corte o mallas de desplazamiento.

`hlmv_backend` integra un módulo `brush_render` que interactúa directamente con `IMesh` y `CMeshBuilder` de Valve. Genera mallas auténticas de brush (Pared, Suelo, Cubo, Displacements) con normales correctas, UVs de lightmap y colores por vértice — los materiales de mapa se visualizan con total precisión.

### Módulos del Proyecto

| Módulo | Descripción |
| --- | --- |
| **`vmt_core`** | Parser de KeyValues, modelo AST y serializador sin pérdidas. Escaneo sin asignaciones, `LineTable` con búsqueda binaria, serialización byte a byte. Preserva comentarios, espacios, casing y comillas. Sin dependencias de Windows ni Qt. |
| **`vtf_reader`** | Decodificador rápido de VTF sobre `VTFLib++`. Soporta DXT1, DXT5, RGBA8888, BC4/BC5, cubemaps y mipmaps. Rutas rápidas optimizadas y memoria pico minimizada. |
| **`hlmv_backend`** | Proceso de 32 bits: núcleo HLMV, MX Toolkit (`mxtk`), escucha IPC y generador de mallas de brush. |
| **`engine_bridge`** | Puente IPC, gestión del subproceso, inspección PE y shims de compatibilidad (vtable fixup para `VFileSystem022` de Garry's Mod). |
| **`game_profiles`** | Detección automática de juegos Source en Steam, parsing de `gameinfo.txt` y validación exhaustiva de binarios PE. |
| **`ui`** | Aplicación Qt 6: docks, inspectores, resaltador de sintaxis y diálogos. |
| **`data/schema`** | Definiciones JSON de shaders: `VertexLitGeneric`, `LightmappedGeneric`, `UnlitGeneric`, `WorldVertexTransition`, `Water`, `Refract`, `Sprite`. |

---

## Juegos y Motores Compatibles

El puente del motor soporta conexión automática con cualquier rama derivada de Source SDK 2013 / Alien Swarm:

| Juego | Notas |
| --- | --- |
| **Half-Life 2** (+ Episodios 1 y 2) | |
| **Portal** | |
| **Counter-Strike: Source** | |
| **Team Fortress 2** | |
| **Source SDK Base 2013** (SP / MP) | |
| **Garry's Mod** | Actualmente no funciona (modificaciones autoritarias en el motor; soporte en desarrollo) |
| **Mods personalizados de Source** | Requieren `gameinfo.txt` + carpeta `/bin` (mods con modificaciones autoritarias no funcionan en este momento) |

> [!WARNING]
> **Los juegos con modificaciones autoritarias no están funcionando en este momento:** Aquellos juegos o mods con modificaciones autoritarias o personalizaciones profundas del motor Source (como *Garry's Mod* o ramas con tablas virtuales alteradas en el sistema de archivos y materialsystem) no son compatibles actualmente para la previsualización de materiales. El soporte para este tipo de motores se encuentra en desarrollo activo.

---

## Versión Portable (Inicio Rápido)

¿Prefieres no compilar? Hay una versión portable lista para usar:

1. **Descargar** — descarga `vmtstudio-v0.2.0-win64.zip` desde la página de [Releases](https://github.com/tu-usuario/VMT-Studio/releases).
2. **Descomprimir** — extrae en cualquier carpeta (ej: `C:\Herramientas\VMT-Studio`). No requiere instalación ni permisos de administrador.
3. **Ejecutar** — lanza `vmtstudio.exe`.
4. **Conectar con Source Engine**:
   - Abre **Game > Connection Settings...**
   - Selecciona tu juego en **Juegos conocidos**, o haz clic en **Discover...** para escanear tus bibliotecas de Steam.
   - Opcionalmente, especifica manualmente la carpeta del juego (con `gameinfo.txt`) y la carpeta `/bin`.
   - Pulsa **Test Connection** para validar la instalación.
5. **Comenzar a Editar** — abre cualquier `.vmt` desde **File > Open** o el **Explorador de Assets (`F3`)**.

> [!NOTE]
> El paquete ZIP incluye `vmtstudio.exe` (frontend Qt 6 de 64 bits), `hlmv_backend.exe` (backend de renderizado de 32 bits), `VTFLib.dll` y todas las dependencias de Qt 6. **No se distribuyen recursos propietarios de Valve** — las DLLs del motor se cargan dinámicamente desde tu instalación local del juego.

---

## Requisitos de Compilación

| Requisito | Versión |
| --- | --- |
| **Sistema Operativo** | Windows 10 / 11 (64 bits) |
| **Compilador** | Visual Studio 2022 (MSVC v143) — carga de trabajo *Desarrollo para el escritorio con C++* |
| **Build System** | CMake 3.21+ |
| **Qt** | Qt 6.8+ (MSVC 2022 64-bit) |
| **Source SDK** | Source SDK 2013 — cabeceras públicas (`SOURCE_SDK_PUBLIC` → `src/public`) |
| **Git** | Con soporte para submódulos |

---

## Guía de Compilación

El backend de renderizado debe compilarse para **Win32 (32 bits)** para cargar las DLLs de Source, mientras que el frontend Qt 6 se compila para **x64 (64 bits)**. La compilación se realiza en dos fases:

### Paso 1: Clonar e Inicializar Submódulos

```powershell
git clone --recursive https://github.com/tu-usuario/VMT-Studio.git
cd VMT-Studio
```

Si ya fue clonado:
```powershell
git submodule update --init --recursive
```

### Paso 2: Compilar VTFLib++ (32 y 64 bits)

1. Aplicar el parche (reemplaza `afxres.h` por `winres.h`, eliminando la dependencia de MFC):
   ```powershell
   cmake -P thirdparty/patches/apply_vtflib_patches.cmake
   ```

2. Compilar en 32 bits (para `hlmv_backend`):
   ```powershell
   cmake -B thirdparty/vtflib/build_vtflib -A Win32 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
   cmake --build thirdparty/vtflib/build_vtflib --config Release
   ```

3. Compilar en 64 bits (para `vmtstudio`):
   ```powershell
   cmake -B thirdparty/vtflib/build_vtflib64 -A x64 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
   cmake --build thirdparty/vtflib/build_vtflib64 --config Release
   ```

### Paso 3: Compilar el Backend HLMV de 32 bits

```powershell
cmake -S . -B build -A Win32 -DSOURCE_SDK_PUBLIC="<ruta-a-source-sdk-2013>/src/public"
cmake --build build --config Release --target hlmv_backend
```

### Paso 4: Compilar el Frontend Qt 6 de 64 bits

CMake copiará automáticamente `hlmv_backend.exe` junto a `vmtstudio.exe` y ejecutará `windeployqt`:

```powershell
cmake -S . -B build64 -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.2/msvc2022_64"
cmake --build build64 --config Release --target vmtstudio
```

El ejecutable final se encontrará en:
```
build64/bin/Release/vmtstudio.exe
```

---

## Uso y Configuración

1. Ejecuta `vmtstudio.exe` (o `build64/bin/Release/vmtstudio.exe` si compilaste desde el código fuente).
2. Abre **Game > Connection Settings...**:
   - Selecciona un juego en **Juegos conocidos** o pulsa **Discover...** para escanear bibliotecas de Steam.
   - O configura manualmente el **Directorio del Juego** (con `gameinfo.txt`) y el **Directorio Bin**.
   - Haz clic en **Test Connection** para validar.
3. Abre cualquier `.vmt` desde **File > Open** o el **Explorador de Assets (`F3`)**.
4. El visor 3D iniciará el backend HLMV automáticamente, renderizando el material con fidelidad idéntica al motor Source.

---

## Visor de Modelos

VMT-Studio puede cargar e inspeccionar modelos de estudio (`.mdl`) de Source Engine con el mismo renderizador nativo:

- **Abrir un modelo** — desde **Model > Open Model... (`F4`)** (Explorador de Assets filtrado a modelos), **arrastrando y soltando** un `.mdl` sobre el visor, desde **Model > Recent Models**, o por línea de comandos: `vmtstudio --model models/props_c17/oildrum001.mdl`.
- **Panel de materiales** — todos los materiales del modelo agrupados por bodypart, con búsqueda y navegación anterior/siguiente.
- **Panel de animación** — lista de secuencias con filtros, línea de tiempo, velocidad de reproducción, blending, capas superpuestas, parámetros de pose, bodygroups y skins.
- **Edición in situ** — al seleccionar un material se abre su VMT en el editor; los cambios se guardan como override suelto en la ruta game-relative del material. El motor re-resuelve solo esa ranura, manteniendo el resto intacto. **Ctrl+S** guarda atómicamente.
- **Exportar materiales del modelo (`Ctrl+Shift+E`)** — exporta todos los materiales a disco como `.vmt`, de forma plana o replicando la jerarquía `materials/`.
- **Aislar (experimental)** — oculta todos los materiales excepto el seleccionado mediante overrides sustitutos transparentes. Se eliminan automáticamente al desactivar, cerrar el modelo, cambiar la conexión o salir de la app.

### Atajos

| Atajo | Acción |
| --- | --- |
| `F3` | Abrir el Explorador de Assets |
| `F4` | Abrir el Explorador de Assets filtrado a modelos |
| `Espacio` | Reproducir / pausar animación |
| `[` / `]` | Reducir / duplicar velocidad de reproducción |
| `Alt+Izquierda` / `Alt+Derecha` | Material anterior / siguiente |
| `Ctrl+Shift+E` | Exportar todos los materiales del modelo |
| `Ctrl+M` | Cargar modelo desde diálogo de archivo |

---

## Pruebas Automatizadas

```powershell
# Pruebas de 64 bits (parser, decodificador VTF, UI)
ctest --test-dir build64 -C Release --output-on-failure

# Pruebas de 32 bits (puente de motor, componentes de sistema)
ctest --test-dir build -C Release --output-on-failure
```

---

## Licencias y Avisos Legales

### Declaración sobre Valve Corporation

- **VMT-Studio NO redistribuye ningún binario propietario, textura, modelo ni archivo con derechos de autor de Valve Corporation.**
- Las DLLs del motor, materiales y modelos se cargan dinámicamente desde una instalación local y legalmente adquirida del usuario.
- *Valve*, el *logotipo de Valve*, *Half-Life*, *Source*, *Counter-Strike* y *Team Fortress* son marcas comerciales o registradas de **Valve Corporation**.

### Licencias de Terceros

| Biblioteca | Licencia | Notas |
| --- | --- | --- |
| **VTFLib++** | LGPL-2.1 | Enlazada dinámicamente. Código fuente completo + parches en `thirdparty/`. Agradecimiento a [@ficool2](https://github.com/nicknine) por esta biblioteca y sus contribuciones a la comunidad de Source Engine. |
| **Qt 6** | LGPLv3 / Comercial | © The Qt Company Ltd. |
| **Source SDK 2013** | Valve Source SDK License | |

### Licencia de VMT-Studio

Este proyecto se distribuye como software de código abierto bajo la licencia **MIT License**. Consulta [LICENSE](LICENSE) para más detalles.

---

## Declaración sobre Inteligencia Artificial

Partes de este proyecto fueron diseñadas, programadas, refactorizadas y documentadas con la asistencia de Inteligencia Artificial (IA).

Si te molesta el software desarrollado con asistencia de IA: **sinceramente nos da completamente igual tu opinión**. La herramienta funciona, renderiza materiales de Source de forma nativa con fidelidad 1:1, soluciona problemas reales y es gratuita y de código abierto. Úsala o no.
