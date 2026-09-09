# OBS Box Layouts

> **Windows:** usa la versión 0.7.3 o posterior. Incluye un renderizador compatible con Direct3D para bordes,
> colores y esquinas redondeadas, además de las correcciones de inicialización gráfica,
> el renderizado de fuentes dentro de los boxes y el cierre seguro de la ventana **Interactuar**.

Plugin nativo para OBS Studio que crea composiciones multicaja al estilo de los *layers* de vMix. Se añade a una
escena como una fuente nueva llamada **Layout de boxes**.

## Funcionalidad incluida en el MVP

- Ocho distribuciones: una caja, dos columnas, dos filas, tres columnas, principal a la izquierda, grilla 2 × 2,
  principal arriba y grilla 3 × 2.
- Modo **Personalizado** con posición X/Y, ancho, alto, orden de capas y hasta seis boxes.
- Edición directa desde **Interactuar** sobre cualquier preset: arrastrar para mover, `Shift` + arrastrar para
  redimensionar, `Cmd/Ctrl` + arrastrar para mover el contenido, rueda para zoom y doble clic para restablecer.
- Dock **Box Layouts – Monitor de medios**: detecta únicamente el layout que está en Program y muestra el progreso,
  tiempo restante, estado, alertas de final y controles de las fuentes multimedia activas, incluso dentro de escenas
  anidadas.
- Dock **Box Layouts – Control en vivo**: detecta automáticamente los layouts de la escena en Program y permite mover
  y redimensionar boxes, corregir el encuadre, ajustar zoom, usar movimiento fino y deshacer sin abrir propiedades.
- Dock **Box Layouts – Playlist**: crea una cola independiente por box con cualquier fuente o escena ya cargada en
  OBS, navegación manual y avance automático para videos, imágenes y escenas.
- Atajos globales para recorrer o seleccionar directamente las primeras seis fuentes de la playlist de cada box,
  compatibles con teclados, controladores y Stream Deck.
- Bloqueo opcional de proporción al redimensionar desde una esquina.
- Hasta seis boxes en un único layout.
- Cada box puede mostrar cualquier fuente o escena con video ya cargada en OBS.
- Encuadre automático tipo `cover`: la fuente llena el box sin deformarse.
- Zoom y desplazamiento horizontal/vertical por box. El shader recorta siempre al marco, por lo que el contenido no
  puede desbordarse.
- Radio de esquina, grosor y color de borde independientes para cada box.
- Separación configurable entre boxes.
- Fondo de color y una fuente de fondo opcional; sirve con imágenes, videos o escenas.
- Mezcla de audio de las fuentes activas. Si la misma fuente se usa en varios boxes, se mezcla una sola vez.
- Detección de referencias recursivas entre layouts y escenas.
- Interfaz en español e inglés.

## Uso

1. Instala el plugin y abre OBS Studio.
2. En una escena, pulsa **+** en el panel Fuentes y elige **Layout de boxes**.
3. Selecciona la distribución y el tamaño del lienzo.
4. En cada sección **Box**, elige una fuente o escena.
5. Ajusta **Zoom**, **Posición horizontal** y **Posición vertical**. Los valores de posición recorren únicamente el
   área disponible del contenido, sin revelar espacio fuera de la fuente.
6. Configura bordes, redondeo, separación y fondo.

Las fuentes que quieras utilizar deben existir antes de abrir las propiedades del layout. Si agregas una fuente nueva,
cierra y vuelve a abrir las propiedades para refrescar la lista.

## Compilación

El proyecto parte de la [plantilla oficial de plugins de OBS](https://github.com/obsproject/obs-plugintemplate) y fija
el SDK de OBS Studio 32.0.3 y Qt 6.8.3.

### macOS

Requiere Xcode completo 16 o posterior y CMake 3.30.5 o posterior:

```sh
cmake --preset macos
cmake --build --preset macos
```

### Windows x64

Requiere Visual Studio 2022 y CMake 3.30.5 o posterior:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

### Ubuntu 24.04

Requiere Ninja, `build-essential` y `pkg-config`:

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

Los workflows incluidos en `.github/workflows` pueden generar artefactos para los tres sistemas desde GitHub Actions.
También generan un ZIP portable y un instalador `.exe` para todos los usuarios de Windows. Consulta
[`DISTRIBUTION.md`](DISTRIBUTION.md) para publicar versiones y trasladar colecciones de escenas.

## Estructura

- `src/box-layout.c`: fuente compuesta, presets, propiedades, render y audio.
- `src/media-dock.cpp`: monitor Qt de fuentes multimedia activas en Program.
- `src/live-control-dock.cpp`: editor Qt seguro de los layouts que están en Program.
- `src/playlist-dock.cpp`: colas y automatización de fuentes por box.
- `data/box-layout.effect`: shader de recorte, borde y esquinas redondeadas.
- `data/locale/`: traducciones.
- `buildspec.json`: nombre, versión y dependencias del plugin.

## Edición visual del modo Personalizado

Asigna las fuentes y pulsa **ABRIR EDITOR VISUAL (ARRASTRE Y ZOOM)** al comienzo de las propiedades. También puedes
hacer clic derecho sobre **Layout de boxes** y elegir **Interactuar**. La edición interna no funciona sobre el lienzo
principal de OBS, porque allí OBS reserva el arrastre para transformar la fuente completa. Funciona
con cualquier preset: al mover o redimensionar por primera vez, el preset se convierte automáticamente en un layout
Personalizado sin cambiar su aspecto inicial. Dentro de esa ventana:

- Arrastra el interior de un box para moverlo.
- Usa `Shift` + arrastrar desde cualquier punto para cambiar el tamaño de forma sencilla.
- También puedes arrastrar directamente un borde o una esquina.
- Usa `Cmd/Ctrl` + arrastrar para desplazar el contenido dentro del box.
- Usa la rueda del mouse sobre un box para ajustar el zoom del contenido.
- Haz doble clic para restablecer zoom y desplazamiento.

La posición queda guardada automáticamente en la colección de escenas. También puede editarse numéricamente desde las
propiedades de cada box.

## Monitor de medios en Program

El dock se abre automáticamente después de instalar la versión 0.3.0. Si lo cierras, puedes recuperarlo desde
**Paneles → Box Layouts – Monitor de medios**.

- Solo busca videos dentro de una instancia de **Layout de boxes** que forme parte de la escena actualmente en Program.
- En modo estudio, las fuentes que están únicamente en Preview no aparecen ni se controlan.
- Detecta fuentes multimedia dentro de escenas anidadas usadas en los boxes.
- La barra permite consultar y cambiar la posición del video.
- El panel cambia a amarillo cuando faltan 30 segundos, naranja a los 10 y rojo durante los últimos 5 segundos.
- Los botones permiten reproducir/pausar, reiniciar y detener.
- Cada video permite activar **Al finalizar** y elegir una escena destino. El cambio solo se ejecuta si el video termina
  mientras ese layout continúa en Program; en modo estudio usa la transición seleccionada en OBS.
- Si una fuente tiene **Bucle** activado, el monitor detecta el salto del final al inicio como una finalización y también
  ejecuta la acción configurada.

El monitor utiliza la API de medios controlables de OBS. Las fuentes multimedia y listas VLC son compatibles; un video
reproducido dentro de una fuente Navegador no expone necesariamente estos controles a OBS.

## Control en vivo

El segundo dock se abre automáticamente y también está disponible en **Paneles → Box Layouts – Control en vivo**.
Solo muestra instancias de **Layout de boxes** presentes en la escena que está en Program; si hay más de una, permite
elegir cuál editar.

Por seguridad, la edición comienza bloqueada y vuelve a bloquearse cada vez que cambia la escena en Program. Después de
activar **Desbloquear edición en Program**:

- Arrastra dentro de un box para moverlo.
- Arrastra un borde o una esquina para cambiar su tamaño; `Shift` permite redimensionar desde cualquier punto.
- Usa `Cmd/Ctrl` + arrastrar, la rueda o los deslizadores para corregir el encuadre y el zoom del contenido.
- Usa las flechas con pasos de 1 o 10 píxeles para realizar ajustes finos.
- Haz doble clic para restablecer el encuadre y **Deshacer** para recuperar el estado anterior.

Los cambios son visibles inmediatamente en la salida y quedan guardados en la colección de escenas de OBS.

## Playlist por box

Abre **Paneles → Box Layouts – Playlist**. El panel sigue la escena que está en Program y guarda una lista separada
para cada box de cada instancia de **Layout de boxes**.

1. Elige el box que quieras controlar.
2. Selecciona una fuente o escena existente y pulsa **Agregar**. Puedes repetir una misma fuente y reordenar la cola.
3. Selecciona un elemento y pulsa **PONER AL AIRE**, o haz doble clic sobre él.
4. Usa **Anterior** y **Siguiente** durante la emisión.
5. Activa **Avanzar automáticamente** para pasar al siguiente elemento cuando termine un video. Para imágenes o
   escenas sin duración propia se usa el tiempo configurado en **Duración de imágenes/escenas**.
6. Activa **Repetir la lista** si quieres volver al primer elemento después del último.

La barra inferior muestra el progreso del video o la cuenta regresiva de una imagen. Al seleccionar un video, el
plugin lo reinicia desde el comienzo. Si una fuente de video tiene su propio bucle activado, el salto al inicio también
se interpreta como final para poder avanzar en la cola. Las fuentes deben existir previamente en OBS; si una se elimina,
el elemento queda marcado con una advertencia y puede quitarse o sustituirse.

Las acciones **Al finalizar** del Monitor de medios siguen siendo independientes. Para evitar dos acciones simultáneas,
conviene desactivarlas en los videos intermedios de una playlist automática.

## Atajos y Stream Deck

La versión 0.7.0 registra acciones globales para los seis boxes. En **OBS → Ajustes → Atajos**, busca
`Box Layouts`. Para cada box encontrarás:

- **Fuente anterior** y **Fuente siguiente**, que recorren su playlist de forma circular.
- **Poner fuente 1–6**, que salta directamente a una de las primeras seis posiciones de la playlist.

Los atajos solo modifican una instancia de **Layout de boxes** que forme parte de la escena actualmente en Program. Si
hay varias instancias en Program, usan la que esté seleccionada en el panel Playlist. Un box sin lista o una posición
que no exista se ignora de forma segura.

Para Stream Deck, asigna primero combinaciones libres en los Atajos de OBS. Después añade una acción
**Sistema → Atajo** en Stream Deck y graba la misma combinación. Por ejemplo, puedes reservar botones para
`Speaker anterior`, `Speaker siguiente`, `Cámara 1`, `Cámara 2`, etc. En macOS puede ser necesario autorizar Stream Deck
en **Ajustes del Sistema → Privacidad y seguridad → Accesibilidad** para que envíe las teclas a OBS.

## Licencia

GPL-2.0, igual que la plantilla oficial y OBS Studio.
