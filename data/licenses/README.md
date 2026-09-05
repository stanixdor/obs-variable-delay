# Licenses and corresponding source / Licencias y código fuente

## English

OBS Dynamic Delay is distributed under **GPL-2.0-or-later**, without warranty.
`obs-dynamic-delay-GPL-2.0.txt` contains the project's license text. The license
choice for the project has not changed with the addition of multistream.

Source code, vendored library sources, and the scripts needed to build and
relink the plugin are available with the matching release:

- [OBS Dynamic Delay 1.2.0 source archive](https://github.com/stanixdor/obs-variable-delay/releases/download/1.2.0/obs-dynamic-delay-1.2.0-source.tar.xz).
- [Versioned repository and build instructions](https://github.com/stanixdor/obs-variable-delay/tree/1.2.0).
- [Release files and checksums](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.2.0).

The source archive includes the original Mbed TLS and zlib source archives,
plus OBS's pinned dependency build recipes and patches, under
`vendor/source-archives`. Their [provenance and SHA-256 checksums](https://github.com/stanixdor/obs-variable-delay/blob/1.2.0/vendor/source-archives/README.md)
are included there; Mbed TLS's archive also includes its framework sources.

The following component notices are retained:

- **librtmp:** LGPL-2.1-or-later; see `librtmp-LGPL-2.1.txt`. The private static
  copy comes from [OBS Studio 32.2.2, plugins/obs-outputs/librtmp](https://github.com/obsproject/obs-studio/tree/32.2.2/plugins/obs-outputs/librtmp).
  Its source is included under `vendor/librtmp` in the plugin source archive.
- **Happy Eyeballs:** MIT; see `happy-eyeballs-MIT.txt`. Source and copyright
  notices come from [OBS Studio 32.2.2, shared/happy-eyeballs](https://github.com/obsproject/obs-studio/tree/32.2.2/shared/happy-eyeballs)
  and are included under `vendor/happy-eyeballs`.
- **Mbed TLS 3.6.4:** the relevant library sources are dual-licensed
  Apache-2.0 **OR** GPL-2.0-or-later. For the statically linked Windows component
  we select **GPL-2.0-or-later**, compatible with the project's existing license.
  Copyright The Mbed TLS Contributors.
  The upstream Apache text is also preserved as `mbedtls-Apache-2.0.txt`; its
  inclusion does not remove the upstream GPL option or relicense the project.
  The pinned source is [commit c765c831e5c2a0971410692f92f7a81d6ec65ec2](https://github.com/Mbed-TLS/mbedtls/tree/c765c831e5c2a0971410692f92f7a81d6ec65ec2).
  See its [license policy](https://github.com/Mbed-TLS/mbedtls/blob/c765c831e5c2a0971410692f92f7a81d6ec65ec2/README.md#license)
  and [OBS dependency recipe and patches](https://github.com/obsproject/obs-deps/blob/2026-07-15/deps.ffmpeg/60-mbedtls.ps1).
  The Apache-only optional Everest backend is not enabled by that configuration.
- **zlib:** see `zlib.txt`, copied from the exact
  [1.3.1 source revision 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf](https://github.com/madler/zlib/blob/51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf/LICENSE)
  pinned in the [Windows dependency recipe](https://github.com/obsproject/obs-deps/blob/2026-07-15/deps.ffmpeg/10-zlib.ps1).
  zlib is a build dependency; no standalone zlib runtime is shipped. Optional
  librtmp SWF hashing (`USE_HASHSWF`) is not enabled in this plugin.

Dependency linkage is platform-specific. **Windows** links Mbed TLS statically
from OBS's pinned dependency bundle. **macOS** uses OBS's shared Mbed TLS
libraries and the system zlib. **Linux** uses distribution-provided shared
Mbed TLS, FFmpeg, Qt, and OBS libraries, with dependencies declared by the DEB.
FFmpeg and Qt runtimes are not copied into these plugin packages. Keep the
licenses of the OBS installation and system dependencies with those components.
No separate upstream `NOTICE` file exists in the pinned Mbed TLS source tree.

## Español

OBS Dynamic Delay se distribuye bajo **GPL-2.0-or-later**, sin garantía.
`obs-dynamic-delay-GPL-2.0.txt` contiene el texto de la licencia del proyecto.
La incorporación de multistream no cambia la licencia del proyecto.

El código fuente, las bibliotecas incluidas y los scripts para compilar y
volver a enlazar el plugin están disponibles junto a la versión correspondiente:

- [Archivo de fuentes de OBS Dynamic Delay 1.2.0](https://github.com/stanixdor/obs-variable-delay/releases/download/1.2.0/obs-dynamic-delay-1.2.0-source.tar.xz).
- [Repositorio de la versión e instrucciones de compilación](https://github.com/stanixdor/obs-variable-delay/tree/1.2.0).
- [Archivos de la versión y checksums](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.2.0).

El archivo de fuentes incluye los archivos originales de Mbed TLS y zlib,
además de las recetas y los parches de dependencias fijados por OBS, dentro de
`vendor/source-archives`. Allí se incluyen su [procedencia y checksums SHA-256](https://github.com/stanixdor/obs-variable-delay/blob/1.2.0/vendor/source-archives/README.md);
el archivo de Mbed TLS contiene también las fuentes de su framework.

Se conservan los siguientes avisos de componentes:

- **librtmp:** LGPL-2.1-or-later; consulta `librtmp-LGPL-2.1.txt`. La copia
  estática privada procede de [OBS Studio 32.2.2, plugins/obs-outputs/librtmp](https://github.com/obsproject/obs-studio/tree/32.2.2/plugins/obs-outputs/librtmp).
  Sus fuentes están en `vendor/librtmp` dentro del archivo de fuentes del plugin.
- **Happy Eyeballs:** MIT; consulta `happy-eyeballs-MIT.txt`. Las fuentes y los
  avisos de copyright proceden de [OBS Studio 32.2.2, shared/happy-eyeballs](https://github.com/obsproject/obs-studio/tree/32.2.2/shared/happy-eyeballs)
  y se incluyen en `vendor/happy-eyeballs`.
- **Mbed TLS 3.6.4:** las fuentes de biblioteca utilizadas tienen licencia dual
  Apache-2.0 **O** GPL-2.0-or-later. Para el componente enlazado estáticamente en
  Windows se elige **GPL-2.0-or-later**, compatible con la licencia existente del
  proyecto. Copyright The Mbed TLS Contributors.
  También se conserva el texto Apache original en
  `mbedtls-Apache-2.0.txt`; incluirlo no elimina la opción GPL original ni cambia
  la licencia del proyecto. La fuente fijada es el
  [commit c765c831e5c2a0971410692f92f7a81d6ec65ec2](https://github.com/Mbed-TLS/mbedtls/tree/c765c831e5c2a0971410692f92f7a81d6ec65ec2).
  Consulta su [política de licencias](https://github.com/Mbed-TLS/mbedtls/blob/c765c831e5c2a0971410692f92f7a81d6ec65ec2/README.md#license)
  y la [receta de compilación y los parches de OBS](https://github.com/obsproject/obs-deps/blob/2026-07-15/deps.ffmpeg/60-mbedtls.ps1).
  Esa configuración no activa el backend opcional Everest, de licencia solo Apache.
- **zlib:** consulta `zlib.txt`, copiado de la
  [revisión exacta 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf de 1.3.1](https://github.com/madler/zlib/blob/51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf/LICENSE)
  fijada en la [receta de dependencias de Windows](https://github.com/obsproject/obs-deps/blob/2026-07-15/deps.ffmpeg/10-zlib.ps1).
  zlib es una dependencia de compilación; no se distribuye un runtime zlib
  independiente. Este plugin no activa el hashing SWF opcional de librtmp
  (`USE_HASHSWF`).

El enlace depende de la plataforma. **Windows** enlaza Mbed TLS estáticamente
desde el paquete fijado de dependencias de OBS. **macOS** utiliza las bibliotecas
Mbed TLS compartidas de OBS y zlib del sistema. **Linux** utiliza Mbed TLS,
FFmpeg, Qt y OBS compartidos de la distribución, con dependencias declaradas en
el DEB. No se copian runtimes FFmpeg ni Qt dentro de los paquetes del plugin.
Conserva las licencias de la instalación de OBS y de las dependencias del sistema
junto a esos componentes. No existe un archivo `NOTICE` original separado en
el árbol de fuentes de Mbed TLS fijado.
