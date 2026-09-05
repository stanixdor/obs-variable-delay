# Test coverage / Cobertura de pruebas

## English

Run the six SDK-free suites from the repository root (C++20 compiler and CMake required):

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

`preview_timing_tests` exercises eight scenarios in the timing helpers used by the production GPU
preview, including the shared priority **Streaming → Multistream → Recording** in every session order and a
paused recording alongside an active Multistream output. `audio_spsc_fifo_tests` exercises the production PCM queue.
`packet_delay_tests` covers the independent packet-core model; it is not a
substitute for the production session tests.

`hold_pipeline_tests` compiles the production shared-encoder lifecycle against a
controlled libobs boundary. It covers compatible sharing, isolated layouts,
late subscribers, coordinated pause, concurrent unsubscribe, and failed-start
cleanup.

`multistream_transport_tests` is deliberately SDK-free despite its name: it tests the **production bounded packet
queue**, not sockets, TLS, FFmpeg, or librtmp. Its six scenarios cover keyframe/aligned-audio entry, immutable
payload sharing, independent slow-destination limits, media/wall-clock limits, B-frame/timestamp extremes, and
invalid packets/clock regression. Network and real-encoder tests are separate below.

## Español

Los comandos anteriores ejecutan las seis suites sin SDK desde la raíz del repositorio; requieren compilador
C++20 y CMake.
`output_session_tests` compila **el código real de producción** de
`src/output-session.cpp` y la gestión de referencias de `ObsPacket`. Sólo se
simulan las llamadas externas a libobs y el ciclo de los codificadores de
`HoldPipeline`. Los paquetes entran por el callback registrado por la sesión
real; se comprueban sus datos, timestamps y referencias. El tamaño simulado del
paquete permite alcanzar los límites de memoria sin reservar gigabytes.

Estas pruebas no certifican la compatibilidad de los bitstreams, el comportamiento
de la GPU ni los controladores del sistema: siguen siendo necesarias pruebas en
OBS real. `preview_timing_tests` comprueba ocho escenarios de los helpers utilizados por la preview GPU, incluida
la prioridad compartida **Streaming → Multistream → Recording** en todos los órdenes de sesión y una grabación
pausada junto a Multistream activo. `audio_spsc_fifo_tests` comprueba la cola PCM de producción. `packet_delay_tests`
comprueba un modelo separado y no sustituye las pruebas de la sesión real.

`hold_pipeline_tests` compila el ciclo real de los codificadores compartidos con
una frontera de libobs controlada. Comprueba la reutilización compatible, la
separación de configuraciones, los suscriptores tardíos, la pausa coordinada,
las bajas concurrentes y la limpieza tras un fallo de arranque.

Aunque su nombre incluya transporte, `multistream_transport_tests` no necesita SDK: prueba **la cola acotada de
paquetes de producción**, no sockets, TLS, FFmpeg ni librtmp. Sus seis escenarios cubren entrada con keyframe/audio
alineado, payloads inmutables compartidos, límites independientes de destinos lentos, límites de tiempo de medios
y pared, extremos de timestamps/B-frames y paquetes inválidos/reinicio del reloj. Las pruebas de red y encoders
reales se explican por separado abajo.

## Real integration / Integración real

### English

Configure the root project with a real OBS SDK, Qt, and `-DENABLE_OBS_INTEGRATION_TESTS=ON` to build
`preview_gpu_integration`, `output_session_integration`, `multistream_integration`, and `multistream_network_tests`.
Only `multistream_network_tests` is registered with CTest (25-second timeout). The three GPU/encoder drivers are
opt-in executables, not automatic CTest entries: they need a working GPU/backend and real encoder modules. They
start an isolated libobs instance with synthetic sources and do not load the user's OBS profiles.

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
`preview_gpu_integration`, `output_session_integration`, `multistream_integration` y `multistream_network_tests`.
Sólo `multistream_network_tests` se registra en CTest (timeout de 25 segundos). Los tres drivers de GPU/encoders
son ejecutables optativos, no entradas automáticas de CTest: necesitan GPU/backend funcional y módulos de encoder
reales. Arrancan libobs aislado con fuentes sintéticas y no cargan los perfiles OBS del usuario.

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

## Multistream 1.2.0: reproducible local tests / Pruebas locales reproducibles

### English

The following commands are for **macOS arm64**, OBS Studio **32.2.2** installed at `/Applications/OBS.app`, and the
repository's local Ninja preset. They do not install the plugin into OBS. Requires the bundled OBS/Qt dependencies,
Node.js, `ffmpeg` and `ffprobe` on `PATH`, and an available Qt TLS plugin for the negative-certificate fixture.
Run from the repository root, close the normal OBS application to avoid competing for GPU/encoder resources, and
use a new temporary artifact directory. Do not replace loopback endpoints with public channels or real keys.

```sh
cmake --preset macos-local-arm64 -DENABLE_OBS_INTEGRATION_TESTS=ON \
  -DMULTISTREAM_TEST_QT_PLUGINS="$PWD/.deps/obs-deps-qt6-2026-07-15-universal/plugins"
cmake --build --preset macos-local-arm64 --parallel \
  --target multistream_integration multistream_network_tests
ctest --test-dir build_macos_arm64 -R '^multistream_network_tests$' --output-on-failure

multistream_artifacts="$(mktemp -d /tmp/obs-multistream-check.XXXXXX)"
QT_PLUGIN_PATH="$PWD/.deps/obs-deps-qt6-2026-07-15-universal/plugins" \
  build_macos_arm64/multistream_integration /Applications/OBS.app \
  /Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib \
  "$multistream_artifacts" "$(command -v ffmpeg)"
node tests/verify-multistream.mjs "$multistream_artifacts"
```

To try another backend, replace only the graphics-module argument with
`/Applications/OBS.app/Contents/Frameworks/libobs-metal.dylib`, using another fresh directory. That backend must
initialize successfully in the isolated process; the successful 1.2.0 run described below used OpenGL, not Metal.
The multistream driver loads both x264 and VideoToolbox modules and currently assumes the macOS OBS bundle layout;
its runtime results do not certify Windows or Linux. Root builds on other platforms can still run the independent
network CTest with their appropriate Qt plugin path. Do not run two instances concurrently: the drivers reserve
fixed loopback ports (`19361–19365` for real RTMP, `19401–19405` for network-boundary fixtures).

#### What each layer establishes

- **Network boundary, `multistream_network_tests`:** compiles the real production transport and private librtmp,
  with real Qt TCP/TLS loopback fixtures. Seven cases check rejected input, a silent peer, stop returning in under
  100 ms/destruction under 1.5 s, an absolute handshake deadline despite slow trickle traffic, rejection of an
  untrusted RTMPS certificate before media publishing, terminal-error restart using the same ID, secret-safe
  diagnostics, and an IPv6 loopback endpoint (`[::1]`) without a URL path. The timeout assertions are test
  requirements, not universal performance guarantees. The test-only
  fault-injection macro is absent from the production transport target.
- **Real encoder/delay/RTMP path, `multistream_integration`:** creates isolated libobs and temporary profile/config
  data, uses synthetic red/blue/green scenes with matching 440/880/220 Hz audio, real H.264/AAC at `30000/1001` fps,
  and real `OutputSession`, hold pipeline, configured encoder creation, controller, and transport code. Only the
  OBS frontend boundary is controlled; it cannot load or publish to the user's native streaming output. FFmpeg
  receiver subprocesses listen only on `127.0.0.1` and remux received packets to FLV.
- The driver checks configuration fallback and compatible encoder sharing, three rapid cancel/rearm cycles, a
  two-second delay, Program changes, return to live, joining/reconnecting without stopping the healthy target,
  and a deliberately suspended receiver with a bounded queue while the master continues. Controller cases check
  save/reload, the eight-target limit, no autostart, Unix configuration permissions, cancellation during metadata
  startup, two destinations sharing one master, and teardown on last stop/profile/collection changes. A
  150-second watchdog fails a stuck run. These tests do not open the native dock or exercise mouse/keyboard UI.
- **Bitstream/content verifier, `verify-multistream.mjs`:** invokes `ffprobe` and full `ffmpeg` decoding. It checks
  H.264/AAC, IDR-first connections with independent zero-based DTS epochs, strictly increasing per-track DTS, and
  identical encoded-payload hashes across the shared destinations. It also invokes `verify-recording.mjs` for
  scene colors, non-silent matching tones, and bounded A/V transitions in the reference/controller recordings.
  A driver PASS alone is not a substitute for this verifier.

Artifacts include `multistream-metrics.txt`, `controller-metrics.txt`, and loopback FLV files (`fast.flv`,
`secondary-before-reconnect.flv`, `secondary-after-reconnect.flv`, `slow.flv`, `controller-hardware.flv`,
`controller-second.flv`, `profile-stop.flv`, and `collection-stop.flv`). Keep logs together with the artifact
directory and the exact commit/commands. Runtime data is
temporary or in the chosen directory, not in the user's OBS profiles.

The network source embeds a **public, test-only self-signed certificate and private key**. They are intentional
fixtures, never real credentials and never installed as trust anchors. The negative certificate test does not
establish a successful connection to every commercial RTMPS provider. No public streaming service is contacted.

Observed local scope during 1.2.0 development: the root CTest run passed seven suites (six SDK-free suites plus
the real-network executable). The seven real-network cases passed on macOS arm64 with ASan/UBSan and separately
with TSan; those sanitizer builds instrumented production transport and vendored librtmp/happy-eyeballs
C, not the external prebuilt Qt/FFmpeg libraries. The six SDK-free suites also passed with ASan/UBSan; the queue
and preview executables contain six and eight scenarios respectively. A macOS OBS 32.2.2/OpenGL run passed all
eight FLV files through decoding/content verification, including x264 at `30000/1001` fps and two real
VideoToolbox-controller destinations, asynchronous metadata startup/cancellation, shared-session teardown, and
profile/collection stops. A suspended receiver forced retry while the healthy stream continued; its observed
maximum queue was 308,460 bytes. The separate Metal attempt failed at `obs_reset_video` before the plugin workflow,
so no Metal 1.2.0 runtime pass is claimed. Subsequent
changes and final platform packages must be revalidated separately; see the [release validation](../docs/releases/1.2.0.md#validation).

### Español

Los comandos siguientes son para **macOS arm64**, OBS Studio **32.2.2** instalado en `/Applications/OBS.app` y el
preset Ninja local del repositorio. No instalan el plugin en OBS. Requieren las dependencias OBS/Qt, Node.js,
`ffmpeg` y `ffprobe` en `PATH` y un plugin TLS de Qt para el caso de certificado no fiable. Ejecútalos desde la raíz
del repo, cierra OBS normal para no competir por GPU/encoders y usa un directorio temporal nuevo. No sustituyas
destinos loopback por canales públicos ni claves reales.

```sh
cmake --preset macos-local-arm64 -DENABLE_OBS_INTEGRATION_TESTS=ON \
  -DMULTISTREAM_TEST_QT_PLUGINS="$PWD/.deps/obs-deps-qt6-2026-07-15-universal/plugins"
cmake --build --preset macos-local-arm64 --parallel \
  --target multistream_integration multistream_network_tests
ctest --test-dir build_macos_arm64 -R '^multistream_network_tests$' --output-on-failure

multistream_artifacts="$(mktemp -d /tmp/obs-multistream-check.XXXXXX)"
QT_PLUGIN_PATH="$PWD/.deps/obs-deps-qt6-2026-07-15-universal/plugins" \
  build_macos_arm64/multistream_integration /Applications/OBS.app \
  /Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib \
  "$multistream_artifacts" "$(command -v ffmpeg)"
node tests/verify-multistream.mjs "$multistream_artifacts"
```

Para probar otro backend, sustituye sólo el argumento del módulo gráfico por
`/Applications/OBS.app/Contents/Frameworks/libobs-metal.dylib`, usando otro directorio nuevo. El backend debe poder
inicializarse en el proceso aislado; la ejecución 1.2.0 correcta descrita abajo usó OpenGL, no Metal. El driver carga los
módulos x264 y VideoToolbox y actualmente asume el layout del bundle OBS de macOS; sus resultados runtime no
certifican Windows ni Linux. Los builds raíz en otras plataformas pueden ejecutar el CTest de red independiente
con su ruta de plugins Qt. No ejecutes dos instancias a la vez: los drivers reservan puertos loopback fijos
(`19361–19365` para RTMP real y `19401–19405` para las pruebas de frontera de red).

#### Qué demuestra cada nivel

- **Frontera de red, `multistream_network_tests`:** compila el transporte de producción y librtmp privado reales,
  con fixtures TCP/TLS Qt loopback reales. Siete casos comprueban entradas rechazadas, un peer silencioso, stop en
  menos de 100 ms/destrucción en menos de 1,5 s, deadline absoluto aunque lleguen bytes lentamente, rechazo de un
  certificado RTMPS no fiable antes de emitir, reinicio del mismo ID tras error terminal, diagnóstico sin claves
  y un destino IPv6 loopback (`[::1]`) sin ruta en la URL.
  Los límites temporales son condiciones del test, no garantías universales de rendimiento. La macro de inyección
  de fallo es exclusiva del test y no está en el target del transporte de producción.
- **Flujo real encoder/delay/RTMP, `multistream_integration`:** crea libobs aislado y datos temporales de
  perfil/configuración; usa escenas rojas/azules/verdes con audio 440/880/220 Hz, H.264/AAC real a `30000/1001` fps y
  el código real de `OutputSession`, espera, creación de encoders configurados, controlador y transporte. Sólo se
  controla la frontera frontend de OBS; no puede cargar ni emitir al stream nativo del usuario. Los procesos
  receptores FFmpeg escuchan únicamente en `127.0.0.1` y remultiplexan los paquetes recibidos a FLV.
- El driver comprueba fallback de configuración y encoders compatibles compartidos, tres ciclos rápidos de
  cancelar/rearmar, delay de dos segundos, cambios de Program, vuelta a live, entrada/reconexión sin detener el
  destino sano y un receptor suspendido con cola acotada mientras continúa el maestro. Los casos del controlador
  comprueban guardar/recargar, límite de ocho destinos, no autostart, permisos Unix, cancelación durante metadatos,
  dos destinos compartiendo un maestro y liberación al parar el último/cambiar perfil/colección. Un watchdog de
  150 segundos hace fallar un bloqueo. No abre el panel nativo ni ejercita la UI con ratón/teclado.
- **Verificador de contenido/bitstream, `verify-multistream.mjs`:** ejecuta `ffprobe` y decodificación completa con
  `ffmpeg`. Comprueba H.264/AAC, conexiones que empiezan en IDR con épocas DTS independientes desde cero, DTS
  estrictamente crecientes por pista y hashes de payload idénticos entre destinos compartidos. También llama a
  `verify-recording.mjs` para comprobar colores, tonos no silenciosos correspondientes y transiciones A/V acotadas
  en las grabaciones de referencia/controlador. Un PASS del driver no sustituye este verificador.

Los resultados incluyen `multistream-metrics.txt`, `controller-metrics.txt` y FLV loopback (`fast.flv`,
`secondary-before-reconnect.flv`, `secondary-after-reconnect.flv`, `slow.flv`, `controller-hardware.flv`,
`controller-second.flv`, `profile-stop.flv` y `collection-stop.flv`). Conserva los logs, el directorio de resultados
y el commit/comandos exactos. Los datos runtime son
temporales o están en el directorio elegido, no en los perfiles OBS del usuario.

El código de red contiene un **certificado autofirmado y una clave privada públicos, sólo de prueba**. Son fixtures
intencionales, nunca credenciales reales ni autoridades de confianza instaladas. El test de certificado negativo
no demuestra una conexión válida con todos los proveedores RTMPS comerciales. No se contacta con servicios de
streaming públicos.

Alcance local observado durante el desarrollo 1.2.0: el CTest raíz pasó siete suites (seis sin SDK y el ejecutable
de red real). Los siete casos de red pasaron en macOS arm64 con ASan/UBSan y por separado con TSan; esos
builds instrumentaron el transporte de producción y el C vendorizado de librtmp/happy-eyeballs, no las bibliotecas
externas precompiladas Qt/FFmpeg. Las seis suites sin SDK también pasaron con ASan/UBSan; los ejecutables de cola
y preview contienen seis y ocho escenarios respectivamente. Una ejecución en OBS 32.2.2/macOS/OpenGL pasó la
verificación de decodificación/contenido de los ocho FLV, incluidos x264 a `30000/1001` fps y dos destinos del
controlador VideoToolbox real, arranque/cancelación de metadatos asíncronos, liberación de sesión compartida y
paradas por perfil/colección. Suspender un receptor forzó reconexión mientras el stream sano continuaba; su cola
máxima observada fue de 308.460 bytes. El intento Metal separado falló en `obs_reset_video`, antes del flujo del
plugin: no se declara un PASS runtime Metal 1.2.0. Los cambios posteriores y los paquetes finales deben validarse por separado; consulta
la [validación de la release](../docs/releases/1.2.0.md#validación).
