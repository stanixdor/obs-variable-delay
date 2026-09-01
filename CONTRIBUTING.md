# Contributing / Cómo contribuir

[English](#english) · [Español](#español)

## English

Thank you for helping improve OBS Dynamic Delay. Bug reports, technical documentation, translations, tests, native
plugin changes, and website improvements are welcome in English or Spanish.

### Before opening an issue

1. Search [existing issues](https://github.com/stanixdor/obs-variable-delay-web/issues).
2. Reproduce the problem with the
   [latest release](https://github.com/stanixdor/obs-variable-delay-web/releases/latest).
3. Read the [English guide](docs/guide.en.md), especially the supported-output and audio-safety limitations.
4. If the dock shows **ERROR / LIVE FALLBACK**, copy the exact detail shown below the state.
5. Create an OBS log after reproducing the issue and remove stream keys, credentials, personal paths, and other
   sensitive information before sharing it.

A useful bug report includes:

- operating system and architecture;
- exact OBS Studio and plugin versions;
- streaming or recording container and active encoders;
- video resolution, frame rate, keyframe interval, and audio-track layout;
- requested delay, hold scene, and hold-audio mode;
- exact steps, expected result, actual result, dock error, and sanitized OBS log;
- whether streaming, recording, or both were active.

Use a public issue only for non-sensitive bugs. Follow [SECURITY.md](SECURITY.md) for vulnerabilities.

### Proposing a change

For a significant behavior, architecture, format-support, or UI change, open an issue first so the design and
real-time cost can be discussed. Small fixes and documentation corrections can go directly to a pull request.

1. Fork and clone the repository over HTTPS.
2. Create a focused branch from <code>main</code>.
3. Keep unrelated formatting or refactors out of the change.
4. Add or update tests for observable behavior.
5. Update both English and Spanish user-facing text.
6. Run the relevant local checks.
7. Open a pull request explaining the problem, approach, validation, performance impact, and known tradeoffs.

Do not commit build directories, dependency caches, local OBS data, credentials, signing material, or generated
release artifacts.

### Native plugin development

The native plugin uses C++17, Qt 6, and libobs. The complete platform commands and prerequisites are in the
[English technical guide](docs/guide.en.md#building).

Example local macOS arm64 build:

~~~bash
cmake --preset macos-local-arm64 -DENABLE_TESTS=ON
cmake --build --preset macos-local-arm64 --parallel
ctest --test-dir build_macos_arm64 --output-on-failure
~~~

Formatting checks:

~~~bash
./build-aux/run-clang-format --check --fail-error
./build-aux/run-gersemi --check --fail-error
~~~

Native changes should preserve these invariants:

- no unbounded allocation, blocking I/O, or mutex acquisition in real-time packet or audio callbacks;
- bounded queues and explicit memory limits;
- monotonic timestamps and video splices only on safe keyframes;
- no mutation of global OBS source, bus, mute, or track routing for hold-scene audio;
- safe fallback to live output or hold-audio silence when a format or topology becomes unsafe;
- balanced libobs references and RAII signal connections;
- separate behavior for simultaneous streaming and recording outputs.

Add focused tests under <code>tests/</code> for packet generations, timebases, cancellation, memory limits,
concurrency, audio phase, underrun, or drift whenever the affected behavior can be isolated from OBS.

### Website development

The bilingual Next.js website is under <code>website/</code> and requires Node.js 24.x.

~~~bash
cd website
npm ci
npm run lint
npm run build
~~~

Keep English and Spanish routes and messages equivalent. Preserve the static/cacheable design, metadata,
canonical/alternate URLs, keyboard behavior, reduced-motion support, and responsive layout. Do not add client-side
JavaScript when a Server Component or CSS is sufficient.

### Documentation and translations

- Keep <code>docs/guide.en.md</code> and <code>docs/guide.es.md</code> technically equivalent.
- Every public README, changelog, contribution, security, and release-note change must be reflected in both
  languages.
- Preserve commands, paths, version numbers, download filenames, limitations, and API links exactly.
- Prefer clear technical language over literal word-for-word translation.

### Pull-request checklist

- [ ] The change is focused and its user-visible behavior is explained.
- [ ] Relevant builds, tests, linters, and formatters pass.
- [ ] New real-time paths remain bounded and non-blocking.
- [ ] English and Spanish content are equivalent.
- [ ] Documentation and changelog entries are updated when required.
- [ ] No secrets, signing keys, personal data, or generated binaries are included.
- [ ] Compatibility and validation claims are limited to what was actually tested.

### Releases

Releases are prepared by maintainers. The tag must exactly match the version in <code>buildspec.json</code>, for
example <code>1.1.1</code>. A tag triggers native builds and creates a draft GitHub Release. Maintainers verify
tests, package layouts, checksums, release notes, signatures, and platform caveats before publishing it.

By contributing, you agree that your contribution is distributed under
[GPL-2.0-or-later](LICENSE).

---

## Español

Gracias por ayudar a mejorar OBS Dynamic Delay. Se aceptan en inglés o español informes de errores, documentación
técnica, traducciones, tests, cambios del plugin nativo y mejoras de la web.

### Antes de abrir una issue

1. Busca en las [issues existentes](https://github.com/stanixdor/obs-variable-delay-web/issues).
2. Reproduce el problema con la
   [última versión](https://github.com/stanixdor/obs-variable-delay-web/releases/latest).
3. Lee la [guía en español](docs/guide.es.md), en especial las limitaciones de outputs compatibles y seguridad de
   audio.
4. Si el panel muestra **ERROR / LIVE FALLBACK**, copia el detalle exacto que aparece debajo del estado.
5. Genera un registro de OBS después de reproducir el problema y elimina claves de streaming, credenciales, rutas
   personales y cualquier otro dato sensible antes de compartirlo.

Un informe de error útil incluye:

- sistema operativo y arquitectura;
- versiones exactas de OBS Studio y del plugin;
- contenedor de streaming o grabación y encoders activos;
- resolución, frame rate, intervalo de keyframes y layout de pistas de audio;
- delay solicitado, escena de espera y modo de audio de espera;
- pasos exactos, resultado esperado, resultado real, error del panel y registro de OBS sanitizado;
- si estaban activos streaming, grabación o ambos.

Usa una issue pública sólo para fallos no sensibles. Sigue [SECURITY.md](SECURITY.md) para vulnerabilidades.

### Proponer un cambio

Para un cambio importante de comportamiento, arquitectura, compatibilidad de formatos o UI, abre primero una issue
para poder discutir el diseño y el coste en tiempo real. Los arreglos pequeños y las correcciones de documentación
pueden ir directamente a una pull request.

1. Haz fork y clona el repositorio por HTTPS.
2. Crea una rama específica desde <code>main</code>.
3. No mezcles en el cambio formateo o refactors no relacionados.
4. Añade o actualiza tests para el comportamiento observable.
5. Actualiza los textos visibles en inglés y español.
6. Ejecuta las comprobaciones locales correspondientes.
7. Abre una pull request explicando el problema, el enfoque, la validación, el impacto de rendimiento y los
   tradeoffs conocidos.

No incluyas directorios de build, cachés de dependencias, datos locales de OBS, credenciales, material de firma ni
artefactos de release generados.

### Desarrollo del plugin nativo

El plugin nativo usa C++17, Qt 6 y libobs. Los comandos y requisitos completos de cada plataforma están en la
[guía técnica en español](docs/guide.es.md#compilación).

Ejemplo de build local para macOS arm64:

~~~bash
cmake --preset macos-local-arm64 -DENABLE_TESTS=ON
cmake --build --preset macos-local-arm64 --parallel
ctest --test-dir build_macos_arm64 --output-on-failure
~~~

Comprobaciones de formato:

~~~bash
./build-aux/run-clang-format --check --fail-error
./build-aux/run-gersemi --check --fail-error
~~~

Los cambios nativos deben conservar estas invariantes:

- ninguna allocation no acotada, I/O bloqueante o adquisición de mutex en callbacks de paquetes o audio en tiempo
  real;
- colas acotadas y límites de memoria explícitos;
- timestamps monótonos y empalmes de vídeo únicamente en keyframes seguros;
- ninguna modificación del routing global de fuentes, buses, mute o pistas de OBS para el audio de espera;
- fallback seguro a output live o silencio del audio de espera cuando un formato o topología deje de ser seguro;
- referencias libobs equilibradas y conexiones de señales RAII;
- comportamiento separado para streaming y grabación simultáneos.

Añade tests específicos en <code>tests/</code> para generaciones de paquetes, timebases, cancelación, límites de
memoria, concurrencia, fase de audio, underrun o drift cuando el comportamiento afectado pueda aislarse de OBS.

### Desarrollo de la web

La web bilingüe en Next.js está dentro de <code>website/</code> y requiere Node.js 24.x.

~~~bash
cd website
npm ci
npm run lint
npm run build
~~~

Mantén equivalentes las rutas y mensajes en inglés y español. Conserva el diseño estático/cacheable, los metadatos,
URLs canonical/alternativas, el comportamiento por teclado, el soporte de movimiento reducido y el layout
responsive. No añadas JavaScript del lado cliente cuando sea suficiente un Server Component o CSS.

### Documentación y traducciones

- Mantén <code>docs/guide.en.md</code> y <code>docs/guide.es.md</code> técnicamente equivalentes.
- Todo cambio público en README, historial, contribución, seguridad o notas de versión debe reflejarse en ambos
  idiomas.
- Conserva exactamente comandos, rutas, números de versión, nombres de descarga, limitaciones y enlaces de APIs.
- Prefiere lenguaje técnico claro frente a una traducción literal palabra por palabra.

### Checklist de pull request

- [ ] El cambio está acotado y explica el comportamiento visible para el usuario.
- [ ] Pasan los builds, tests, linters y formateadores correspondientes.
- [ ] Las rutas nuevas en tiempo real siguen siendo acotadas y no bloqueantes.
- [ ] El contenido en inglés y español es equivalente.
- [ ] Se han actualizado la documentación y el historial cuando corresponde.
- [ ] No se incluyen secretos, claves de firma, datos personales ni binarios generados.
- [ ] Las afirmaciones de compatibilidad y validación se limitan a lo que se ha probado realmente.

### Releases

Las releases las preparan los mantenedores. El tag debe coincidir exactamente con la versión de
<code>buildspec.json</code>, por ejemplo <code>1.1.1</code>. Un tag lanza los builds nativos y crea un draft de
GitHub Release. Antes de publicarlo, los mantenedores verifican tests, layouts de paquetes, checksums, notas de
versión, firmas y advertencias específicas de cada plataforma.

Al contribuir, aceptas que tu contribución se distribuya bajo
[GPL-2.0-or-later](LICENSE).
