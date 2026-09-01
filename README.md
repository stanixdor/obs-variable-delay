# OBS Dynamic Delay

Plugin nativo para OBS Studio que permite **añadir, cancelar, cambiar o quitar delay sin detener la emisión ni la grabación**. Trabaja sobre los paquetes ya comprimidos que OBS entrega al output, por lo que el coste estable es principalmente RAM y no una segunda codificación permanente.

Versión: `1.0.0`<br>
Compatibilidad de referencia: OBS Studio `32.2.2`, Qt `6.11.1`, macOS 13+ y Windows 10/11 x64.

## Qué hace

- Controla streaming y grabación activos de forma independiente, también cuando funcionan a la vez.
- Permite elegir de 1 a 300 segundos con slider y campo numérico.
- Activa, cancela o elimina el delay sin reiniciar el output.
- Mantiene el uso normal de OBS: se pueden cambiar escenas y operar Studio con el delay activo.
- Muestra una escena elegida mientras construye el buffer inicial.
- Calcula la RAM prevista a partir del bitrate real observado o del bitrate configurado.
- Incluye una vista de audiencia plegable, a 320×180 y 2 fps, que se captura únicamente mientras está abierta.
- Conserva todas las pistas de audio codificadas y hace los empalmes de vídeo únicamente en keyframes.
- Se rearma por separado tras una reconexión de streaming y vuelve a un estado seguro si detecta un formato no compatible o un límite de RAM.

## Flujo exacto

1. El usuario elige duración y escena de espera y pulsa **Add delay**.
2. El programa sigue en directo mientras se prepara un encoder auxiliar compatible y se espera un keyframe seguro.
3. En ese keyframe, el output pasa a la escena de espera y el contenido normal empieza a guardarse como paquetes comprimidos.
4. Al completar el tiempo configurado, el output empieza a emitir ese buffer con el delay elegido. El encoder auxiliar se apaga.
5. Al pulsar **Remove delay / cancel**, el plugin sigue entregando una salida válida hasta el siguiente keyframe vivo y vuelve entonces al directo, sin parar streaming o grabación.

Si se cancela durante la preparación o el llenado, el plugin vuelve del mismo modo a la señal viva. Cambiar segundos o escena con el delay activo ejecuta un rearme automático con la nueva configuración; el output permanece activo.

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

## Limitaciones deliberadas de la versión 1.0

- **Transición:** el modo de producción es `Cut on keyframe`. La opción de fade aparece deshabilitada porque un crossfade real entre vídeo ya retrasado y vídeo vivo exige decodificar, componer y volver a codificar toda la señal. Eso contradice el objetivo de bajo consumo y puede degradar imagen y latencia.
- **Audio de la escena de espera:** durante el llenado se emite silencio codificado en todas las pistas. El vídeo de la escena sí se reproduce. La API pública de libobs no ofrece un mixer privado, independiente y estable para renderizar el audio arbitrario de una segunda escena; reutilizar el mix global puede duplicar o saltar bloques.
- **Quitar delay:** la vuelta es inmediata en términos de control, pero el empalme efectivo espera el siguiente keyframe para mantener el stream decodificable. El máximo habitual es el intervalo GOP configurado.
- **Vista de audiencia:** representa aproximadamente los frames enviados por OBS. No incluye el buffer del servidor, CDN ni reproductor del espectador. Durante la escena de espera muestra su estado en lugar de mantener otra renderización auxiliar.
- Se admiten outputs codificados con audio y vídeo. No se admiten outputs raw, solo-audio, solo-vídeo, el delay nativo de OBS ni `Multi-track Video / Enhanced Broadcasting`.
- El binario Windows se compila y valida estructuralmente contra la distribución oficial de OBS 32.2.2; la prueba funcional completa de esta entrega se ha realizado en macOS arm64.

## Arquitectura

El callback síncrono de paquetes de cada output actúa como un portador de ritmo. El plugin conserva sus timestamps de pared, pista, encoder y timebase y sustituye únicamente el payload por el paquete correspondiente del buffer. Cada cambio de procedencia crea una nueva época temporal y aplica un puente común A/V cuando hace falta, evitando PTS no monótonos con encoders que usan B-frames.

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

- 26 casos del núcleo: delay exacto, cancelación, múltiples pistas, keyframes, generaciones/rearmado, límites de memoria, timebases no unitarias y continuidad A/V.
- Build estricto con warnings como errores.
- ASan + UBSan, y TSan sobre el núcleo.
- Prueba real en OBS 32.2.2 macOS arm64: preparar, llenar, cambiar de escenas, preview plegable, cancelar tanto en preparación como en llenado, activar delay y volver a live sin detener la grabación.
- Análisis con FFmpeg del archivo resultante: 0 regresiones DTS, 0 regresiones de PTS decodificado y decodificación completa sin errores.
- DLL Windows x64: formato PE32+ validado, ocho exports OBS presentes e imports resueltos contra la distribución oficial.

## Licencia

GPL-2.0-or-later. Consulta `LICENSE`.
