# PERF-05: resultados y reproducción

Mediciones locales del 14 de noviembre de 2026. Se completan los cinco puntos
PERF del plan; esta entrega mide primero y corrige los costes observados.
Windows 10.0.26200, x86_64, 16 procesadores lógicos y Qt 6.8.2. Los tiempos de
comparación son de Release x64; el renderer es Win32.

## Qué se mide

- Catálogos deterministas de 10.000, 50.000 y 150.000 assets con rutas largas,
  distintos tipos y fuentes. Se ejercitan índice, pack, modelo Qt y el navegador
  real, incluida su tabla, cuadrícula, debounce y renderizado mediante `grab()`.
- VMT con 64, 512 y 2.048 parámetros: apertura, primer render del inspector,
  actualizaciones, guardado atómico y escritura de preview.
- VTF RGBA de 4096 × 4096 con 13 mips: unos 85,3 MiB de archivo. La generación
  usa bloques pequeños para no confundir el pico de memoria con una copia de
  preparación de la fixture.
- Source SDK Base 2013 Singleplayer, `hl2`, con host nativo oculto: 12.315 assets
  sin truncamiento y `materials/brick/brickfloor001a.vtf` dentro de VPK
  (174.968 bytes). Se comprueban lectura, preview, cancelación y reconexión.

Cada escenario usa un proceso nuevo. Hay 15 muestras y un calentamiento por
métrica; primera apertura y ciclos de vida se observan una vez y se identifican
como tales en JSON. Son mediciones con cachés de disco/componentes calientes,
sin vaciar las cachés del sistema. El p95 usa el rango superior más cercano:
con 15 muestras coincide con el máximo. No es un intervalo de confianza.

El banco valida resultados: número de filas, búsquedas, texto conservado,
dimensiones, imágenes decodificadas, longitud RAW, cancelación, limpieza y salida
de procesos. No se aceptan ejecuciones fallidas como muestras correctas. CTest
comprueba comportamiento; los tiempos dependen de la máquina y no bloquean CI.

## Comparación de los costes corregidos

Valores en milisegundos. «Antes» procede del banco anterior a las correcciones;
«después», de la ejecución aceptada. Los casos de ciclo de vida son observaciones
individuales, no distribuciones estadísticas.

| Operación | Antes | Después | Alcance |
| --- | ---: | ---: | --- |
| Ordenar por tipo, 150.000 assets | 200,16 | 20,07 | Modelo Qt |
| Refrescar filtro vacío con ordenación activa | 247,97 | 31,80 | Modelo Qt |
| Búsqueda amplia con ordenación activa | 165,57 | 29,36 | Modelo Qt |
| Preview VTF 4K, límite de 512 px | 586,75 | 124,09 | Panel real, lectura y entrega incluidas |
| Miniatura VTF 4K, límite de 128 px | 589,42 | 123,88 | Servicio real, sin caché de imagen |
| Metadatos VTF en VPK | 47,04 | 31,04 | Backend real, dos prefijos |
| Lectura VPK y decodificación a 128 px | 31,26 | 15,53 | Backend real |
| Cerrar backend tras cancelar un escaneo activo | 1.636,89 | 127,99 | Una observación por versión |

Se eliminaron las llamadas a `tolower` por carácter y por comparación: el
índice ya conserva `path_lower`. El orden por nombre se restaura con rangos
enteros, coherentes con la ordenación inicial y los filtros. Las regresiones
comprueban columnas, rutas UTF-8 y selección persistente.

Las lecturas locales reservan el tamaño una vez y usan `read()` en bloque.
La comparación conserva archivos vacíos, bytes binarios y rutas Unicode; una
lectura incompleta no se presenta como un archivo válido. La exclusión anterior
a cargar bytes y la selección de mip siguen activas.

Los avisos de directorio reducen esperas de respuestas IPC. La cancelación
publica `.cancel`; los escaneos y sus salidas la consultan cada 5 ms entre llamadas
al motor. Los errores se publican completos mediante renombrado. Los tokens
cancelados se conservan hasta su confirmación o la salida del proceso, para
limpiar también `.part` y `.cancel` sin perder una cancelación encolada.

El renderer recibe QUIT y 100 ms de gracia. Si las DLL del juego siguen
cerrándose, se usa la terminación del proceso que ya existía como alternativa.
El frontend conserva los documentos y termina sus escrituras y workers antes;
se espera la salida del hijo antes de reutilizar el host. Esta política evita
esperar un segundo por limpieza de un helper que solo posee estado de preview.

## Recorridos completos observados

| Recorrido | Mediana ms | p95 ms |
| --- | ---: | ---: |
| Abrir el navegador desde pack, 150.000 assets | 248,29 | Una observación |
| Buscar con tabla visible y ordenación por tipo | 201,54 | 223,37 |
| Buscar con cuadrícula visible y ordenación por tipo | 204,48 | 221,99 |
| Guardar VMT de 2.048 parámetros | 2,31 | 2,75 |
| Escribir su preview atómica, sin recarga del motor | 3,30 | 5,03 |
| Escribir preview y recibir ACK de recarga en Source | 15,35 | 16,15 |

Las búsquedas visibles incluyen los 150 ms de debounce del producto. Los
tiempos menores del modelo de datos no equivalen a esa latencia completa.
El pack de la fixture contiene el índice sin comprimir, un formato admitido por
el producto; la lectura también valida su checksum y el parser.

| Parámetros del VMT | Primera apertura/render ms | Apertura posterior ms | Texto → parser/inspector ms | Edición de panel ms |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 19,17 | 4,11 | 0,07 | 3,79 |
| 512 | 49,39 | 8,86 | 0,39 | 8,71 |
| 2.048 | 194,35 | 25,93 | 2,13 | 25,97 |

La primera apertura es una observación; el resto son medianas. No hubo cambios
de algoritmo en ingestión/parser: la lectura y parseo del pack de 150.000 filas
se midieron en 184,79 ms, y la búsqueda amplia por defecto en 12,04 ms. Los JSON
conservan también esos controles y su variación entre pasadas.

## Memoria y recursos

`working_set_mib` es memoria residente; `private_mib` es memoria privada
comprometida. `peak_working_set_mib` es el máximo acumulado de ese proceso hasta
la fila medida. No se suman picos de procesos tomados en instantes distintos.

| Escenario Release | Observación |
| --- | --- |
| Metadatos del VTF de 85,3 MiB | 0,14 ms; pico de working set 16,42 MiB |
| Apertura VTF de sesión | 0,12 ms; pico 16,59 MiB |
| Carga/preview/miniaturas VTF 4K | Pico acumulado 193,91 MiB; working set final 22,76 MiB |
| Catálogo de 150.000 assets y navegador | Pico acumulado 145,10 MiB; navegador con unos 72,29 MiB residentes al final |
| Renderer Source, catálogo real | Pico 89,21 MiB |
| Consultas de metadatos repetidas | Crecimiento observado de handles nativos: 0 |

La memoria del escenario incluye el propio banco y sus preparaciones. No se mide
VRAM ni FPS. La recarga de Source se cronometra hasta su ACK; no hasta presentar
un nuevo frame de GPU. La exclusión limita las cargas simultáneas, y no establece
un presupuesto nuevo de bytes para una textura individual.

## Verificación funcional

- Aplicación x64 y banco manual compilados en Release/Debug; renderer Win32
  compilado en ambas configuraciones. Pasan los 8 objetivos CTest en ambas.
- El grupo Qt de cargas tiene 12 casos y 100 comprobaciones. Incluye un fallo
  reproducido antes del arreglo: al cerrar una carga activa, la última referencia
  destruía objetos de entrega en un worker. `deleteLater()` devuelve esa
  destrucción al hilo de interfaz y la misma regresión pasa después del cambio.
- Un ZIP de 8.192 entradas vacías se genera para observar un extractor real vivo,
  cancelar su trabajo y comprobar que termina sin preparar/aplicar una actualización.
- Source pasa en Release y Debug: VPK, error con respuestas limpias, cancelación
  antes de ejecutar un trabajo, cancelación activa confirmada por el backend,
  consulta posterior, cierre con otro escaneo activo, reconexión y cancelación
  de una inicialización después de observar un hijo vivo. No quedan esos hijos.
- La primera matriz con `scratch/perf05-final-release` abortó al reemplazar el
  VMT durante `save_atomic`; se conserva el registro y su manifiesto se marca
  incompleto. El diagnóstico aislado repitió el escenario 30 veces correctamente.
  La escritura atómica compartida se mantuvo; el banco incorpora el detalle de
  error de Windows y protege los informes anteriores al repetir una ejecución.

Las pruebas manuales completas del editor, las demás ramas de Source, la
actualización instalada y la comprobación en una máquina limpia siguen en el
plan de publicación. No se ha creado un paquete, commit, push ni release.

## Reproducción y evidencia

Desde el repositorio, con CMake y Qt disponibles, construir primero el renderer
y después la aplicación/banco. Copiar el backend explícitamente también cubre
el caso en que la aplicación no necesita reenlazarse y su POST_BUILD no se ejecuta:

```powershell
cmake --build build --config Release --target hlmv_backend
cmake --build build64 --config Release --target vmtstudio performance_benchmarks
Copy-Item -LiteralPath build/bin/Release/hlmv_backend.exe -Destination build64/bin/Release/hlmv_backend.exe
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/measure_performance.ps1 `
  -OutputDirectory scratch/performance-new-run `
  -BinDirectory '<SOURCE_SDK_BASE_INSTALL>/bin' `
  -GameDirectory '<SOURCE_SDK_BASE_INSTALL>/hl2'
```

Omitir los dos directorios de juego ejecuta solo las fixtures. Para Debug,
usar esa configuración en los builds y en ambas rutas de la copia; pasar
`-Configuration Debug` al script. El banco queda fuera de `ALL_BUILD` y no se
añade a CTest ni al paquete. El script exige un directorio de resultados nuevo
y solo marca `completed: true` después de completar todos sus escenarios.

Evidencia local:

- Antes: `scratch/perf05-baseline/*.json` y `scratch/perf05-engine-baseline.json`.
- Pasada aceptada: `scratch/perf05-acceptance-release/*.json`, con manifiesto
  completo, hashes de código/banco/backend y muestras de los escenarios.
- Backend Debug: `scratch/perf05-engine-debug.json`.
- Builds y suites: `scratch/perf05-build*.log`, `scratch/perf05-tests-*.log`.
- Regresión Qt: `scratch/perf05-delivery-before.log` y
  `scratch/perf05-delivery-{release,debug}.log`.

La implementación se apoya en las reglas de
[esperas y mensajes de Windows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjects)
y la [destrucción diferida de QObject](https://doc.qt.io/qt-6/qobject.html#deleteLater).
