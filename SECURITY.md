# Security policy / Política de seguridad

[English](#english) · [Español](#español)

## English

### Supported versions

| Version | Security support |
| --- | --- |
| 1.1.x | Supported |
| 1.0.x and older | Not supported; upgrade to the latest release |

Security fixes are released from the newest maintained series. Support refers to this repository's official source
and artifacts, not third-party repackaging or modified builds.

### Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub's private
[Report a vulnerability](https://github.com/stanixdor/obs-variable-delay/security/advisories/new) form and
include:

- the affected version, operating system, architecture, and OBS Studio version;
- the vulnerable component: native plugin, package/installer, release workflow, or website;
- a minimal reproduction or proof of concept;
- impact, prerequisites, and whether untrusted media, configuration, or network input is required;
- relevant logs or crash reports after removing stream keys, tokens, credentials, personal paths, and private
  content;
- any proposed mitigation, if known.

If private vulnerability reporting is temporarily unavailable, open a public issue containing only a request for a
private security contact. Do not include vulnerability details in that issue.

### What to expect

- A maintainer will aim to acknowledge a complete report within 7 days.
- The report will be assessed for reproducibility, impact, affected versions, and whether the issue belongs to OBS,
  an encoder/driver, a dependency, or this project.
- Progress and disclosure timing will be coordinated privately with the reporter.
- Once a fix and release are available, a GitHub Security Advisory may be published with credit if the reporter
  wishes.

These are best-effort targets for a volunteer project, not guaranteed service-level commitments.

### Security scope

Examples that belong in a private report include:

- memory corruption, use-after-free, race conditions, or unsafe lifetime handling reachable through normal plugin
  use;
- arbitrary code execution or unsafe package/install paths;
- release-workflow or artifact-integrity weaknesses;
- exposure of stream credentials, private configuration, or media content;
- website vulnerabilities that affect users of <https://www.obsdelay.com>;
- a bypass of bounds, format checks, or safe fallback that can cause uncontrolled memory growth or invalid output.

Normal resource use proportional to the configured delay, documented unsupported output formats, unsigned-package
warnings, a keyframe-bounded return to live, or the documented short AAC splice are not vulnerabilities by
themselves.

### Artifact integrity and platform signing

Download only from the repository's
[GitHub Releases](https://github.com/stanixdor/obs-variable-delay/releases). Each release publishes SHA-256
checksums; verify the downloaded filename and hash before installation.

The 1.1.1 macOS plugin bundle is ad-hoc signed. The PKG has no Developer ID signature and is not Apple-notarized.
The Windows DLL is not Authenticode-signed. Those limitations are disclosed in the release notes and do not replace
checksum verification. Never install a binary received through an unrelated mirror or direct message.

### Coordinated disclosure

Please allow a reasonable remediation window before public disclosure. The maintainers will avoid requesting
unnecessary secrecy, and the reporter remains free to decline attribution. Do not test against systems, streams, or
accounts that you do not own or have explicit permission to assess.

---

## Español

### Versiones compatibles

| Versión | Soporte de seguridad |
| --- | --- |
| 1.1.x | Compatible |
| 1.0.x y anteriores | Sin soporte; actualiza a la última versión |

Las correcciones de seguridad se publican desde la serie mantenida más reciente. El soporte se refiere al código
fuente y los artefactos oficiales de este repositorio, no a repaquetados de terceros o builds modificados.

### Informar de una vulnerabilidad

No abras una issue pública para una posible vulnerabilidad. Usa el formulario privado de GitHub
[Report a vulnerability](https://github.com/stanixdor/obs-variable-delay/security/advisories/new) e incluye:

- versión afectada, sistema operativo, arquitectura y versión de OBS Studio;
- componente vulnerable: plugin nativo, paquete/instalador, workflow de release o web;
- reproducción mínima o prueba de concepto;
- impacto, requisitos previos y si se necesita media, configuración o entrada de red no confiable;
- registros o informes de crash relevantes después de eliminar claves de streaming, tokens, credenciales, rutas
  personales y contenido privado;
- cualquier mitigación propuesta, si se conoce.

Si el reporte privado de vulnerabilidades no está disponible temporalmente, abre una issue pública que sólo
solicite un contacto privado de seguridad. No incluyas detalles de la vulnerabilidad en esa issue.

### Qué esperar

- Un mantenedor intentará confirmar la recepción de un informe completo en un plazo de 7 días.
- El informe se evaluará por reproducibilidad, impacto, versiones afectadas y si el problema pertenece a OBS, a un
  encoder/driver, a una dependencia o a este proyecto.
- El progreso y el momento de la divulgación se coordinarán en privado con la persona que informa.
- Cuando haya una corrección y release disponibles, se podrá publicar un GitHub Security Advisory dando crédito si
  la persona que informa lo desea.

Estos son objetivos de mejor esfuerzo para un proyecto voluntario, no compromisos garantizados de nivel de servicio.

### Alcance de seguridad

Ejemplos que deben incluirse en un informe privado:

- corrupción de memoria, use-after-free, condiciones de carrera o gestión de lifetime insegura accesible mediante
  el uso normal del plugin;
- ejecución arbitraria de código o rutas de paquete/instalación inseguras;
- debilidades de integridad en workflows de release o artefactos;
- exposición de credenciales de streaming, configuración privada o contenido multimedia;
- vulnerabilidades de la web que afecten a usuarios de <https://www.obsdelay.com>;
- bypass de límites, comprobaciones de formato o fallback seguro que pueda provocar crecimiento de memoria no
  controlado o un output inválido.

El uso normal de recursos proporcional al delay configurado, los formatos de output no compatibles documentados,
los avisos de paquetes sin firma, una vuelta a live limitada por keyframe o el breve empalme AAC documentado no son
vulnerabilidades por sí mismos.

### Integridad de artefactos y firma por plataforma

Descarga únicamente desde las
[GitHub Releases](https://github.com/stanixdor/obs-variable-delay/releases) del repositorio. Cada release
publica checksums SHA-256; comprueba el nombre y el hash del archivo descargado antes de instalar.

El bundle del plugin para macOS 1.1.1 tiene firma ad hoc. El PKG no tiene firma Developer ID ni está notarizado por
Apple. El DLL de Windows no tiene firma Authenticode. Estas limitaciones se indican en las notas de versión y no
sustituyen la comprobación de checksums. No instales un binario recibido mediante un mirror no relacionado o un
mensaje directo.

### Divulgación coordinada

Permite un plazo razonable de corrección antes de divulgar públicamente. Los mantenedores evitarán solicitar secreto
innecesario, y la persona que informa puede rechazar la atribución. No realices pruebas contra sistemas, directos o
cuentas que no sean tuyos o para los que no tengas permiso explícito de evaluación.
