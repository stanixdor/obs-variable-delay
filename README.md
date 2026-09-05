# OBS Dynamic Delay

<div align="center">

**Variable output delay for OBS Studio — add, change, cancel, or remove delay without stopping your stream or recording.**

**Delay variable para OBS Studio — añade, cambia, cancela o elimina el delay sin detener el directo ni la grabación.**

[![Latest release](https://img.shields.io/github/v/release/stanixdor/obs-variable-delay?display_name=tag&sort=semver&label=release)](https://github.com/stanixdor/obs-variable-delay/releases/latest)
[![Build](https://github.com/stanixdor/obs-variable-delay/actions/workflows/push.yaml/badge.svg?branch=main)](https://github.com/stanixdor/obs-variable-delay/actions/workflows/push.yaml)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
[![OBS Studio 32.2.x](https://img.shields.io/badge/OBS%20Studio-32.2.x-302E31)](https://obsproject.com/)

[Website](https://www.obsdelay.com) · [Latest release](https://github.com/stanixdor/obs-variable-delay/releases/latest) · [English guide](docs/guide.en.md) · [Guía en español](docs/guide.es.md)

</div>

English | [Español](#español)

## English

OBS Dynamic Delay is a native OBS Studio plugin for introducing a variable delay into an active streaming or
recording output. It buffers the encoded packets that OBS already produces, so an active delay primarily costs RAM
instead of requiring permanent decoding and re-encoding.

Version **1.1.2** supports OBS Studio 32.2.x and Qt 6 on macOS 13+, Windows 10/11 x64, and native Ubuntu 24.04
x86_64.

### Highlights

- Add 1–300 seconds of delay without restarting streaming or recording.
- Cancel while the initial buffer is filling, or return to live on the next safe keyframe.
- Change scenes and continue using OBS normally while delay is active.
- Select a hold scene that is shown while the buffer is built.
- Keep hold-scene audio through a private mixer without changing global OBS tracks, buses, or sources.
- Estimate RAM from the observed or configured bitrate.
- Open an optional low-cost audience preview only when needed.
- Control simultaneous streaming and recording outputs independently.
- Support normal single-video-track Hybrid MP4/MOV, RTMP, and FLV outputs in OBS 32.

Version 1.1.2 fixes rapid cancel/rearm hangs, paused-buffer progress, reconnect timestamps, and persistent
audio/video skew after encoder gaps, including gaps in individual audio tracks. It also reduces preview readback
and compatible auxiliary-encoding work. See the [release notes](docs/releases/1.1.2.md).

### Download 1.1.2

| Platform | Recommended download | Alternatives |
| --- | --- | --- |
| macOS 13+ (Apple Silicon and Intel) | [Universal ZIP](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-macos-universal.zip) | [Universal PKG — unsigned/not notarized](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-macos-universal.pkg) · [Debug symbols](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-macos-universal-dSYMs.tar.xz) |
| Windows 10/11 x64 | [Windows ZIP](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-windows-x64.zip) | [Debug symbols](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-windows-x64-symbols.zip) |
| Ubuntu 24.04 x86_64 | [Debian package](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-x86_64-linux-gnu.deb) | [Native tar.xz](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-x86_64-ubuntu-gnu.tar.xz) · [Debug symbols](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-x86_64-linux-gnu-dbgsym.ddeb) |
| Source | [Source tarball](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-source.tar.xz) | [All assets and checksums](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.2) |

Read the [1.1.2 release notes](docs/releases/1.1.2.md) before installing. The ZIP is the recommended macOS download:
its plugin bundle is ad-hoc signed, while the alternative PKG is not signed with Developer ID and is not
Apple-notarized. The Windows DLL is not Authenticode-signed.

### Quick installation

**macOS**

1. Close OBS.
2. Copy <code>obs-dynamic-delay.plugin</code> from the recommended ZIP to
   <code>~/Library/Application Support/obs-studio/plugins/</code>.
3. Open OBS and select **Docks → Dynamic Delay**.

**Windows x64**

1. Close OBS.
2. Extract the complete <code>obs-dynamic-delay</code> folder from the ZIP into
   <code>%PROGRAMDATA%\obs-studio\plugins\</code>; create this directory if it does not exist.
3. Confirm that the DLL is at
   <code>%PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll</code>.
4. Open OBS and select **Docks → Dynamic Delay**.

Do not copy loose <code>bin</code> and <code>data</code> folders into the OBS installation directory, and do not use
<code>%APPDATA%</code>.

**Ubuntu 24.04 x86_64**

~~~bash
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt update
sudo apt install ./obs-dynamic-delay-1.1.2-x86_64-linux-gnu.deb
~~~

Restart OBS and select **Docks → Dynamic Delay**.

Linux packages target the OBS 32.2.x module ABI on native Ubuntu 24.04 x86_64; they are not Flatpak, Snap, or
generic cross-distribution packages. Verify downloads against
[SHA256SUMS.txt](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/SHA256SUMS.txt).

For complete installation, audio-routing, usage, performance, architecture, and build instructions, read the
**[English technical guide](docs/guide.en.md)**.

### How it works

When delay is requested, the plugin prepares a compatible auxiliary encoder and waits for a safe keyframe. It then
shows the selected hold scene while storing normal encoded content in RAM. Once the configured duration is filled,
the buffered content becomes the output and the auxiliary encoder shuts down. Removing delay keeps a valid output
until the next live keyframe and then returns directly to live.

~~~text
LIVE → PREPARING → FILLING → DELAY ACTIVE → RETURNING LIVE → LIVE
                    ↘ cancellation ────────────────↗
~~~

Video remains packet-based. The hold-scene audio uses a private bounded PCM bridge and supports **Scene mix**,
**Dedicated source**, **Reserved OBS track**, and explicit **Silence** modes.

### Performance and intentional limitations

- Buffer memory is estimated as <code>total bitrate × seconds ÷ 8</code>, plus a 20% margin.
- A temporary second video encoder runs only during **Preparing/Filling**.
- Compatible outputs can share one complete auxiliary video/audio encoder bundle when the original encoder
  identities, every audio-track position, and hold-media hub match. Sharing video alone is not sufficient;
  partial matches stay isolated. Pause follows the same original encoders shared by those outputs.
- The preview downsizes on the GPU to 320×180 before readback, at most twice per second. It does not create a raw
  video consumer or keep OBS video settings locked. Folding it releases capture and history; hiding the whole dock
  retains low-rate history without repainting it.
- Explicit **Silence** mode does not allocate the PCM FIFO.
- Production transitions use **Cut on keyframe**. A true fade would require continuous decoding, compositing, and
  re-encoding, which conflicts with the low-overhead goal.
- Returning live waits for the next keyframe to preserve a decodable bitstream.
- After missing encoder packets, recovery may shorten the effective delay to a safe buffered GOP and realign all
  audio tracks. This does not re-encode the program or insert another hold scene.
- A one- or two-frame AAC gap can occur when switching audio encoders.
- B-frame/AAC splices are not sample-exact. OBS 32.2.2 also has a native x264 FPS-reduction pause issue reproduced
  without this plugin; use FPS divisor 1 when pausing recordings. See the release validation for details.
- Raw, audio-only, video-only, OBS native-delay, and actual multivideo/Enhanced Broadcasting outputs are not
  supported.
- The audience preview approximates what OBS has sent; it does not include server, CDN, or player buffering.

### Development

The repository contains the native C++ plugin at the root and the Next.js website under <code>website/</code>.
Builds and tests run for universal macOS, Windows x64, and Ubuntu 24.04 x86_64.

The five SDK-free regression suites exercise the production workflow, including the **real production OutputSession and
HoldPipeline** with controlled libobs boundaries. They pass on macOS with strict warnings, ASan+UBSan, and TSan.
The separate packet-core model is not a substitute for production-session coverage. Optional real-libobs GPU and
recording integration tests are available separately; see [test scope](tests/README.md) and the
[validation notes](docs/releases/1.1.2.md#validation).

- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)
- [License](LICENSE)

---

[English](#english) | Español

## Español

OBS Dynamic Delay es un plugin nativo para OBS Studio que introduce un delay variable en un output de streaming o
grabación activo. Almacena los paquetes codificados que OBS ya produce, por lo que mantener el delay consume
principalmente RAM y no exige decodificar y recodificar permanentemente.

La versión **1.1.2** es compatible con OBS Studio 32.2.x y Qt 6 en macOS 13+, Windows 10/11 x64 y Ubuntu 24.04
x86_64 nativo.

### Funciones principales

- Añade entre 1 y 300 segundos de delay sin reiniciar el directo ni la grabación.
- Cancela mientras se llena el buffer inicial o vuelve a live en el siguiente keyframe seguro.
- Permite cambiar de escena y seguir usando OBS normalmente con el delay activo.
- Reproduce una escena de espera seleccionada mientras construye el buffer.
- Mantiene el audio de la escena de espera mediante un mezclador privado sin cambiar pistas, buses ni fuentes
  globales de OBS.
- Estima la RAM a partir del bitrate observado o configurado.
- Abre una vista de audiencia opcional y de bajo coste sólo cuando se necesita.
- Controla de forma independiente streaming y grabación simultáneos.
- Admite outputs Hybrid MP4/MOV, RTMP y FLV normales con una sola pista de vídeo en OBS 32.

La versión 1.1.2 corrige bloqueos al cancelar/rearmar rápidamente, el progreso del buffer al pausar, los timestamps
tras reconectar y la desincronización A/V persistente tras huecos del encoder, también en pistas de audio
individuales. Reduce además la lectura de la preview y la codificación auxiliar compatible. Consulta las
[notas de la versión](docs/releases/1.1.2.md).

### Descargar 1.1.2

| Plataforma | Descarga recomendada | Alternativas |
| --- | --- | --- |
| macOS 13+ (Apple Silicon e Intel) | [ZIP universal](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-macos-universal.zip) | [PKG universal — sin firma/notarización](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-macos-universal.pkg) · [Símbolos de depuración](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-macos-universal-dSYMs.tar.xz) |
| Windows 10/11 x64 | [ZIP para Windows](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-windows-x64.zip) | [Símbolos de depuración](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-windows-x64-symbols.zip) |
| Ubuntu 24.04 x86_64 | [Paquete Debian](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-x86_64-linux-gnu.deb) | [tar.xz nativo](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-x86_64-ubuntu-gnu.tar.xz) · [Símbolos de depuración](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-x86_64-linux-gnu-dbgsym.ddeb) |
| Código fuente | [Tarball de fuentes](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/obs-dynamic-delay-1.1.2-source.tar.xz) | [Todos los archivos y checksums](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.1.2) |

Lee las [notas de la versión 1.1.2](docs/releases/1.1.2.md) antes de instalar. El ZIP es la descarga recomendada para
macOS: su bundle lleva firma ad hoc, mientras que el PKG alternativo no tiene firma Developer ID ni está notarizado
por Apple. El DLL de Windows no tiene firma Authenticode.

### Instalación rápida

**macOS**

1. Cierra OBS.
2. Copia <code>obs-dynamic-delay.plugin</code> del ZIP recomendado en
   <code>~/Library/Application Support/obs-studio/plugins/</code>.
3. Abre OBS y selecciona **Paneles/Docks → Delay dinámico**.

**Windows x64**

1. Cierra OBS.
2. Extrae la carpeta completa <code>obs-dynamic-delay</code> del ZIP dentro de
   <code>%PROGRAMDATA%\obs-studio\plugins\</code>; crea ese directorio si no existe.
3. Comprueba que el DLL esté en
   <code>%PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll</code>.
4. Abre OBS y selecciona **Paneles/Docks → Dynamic Delay**.

No copies las carpetas <code>bin</code> y <code>data</code> sueltas en el directorio de instalación de OBS y no
uses <code>%APPDATA%</code>.

**Ubuntu 24.04 x86_64**

~~~bash
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt update
sudo apt install ./obs-dynamic-delay-1.1.2-x86_64-linux-gnu.deb
~~~

Reinicia OBS y selecciona **Docks → Dynamic Delay**.

Los paquetes Linux apuntan a la ABI de módulos OBS 32.2.x en Ubuntu 24.04 x86_64 nativo; no son paquetes Flatpak,
Snap ni genéricos para otras distribuciones. Comprueba las descargas con
[SHA256SUMS.txt](https://github.com/stanixdor/obs-variable-delay/releases/download/1.1.2/SHA256SUMS.txt).

Para consultar la instalación completa, el routing de audio, el uso, el rendimiento, la arquitectura y la
compilación, lee la **[guía técnica en español](docs/guide.es.md)**.

### Cómo funciona

Cuando se solicita el delay, el plugin prepara un encoder auxiliar compatible y espera un keyframe seguro. A
continuación muestra la escena de espera seleccionada mientras almacena en RAM el contenido codificado normal. Al
completar la duración configurada, el contenido del buffer pasa a ser el output y el encoder auxiliar se apaga.
Quitar el delay mantiene un output válido hasta el siguiente keyframe vivo y entonces vuelve directamente a live.

~~~text
LIVE → PREPARING → FILLING → DELAY ACTIVE → RETURNING LIVE → LIVE
                    ↘ cancelación ────────────────↗
~~~

El vídeo sigue trabajando con paquetes. El audio de la escena de espera usa un puente PCM privado y acotado, y
admite los modos **Scene mix**, **Dedicated source**, **Reserved OBS track** y **Silence** explícito.

### Rendimiento y limitaciones deliberadas

- La memoria del buffer se estima como <code>bitrate total × segundos ÷ 8</code>, más un margen del 20 %.
- Un segundo encoder de vídeo temporal funciona únicamente durante **Preparing/Filling**.
- Los outputs compatibles pueden compartir el conjunto auxiliar completo de vídeo/audio si coinciden los encoders
  originales, cada posición de pista de audio y el hub de medios de espera. No basta con compartir sólo vídeo;
  las coincidencias parciales quedan aisladas. La pausa sigue los encoders originales compartidos por esos outputs.
- La preview reduce a 320×180 en la GPU antes de leer a RAM, como máximo dos veces por segundo. No crea un
  consumidor de vídeo raw ni bloquea los ajustes de vídeo de OBS. Plegarla libera captura e historial; ocultar el
  panel completo conserva el historial de baja frecuencia sin repintarlo.
- El modo **Silence** explícito no reserva la FIFO PCM.
- Las transiciones de producción usan **Cut on keyframe**. Un fade real exigiría decodificar, componer y
  recodificar continuamente, lo que contradice el objetivo de bajo consumo.
- La vuelta a live espera el siguiente keyframe para conservar un bitstream decodificable.
- Tras perder paquetes del encoder, la recuperación puede acortar el delay efectivo hasta un GOP seguro del
  buffer y realinear todas las pistas de audio. No recodifica Program ni inserta otra escena de espera.
- Al cambiar de encoder de audio puede aparecer un hueco de una o dos tramas AAC.
- Los empalmes B-frame/AAC no son exactos por muestra. OBS 32.2.2 también presenta un fallo de pausa con reducción
  de FPS x264 reproducido sin el plugin; usa divisor de FPS 1 si necesitas pausar. Consulta la validación de la release.
- No se admiten outputs raw, sólo-audio, sólo-vídeo, el delay nativo de OBS ni multivídeo/Enhanced Broadcasting
  real.
- La vista de audiencia aproxima lo que OBS ha enviado; no incluye el buffer del servidor, CDN o reproductor.

### Desarrollo

El repositorio contiene el plugin nativo en C++ en la raíz y la web Next.js dentro de <code>website/</code>. Los
builds y tests se ejecutan para macOS universal, Windows x64 y Ubuntu 24.04 x86_64.

Las cinco suites de regresión sin SDK ejercitan el flujo de producción, incluido el **código real de OutputSession y
HoldPipeline** con una frontera de libobs controlada. Pasan en macOS con warnings estrictos, ASan+UBSan y TSan.
El modelo separado del núcleo de paquetes no sustituye la cobertura de la sesión real. Hay pruebas opcionales de
integración GPU y grabación con libobs real; consulta el [alcance de los tests](tests/README.md) y las
[notas de validación](docs/releases/1.1.2.md#validación).

- [Cómo contribuir](CONTRIBUTING.md)
- [Política de seguridad](SECURITY.md)
- [Historial de cambios](CHANGELOG.md)
- [Licencia](LICENSE)

## License / Licencia

OBS Dynamic Delay is distributed under **GPL-2.0-or-later**. See [LICENSE](LICENSE).

OBS Dynamic Delay se distribuye bajo **GPL-2.0-or-later**. Consulta [LICENSE](LICENSE).
