# VMT-Studio

[![Plataforma: Windows](https://img.shields.io/badge/Plataforma-Windows-blue.svg)](https://microsoft.com/windows)
[![C++17](https://img.shields.io/badge/Lenguaje-C%2B%2B17-00599C.svg)](https://isocpp.org/)
[![UI: Qt 6](https://img.shields.io/badge/UI-Qt%206-41CD52.svg)](https://www.qt.io/)
[![Motor: Source Engine](https://img.shields.io/badge/Renderizador-Source%20Engine%20(Nativo%20HLMV)-F37021.svg)](https://developer.valvesoftware.com/wiki/Half-Life_Model_Viewer)
[![Licencia: MIT](https://img.shields.io/badge/Licencia-MIT-green.svg)](LICENSE)

*Leer en otros idiomas: [English](README.md)*

**VMT-Studio** es un entorno y estudio de materiales moderno y de alto rendimiento, inspector de parámetros y previsualizador 3D en tiempo real para materiales (`.vmt`) y texturas (`.vtf`) del motor Source de Valve.

A diferencia de visores genéricos que intentan aproximar los shaders de Source mediante reimplementaciones en OpenGL o Vulkan, **VMT-Studio utiliza de forma nativa el motor de renderizado de Source a través de una arquitectura basada en HLMV (Half-Life Model Viewer)**. Esto garantiza una fidelidad visual idéntica al 100% (1:1) a la del juego real en todos los shaders complejos de Source (Phong, Bumpmaps, cubemaps de Envmap, Lightwarps, texturas de detalle y geometría de brushes).

---

## Tabla de Contenidos

- [Características Principales](#características-principales)
- [Arquitectura](#arquitectura)
  - [Arquitectura de Doble Proceso (UI en x64 + HLMV en Win32)](#arquitectura-de-doble-proceso-ui-en-x64--hlmv-en-win32)
  - [Renderizado Nativo de Brushes y Modelos Studio](#renderizado-nativo-de-brushes-y-modelos-studio)
  - [Módulos del Proyecto](#módulos-del-proyecto)
- [Juegos y Motores Compatibles](#juegos-y-motores-compatibles)
- [Requisitos de Compilación](#requisitos-de-compilación)
- [Guía de Compilación](#guía-de-compilación)
  - [Paso 1: Clonar e Inicializar Submódulos](#paso-1-clonar-e-inicializar-submódulos)
  - [Paso 2: Compilar VTFLib++ (32 y 64 bits)](#paso-2-compilar-vtflib-32-y-64-bits)
  - [Paso 3: Compilar el Backend HLMV de 32 bits](#paso-3-compilar-el-backend-hlmv-de-32-bits)
  - [Paso 4: Compilar el Frontend Qt 6 de 64 bits](#paso-4-compilar-el-frontend-qt-6-de-64-bits)
- [Uso y Configuración](#uso-y-configuración)
- [Pruebas Automatizadas](#pruebas-automatizadas)
- [Licencias y Avisos Legales](#licencias-y-avisos-legales)
- [Declaración sobre Inteligencia Artificial](#declaración-sobre-inteligencia-artificial)

---

## Características Principales

- **Renderizado 100% Nativo de Source Engine**:
  - Utiliza directamente las bibliotecas Direct3D 9 del juego instalado (`materialsystem.dll`, `tier0.dll`, `vstdlib.dll`).
  - Sin aproximaciones: specular highlights exactos, curvas de fresnel, sombreado half-lambert, lightwarps, modos de mezcla y reflejos cubemap originales.
- **Previsualización de Modelos y Brushes**:
  - **Modelos Studio**: Esfera, Cubo, Cilindro, Plano o modelos `.mdl` personalizados con control de reproducción de animaciones y velocidad.
  - **Geometría de Brush Nativa**: Generación procedural de superficies de mundo reales (`brush_wall`, `brush_floor`, `brush_cube` y `brush_displacement`) mediante `CMeshBuilder` con normales, coordenadas UV de lightmap y colores de vértice auténticos para shaders de mundo (`LightmappedGeneric`, `WorldVertexTransition`).
- **Edición Bidireccional en Tiempo Real**:
  - **Inspector Visual de Parámetros**: Interfaz dinámica basada en esquemas oficiales, con deslizadores, casillas, selectores de color y explorador de texturas.
  - **Editor de Código**: Editor de texto limpio con resaltado de sintaxis para archivos VMT.
  - **Sincronización Inmediata con Debounce**: Cualquier cambio en el inspector o en el editor de texto actualiza la vista 3D en tiempo real (debounce de 250 ms).
  - **Deshacer / Rehacer Completo**: Historial unificado de cambios para ambos editores (`Ctrl+Z` / `Ctrl+Y`).
- **Explorador de Materiales (`F3`)**:
  - Escaneo recursivo de directorios y lectura directa de archivos empaquetados en `.vpk` sin necesidad de extraerlos.
  - Generación de miniaturas (thumbnails) en tiempo real renderizadas por el propio motor, con caché local en disco (`%LOCALAPPDATA%\VMTStudio\thumbnails\`).
- **Validador de Sintaxis y Diagnósticos (`F7`)**:
  - Comprobación contra esquemas oficiales de shaders de Source (`data/schema`).
  - Detección de shaders desconocidos, parámetros obsoletos, incompatibilidades de tipo y parámetros dependientes faltantes (ej: `$phongboost` sin `$phong`).
  - **Verificación de Texturas en VFS**: Consulta el sistema de archivos virtual (`IFileSystem`) del juego para verificar si las texturas VTF referenciadas existen realmente en el disco o en los VPK montados.
- **Estudio de Iluminación y Escena**:
  - Control de luz ambiental, luz principal (dirección, acimut, elevación, color), luz de relleno y color de fondo.
  - Presets de iluminación: *Studio Dark*, *Studio Light*, *Outdoor* y *Flat*.
- **Modos de Sombreado y Depuración**:
  - Textured / Shaded, Flatshaded, Wireframe y vista de depuración de Normal Map / Bumpmap.
- **Flujo de Trabajo Seguro y Atómico**:
  - Guardado atómico mediante archivos `.tmp` para evitar corrupciones en caso de cierre inesperado.
  - Guardado como override suelto: opción `File > Save to game folder...` para guardar directamente en la carpeta `materials/` del juego activo sobreescribiendo el contenido de los VPK.

---

## Arquitectura

### Arquitectura de Doble Proceso (UI en x64 + HLMV en Win32)

Los binarios de Source Engine 2013 y de juegos comerciales (Half-Life 2, Counter-Strike: Source, Team Fortress 2, Garry's Mod) son aplicaciones heredadas de 32 bits (`x86`). Por el contrario, los frameworks de interfaz gráfica modernos como **Qt 6** están optimizados para arquitecturas de 64 bits (`x64`). En Windows, un proceso de 64 bits no puede cargar directamente DLLs de 32 bits en su mismo espacio de memoria.

Para solucionar este desafío, **VMT-Studio** utiliza una arquitectura cliente-servidor out-of-process:

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

1. **`vmtstudio` (Qt 6 x64)**: Ejecuta la interfaz de usuario moderna, maneja la carga y guardado de archivos, procesa los esquemas de shaders, ofrece resaltado de sintaxis y aloja un contenedor nativo Win32 (`ViewportWidget`).
2. **`hlmv_backend` (Win32 x86)**: Se lanza como proceso hijo con los parámetros `-embed <HWND> -game <dir> -bin <dir>`. Carga las bibliotecas nativas de Source Engine del juego, inicializa Direct3D 9 y reubica su ventana de renderizado dentro del widget de Qt mediante `SetParent`.
3. **Comunicación IPC de Ultra Baja Latencia**: La interfaz envía comandos (`SET_MATERIAL`, `RELOAD_MATERIAL`, `SET_SHAPE`, `SET_LIGHTING`, etc.) mediante mensajes de Windows `WM_COPYDATA` sin latencia perceptible.

### Renderizado Nativo de Brushes y Modelos Studio

La mayoría de visores externos se limitan a mostrar materiales en una esfera de modelo 3D. En Source Engine, los materiales se dividen en dos categorías fundamentales:
- **Modelos Studio (`VertexLitGeneric`)**: Requieren cálculo de huesos, cubos ambientales y tubería de iluminación de personajes o props.
- **Brushes de Mundo (`LightmappedGeneric`, `WorldVertexTransition`)**: Requieren lightmaps de superficie, planos de corte o mallas de desplazamiento (displacements).

El backend `hlmv_backend` integra un módulo dedicado `brush_render` que interactúa directamente con `IMesh` y `CMeshBuilder` de Valve. Genera mallas reales de brush (Pared, Suelo, Cubo y Displacements) con normales correctas, coordenadas UV para lightmaps y colores por vértice, logrando que los materiales de mapa se visualicen con total precisión.

### Módulos del Proyecto

- **`vmt_core`**: Parser de KeyValues, modelo AST y serializador VMT sin pérdidas. Preserva comentarios, espacios exactos, mayúsculas/minúsculas y comillas. No depende de Windows ni de Qt.
- **`vtf_reader`**: Decodificador rápido de texturas VTF basado en `VTFLib++`. Decodifica formatos como DXT1, DXT5, RGBA8888, BC4/BC5, cubemaps y mipmaps a buffers RGBA8.
- **`hlmv_backend`**: Proceso de 32 bits que contiene el código de Half-Life Model Viewer, MX Toolkit (`mxtk`), renderizado de brushes y escucha IPC.
- **`engine_bridge`**: Capa de comunicación IPC, gestión del ciclo de vida del subproceso y shims de compatibilidad (como el ajuste de vtable para `VFileSystem022` de Garry's Mod).
- **`game_profiles`**: Detección de juegos instalados mediante la lectura de `gameinfo.txt` y localización de carpetas `/bin`.
- **`ui`**: Aplicación gráfica Qt 6 (`vmtstudio`), docks acoplables, diálogos, inspectores y resaltador de sintaxis.
- **`data/schema`**: Definición en formato JSON de las propiedades válidas de cada shader de Source.

---

## Juegos y Motores Compatibles

El puente del motor es compatible de forma automática con cualquier rama derivada de Source SDK 2013 / Alien Swarm:
- **Half-Life 2** (incluidos Episodio 1 y Episodio 2)
- **Portal**
- **Counter-Strike: Source**
- **Team Fortress 2**
- **Source SDK Base 2013 Singleplayer / Multiplayer**
- **Garry's Mod** (escaneo de VPK y sistema de archivos mediante shim de vtable)
- **Mods personalizados de Source** que cuenten con un archivo `gameinfo.txt` y carpeta `/bin`.

---

## Requisitos de Compilación

- **Sistema Operativo**: Windows 10 o Windows 11 (64 bits).
- **Compilador**: Visual Studio 2022 (MSVC v143) con la carga de trabajo **Desarrollo para el escritorio con C++**.
- **Sistema de Construcción**: CMake 3.21 o superior.
- **Framework Qt**: Qt 6.8+ (MSVC 2022 de 64 bits).
- **Source SDK 2013**: Cabeceras públicas y librerías de importación (`SOURCE_SDK_PUBLIC` apuntando a `src/public`).
- **Git**: Con soporte para submódulos.

---

## Guía de Compilación

Dado que el backend de renderizado debe ser de **32 bits (Win32)** para cargar las DLLs de Source, mientras que la interfaz Qt 6 es de **64 bits (x64)**, la compilación se realiza en dos fases:

### Paso 1: Clonar e Inicializar Submódulos

```powershell
git clone --recursive https://github.com/tu-usuario/VMT-Studio.git
cd VMT-Studio
```

Si el repositorio ya fue clonado:
```powershell
git submodule update --init --recursive
```

### Paso 2: Compilar VTFLib++ (32 y 64 bits)

1. Aplicar el parche a VTFLib++ (reemplaza `afxres.h` por `winres.h` eliminando la dependencia de MFC):
   ```powershell
   cmake -P thirdparty/patches/apply_vtflib_patches.cmake
   ```

2. Compilar VTFLib++ en 32 bits (para `hlmv_backend`):
   ```powershell
   cmake -B thirdparty/vtflib/build_vtflib -A Win32 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
   cmake --build thirdparty/vtflib/build_vtflib --config Release
   ```

3. Compilar VTFLib++ en 64 bits (para `vmtstudio`):
   ```powershell
   cmake -B thirdparty/vtflib/build_vtflib64 -A x64 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
   cmake --build thirdparty/vtflib/build_vtflib64 --config Release
   ```

### Paso 3: Compilar el Backend HLMV de 32 bits

Genera el ejecutable de renderizado nativo `hlmv_backend.exe`:

```powershell
cmake -S . -B build -A Win32 -DSOURCE_SDK_PUBLIC="<ruta-a-source-sdk-2013>/src/public"
cmake --build build --config Release --target hlmv_backend
```

### Paso 4: Compilar el Frontend Qt 6 de 64 bits

Configura y compila la interfaz principal. El sistema copiará automáticamente `hlmv_backend.exe` junto a `vmtstudio.exe` y ejecutará `windeployqt`:

```powershell
cmake -S . -B build64 -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.2/msvc2022_64"
cmake --build build64 --config Release --target vmtstudio
```

El ejecutable listo para usar se encontrará en:
```
build64/bin/Release/vmtstudio.exe
```

---

## Uso y Configuración

1. Ejecuta `build64/bin/Release/vmtstudio.exe`.
2. Dirígete a **Game > Connection Settings...**:
   - Selecciona el **Game Directory** (carpeta que contiene `gameinfo.txt`, por ejemplo: `C:/Program Files (x86)/Steam/steamapps/common/Half-Life 2/hl2`).
   - Selecciona el **Bin Directory** (ejemplo: `C:/Program Files (x86)/Steam/steamapps/common/Half-Life 2/bin`).
3. Abre cualquier archivo `.vmt` desde **File > Open**, o utiliza el **Material Browser (`F3`)** para explorar los materiales instalados o empaquetados en VPK.
4. El visor 3D iniciará el backend de HLMV de manera transparente, renderizando el material con fidelidad idéntica al motor Source.

---

## Pruebas Automatizadas

Puedes ejecutar las baterías de pruebas con CTest:

```powershell
# Pruebas de 64 bits: parser vmt_core, decodificador VTF y UI
ctest --test-dir build64 -C Release --output-on-failure

# Pruebas de 32 bits: puente de motor y componentes de sistema
ctest --test-dir build -C Release --output-on-failure
```

---

## Licencias y Avisos Legales

### Declaración sobre Valve Corporation
- **VMT-Studio NO redistribuye ningún binario propietario, textura, modelo ni archivo con derechos de autor de Valve Corporation.**
- Las DLLs del motor, materiales y modelos son cargados dinámicamente en tiempo de ejecución desde una instalación local y legalmente adquirida de un juego con motor Source perteneciente al usuario.
- *Valve*, el *logotipo de Valve*, *Half-Life*, *Source*, *Portal*, *Counter-Strike* y *Team Fortress* son marcas comerciales o registradas de **Valve Corporation**.

### Licencias de Terceros
- **VTFLib++**: Distribuida bajo la licencia **GNU Lesser General Public License v2.1 (LGPL-2.1)**. El texto de la licencia se encuentra en `thirdparty/vtflib/LGPL.txt`. Se enlaza de forma dinámica (DLL) y se proporciona el código fuente completo con parches en `thirdparty/`.
- **Qt 6**: Distribuido bajo licencia **LGPLv3** / Comercial por The Qt Company Ltd.
- **Source SDK 2013**: Utilizado bajo la licencia de Valve Source SDK License.

### Licencia de VMT-Studio
Este proyecto se distribuye como software de código abierto bajo la licencia **MIT License**. Consulta el archivo [LICENSE](LICENSE) para más detalles.

---

## Declaración sobre Inteligencia Artificial

Partes de este proyecto fueron diseñadas, programadas, refactorizadas y documentadas con la asistencia de Inteligencia Artificial (IA).

Si te molesta o tienes un problema con el software desarrollado con asistencia de IA: **sinceramente nos da completamente igual tu opinión**. La herramienta funciona, renderiza materiales del motor Source de forma nativa con fidelidad 1:1, soluciona problemas reales y es gratuita y de código abierto. Úsala o no.

