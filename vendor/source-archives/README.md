# Corresponding dependency sources / Fuentes correspondientes

## English

These unmodified upstream archives accompany the release source package so the
new statically linked Windows dependencies can be rebuilt without depending on
an external source-code download remaining available. They are **source only**:
CMake does not compile or install these archives into the plugin. The native
build uses the pinned OBS dependency bundle recorded in `buildspec.json`.

| Archive | Upstream | Source version |
| --- | --- | --- |
| `mbedtls-3.6.4.tar.bz2` | [Mbed TLS release source](https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2) | `c765c831e5c2a0971410692f92f7a81d6ec65ec2`; includes the framework sources. |
| `zlib-1.3.1.tar.gz` | [zlib release source](https://zlib.net/fossils/zlib-1.3.1.tar.gz) | OBS pins `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf`. |
| `obs-deps-2026-07-15.tar.gz` | [OBS dependency build recipes](https://github.com/obsproject/obs-deps/tree/2026-07-15) | Pinned recipes, toolchain options and patches. |

The OBS recipes `deps.ffmpeg/60-mbedtls.ps1` and `10-zlib.ps1` specify the Windows
build configuration. Apply their included patches to the corresponding source
trees: `patches/mbedtls/0001-enable-dtls-srtp-support-windows.patch` and
`patches/zlib/0001-fix-unistd-detection.patch`. The full OBS build scripts and
instructions are included in the recipe archive. Mbed TLS uses its
GPL-2.0-or-later licensing option for this distribution; see the packaged notices
in `data/licenses/README.md`. Upstream license texts and public test fixtures are
preserved inside the source archives. Test certificates/keys are not user secrets.

## Español

Estos archivos originales acompañan al paquete de fuentes de la release para
poder recompilar las nuevas dependencias enlazadas estáticamente en Windows sin
depender de que una descarga externa de código siga disponible. Son **sólo
fuentes**: CMake no compila ni instala estos archivos dentro del plugin. El build
nativo usa el paquete de dependencias OBS fijado en `buildspec.json`.

La tabla anterior identifica las fuentes originales, versiones y recetas. El
archivo Mbed TLS incluye también su framework. Las recetas OBS
`deps.ffmpeg/60-mbedtls.ps1` y `10-zlib.ps1` especifican la configuración Windows.
Aplica a los árboles de fuentes los parches incluidos
`patches/mbedtls/0001-enable-dtls-srtp-support-windows.patch` y
`patches/zlib/0001-fix-unistd-detection.patch`. El archivo de recetas contiene los
scripts completos y las instrucciones de OBS. Para esta distribución se elige
la opción GPL-2.0-or-later de Mbed TLS; consulta `data/licenses/README.md`.
Se conservan las licencias originales y los fixtures públicos de prueba dentro
de los archivos. Sus certificados/claves de test no son secretos del usuario.

## SHA-256

```text
ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110  mbedtls-3.6.4.tar.bz2
9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23  zlib-1.3.1.tar.gz
88a2d9c818b1d8cc9502908aff4e632640d1eefb75e11f401ad596acd8bf3626  obs-deps-2026-07-15.tar.gz
```
