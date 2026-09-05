# Third-party sources / Fuentes de terceros

## English

`librtmp/` and `happy-eyeballs/` are pinned copies from
[OBS Studio 32.2.2](https://github.com/obsproject/obs-studio/tree/32.2.2):
`plugins/obs-outputs/librtmp` and `shared/happy-eyeballs` respectively.
The LGPL 2.1-or-later license for librtmp is included in `librtmp/COPYING`;
the Happy Eyeballs MIT license is preserved in its source headers.

This private, hidden-symbol copy keeps transport logging separate from OBS.
Stream credentials are never handed to FFmpeg; FFmpeg only muxes encoded
H.264/AAC into FLV through a custom I/O callback. TLS uses the shared Mbed TLS
runtime and the certificate verification in OBS's RTMP implementation.

## Español

`librtmp/` y `happy-eyeballs/` son copias fijadas de
[OBS Studio 32.2.2](https://github.com/obsproject/obs-studio/tree/32.2.2):
`plugins/obs-outputs/librtmp` y `shared/happy-eyeballs`, respectivamente.
La licencia LGPL 2.1 o posterior de librtmp está en `librtmp/COPYING`;
la licencia MIT de Happy Eyeballs se conserva en sus cabeceras.

Esta copia privada con símbolos ocultos separa los registros del transporte
de los de OBS. Las claves nunca se entregan a FFmpeg: sólo empaqueta H.264/AAC
codificado como FLV mediante una función de escritura propia. TLS utiliza
Mbed TLS compartido y la verificación de certificados del transporte RTMP de OBS.
