# Test coverage / Cobertura de pruebas

## English

Run every suite without the OBS SDK:

```sh
cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

`output_session_tests` compiles the **production** `src/output-session.cpp` and
`ObsPacket` ownership code. Only external libobs calls and `HoldPipeline` encoder
lifecycle are controlled fakes. Tests deliver packets through the callback that
the real session registers, inspect emitted payload identities and timestamps,
and count payload references. The fake payload size can represent a large memory
limit without allocating gigabytes. These tests do not certify codec bitstream
compatibility, GPU behavior, or operating-system encoder drivers; real OBS smoke
tests remain necessary.

`preview_timing_tests` exercises the timing helpers used by the production GPU
preview. `audio_spsc_fifo_tests` exercises the production PCM queue.
`packet_delay_tests` covers the independent packet-core model; it is not a
substitute for the production session tests.

`hold_pipeline_tests` compiles the production shared-encoder lifecycle against a
controlled libobs boundary. It covers compatible sharing, isolated layouts,
late subscribers, coordinated pause, concurrent unsubscribe, and failed-start
cleanup.

## Español

Los comandos anteriores ejecutan todas las suites sin necesitar el SDK de OBS.
`output_session_tests` compila **el código real de producción** de
`src/output-session.cpp` y la gestión de referencias de `ObsPacket`. Sólo se
simulan las llamadas externas a libobs y el ciclo de los codificadores de
`HoldPipeline`. Los paquetes entran por el callback registrado por la sesión
real; se comprueban sus datos, timestamps y referencias. El tamaño simulado del
paquete permite alcanzar los límites de memoria sin reservar gigabytes.

Estas pruebas no certifican la compatibilidad de los bitstreams, el comportamiento
de la GPU ni los controladores del sistema: siguen siendo necesarias pruebas en
OBS real. `preview_timing_tests` comprueba los helpers utilizados por la preview
GPU; `audio_spsc_fifo_tests`, la cola PCM de producción. `packet_delay_tests`
comprueba un modelo separado y no sustituye las pruebas de la sesión real.

`hold_pipeline_tests` compila el ciclo real de los codificadores compartidos con
una frontera de libobs controlada. Comprueba la reutilización compatible, la
separación de configuraciones, los suscriptores tardíos, la pausa coordinada,
las bajas concurrentes y la limpieza tras un fallo de arranque.

## Real integration / Integración real

### English

Configure the root project with a real OBS SDK, Qt, and `-DENABLE_OBS_INTEGRATION_TESTS=ON` to build
`preview_gpu_integration` and `output_session_integration`. These are opt-in executables, not automatic CTest
entries: they need a working GPU/backend and real encoder modules. They start an isolated libobs instance with
synthetic sources and do not load the user's OBS profiles.

The preview driver accepts a graphics-module path, an artifact directory, and optionally the libobs data path.
It checks real 320×180 thumbnails, orientation/colors, sampling cadence, resolution reset, long pause/history,
and lifecycle without keeping `obs_video_active()` true.

The recording driver currently loads modules from the macOS OBS application bundle. Example:

```sh
build/output_session_integration /Applications/OBS.app \
  /Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib \
  /tmp/obs-delay-recording-test obs_x264 1
node tests/verify-recording.mjs /tmp/obs-delay-recording-test/hybrid-mp4-integration.mp4
```

The last two optional arguments select encoder and FPS divisor; append `no-pause` to omit recording pause or
`native-pause` to test native OBS pause **without creating a plugin session or hold pipeline**. A 60-second
watchdog fails a stuck test. `verify-recording.mjs` requires Node.js, FFmpeg, and ffprobe: it verifies full decode,
DTS, scene colors, source-specific tones, and A/V transition offsets. See the release notes for observed results
and the native OBS 32.2.2 x264/divisor-2 pause issue; these drivers do not certify all OS/driver combinations.

### Español

Configura el proyecto raíz con SDK de OBS real, Qt y `-DENABLE_OBS_INTEGRATION_TESTS=ON` para construir
`preview_gpu_integration` y `output_session_integration`. Son ejecutables optativos, no entradas automáticas de
CTest: necesitan GPU/backend funcional y módulos de encoder reales. Arrancan libobs aislado con fuentes
sintéticas y no cargan los perfiles OBS del usuario.

El driver de preview recibe la ruta del módulo gráfico, un directorio de resultados y, opcionalmente, la ruta
de datos de libobs. Comprueba miniaturas 320×180 reales, orientación/colores, cadencia, cambio de resolución,
pausas largas/historial y ciclo de vida sin mantener `obs_video_active()` activo.

El driver de grabación carga actualmente los módulos del bundle de OBS para macOS; el ejemplo anterior es
ejecutable en ese sistema. Los dos últimos argumentos opcionales seleccionan encoder y divisor de FPS. Añade
`no-pause` para omitir la pausa o `native-pause` para comprobar la pausa nativa **sin crear una sesión del plugin
ni codificadores de espera**. Un watchdog de 60 segundos hace fallar una prueba bloqueada.
`verify-recording.mjs` necesita Node.js, FFmpeg y ffprobe: comprueba decodificación completa, DTS, colores de
escena, tonos específicos de cada fuente y desfases A/V en los cambios. Consulta las notas de release para los
resultados y el fallo de pausa nativo de OBS 32.2.2 con x264/divisor 2; no certifica todas las combinaciones de
sistema operativo y drivers.
