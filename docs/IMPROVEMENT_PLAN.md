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
- [ ] CACHE-02: revisar reemplazo y coordinación entre instancias del archivo
  cache.pack; es reconstruible y conserva su implementación previa.
- [ ] PERF-01: actualizar valores del inspector sin recrear todos los widgets
  en cada pulsación.
- [x] PERF-02: elegir un mip adecuado antes de decodificar miniaturas VTF.
- [ ] PERF-03: leer únicamente la cabecera necesaria para metadatos VTF y
  limitar memoria en vuelo.
- [ ] PERF-04: mover descubrimiento, exportación y extracción de actualizaciones
  a trabajos cancelables que no bloqueen la interfaz. Incluir cancelación de
  consultas al cambiar de juego/cerrar: actualmente se drenan antes de destruir
  el motor y pueden esperar a que finalice una operación.
- [ ] PERF-05: medir tiempos de apertura, escritura, preview, búsqueda y memoria
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

En esta etapa se revisan diffs, recursos, referencias CMake y contratos entre
módulos sin compilar. Las pruebas de C++ añadidas quedan pendientes de ejecución.
Antes de publicar una versión deberán completarse:

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
