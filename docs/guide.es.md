# OBS Dynamic Delay

Plugin nativo para OBS Studio que permite **añadir, cancelar, cambiar o quitar delay sin detener la emisión ni la grabación**. Trabaja sobre los paquetes ya comprimidos que OBS entrega al output, por lo que el coste estable es principalmente RAM y no una segunda codificación permanente.

Versión: `1.2.0`<br>
Compatibilidad de referencia: OBS Studio `32.2.x`, Qt 6, macOS 13+, Windows 10/11 x64 y Ubuntu 24.04 x86_64.

La versión 1.2.0 añade un panel de multistream integrado y autónomo en **macOS, Windows y Linux**. Hasta ocho
destinos RTMP/RTMPS comparten un stream Program con delay sin necesitar que el directo nativo de OBS esté activo.
Se mantienen los controles de delay y las correcciones de funcionamiento y rendimiento de 1.1.2. Descarga los
[artefactos y checksums oficiales 1.2.0](https://github.com/stanixdor/obs-variable-delay/releases/tag/1.2.0).

## Qué hace

- Controla streaming y grabación activos de forma independiente, también cuando funcionan a la vez.
- Envía un stream con delay compartido a hasta ocho destinos RTMP/RTMPS que se inician y detienen por separado.
- Permite elegir de 1 a 300 segundos con slider y campo numérico.
- Activa, cancela o elimina el delay sin reiniciar el output.
- Mantiene el uso normal de OBS: se pueden cambiar escenas y operar Studio con el delay activo.
- Muestra una escena elegida mientras construye el buffer inicial.
- Calcula la RAM prevista a partir del bitrate real observado o del bitrate configurado.
- Incluye una vista de audiencia plegable, reducida a 320×180 en la GPU antes de leer a RAM, a un máximo de 2 fps.
- Reproduce audio durante la escena de espera mediante un mezclador privado; incluye modos de escena, fuente
  dedicada, pista reservada y silencio explícito.
- Conserva el layout de pistas del output y hace los empalmes de vídeo únicamente en keyframes.
- Se rearma por separado tras una reconexión de streaming y vuelve a un estado seguro si detecta un formato no compatible o un límite de RAM.

## Flujo exacto

1. El usuario elige duración y escena de espera y pulsa **Add delay**.
2. El programa sigue en directo mientras se preparan encoders auxiliares compatibles, se valida el audio y se
   espera un keyframe seguro.
3. En ese keyframe, el output pasa a la escena de espera con el modo de audio elegido y el contenido normal
   empieza a guardarse como paquetes comprimidos.
4. Al completar el tiempo configurado, el output empieza a emitir ese buffer con el delay elegido. El encoder auxiliar se apaga.
5. Al pulsar **Remove delay / cancel**, el plugin sigue entregando una salida válida hasta el siguiente keyframe vivo y vuelve entonces al directo, sin parar streaming o grabación.

Si se cancela durante la preparación o el llenado, el plugin vuelve del mismo modo a la señal viva. Cambiar segundos o escena con el delay activo ejecuta un rearme automático con la nueva configuración; el output permanece activo.

La pausa de grabación detiene el progreso de llenado según el reloj de medios y pausa el output auxiliar privado.
Al reanudar continúa el mismo buffer, sin contar el tiempo pausado como contenido nuevo. Una reconexión de streaming
vacía las colas y el puente temporal anterior antes del primer paquete de audio o vídeo de la nueva época; después
se rearma.

Si el encoder pierde frames o aparece un hueco en una pista de audio, el plugin puede realinear todas las pistas
de audio activas y el vídeo en un GOP completo del buffer. Esta recuperación puede acortar el delay efectivo; no
decodifica/recodifica Program, no empalma en P/B-frames dependientes ni muestra otra escena de espera. El slider
configurado y el frame realmente emitido son distintos: la preview sigue el timestamp de captura emitido, no un
valor del slider que todavía no se ha aplicado.

## Multistream integrado

Abre **Paneles/Docks → Delay dinámico — Multistream**. Este panel gestiona los destinos; los segundos de delay,
la escena de espera, su audio y la vuelta a live siguen controlándose desde **Delay dinámico**.

1. Configura la salida de streaming de OBS con **vídeo H.264 y audio AAC**. Usa el canvas principal de Program,
   SDR NV12/I420 y audio mono o estéreo. La pista principal de audio de streaming seleccionada en OBS será la
   pista de audio compartida.
2. Selecciona **Añadir destino** e introduce un nombre, un servidor como `rtmps://example.com/live` y la clave de
   emisión. Incluye los parámetros de consulta que facilite el servicio en la clave, no en la dirección del
   servidor. Se rechazan datos de usuario, espacios, consultas y fragmentos en el servidor. La clave se oculta
   salvo que marques expresamente **Mostrar clave de emisión**.
3. Guarda y pulsa **Iniciar** en cada destino que quieras usar. Guardar no publica nada y los destinos guardados
   nunca arrancan automáticamente al abrir OBS. Se pueden guardar hasta ocho.
4. Usa **Add delay** o **Remove delay / cancel** en Delay dinámico. Todos los destinos multistream del plugin
   reciben el mismo stream maestro con delay; no tienen buffers de delay independientes.
5. Detén un destino antes de editarlo o eliminarlo. El borrado pide confirmación. Detener uno no detiene los demás
   destinos ni los outputs nativos de streaming/grabación de OBS.

El panel indica conectando, esperando keyframe, en directo, reconectando, detenido y error. Un destino nuevo o
reconectado espera un keyframe seguro del stream compartido actual; no reproduce todo el historial de delay ni
inserta otra espera para los demás. Un servidor lento puede retrasarse o reconectar de forma independiente: que
el contenido codificado sea idéntico no implica que llegue a la vez ni que los reproductores tengan igual latencia.

### Codificación, subida y alcance

Al iniciar el primer destino, se reutilizan los encoders de un **stream nativo de OBS compatible ya activo**.
En caso contrario, una pareja privada de encoders usa los ajustes de streaming configurados en OBS. Todos los
destinos del plugin comparten esa pareja y **un** buffer de delay. Iniciar después el streaming nativo puede
añadir trabajo de codificación; el plugin no sustituye al gestor de outputs de OBS ni trasplanta encoders activos.

El streaming y la grabación nativos conservan sus propias sesiones de delay. El maestro multistream integrado es
otra sesión gestionada por los mismos controles de delay. No se modifican plugins de terceros ni sus hooks de
output. Multistream no ofrece distintos bitrates, mezclas de audio, resoluciones o canvases por destino. Admite
una pista de vídeo H.264 y una de audio AAC, no HEVC, AV1, Opus, HDR/10-bit, surround, Enhanced Broadcasting ni
canvases independientes. Detén todos los destinos antes de cambiar la configuración de codificación de origen.

Cada destino añade otra copia en la red: con un total de 8 Mbit/s, tres destinos necesitan unos 24 Mbit/s de subida
más el overhead del protocolo. Los payloads comprimidos se comparten en memoria entre colas de destino acotadas;
cada destino sigue teniendo costes de socket, multiplexado y cola. Añadir uno no crea otro encoder Program ni
otro buffer completo de delay. Preparing/Filling sigue necesitando el encoder temporal de espera descrito abajo.

### Credenciales y transporte

Nombres, URLs y claves se guardan en `multistream.json`, dentro de la configuración local del plugin en OBS, con
permisos de lectura/escritura sólo para el propietario. **No es cifrado ni un almacén de credenciales del sistema.**
Otro software ejecutado con tu cuenta puede leerlo. No publiques el archivo, no lo incluyas en copias públicas ni
compartas capturas con la clave visible. La lista muestra sólo el host del servidor, no rutas de URL que puedan
contener información sensible.

RTMPS verifica los certificados y nombres de servidor mediante el transporte librtmp privado derivado de OBS.
FFmpeg se usa para multiplexar FLV, no como encoder adicional ni transporte de red. RTMP no está cifrado; usa RTMPS
si tu proveedor lo admite. El plugin no registra claves ni errores de red crudos que puedan contener credenciales.
Al pedir ayuda, elimina los datos de destinos de cualquier log externo de OBS o del proveedor que adjuntes.

## Audio de la escena de espera

El modo predeterminado es **Scene mix (recommended)**. Libobs renderiza la escena de espera en una vista privada;
el plugin copia su PCM ya mezclado a una FIFO SPSC acotada y lo entrega a un reloj de audio privado. No asigna,
silencia ni modifica fuentes, pistas o buses globales de OBS.

- **Scene mix:** conserva el routing de pistas de las fuentes de la escena. Es la opción normal.
- **Dedicated source:** usa una única fuente de audio exclusiva para la espera. Conserva la asignación de pistas
  de esa fuente.
- **Reserved OBS track (advanced):** toma el mix de una pista OBS 1-6 reservada y lo replica en las pistas de
  audio del output de espera. Esa pista no puede estar siendo codificada por ningún output activo.
- **Silence:** silencio intencional, útil como política explícita o para diagnosticar configuraciones.

Antes de cada activación se hace un *preflight* conservador que recorre escenas, grupos, elementos ocultos,
canvases, fuentes y outputs activos. Si `Scene mix` comparte una fuente de audio con otra escena o con Program,
se usa la fuente dedicada configurada cuando es exclusiva; si tampoco es segura, sólo el audio de espera cae a
silencio. El vídeo y el delay siguen funcionando. Si la topología cambia durante el llenado, la degradación a
silencio queda fijada hasta la siguiente activación para evitar duplicar audio en Program.

Para configurar **Reserved OBS track**:

1. En **Propiedades de audio avanzadas**, asigna a la pista elegida únicamente las fuentes de la escena de espera.
2. Quita esa pista de todas las demás fuentes.
3. No selecciones esa pista para streaming, grabación, Replay Buffer, Virtual Camera ni otro output/plugin activo.
4. Selecciona la misma pista en el panel. El diagnóstico debe confirmar que es exclusiva antes de activar el delay.

## Instalación

### macOS

1. Cierra OBS.
2. Copia `obs-dynamic-delay.plugin` en:

   ```text
   ~/Library/Application Support/obs-studio/plugins/
   ```

3. Abre OBS y activa **Paneles/Docks → Delay dinámico**.

Usa el ZIP universal como descarga recomendada. Su bundle tiene firma *ad hoc*. El PKG universal alternativo no
tiene firma Developer ID ni está notarizado por Apple; macOS puede pedir autorización explícita. Comprueba la
descarga con [SHA256SUMS.txt](https://github.com/stanixdor/obs-variable-delay/releases/download/1.2.0/SHA256SUMS.txt).

### Windows x64

1. Cierra OBS.
2. Abre esta ruta en la barra de direcciones del Explorador de archivos (la carpeta `ProgramData` está oculta
   normalmente). Si `obs-studio\plugins` no existe, créala; Windows puede pedir permisos de administrador:

   ```text
   %PROGRAMDATA%\obs-studio\plugins\
   ```

3. Extrae ahí **la carpeta completa `obs-dynamic-delay`** del ZIP. No copies `bin` y `data` sueltas en la
   carpeta de instalación de OBS y no uses `%APPDATA%`.
4. Comprueba que el DLL termine exactamente en:

   ```text
   %PROGRAMDATA%\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll
   ```

   También puedes comprobarlo desde PowerShell:

   ```powershell
   Test-Path "$env:ProgramData\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll"
   ```

   El resultado debe ser `True`.
5. Abre OBS y activa **Paneles/Docks → Dynamic Delay**.

Estas rutas corresponden a una instalación normal, no portable, de OBS. Si el panel no aparece, abre
**Ayuda → Archivos de registro → Ver registro actual** y comprueba que `obs-dynamic-delay.dll` figure entre los
módulos cargados.

Si el panel muestra **ERROR / LIVE FALLBACK**, lee primero el texto pequeño situado debajo del estado: contiene
la causa exacta. El mismo motivo aparece en el registro con el prefijo `[obs-dynamic-delay]`. Al solicitar ayuda,
adjunta el registro generado después de reproducir el fallo; los registros están en `%APPDATA%\obs-studio\logs\`.

El DLL incluido no lleva firma Authenticode. La firma para distribución pública requiere un certificado de firma de código del editor.

### Ubuntu 24.04 x86_64

El paquete Debian es la opción recomendada para una instalación nativa. La release apunta a la ABI de módulos
OBS Studio 32.2.x en Ubuntu 24.04 x86_64, no a cualquier ABI posterior. Instala un build compatible de OBS 32.2.x;
el PPA oficial de OBS es una opción, pero comprueba la versión que ofrece antes de instalar:

```bash
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt update
sudo apt install ./obs-dynamic-delay-1.2.0-x86_64-linux-gnu.deb
```

Como archivo alternativo, el `tar.xz` contiene el layout estándar `lib/share` para `/usr`:

```bash
sudo tar -xJf obs-dynamic-delay-1.2.0-x86_64-ubuntu-gnu.tar.xz -C /usr
```

Reinicia OBS y activa **Docks → Dynamic Delay**. Estos dos artefactos están construidos para OBS nativo en
Ubuntu 24.04 x86_64; no se presentan como paquetes para Flatpak, Snap ni otras distribuciones.

## Uso y rendimiento

La estimación de memoria usa `bitrate total × segundos ÷ 8`, con un margen del 20 %. Con CQP, CRF, ICQ, lossless o
una pista sin bitrate fijo, el panel mide el valor real tras una ventana de dos segundos de output. Al abrir la
preview se añade historial RGB16: un historial normal de 300 segundos consume unos 67 MiB. También conserva el
delay efectivo actual y los frames anteriores a una pausa que sigan siendo necesarios, con una cantidad de frames
acotada. El buffer principal de paquetes tiene un límite de seguridad de 4 GiB y el preroll auxiliar, de 128 MiB.

Durante **Preparing/Filling** se crea un segundo encoder de vídeo con la misma configuración que el output. Esto es necesario para producir la escena de espera con headers compatibles. Se destruye al entrar en **Delay active**. Para el menor impacto al jugar:

- usa un encoder por hardware;
- mantén cerrada la vista de audiencia cuando no la necesites;
- evita ejecutar streaming y grabación con configuraciones distintas si no necesitas ambos outputs;
- configura un intervalo de keyframes razonable (por ejemplo, 2 segundos), ya que todas las entradas y salidas esperan un keyframe seguro.

Los outputs simultáneos pueden compartir la vista, el reloj y el puente PCM de la escena de espera. Los outputs
compatibles también reutilizan un conjunto auxiliar completo de codificación, únicamente si
coinciden el encoder de vídeo original, todos los encoders de audio y sus posiciones de pista, y el hub de medios
de espera. No basta con que coincida el vídeo: las coincidencias parciales conservan encoders auxiliares privados.
Los outputs que coinciden por completo ya comparten los encoders originales; la pausa auxiliar sigue ese estado
compartido, también si la grabación admite pausa. Detener un suscriptor no destruye el conjunto mientras otro lo
necesite.

El puente PCM es fijo y acotado: 16 bloques × 1024 frames × 6 mixes × 8 canales, unos 3 MiB por activación, sin
depender de los segundos de delay. El modo **Silence** explícito no reserva esta FIFO. Se eliminan borrados y copias
PCM redundantes, conservando el mismo routing y comportamiento de silencio. El lookahead interno es de un quantum
de OBS, unos 21,3 ms a 48 kHz.

La preview reutiliza la textura Program que OBS ya ha renderizado: reduce a 320×180 en la GPU y lee a RAM como
máximo dos veces por segundo, mapeando la transferencia en el siguiente frame de vídeo. No registra un consumidor
de vídeo raw, no fuerza lecturas por frame a resolución completa ni mantiene `obs_video_active` activo por sí sola.
Plegarla libera captura, recursos GPU e historial. Ocultar el panel completo conserva el historial de baja
frecuencia para reabrirlo, pero detiene el repintado. Durante una pausa de grabación suspende la captura y conserva
el historial necesario para reanudar. Si coexisten streaming y grabación, la preview sigue el streaming; en caso
contrario sigue la grabación. No incluye la latencia del servidor, CDN ni reproductor.

## Limitaciones deliberadas de la versión 1.2.0

- **Transición:** el modo de producción es `Cut on keyframe`. La opción de fade aparece deshabilitada porque un crossfade real entre vídeo ya retrasado y vídeo vivo exige decodificar, componer y volver a codificar toda la señal. Eso contradice el objetivo de bajo consumo y puede degradar imagen y latencia.
- **Empalme de audio:** el cambio entre el encoder principal y el auxiliar puede introducir un hueco muy breve de
  una o dos tramas AAC. No existe ya el silencio de varios segundos durante el llenado.
- **Alineación del codec:** la reordenación B-frame y los límites de los paquetes AAC pueden añadir un pequeño
  desfase A/V en el empalme. No es exacto por muestra; no se decodifica/recodifica Program para eliminarlo.
- **Fallo de pausa nativo de OBS:** OBS 32.2.2 puede bloquearse tras pausar una grabación x264 con divisor de FPS 2,
  también sin el plugin. Usa divisor 1 si necesitas pausar; el plugin no parchea los internos de OBS.
- **Seguridad de audio:** una fuente compartida o una pista reservada que deje de ser exclusiva provoca silencio
  sólo en la escena de espera. El plugin no altera el routing global para intentar corregirlo automáticamente.
- **Quitar delay:** la vuelta es inmediata en términos de control, pero el empalme efectivo espera el siguiente keyframe para mantener el stream decodificable. El máximo habitual es el intervalo GOP configurado.
- **Vista de audiencia:** representa aproximadamente los frames enviados por OBS. No incluye el buffer del servidor, CDN ni reproductor del espectador. Durante la escena de espera muestra su estado en lugar de mantener otra renderización auxiliar.
- Se admiten outputs codificados con audio y vídeo. Hybrid MP4/MOV y RTMP normales son compatibles cuando
  tienen exactamente una pista de vídeo activa. No se admiten outputs raw, solo-audio, solo-vídeo, el delay
  nativo de OBS ni layouts con varias pistas de vídeo como `Enhanced Broadcasting`.
- La recuperación segura tras huecos del encoder puede acortar el delay efectivo hasta un GOP alineado en lugar
  de conservar un desfase A/V incorrecto. No se puede garantizar el delay exacto si faltan paquetes de origen.
- La compatibilidad runtime de codecs y drivers requiere pruebas con OBS real; superar las suites sin SDK no
  certifica por sí solo un flujo completo de grabación en Windows, macOS ni Linux.

## Arquitectura

El callback síncrono de paquetes de cada output actúa como un portador de ritmo. El plugin conserva sus timestamps de pared, pista, encoder y timebase y sustituye únicamente el payload por el paquete correspondiente del buffer. Cada cambio de procedencia crea una nueva época temporal y aplica un puente común A/V cuando hace falta, evitando PTS no monótonos con encoders que usan B-frames.

El audio auxiliar no usa el bus de Program. Una fuente privada `COMPOSITE` participa en el árbol de render de
audio de la vista auxiliar sin registrarse como fuente global de audio. El productor de libobs escribe bloques PCM
fijos en una FIFO SPSC; el consumidor, que corre en el reloj privado, aplica una sola corrección de fase/sync y
después avanza por frames para evitar drift. Si el backend deja de ser seguro, un kill-switch atómico vacía la
enumeración de audio sin desmontar la vista de vídeo. Los observadores usan conexiones RAII al `signal_handler`,
por lo que no mantienen vivas escenas o fuentes creadas por otros plugins.

El estado por output es:

```text
LIVE → PREPARING → FILLING → DELAY ACTIVE → RETURNING LIVE → LIVE
                    ↘ cancelación ────────────────↗
```

La máquina de estados de producción está en `src/output-session.cpp`; las pruebas compilan ese mismo archivo y
ejercitan su callback de paquetes registrado con fronteras controladas de libobs y del encoder auxiliar. El modelo
separado `src/core/packet_delay.cpp` no se enlaza al plugin y sus tests no demuestran el comportamiento de producción.
En cambio, `src/core/preview-timing.hpp` y la FIFO PCM sí se usan en producción y se prueban directamente.

APIs de OBS usadas como contrato:

- [Callbacks de paquetes del output](https://docs.obsproject.com/reference-outputs#c.obs_output_add_packet_callback)
- [Outputs y delay nativo](https://docs.obsproject.com/reference-outputs#c.obs_output_set_delay)
- [Encoders](https://docs.obsproject.com/reference-encoders)
- [Frontend API](https://docs.obsproject.com/reference-frontend-api)
- [Displays y vistas](https://docs.obsproject.com/frontends#displays)

## Compilación

El proyecto parte del sistema oficial `obs-plugintemplate`; descarga y fija por hash las fuentes de OBS 32.2.2, `obs-deps` y Qt 6.11.1 definidos en `buildspec.json`.

Multistream enlaza las bibliotecas `avformat`, `avcodec` y `avutil` de FFmpeg para multiplexar FLV, además de mbedTLS
y zlib para el transporte librtmp privado derivado de OBS. macOS/Windows usan el bundle de dependencias de OBS, sin
distribuir otro runtime de FFmpeg ni Qt. Linux necesita `libavformat-dev`, `libavcodec-dev`, `libavutil-dev`,
`libmbedtls-dev` y `zlib1g-dev`, además de las dependencias siguientes. Consulta los avisos upstream en `vendor/`.

### macOS arm64 local

Requiere OBS 32.2.2 instalado en `/Applications/OBS.app`, Xcode Command Line Tools, CMake y Ninja:

```bash
cmake --preset macos-local-arm64 -DENABLE_TESTS=ON
cmake --build --preset macos-local-arm64 --parallel
ctest --test-dir build_macos_arm64 --output-on-failure
```

### macOS universal

Requiere Xcode 16 completo —no basta con Command Line Tools— y CMake 3.28 o posterior:

```bash
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo --parallel
ctest --test-dir build_macos --build-config RelWithDebInfo --output-on-failure
```

### Windows x64

Desde un Developer PowerShell de Visual Studio 2022 con CMake 3.28+:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
ctest --test-dir build_x64 --build-config RelWithDebInfo --output-on-failure
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

### Ubuntu 24.04 x86_64

Requiere CMake 3.28+, Ninja, GCC, Qt 6 y la instalación nativa de OBS Studio 32.2.x con sus archivos de desarrollo:

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --parallel
ctest --test-dir build_x86_64 --output-on-failure
cmake --install build_x86_64 --prefix release/RelWithDebInfo
```

Los workflows incluidos construyen y prueban macOS universal, Windows x64 y Ubuntu 24.04 x86_64 en GitHub
Actions. Las releases incluyen ZIP y PKG universales de macOS, ZIP de Windows, archivo y paquete Debian de Linux,
símbolos de depuración, archivo de fuentes y `SHA256SUMS.txt`. El tag debe coincidir con la versión de
`buildspec.json`; se permiten sufijos de prerelease
como `-beta2` o `-rc1`.

## Validación de 1.2.0

La validación de la release está en curso. Los resultados de los binarios, la UI, el transporte, las pruebas
loopback con libobs real y los paquetes multiplataforma se registrarán en las
[notas 1.2.0](releases/1.2.0.md#validación). La integración multistream usa únicamente destinos loopback; no publica
en canales reales.

### Cobertura histórica del delay (1.1.2)

Las cinco suites sin SDK del delay pasaron para 1.1.2 en macOS con warnings como errores, ASan+UBSan y TSan:

- **Casos de OutputSession de producción:** callbacks reales, referencias de paquetes, limpieza al
  cancelar/rearmar, llenado según reloj de medios al pausar, reconexión con audio/vídeo primero, timestamps de
  composición B-frame, huecos de vídeo o de pistas de audio durante Filling y Delayed, alineación multipista,
  límites de RAM y fallback seguro.
- **Casos de HoldPipeline de producción:** compartir layouts completos, encoders aislados, suscriptores tardíos,
  pausa coordinada, bajas concurrentes y limpieza tras fallo de arranque, con una frontera de libobs controlada.
- **5 casos de timing de preview** y **9 escenarios de FIFO de audio**, ambos de producción.
- **26 casos del modelo separado de paquetes**, conservados como cobertura del modelo, no como prueba de la sesión
  real.

Ejecución sin SDK de OBS:

```bash
cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

Los targets opcionales de integración `preview_gpu_integration` y `output_session_integration` usan libobs real y
un backend gráfico. Se habilitan con `-DENABLE_OBS_INTEGRATION_TESTS=ON` al compilar con el SDK de OBS; son
independientes de las cinco suites sin SDK y esos comandos CTest no los ejecutan automáticamente. Sus resultados
runtime nativos deben comunicarse por separado; consulta la [validación histórica 1.1.2](releases/1.1.2.md#validación).

La grabación de 174,002 segundos y las pruebas de arranque Linux con Docker/Xvfb/llvmpipe corresponden a **1.1.1**,
no a 1.2.0. Sus resultados históricos y limitaciones siguen en las
[notas de la versión 1.1.1](releases/1.1.1.md#validación).

## Licencia

GPL-2.0-or-later. Consulta `LICENSE`.
