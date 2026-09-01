# Changelog / Historial de cambios

[English](#english) · [Español](#español)

This project follows [Semantic Versioning](https://semver.org/) and the structure proposed by
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Este proyecto sigue [Versionado Semántico](https://semver.org/lang/es/) y la estructura propuesta por
[Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).

## English

### Unreleased

No user-facing changes have been published after 1.1.1.

### [1.1.1] — 2026-09-01

[Release page and downloads](https://github.com/stanixdor/obs-variable-delay-web/releases/tag/1.1.1) ·
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

No se han publicado cambios visibles para el usuario después de la versión 1.1.1.

### [1.1.1] — 2026-09-01

[Página de la versión y descargas](https://github.com/stanixdor/obs-variable-delay-web/releases/tag/1.1.1) ·
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

[1.1.1]: https://github.com/stanixdor/obs-variable-delay-web/releases/tag/1.1.1
