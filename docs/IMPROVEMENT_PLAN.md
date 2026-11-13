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

- [ ] SAFE-01: centralizar escritura y reemplazo atómicos; nunca borrar el
  destino cuando falla el reemplazo; usar temporales únicos.
- [ ] SAFE-02: comprobar diagnósticos del parser al determinar la validez de
  la sesión; permitir abrir y corregir archivos malformados sin guardarlos
  accidentalmente.
- [ ] SAFE-03: integrar `PreviewSync` en el flujo Qt y conservar la última
  revisión válida ante errores de escritura o recarga.
- [ ] LIFE-01: gestionar el fin de las tareas de inicialización y consulta
  antes de destruir el motor; desconectar viewport y docks al cambiar de juego.
- [ ] SAVE-01: guardar materiales de modelos en un destino permanente,
  conservando separada la copia temporal de previsualización.
- [ ] SAVE-02: conservar la jerarquía relativa al guardar en la carpeta del
  juego y confirmar reemplazos.
- [ ] PREVIEW-01: aislar directorios de previsualización por sesión y juego.
- [ ] PARSE-01: insertar y eliminar parámetros en VMT compactos sin alterar
  parámetros vecinos, comentarios o llaves.
- [ ] DIST-01: cargar los esquemas del inspector desde recursos portables,
  sin depender de rutas del equipo donde se compiló.
- [ ] VFS-01: comprobar existencia de assets dentro de VPK y rutas montadas
  desde el frontend x64, distinguiendo ausente de no comprobable.

## Entrega 2: fluidez y distribución

- [ ] CACHE-01: invalidar miniaturas en memoria al refrescar; cubrir cambios
  en las texturas referenciadas por un material.
- [ ] PERF-01: actualizar valores del inspector sin recrear todos los widgets
  en cada pulsación.
- [ ] PERF-02: elegir un mip adecuado antes de decodificar miniaturas VTF.
- [ ] PERF-03: leer únicamente la cabecera necesaria para metadatos VTF y
  limitar memoria en vuelo.
- [ ] PERF-04: mover descubrimiento, exportación y extracción de actualizaciones
  a trabajos cancelables que no bloqueen la interfaz.
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
