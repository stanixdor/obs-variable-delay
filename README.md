# OBS Dynamic Delay

Plugin nativo para OBS Studio que permite **añadir, cancelar, cambiar o quitar delay sin detener la emisión ni la grabación**. Trabaja sobre los paquetes ya comprimidos que OBS entrega al output, por lo que el coste estable es principalmente RAM y no una segunda codificación permanente.

Versión: `1.1.0`<br>
Compatibilidad de referencia: OBS Studio `32.2.2`, Qt `6.11.1`, macOS 13+ y Windows 10/11 x64.

## Qué hace

- Controla streaming y grabación activos de forma independiente, también cuando funcionan a la vez.
- Permite elegir de 1 a 300 segundos con slider y campo numérico.
- Activa, cancela o elimina el delay sin reiniciar el output.
- Mantiene el uso normal de OBS: se pueden cambiar escenas y operar Studio con el delay activo.
- Muestra una escena elegida mientras construye el buffer inicial.
- Calcula la RAM prevista a partir del bitrate real observado o del bitrate configurado.
- Incluye una vista de audiencia plegable, a 320×180 y 2 fps, que se captura únicamente mientras está abierta.
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

El artefacto local está firmado *ad hoc*. No está notarizado con una cuenta Apple Developer, por lo que macOS puede pedir autorización al instalarlo fuera de este equipo.

### Windows x64

1. Cierra OBS.
2. Extrae la carpeta `obs-dynamic-delay` del ZIP dentro de:

   ```text
   %APPDATA%\obs-studio\plugins\
   ```

3. Comprueba que exista esta ruta:

   ```text
   %APPDATA%\obs-studio\plugins\obs-dynamic-delay\bin\64bit\obs-dynamic-delay.dll
   ```

4. Abre OBS y activa **Docks → Dynamic Delay**.

El DLL incluido no lleva firma Authenticode. La firma para distribución pública requiere un certificado de firma de código del editor.

## Uso y rendimiento

La estimación de memoria usa `bitrate total × segundos ÷ 8`, con un margen del 20 %. Con CQP, CRF, ICQ, lossless o una pista sin bitrate fijo, el panel indica que está midiendo y muestra el valor real tras una ventana de dos segundos de output. Al abrir la vista de audiencia se suma su historial RGB16; en el peor caso de 300 segundos son aproximadamente 67 MiB adicionales. El buffer principal tiene un límite de seguridad de 4 GiB y el preroll auxiliar uno de 128 MiB.

Durante **Preparing/Filling** se crea un segundo encoder de vídeo con la misma configuración que el output. Esto es necesario para producir la escena de espera con headers compatibles. Se destruye al entrar en **Delay active**. Para el menor impacto al jugar:

- usa un encoder por hardware;
- mantén cerrada la vista de audiencia cuando no la necesites;
- evita ejecutar streaming y grabación con configuraciones distintas si no necesitas ambos outputs;
- configura un intervalo de keyframes razonable (por ejemplo, 2 segundos), ya que todas las entradas y salidas esperan un keyframe seguro.

Streaming y grabación simultáneos comparten una sola vista, reloj y FIFO de la escena de espera. El puente PCM es
fijo y acotado: 16 bloques × 1024 frames × 6 mixes × 8 canales, aproximadamente 3 MiB por activación, sin
reservar audio proporcional a los segundos de delay. Su lookahead interno es de un quantum de OBS, unos 21,3 ms
a 48 kHz.

## Limitaciones deliberadas de la versión 1.1

- **Transición:** el modo de producción es `Cut on keyframe`. La opción de fade aparece deshabilitada porque un crossfade real entre vídeo ya retrasado y vídeo vivo exige decodificar, componer y volver a codificar toda la señal. Eso contradice el objetivo de bajo consumo y puede degradar imagen y latencia.
- **Empalme de audio:** el cambio entre el encoder principal y el auxiliar puede introducir un hueco muy breve de
  una o dos tramas AAC. No existe ya el silencio de varios segundos durante el llenado.
- **Seguridad de audio:** una fuente compartida o una pista reservada que deje de ser exclusiva provoca silencio
  sólo en la escena de espera. El plugin no altera el routing global para intentar corregirlo automáticamente.
- **Quitar delay:** la vuelta es inmediata en términos de control, pero el empalme efectivo espera el siguiente keyframe para mantener el stream decodificable. El máximo habitual es el intervalo GOP configurado.
- **Vista de audiencia:** representa aproximadamente los frames enviados por OBS. No incluye el buffer del servidor, CDN ni reproductor del espectador. Durante la escena de espera muestra su estado en lugar de mantener otra renderización auxiliar.
- Se admiten outputs codificados con audio y vídeo. No se admiten outputs raw, solo-audio, solo-vídeo, el delay nativo de OBS ni `Multi-track Video / Enhanced Broadcasting`.
- El binario Windows se compila y valida estructuralmente contra la distribución oficial de OBS 32.2.2; la prueba funcional completa de esta entrega se ha realizado en macOS arm64.

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

El núcleo puro en `src/core/` modela colas por pista, generaciones, límites y mapeo temporal sin depender de OBS. El adaptador de `src/output-session.cpp` aplica la misma estrategia a `encoder_packet` y gestiona las peculiaridades del lifecycle real de libobs.

APIs de OBS usadas como contrato:

- [Callbacks de paquetes del output](https://docs.obsproject.com/reference-outputs#c.obs_output_add_packet_callback)
- [Outputs y delay nativo](https://docs.obsproject.com/reference-outputs#c.obs_output_set_delay)
- [Encoders](https://docs.obsproject.com/reference-encoders)
- [Frontend API](https://docs.obsproject.com/reference-frontend-api)
- [Displays y vistas](https://docs.obsproject.com/frontends#displays)

## Compilación

El proyecto parte del sistema oficial `obs-plugintemplate`; descarga y fija por hash las fuentes de OBS 32.2.2, `obs-deps` y Qt 6.11.1 definidos en `buildspec.json`.

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
cmake --build build_macos --target packet_delay_tests --config RelWithDebInfo --parallel
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

Los workflows incluidos construyen y prueban macOS universal y Windows x64 en GitHub Actions. Generan un ZIP para Windows y un PKG o `tar.xz` para macOS. El tag de release debe coincidir con la versión de `buildspec.json`; se permiten sufijos de prerelease como `-beta2` o `-rc1`.

## Validación realizada

- 26 casos del núcleo de paquetes: delay exacto, cancelación, múltiples pistas, keyframes,
  generaciones/rearmado, límites de memoria, timebases no unitarias y continuidad A/V.
- 9 escenarios del puente de audio: FIFO acotada/concurrente, offsets positivos/negativos, discontinuidades,
  fase 48 kHz, underrun y ausencia de drift.
- Build estricto con warnings como errores.
- ASan + UBSan y TSan sobre ambos núcleos, sin hallazgos.
- Prueba real en OBS 32.2.2 macOS arm64: iniciar una grabación, cancelar durante `Preparing/Filling`, activar un
  delay completo, cambiar Program de escena, abrir/cerrar el preview y volver a live sin detener el output.
- Grabación de validación de 174,002 s: H.264 1920×1080 + AAC 48 kHz estéreo, decodificación completa sin
  errores y 0 regresiones DTS en 10.425 paquetes de vídeo y 8.154 de audio.
- Audio no silencioso medido durante ambas escenas de espera: −21,5 dB y −21,9 dB de volumen medio.
- DLL Windows x64: formato PE32+ validado, ocho exports OBS presentes e imports resueltos contra la distribución oficial.

## Licencia

GPL-2.0-or-later. Consulta `LICENSE`.
