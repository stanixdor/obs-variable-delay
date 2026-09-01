# OBS Dynamic Delay — website

[Español](#español) · [English](#english)

## Español

Landing oficial de [OBS Dynamic Delay](https://www.obsdelay.com), integrada en el monorepo del plugin.

### Desarrollo local

Requiere Node.js 24.x.

```bash
npm install
npm run dev
```

Abre `http://localhost:3000/es`. Para validar una compilación de producción:

```bash
npm run lint
npm run build
```

La web usa Next.js 16, App Router, `next-intl` y Cache Components. El contenido público está disponible en `/en` y `/es`; la raíz redirige al idioma predeterminado.

### Despliegue

Vercel compila esta carpeta como directorio raíz del proyecto. El dominio canónico es `https://www.obsdelay.com`.

## English

Official [OBS Dynamic Delay](https://www.obsdelay.com) landing page, included in the plugin monorepo.

### Local development

Node.js 24.x is required.

```bash
npm install
npm run dev
```

Open `http://localhost:3000/en`. To validate a production build:

```bash
npm run lint
npm run build
```

The site uses Next.js 16, App Router, `next-intl`, and Cache Components. Public content is available at `/en` and `/es`; the root redirects to the default locale.

### Deployment

Vercel builds this folder as the project root directory. The canonical domain is `https://www.obsdelay.com`.
