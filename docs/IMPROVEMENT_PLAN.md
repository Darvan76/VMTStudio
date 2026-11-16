# VMT Studio: plan de mejora

Este documento sigue el trabajo iniciado a partir de la revisión del 13 de
noviembre de 2026. Los cambios se entregan en commits separados en
`codex/stability-improvements`. Una casilla completada indica implementación
y revisión del cambio; la validación pendiente se registra expresamente.

## Restricciones de esta etapa

- No compilar ni generar paquetes salvo que resulte necesario para verificar
  un problema concreto.
- No publicar versiones ni activar despliegues.
- Conservar los cambios previos ajenos al trabajo, incluido
  `.github/workflows/ci.yml`.
- Añadir pruebas de regresión para los fallos de datos, parser e IPC. No
  presentar pruebas nuevas como ejecutadas hasta compilarlas y ejecutarlas.

## Entrega 1: fiabilidad y datos

- [x] SAFE-01: centralizar escritura y reemplazo atómicos de materiales, ajustes,
  previews y exportaciones; nunca borrar el destino cuando falla el reemplazo;
  usar temporales únicos.
- [x] SAFE-02: comprobar diagnósticos del parser al determinar la validez de
  la sesión; permitir abrir y corregir archivos malformados sin guardarlos
  accidentalmente.
- [x] SAFE-03: integrar `PreviewSync` en el flujo Qt y conservar la última
  revisión válida ante errores de escritura o recarga.
- [x] LIFE-01: gestionar el fin de las tareas de inicialización y consulta
  antes de destruir el motor; desconectar viewport y docks al cambiar de juego.
- [x] SAVE-01: guardar materiales de modelos en un destino permanente,
  conservando separada la copia temporal de previsualización.
- [x] SAVE-02: conservar la jerarquía relativa al guardar en la carpeta del
  juego y confirmar reemplazos.
- [x] PREVIEW-01: aislar directorios de previsualización por sesión y juego.
- [x] PARSE-01: insertar y eliminar parámetros en VMT compactos sin alterar
  parámetros vecinos, comentarios o llaves.
- [x] DIST-01: cargar los esquemas del inspector desde recursos portables,
  sin depender de rutas del equipo donde se compiló.
- [x] VFS-01: comprobar existencia de assets dentro de VPK y rutas montadas
  desde el frontend x64, distinguiendo ausente de no comprobable.

## Entrega 2: fluidez y distribución

- [x] CACHE-01: invalidar miniaturas en memoria al refrescar; cubrir cambios
  en las texturas referenciadas por un material.
- [x] CACHE-02: coordinar cache.pack entre instancias con propiedad exclusiva,
  conservar el archivo y el índice ante fallos de compactación y permitir que
  otras instancias trabajen en memoria mientras la caché está ocupada.
- [x] PERF-01: actualizar valores del inspector sin recrear todos los widgets
  en cada pulsación; conservar controles y estado al filtrar.
- [x] PERF-02: elegir un mip adecuado antes de decodificar miniaturas VTF.
- [x] PERF-03: leer únicamente la cabecera necesaria para metadatos VTF y
  limitar memoria en vuelo.
- [x] PERF-04: mover descubrimiento, exportación y extracción de actualizaciones
  a trabajos cancelables que no bloqueen la interfaz. Cancelar las esperas de
  consultas al cambiar de juego/cerrar y drenar los trabajos antes de destruir
  el motor.
- [x] PERF-05: medir tiempos de apertura, escritura, preview, búsqueda y memoria
  con proyectos grandes antes de introducir más optimizaciones.
- [ ] TEXT-01: conservar codificación, BOM y finales de línea durante la edición
  desde Qt.
- [ ] UPDATE-01: seleccionar explícitamente el paquete Release x64, verificar
  integridad y rutas antes de extraer, y permitir recuperación de una
  actualización fallida.
- [ ] FILE-01: detectar modificaciones externas antes de sobrescribir archivos
  abiertos.
- [ ] DIST-02: verificar el paquete en una máquina sin repositorio, herramientas
  de desarrollo ni rutas locales del autor.

## Entrega 3: edición intuitiva

- [ ] EDIT-01: recuperación automática tras cierres inesperados sin sobrescribir
  originales.
- [ ] EDIT-02: documentos en pestañas y restauración de sesiones de trabajo.
- [ ] EDIT-03: selectores de color y controles de vector y transformación UV;
  respetar tipos numéricos y precisión.
- [ ] EDIT-04: añadir, eliminar y restablecer parámetros; distinguir valores
  por defecto de valores presentes en el documento.
- [ ] EDIT-05: autocompletado de parámetros y texturas, números de línea,
  búsqueda/reemplazo y diagnósticos junto al texto.
- [ ] EDIT-06: comparación original/editado conservando cámara e iluminación.
- [ ] UX-01: unificar idioma, mensajes y estados de error/guardado.

## Entrega 4: materiales avanzados

- [ ] MAT-01: resolver y editar `Patch`, `include`, `insert` y `replace`, con
  límites de recursión y procedencia de valores heredados.
- [ ] MAT-02: editor de proxies y simulación de los independientes del juego;
  indicar explícitamente proxies que requieren estado de entidades.
- [ ] MAT-03: exportar VMT y dependencias VTF/herencia conservando rutas e
  informando de archivos faltantes.
- [ ] MAT-04: visor VTF con canales, zoom 1:1, mipmaps, fotogramas y caras de
  cubemap.
- [ ] MAT-05: edición por lotes con vista previa y posibilidad de deshacer.
- [ ] COMPAT-01: ampliar esquemas con parámetros verificados por rama del motor
  y una matriz de compatibilidad comprobada.
- [ ] ARCH-01: extraer gestión de documentos, motor, previews y guardado de
  `MainWindow` en componentes con responsabilidades explícitas.
- [ ] DOC-01: actualizar documentación y empaquetado a las capacidades realmente
  verificadas.

## Validación

El primer lote se revisó sin compilar. Las compilaciones y pruebas posteriores
se registran por entrega más abajo. Las suites automatizadas y los builds no
sustituyen los flujos completos en un juego o paquete instalado; antes de
publicar una versión deberán completarse:

- [ ] QA-01: ejecutar la suite de pruebas en x64 y las comprobaciones relevantes
  del backend Win32.
- [ ] QA-02: verificar el flujo Qt abrir, editar, deshacer, guardar y reabrir.
- [ ] QA-03: cambiar de juego y cerrar la aplicación durante cargas/consultas.
- [ ] QA-04: probar archivos bloqueados, fallos de reemplazo y destinos no
  escribibles conservando los originales.
- [ ] QA-05: probar VPK, dos instancias simultáneas y juegos con nombres de
  materiales coincidentes.
- [ ] QA-06: validar actualización, cancelación y recuperación con un paquete
  completo fuera del entorno de desarrollo.

## Registro de entregas

- Inicio: inventario y reparto entre tres subagentes; sin builds. La rama parte
  del commit `784518e`. El cambio previo del workflow queda fuera de las entregas.


### Primer lote implementado (sin build)

| Commit | Cambio |
| --- | --- |
| `2901011` | Plan y criterios de validación. |
| `757bb29` | Escritura atómica compartida y validez de sesiones basada en diagnósticos. |
| `68a6961` | Consulta de existencia a VFS, protocolo y regresiones. |
| `34c7df0` | Invalidación de imágenes decodificadas al refrescar. |
| `6406cdb` | Edición de VMT compactos sin borrar parámetros vecinos. |
| `c0f8a4e` | Ocho esquemas incorporados a recursos Qt y carga independiente del checkout. |
| `4b32dc5` | Trabajos del motor gestionados y drenados antes del cierre/reconexión. |
| `32e6a40` | Selección de mip apropiado para miniaturas VTF. |
| `9536b80` | Invalidación persistente en escaneos y protección frente a trabajos obsoletos. |
| `4c55808` | Preview protegida, carpetas aisladas, destinos permanentes y rutas de guardado. |

La integración posterior de previews y guardados permanentes utiliza una carpeta
temporal propia de cada ventana/juego, la protección `PreviewSync` y destinos
permanentes recordados para los materiales de modelos. Un rollback de render no
sustituye la copia permanente al reabrir un slot. Los errores de preview se
muestran aunque el guardado del archivo haya tenido éxito.

Comprobaciones realizadas:

- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check_source_integrity.ps1`:
  PASS. Los 100 archivos fuente directos
  declarados en CMake existen; todos los archivos de pruebas están registrados;
  los 10 recursos Qt existen e incluyen los 8 esquemas JSON.
- Revisión cruzada por subagentes de persistencia, parser/recursos e integración
  del motor/preview.
- Fixture VTF sintético de varios mips/fotogramas validado con la DLL VTFLib
  existente mediante `ctypes`; no se compiló un binario nuevo.
- 31 casos C++ nuevos: el árbol declara ahora 247 casos. **No ejecutados**.
- Sin builds, paquetes, releases ni ejecuciones de la aplicación Qt.

Antes de considerar este lote validado en ejecución siguen pendientes QA-01 a
QA-06. La comprobación de fuentes no demuestra compilación correcta ni sustituye
las pruebas del backend y la interfaz. PERF-03, PERF-04 y las funciones de las
entregas 3/4 continúan pendientes; no se ha publicado una versión del programa.

### CACHE-02: coordinación y reemplazo de la caché

Implementación local posterior a la limpieza de la auditoría:

- Un handle exclusivo de Windows sobre `cache.pack.lock` mantiene un único
  propietario durante lectura, escritura, recuperación y compactación. Se
  libera también al terminar inesperadamente el proceso; la existencia del
  archivo `.lock` por sí sola no bloquea la caché.
- La contención no espera ni modifica el archivo: la segunda instancia puede
  escanear y usar miniaturas en memoria, y vuelve a intentar abrir la caché al
  refrescar. No se añaden dependencias ni un sistema de transacciones.
- La compactación reemplaza el archivo en una operación y nunca elimina el
  original como alternativa si falla. La eliminación por etiquetas solo cambia
  el índice en memoria después de completar el reemplazo.
- El navegador conserva el `AssetPack` recibido del trabajo de carga/escaneo
  antes de pasarlo al servicio de miniaturas.

Validación del cambio:

- Aplicación x64 y suite completa Release: compiladas; 6 de 6 objetivos CTest
  pasan, incluidas dos nuevas regresiones de propiedad y compactación fallida.
- Aplicación x64 y suite completa Debug: compiladas; 6 de 6 objetivos CTest pasan.
- Prueba con dos procesos: el segundo no abre ni modifica el archivo ocupado;
  después de terminar abruptamente el propietario, un proceso nuevo abre el
  mismo archivo y recupera sus registros. PASS.
- Integridad de fuentes y recursos y comprobación de espacios del diff: PASS.
- Registros: `scratch/cache02-*.log`. Comprobación de procesos reproducible:
  `scratch/cache02_build_probe.cmd` y `scratch/cache02_process_check.py`.
- No se ha generado un paquete nuevo ni se ha hecho commit, push o publicación.

Las reglas nativas usadas están documentadas en Microsoft Learn:
[CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
y [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw).

### PERF-01: actualización incremental del inspector

- Los cambios de valores actualizan los controles existentes. Solo se
  reconstruyen las filas cuando cambia el shader o la estructura/metadatos del
  modelo: grupos, nombres, tipos, rangos, opciones o mensajes del esquema.
- Los filtros ocultan y muestran filas sin destruirlas; las filas ocultas
  también reciben las actualizaciones del documento.
- Se conserva el foco, el cursor, el desplazamiento y el texto parcial cuando
  el valor efectivo no cambia. Un cambio externo real, incluido deshacer,
  actualiza incluso el control enfocado.
- Las actualizaciones programáticas no generan ediciones de vuelta. Slider y
  campo numérico se sincronizan con una sola emisión por acción. Los valores
  enum desconocidos se muestran sin conservar una selección obsoleta.
- La edición desde el inspector y la selección de texturas actualizan su
  modelo bajo la protección de sincronización existente de MainWindow.

Validación:

- Aplicación x64 compilada en Release y Debug. Las suites completas pasan:
  **7 de 7 objetivos CTest en ambas configuraciones**.
- Nuevo objetivo `inspector_widget_tests`, con tres casos Qt offscreen y el
  harness existente, sin añadir QtTest ni otras dependencias. Comprueba
  identidad de widgets, foco/cursor/scroll, texto parcial, señales, controles
  numéricos, valores enum, presencia de parámetros, filtros y cambios de
  filas/shader/esquema.
- Integridad de fuentes y recursos: 98 entradas CMake y 10 recursos, incluidos
  los 8 esquemas. Comprobación de espacios del diff: PASS.
- Revisión cruzada del flujo completo de llamadas y de la reentrada de señales.
- Registros de compilación y ejecución: `scratch/perf01-*.log`.
- No se ha generado un paquete nuevo ni se ha hecho commit, push o publicación.

Las APIs de bloqueo de señales y visibilidad de filas usadas están documentadas
por Qt: [QSignalBlocker](https://doc.qt.io/qt-6/qsignalblocker.html) y
[QFormLayout::setRowVisible](https://doc.qt.io/qt-6/qformlayout.html#setRowVisible).

### PERF-03: cabeceras VTF y carga limitada

- Sesión, preview e información de materiales consultan metadatos sin cargar
  los píxeles. La lectura local solicita 16 bytes y después la cabecera
  declarada, validada contra el máximo de 336 bytes de la VTFLib incorporada.
  Conserva las rutas Unicode y la información de archivos legibles pero dañados.
- El protocolo `RAW_PREFIX` ofrece la misma lectura acotada desde las rutas
  montadas del motor: tamaño total de archivo y hasta 4096 bytes solicitados,
  con validación de límites y longitud en ambos extremos. Los consumidores de
  metadatos solicitan solo la cabecera, sin recurrir a una transferencia completa.
- Miniaturas y preview comparten una exclusión antes de cargar los bytes de
  origen. VTFLib libera el contenido cargado después de copiar el resultado.
  Este límite permite una carga/decodificación simultánea; el tamaño de una
  textura individual sigue sujeto a los límites existentes del lector/IPC.
- La preview conserva una solicitud pendiente y un resultado reciente. Las
  selecciones intermedias se sustituyen y los trabajos cancelados comprueban
  su generación antes de leer, también después de esperar al motor.

Validación:

- Aplicación x64 y suite completa compiladas en Release y Debug:
  **8 de 8 objetivos CTest pasan en ambas configuraciones**.
- Backend HLMV Win32 compilado en Release y Debug.
- Regresiones con los bytes de píxeles bloqueados, cabeceras VTF antiguas y
  con recursos, rutas Unicode, metadatos sin lectura completa, framing IPC,
  selección rápida, cancelación y exclusión entre preview y miniaturas.
- El nuevo objetivo Qt `asset_loading_tests` utiliza el harness existente,
  ejecución offscreen y un motor simulado; no añade dependencias.
- Integridad de fuentes y recursos: 105 entradas CMake y los 10 recursos,
  incluidos los 8 esquemas. Comprobación de espacios del diff: PASS.
- Registros: `scratch/perf03-*.log`. PERF-05 añade la lectura de un VPK real
  mediante Source SDK 2013/HL2; el resto del recorrido de publicación de QA-05
  permanece pendiente.
- No se ha generado un paquete nuevo ni se ha hecho commit, push o publicación.

### PERF-04: trabajos cancelables fuera de la interfaz

- El descubrimiento inicial y manual de juegos, incluidas las bibliotecas de
  Steam, se ejecuta con un catálogo propio en segundo plano. Recorre las carpetas
  comprobando cancelación; cerrar o cancelar descarta los resultados sin alterar
  las rutas recordadas. El botón de descubrimiento permite cancelar el escaneo.
- La exportación captura plan y opciones y realiza lecturas, creación de
  carpetas y escrituras atómicas en un trabajo propio. La interfaz muestra
  progreso y permite cancelar, sin llamar a `processEvents()` desde el bucle de
  exportación. Cerrar el diálogo cancela y espera su resultado de forma asíncrona
  antes de cerrarlo. Se conservan los recuentos y el respeto a archivos existentes.
- La preparación de actualizaciones ejecuta los extractores y las comprobaciones
  en un trabajo cancelable. Cancelar termina el extractor y una preparación
  cancelada nunca inicia el script ni cierra la aplicación. Los intentos utilizan
  carpetas de extracción únicas; los fallidos se limpian en el propio trabajo.
- El propietario puede cancelar la inicialización antes de adoptar el motor.
  Las consultas comprueban cancelación al sondear respuestas; la exportación
  dispone además de una bandera propia para su lectura actual. La desconexión
  retira el puntero compartido antes de drenar los trabajos, impidiendo que un
  trabajo en espera inicie otra consulta contra el motor anterior.
- La cancelación es cooperativa entre archivos y durante las esperas. Una
  escritura atómica ya iniciada termina; la confirmación nativa de un comando
  mantiene un límite de un segundo. PERF-05 amplía este flujo con cancelación
  cooperativa en el backend, limpieza de respuestas y un cierre acotado.

Validación:

- Aplicación x64 y pruebas compiladas en Release y Debug. Las suites completas
  pasan: **8 de 8 objetivos CTest en ambas configuraciones**.
- Siete regresiones nuevas sobre el harness existente: exportación con lectura
  detenida e interfaz activa, omisión de archivos existentes, cierre durante
  exportación, cancelación/reintento de preparación, cancelación de consultas y
  reconexión, descubrimiento Qt, catálogo cancelado e inicialización cancelada.
  La omisión de archivos existentes se comprueba dentro del caso de exportación.
- Los casos Qt se ejecutan offscreen. La extracción se detiene antes de iniciar
  un actualizador; no se ha aplicado una actualización real ni se ha cerrado la
  aplicación mediante el script durante las pruebas.
- Integridad: 111 entradas CMake, 10 recursos Qt y 8 esquemas. Diff sin errores
  de espacios. Registros: `scratch/perf04-*.log`.
- Siguen pendientes QA-03 y QA-06 con un juego y un paquete completos, así como
  la selección, integridad, validación previa de rutas y recuperación de UPDATE-01.
- No se ha generado un paquete nuevo ni se ha hecho commit, push o publicación.

### PERF-05: mediciones y correcciones guiadas por resultados

- Banco manual reproducible sobre las clases reales del producto, sin un
  framework nuevo ni umbrales de tiempo en CTest. Incluye el navegador completo
  con tabla/cuadrícula, editor e inspector, preview VTF y backend Win32 real.
- Matriz Release: catálogos de 10.000, 50.000 y 150.000 assets; VMT de 64, 512 y
  2.048 parámetros; VTF RGBA de 4096² con mips. Se registran mediana, p95, memoria
  privada, working set y pico por proceso, con 15 muestras y un calentamiento.
- El perfil detectó conversiones repetidas a minúsculas al ordenar y lecturas
  VTF byte a byte. Se reutilizan las claves normalizadas del índice y se leen los
  archivos en un bloque. El orden por nombre usa los rangos canónicos del índice.
- Las respuestas IPC despiertan por notificaciones nativas. Las marcas de
  cancelación llegan a los trabajos del backend, los errores se publican
  atómicamente y se limpian las respuestas y los recursos de consultas abandonadas.
- El renderer, que no posee documentos editados, recibe 100 ms de gracia tras
  QUIT y se confirma su salida antes de reutilizar el HWND. Se sirven mensajes
  nativos enviados durante esa espera, sin reentrar en la cola de eventos Qt.
- Una regresión nueva reprodujo la destrucción de entregas Qt desde un worker
  al cerrar cargas activas. Preview y miniaturas usan ahora `deleteLater()`;
  la misma prueba comprueba la destrucción efectiva en el hilo de interfaz.

Validación:

- Aplicación x64, pruebas y banco manual compilados en Release y Debug; backend
  Win32 compilado en ambas configuraciones. **8/8 objetivos CTest pasan en ambas**.
- El objetivo Qt de carga contiene 12 casos, incluidos cierre activo y
  cancelación de un extractor real sobre un ZIP generado para la prueba. No se
  aplica una actualización ni se ejecuta su script durante la comprobación.
- Source SDK 2013/HL2: 12.315 assets, sin truncamiento. Se verifican metadatos y
  decodificación desde VPK, cancelación previa y activa, limpieza tras errores,
  reconexión y cancelación de un arranque con proceso hijo vivo, en Release y Debug.
- La matriz aceptada tiene `completed: true` en
  `scratch/perf05-acceptance-release/manifest.json`, con hashes de código y binarios.
- Integridad final: 128 entradas CMake, 10 recursos Qt y 8 esquemas; el script
  del banco se analiza correctamente y el diff no presenta errores de espacios.
- Medianas observadas: ordenación por tipo 200 → 20 ms; filtrado ordenado del
  modelo 248 → 32 ms; preview VTF 4K 587 → 124 ms; cierre de backend cancelado
  1637 → 128 ms. El navegador completo tarda unos 202–204 ms en buscar, incluido
  su debounce de 150 ms y el renderizado.
- Informe, reproducción, memoria y límites: [PERFORMANCE_RESULTS.md](PERFORMANCE_RESULTS.md).
  Los recorridos manuales completos y la matriz de compatibilidad de publicación
  siguen separados de estas pruebas de rendimiento. No se ha generado un paquete
  nuevo ni se ha hecho commit, push o publicación.
