# Third-party sources / Fuentes de terceros

## English

`librtmp/` and `happy-eyeballs/` are pinned copies from
[OBS Studio 32.2.2](https://github.com/obsproject/obs-studio/tree/32.2.2):
`plugins/obs-outputs/librtmp` and `shared/happy-eyeballs` respectively.
The LGPL 2.1-or-later license for librtmp is included in `librtmp/COPYING`;
the Happy Eyeballs MIT license is preserved in its source headers.

This private, hidden-symbol copy keeps transport logging separate from OBS.
Stream credentials are never handed to FFmpeg; FFmpeg only muxes encoded
H.264/AAC into FLV through a custom I/O callback. TLS uses Mbed TLS and the
certificate verification in OBS's RTMP implementation. Mbed TLS is linked
statically on Windows from OBS's pinned dependency bundle; macOS uses OBS's
shared libraries, and Linux uses distribution-provided shared libraries.
The relevant Mbed TLS 3.6.4 sources are Apache-2.0 OR GPL-2.0-or-later; the
Windows component uses the GPL-2.0-or-later option, preserving the project's
existing license. See [packaged component notices and source links](../data/licenses/README.md).
The release source archive includes the original Mbed TLS and zlib sources,
OBS dependency recipes, patches, and checksums in
[`source-archives/`](source-archives/README.md).

## Español

`librtmp/` y `happy-eyeballs/` son copias fijadas de
[OBS Studio 32.2.2](https://github.com/obsproject/obs-studio/tree/32.2.2):
`plugins/obs-outputs/librtmp` y `shared/happy-eyeballs`, respectivamente.
La licencia LGPL 2.1 o posterior de librtmp está en `librtmp/COPYING`;
la licencia MIT de Happy Eyeballs se conserva en sus cabeceras.

Esta copia privada con símbolos ocultos separa los registros del transporte
de los de OBS. Las claves nunca se entregan a FFmpeg: sólo empaqueta H.264/AAC
codificado como FLV mediante una función de escritura propia. TLS utiliza
Mbed TLS y la verificación de certificados del transporte RTMP de OBS. Windows
enlaza Mbed TLS estáticamente desde el paquete fijado de dependencias de OBS;
macOS utiliza las bibliotecas compartidas de OBS y Linux las de la distribución.
Las fuentes utilizadas de Mbed TLS 3.6.4 tienen licencia Apache-2.0 O
GPL-2.0-or-later; el componente de Windows utiliza la opción GPL-2.0-or-later,
manteniendo la licencia existente del proyecto. Consulta los
[avisos distribuidos y enlaces a las fuentes](../data/licenses/README.md).
El archivo de fuentes de la release incluye las fuentes originales de Mbed TLS
y zlib, las recetas de dependencias OBS, los parches y los checksums en
[`source-archives/`](source-archives/README.md).
