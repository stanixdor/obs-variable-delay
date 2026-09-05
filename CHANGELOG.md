# Changelog / Historial de cambios

[English](#english) · [Español](#español)

This project follows [Semantic Versioning](https://semver.org/) and the structure proposed by
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Este proyecto sigue [Versionado Semántico](https://semver.org/lang/es/) y la estructura propuesta por
[Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).

## English

### Unreleased

No additional changes listed.

### [1.2.0] — 2026-09-05

[Release page and downloads](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.2.0) ·
[Full release notes](docs/releases/1.2.0.md)

#### Added

- Integrated multistream dock for up to eight named RTMP/RTMPS destinations, with hidden keys, explicit per-target
  start/stop, editable saved configuration, and safe status messages. Saved destinations never start automatically.
- Autonomous Program output: all plugin destinations share one H.264/AAC stream and one delay buffer, without
  needing the native OBS stream to be running. Native streaming/recording retain independent delay sessions.
- Reuse of compatible already-active native streaming encoders, otherwise one private configured encoder pair
  for all plugin destinations. Adding destinations does not add encoders or full delay buffers; native streaming
  started later may need additional encoding.
- Bounded independent destination queues, safe-keyframe joining/reconnection, and RTMP/RTMPS delivery through a
  private OBS-derived librtmp transport with TLS verification. FFmpeg is used only for FLV muxing.
- Local destination persistence in `multistream.json`, without encryption or key logging: Unix permissions are
  owner-only (`0600`); Windows inherits the configuration directory's OS access controls. English and Spanish
  setup, security, bandwidth, scope, and download documentation.

#### Scope and validation

- Multistream targets main Program, SDR NV12/I420, H.264/AAC, and mono/stereo. No per-target bitrate, audio mix,
  resolution, canvas, HDR, HEVC, AV1, Opus, or Enhanced Broadcasting; third-party output plugins are untouched.
- Existing delay behavior and the 1.1.2 correctness/performance fixes are retained.
- Final 1.2.0 builds, regression/sanitizer results, packages, and real-libobs loopback integration are pending.
  Historical runtime validation below is not attributed to the new release. No real channels are used by tests.

### [1.1.2] — 2026-09-05

[Release page and downloads](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.2) ·
[Full release notes](docs/releases/1.1.2.md)

#### Fixed

- Release retired auxiliary encoders outside the session mutex, preventing a deadlock on rapid cancel/rearm.
- Freeze filling progress while a recording is paused and resume against media time, not elapsed wall time.
- Reset queues and the shared A/V timestamp bridge before the first packet of a reconnected output, whether audio
  or video arrives first; retain B-frame composition offsets.
- Recover persistent A/V skew after missing video packets or gaps in individual audio tracks, including holes
  buffered during Filling. Recovery advances all active tracks to a safe buffered GOP and may shorten the
  effective delay; it neither re-encodes Program nor inserts an additional hold scene.
- Follow the actual emitted video capture timestamp in the preview, independently of slider changes, and retain
  required history across recording pauses.
- Avoid keeping OBS video active or locking video settings merely because the preview is enabled.
- Remove literal shell quotes from macOS PKG identifiers/versions; validate final installer metadata in CI.

#### Performance

- Downscale the existing Program texture to 320×180 on the GPU before readback, at most 2 fps; do not attach a raw
  video consumer. Folding releases capture/resources/history; hiding the whole dock retains history without
  repainting.
- Share a complete auxiliary video/audio encoder bundle only for outputs with identical original encoders, every
  audio-track position, and media hub. Partial matches stay isolated; video-only matching is not enough. Pause
  follows the original encoder state already shared by fully matching outputs.
- Avoid the PCM FIFO allocation in explicit Silence mode and remove redundant PCM clears/copies without changing
  audio routing.

#### Validation

- Five SDK-free suites pass on macOS with warnings as errors, ASan+UBSan, and TSan.
- Tests now compile the real `OutputSession` and `HoldPipeline`, controlling only external libobs/encoder
  boundaries. The older packet-core model remains separate and does not substitute for production tests.
- Opt-in GPU and recording integration targets use real libobs and a graphics backend; their runtime results are
  reported separately in the release notes. The 174-second recording and Linux Docker smoke below belong to 1.1.1.
- Linux builds target native Ubuntu 24.04 x86_64 and the OBS 32.2.x module ABI.

### [1.1.1] — 2026-09-01

[Release page and downloads](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.1) ·
[Full release notes](docs/releases/1.1.1.md)

#### Added

- Native packages for universal macOS, Windows x64, and Ubuntu 24.04 x86_64.
- Hold-scene audio through a private bounded PCM mixer, without modifying OBS global buses, sources, or track
  assignments.
- Scene mix, dedicated source, reserved OBS track, and explicit silence modes.
- A collapsible 320×180, 2 fps audience preview that captures only while open.
- Independent delay control for simultaneous streaming and recording outputs.
- Conservative audio preflight and automatic safe degradation to dedicated audio or silence.

#### Fixed

- Validate the number of video encoders actually attached to an output instead of rejecting an output merely
  because its type advertises multivideo capability.
- Accept normal single-video-track Hybrid MP4/MOV, RTMP, and FLV outputs on macOS, Windows, and Linux while still
  rejecting actual multivideo and Enhanced Broadcasting layouts.
- Correct the Windows global installation path to
  <code>%PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay</code>.
- Allow the Windows plugin directory to be created when it is absent instead of instructing users to copy loose
  <code>bin</code> and <code>data</code> directories into the OBS application folder.

#### Changed

- Expanded live-fallback diagnostics with output type, flags, actual encoder counts, encoder ID, and the exact
  auxiliary-capture stage that failed.
- Preserved encoder-provided error details, including hardware-encoder initialization failures.
- Documented installation, audio routing, performance, architecture, validation, and deliberate limitations in
  English and Spanish.

#### Validation

- 26 packet-delay core cases and 9 audio-bridge scenarios pass.
- Native macOS tests pass on arm64 and x86_64; the universal bundle contains both architectures.
- A complete OBS 32.2.2 macOS arm64 recording workflow was tested, including cancellation, a full delay, a Program
  scene change, audience-preview toggling, and return to live.
- Linux was built natively against OBS 32.2.0 and Qt 6.4.2; both suites pass and <code>ldd -r</code> reports no
  unresolved symbols.
- The Windows PE32+ DLL was validated against the official OBS 32.2.2 and Qt 6.11.1 binaries. This release does not
  claim a complete Windows runtime test.
- ASan, UBSan, and TSan report no findings in the packet and audio cores.

#### Known limitations

- Production transitions use Cut on keyframe; fade remains intentionally disabled to avoid permanent
  decode/composite/re-encode overhead.
- Returning to live waits for the next keyframe.
- An audio-encoder splice can introduce a one- or two-frame AAC gap.
- Raw, audio-only, video-only, OBS native-delay, and actual multivideo outputs are unsupported.
- The macOS plugin bundle is ad-hoc signed; the PKG has no Developer ID signature and is not notarized. The Windows
  DLL is not Authenticode-signed.

---

## Español

### Sin publicar

No hay cambios adicionales enumerados.

### [1.2.0] — 2026-09-05

[Página de la versión y descargas](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.2.0) ·
[Notas completas](docs/releases/1.2.0.md)

#### Añadido

- Panel multistream integrado para hasta ocho destinos RTMP/RTMPS con nombre, claves ocultas, inicio/parada
  explícitos por destino, configuración guardada editable y estados seguros. Nunca arrancan automáticamente.
- Output Program autónomo: todos los destinos del plugin comparten un stream H.264/AAC y un buffer de delay, sin
  necesitar el directo nativo de OBS. Streaming y grabación nativos conservan sesiones de delay independientes.
- Reutilización de encoders de streaming nativos compatibles ya activos o, en su defecto, una pareja privada
  configurada para todos los destinos del plugin. Añadir destinos no añade encoders ni buffers completos; iniciar
  después el streaming nativo puede requerir codificación adicional.
- Colas independientes acotadas, entrada/reconexión en keyframes seguros y envío RTMP/RTMPS mediante un transporte
  librtmp privado derivado de OBS con verificación TLS. FFmpeg se usa sólo para multiplexar FLV.
- Persistencia local en `multistream.json`, sin cifrado ni logs de claves: Unix usa permisos sólo para el
  propietario (`0600`); Windows hereda los controles de acceso del sistema del directorio de configuración.
  Documentación en inglés/español de configuración, seguridad, subida, alcance y descargas.

#### Alcance y validación

- Multistream usa Program principal, SDR NV12/I420, H.264/AAC y mono/estéreo. No ofrece bitrate, mezcla, resolución
  ni canvas por destino, HDR, HEVC, AV1, Opus o Enhanced Broadcasting; no se modifican outputs de otros plugins.
- Se conservan el comportamiento del delay y las correcciones de funcionamiento/rendimiento de 1.1.2.
- Están pendientes los builds finales 1.2.0, regresiones/sanitizers, paquetes e integración loopback con libobs
  real. La validación runtime histórica siguiente no se atribuye a la nueva release. No se usan canales reales.

### [1.1.2] — 2026-09-05

[Página de la versión y descargas](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.2) ·
[Notas completas](docs/releases/1.1.2.md)

#### Corregido

- Liberación de encoders auxiliares retirados fuera del mutex de sesión para evitar bloqueos al cancelar/rearmar
  rápidamente.
- El progreso de llenado se congela durante una pausa de grabación y continúa según el reloj de medios, no el
  tiempo de pared transcurrido.
- Limpieza de colas y del puente temporal A/V antes del primer paquete tras reconectar, llegue primero audio o
  vídeo; se conservan los offsets de composición de B-frames.
- Recuperación de la desincronización A/V persistente tras perder paquetes de vídeo o sufrir huecos en pistas
  de audio individuales, también si se almacenaron durante Filling. Avanza todas las pistas activas hasta un GOP
  seguro del buffer y puede acortar el delay efectivo; no recodifica Program ni inserta otra escena de espera.
- La preview sigue el timestamp de captura del vídeo realmente emitido, independientemente de cambios del slider,
  y conserva el historial necesario durante las pausas de grabación.
- La preview no mantiene activo el vídeo de OBS ni bloquea sus ajustes sólo por estar habilitada.
- Se eliminan comillas literales del identificador/versión del PKG de macOS; la CI valida sus metadatos finales.

#### Rendimiento

- Reducción de la textura Program existente a 320×180 en la GPU antes de leer a RAM, como máximo a 2 fps, sin
  consumidor de vídeo raw. Plegar libera captura/recursos/historial; ocultar el panel conserva historial sin
  repintarlo.
- Reutilización del conjunto auxiliar completo de encoders de vídeo/audio sólo entre outputs con idénticos
  encoders originales, cada posición de pista de audio y hub de medios. Las coincidencias parciales quedan
  aisladas; no basta con que coincida el vídeo. La pausa sigue el estado de encoders originales ya compartido por
  los outputs que coinciden por completo.
- El modo Silence explícito no reserva la FIFO PCM; se eliminan borrados/copias PCM redundantes sin cambiar el
  routing de audio.

#### Validación

- Cinco suites sin SDK pasan en macOS con warnings como errores, ASan+UBSan y TSan.
- Las pruebas compilan ahora el código real de `OutputSession` y `HoldPipeline`, controlando sólo las fronteras
  externas de libobs/encoders. El modelo anterior de paquetes sigue separado y no sustituye los tests de producción.
- Los targets opcionales de integración GPU y grabación usan libobs real y un backend gráfico; sus resultados
  runtime se comunican por separado en las notas de la release. La grabación de 174 segundos y el smoke Linux
  Docker que figuran abajo pertenecen a 1.1.1.
- La compilación Linux apunta a Ubuntu 24.04 x86_64 nativo y a la ABI de módulos OBS 32.2.x.

### [1.1.1] — 2026-09-01

[Página de la versión y descargas](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.1) ·
[Notas completas de la versión](docs/releases/1.1.1.md)

#### Añadido

- Paquetes nativos para macOS universal, Windows x64 y Ubuntu 24.04 x86_64.
- Audio de la escena de espera mediante un mezclador PCM privado y acotado, sin modificar buses, fuentes ni
  asignaciones de pistas globales de OBS.
- Modos Scene mix, fuente dedicada, pista OBS reservada y silencio explícito.
- Vista de audiencia plegable a 320×180 y 2 fps que sólo captura mientras está abierta.
- Control independiente del delay para streaming y grabación simultáneos.
- Preflight de audio conservador y degradación automática segura a audio dedicado o silencio.

#### Corregido

- Validación del número de encoders de vídeo realmente conectados a un output en lugar de rechazarlo sólo porque
  su tipo anuncie capacidad multivídeo.
- Compatibilidad con outputs Hybrid MP4/MOV, RTMP y FLV normales con una sola pista de vídeo en macOS, Windows y
  Linux, manteniendo el rechazo de layouts multivídeo reales y Enhanced Broadcasting.
- Corrección de la ruta de instalación global de Windows a
  <code>%PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay</code>.
- Posibilidad de crear el directorio de plugins de Windows cuando no exista, en lugar de indicar que se copien
  carpetas <code>bin</code> y <code>data</code> sueltas en la carpeta de la aplicación OBS.

#### Cambiado

- Diagnóstico de live fallback ampliado con tipo de output, flags, cantidad real de encoders, ID del encoder y
  etapa exacta de captura auxiliar que haya fallado.
- Conservación del detalle de error proporcionado por el encoder, incluidos los fallos de inicialización del
  encoder por hardware.
- Documentación en inglés y español de instalación, routing de audio, rendimiento, arquitectura, validación y
  limitaciones deliberadas.

#### Validación

- Pasan 26 casos del núcleo de delay de paquetes y 9 escenarios del puente de audio.
- Los tests nativos de macOS pasan en arm64 y x86_64; el bundle universal contiene ambas arquitecturas.
- Se probó un flujo completo de grabación en OBS 32.2.2 macOS arm64, incluida la cancelación, un delay completo, un
  cambio de escena Program, abrir/cerrar la vista de audiencia y volver a live.
- Linux se compiló de forma nativa contra OBS 32.2.0 y Qt 6.4.2; pasan ambas suites y <code>ldd -r</code> no
  encuentra símbolos sin resolver.
- El DLL PE32+ de Windows se validó contra los binarios oficiales de OBS 32.2.2 y Qt 6.11.1. Esta versión no
  declara una prueba runtime completa en Windows.
- ASan, UBSan y TSan no encuentran problemas en los núcleos de paquetes y audio.

#### Limitaciones conocidas

- Las transiciones de producción usan Cut on keyframe; el fade sigue deshabilitado intencionadamente para evitar
  el coste permanente de decodificar, componer y recodificar.
- La vuelta a live espera el siguiente keyframe.
- El empalme de encoders de audio puede introducir un hueco de una o dos tramas AAC.
- No se admiten outputs raw, sólo-audio, sólo-vídeo, el delay nativo de OBS ni multivídeo real.
- El bundle del plugin para macOS tiene firma ad hoc; el PKG no tiene firma Developer ID ni está notarizado. El DLL
  de Windows no tiene firma Authenticode.

[1.1.1]: https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.1
[1.1.2]: https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.2
[1.2.0]: https://github.com/stanixdor/obs-variable-delay/releases/tag/1.2.0
[1.1.2]: https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.2
