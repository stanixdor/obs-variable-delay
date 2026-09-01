# OBS Dynamic Delay

<div align="center">

**Variable output delay for OBS Studio — add, change, cancel, or remove delay without stopping your stream or recording.**

**Delay variable para OBS Studio — añade, cambia, cancela o elimina el delay sin detener el directo ni la grabación.**

[![Latest release](https://img.shields.io/github/v/release/stanixdor/obs-variable-delay-web?display_name=tag&sort=semver&label=release)](https://github.com/stanixdor/obs-variable-delay-web/releases/latest)
[![Build](https://github.com/stanixdor/obs-variable-delay-web/actions/workflows/push.yaml/badge.svg?branch=main)](https://github.com/stanixdor/obs-variable-delay-web/actions/workflows/push.yaml)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
[![OBS Studio 32.2.x](https://img.shields.io/badge/OBS%20Studio-32.2.x-302E31)](https://obsproject.com/)

[Website](https://www.obsdelay.com) · [Latest release](https://github.com/stanixdor/obs-variable-delay-web/releases/latest) · [English guide](docs/guide.en.md) · [Guía en español](docs/guide.es.md)

</div>

English | [Español](#español)

## English

OBS Dynamic Delay is a native OBS Studio plugin for introducing a variable delay into an active streaming or
recording output. It buffers the encoded packets that OBS already produces, so an active delay primarily costs RAM
instead of requiring permanent decoding and re-encoding.

Version **1.1.1** supports OBS Studio 32.2.x and Qt 6 on macOS 13+, Windows 10/11 x64, and native Ubuntu 24.04
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

### Download 1.1.1

| Platform | Recommended download | Alternatives |
| --- | --- | --- |
| macOS 13+ (Apple Silicon and Intel) | [Universal ZIP](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-macos-universal.zip) | [Universal PKG — unsigned/not notarized](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-macos-universal.pkg) · [Debug symbols](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-macos-universal-dSYMs.tar.xz) |
| Windows 10/11 x64 | [Windows ZIP](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-windows-x64.zip) | [Debug symbols](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-windows-x64-symbols.zip) |
| Ubuntu 24.04 x86_64 | [Debian package](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-x86_64-linux-gnu.deb) | [Native tar.xz](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-x86_64-ubuntu-gnu.tar.xz) · [Debug symbols](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-x86_64-linux-gnu-dbgsym.ddeb) |
| Source | [Source tarball](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-source.tar.xz) | [All assets and checksums](https://github.com/stanixdor/obs-variable-delay-web/releases/tag/1.1.1) |

Read the [1.1.1 release notes](docs/releases/1.1.1.md) before installing. The ZIP is the recommended macOS download:
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
   <code>%PROGRAMDATA%\obs-studio\plugins\</code>.
3. Confirm that the DLL is at
   <code>%PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll</code>.
4. Open OBS and select **Docks → Dynamic Delay**.

Do not copy loose <code>bin</code> and <code>data</code> folders into the OBS installation directory, and do not use
<code>%APPDATA%</code>.

**Ubuntu 24.04 x86_64**

~~~bash
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt update
sudo apt install ./obs-dynamic-delay-1.1.1-x86_64-linux-gnu.deb
~~~

Restart OBS and select **Docks → Dynamic Delay**.

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
- Production transitions use **Cut on keyframe**. A true fade would require continuous decoding, compositing, and
  re-encoding, which conflicts with the low-overhead goal.
- Returning live waits for the next keyframe to preserve a decodable bitstream.
- A one- or two-frame AAC gap can occur when switching audio encoders.
- Raw, audio-only, video-only, OBS native-delay, and actual multivideo/Enhanced Broadcasting outputs are not
  supported.
- The audience preview approximates what OBS has sent; it does not include server, CDN, or player buffering.

### Development

The repository contains the native C++ plugin at the root and the Next.js website under <code>website/</code>.
Builds and tests run for universal macOS, Windows x64, and Ubuntu 24.04 x86_64.

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

La versión **1.1.1** es compatible con OBS Studio 32.2.x y Qt 6 en macOS 13+, Windows 10/11 x64 y Ubuntu 24.04
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

### Descargar 1.1.1

| Plataforma | Descarga recomendada | Alternativas |
| --- | --- | --- |
| macOS 13+ (Apple Silicon e Intel) | [ZIP universal](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-macos-universal.zip) | [PKG universal — sin firma/notarización](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-macos-universal.pkg) · [Símbolos de depuración](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-macos-universal-dSYMs.tar.xz) |
| Windows 10/11 x64 | [ZIP para Windows](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-windows-x64.zip) | [Símbolos de depuración](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-windows-x64-symbols.zip) |
| Ubuntu 24.04 x86_64 | [Paquete Debian](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-x86_64-linux-gnu.deb) | [tar.xz nativo](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-x86_64-ubuntu-gnu.tar.xz) · [Símbolos de depuración](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-x86_64-linux-gnu-dbgsym.ddeb) |
| Código fuente | [Tarball de fuentes](https://github.com/stanixdor/obs-variable-delay-web/releases/download/1.1.1/obs-dynamic-delay-1.1.1-source.tar.xz) | [Todos los archivos y checksums](https://github.com/stanixdor/obs-variable-delay-web/releases/tag/1.1.1) |

Lee las [notas de la versión 1.1.1](docs/releases/1.1.1.md) antes de instalar. El ZIP es la descarga recomendada para
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
   <code>%PROGRAMDATA%\obs-studio\plugins\</code>.
3. Comprueba que el DLL esté en
   <code>%PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll</code>.
4. Abre OBS y selecciona **Paneles/Docks → Dynamic Delay**.

No copies las carpetas <code>bin</code> y <code>data</code> sueltas en el directorio de instalación de OBS y no
uses <code>%APPDATA%</code>.

**Ubuntu 24.04 x86_64**

~~~bash
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt update
sudo apt install ./obs-dynamic-delay-1.1.1-x86_64-linux-gnu.deb
~~~

Reinicia OBS y selecciona **Docks → Dynamic Delay**.

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
- Las transiciones de producción usan **Cut on keyframe**. Un fade real exigiría decodificar, componer y
  recodificar continuamente, lo que contradice el objetivo de bajo consumo.
- La vuelta a live espera el siguiente keyframe para conservar un bitstream decodificable.
- Al cambiar de encoder de audio puede aparecer un hueco de una o dos tramas AAC.
- No se admiten outputs raw, sólo-audio, sólo-vídeo, el delay nativo de OBS ni multivídeo/Enhanced Broadcasting
  real.
- La vista de audiencia aproxima lo que OBS ha enviado; no incluye el buffer del servidor, CDN o reproductor.

### Desarrollo

El repositorio contiene el plugin nativo en C++ en la raíz y la web Next.js dentro de <code>website/</code>. Los
builds y tests se ejecutan para macOS universal, Windows x64 y Ubuntu 24.04 x86_64.

- [Cómo contribuir](CONTRIBUTING.md)
- [Política de seguridad](SECURITY.md)
- [Historial de cambios](CHANGELOG.md)
- [Licencia](LICENSE)

## License / Licencia

OBS Dynamic Delay is distributed under **GPL-2.0-or-later**. See [LICENSE](LICENSE).

OBS Dynamic Delay se distribuye bajo **GPL-2.0-or-later**. Consulta [LICENSE](LICENSE).
