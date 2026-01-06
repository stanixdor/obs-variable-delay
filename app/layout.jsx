import { JetBrains_Mono, Inter } from "next/font/google";
import "./globals.css";

const inter = Inter({
  variable: "--font-inter",
  subsets: ["latin"],
  display: "swap",
});

const jetbrainsMono = JetBrains_Mono({
  variable: "--font-jetbrains",
  subsets: ["latin"],
  display: "swap",
});

// Base URL for production
const siteUrl = process.env.NEXT_PUBLIC_SITE_URL || "https://obs-delay-controller.vercel.app";

export const metadata = {
  // Basic metadata
  title: {
    default: "OBS Delay Controller - Control de Delay Profesional para Streaming",
    template: "%s | OBS Delay Controller",
  },
  description:
    "Herramienta profesional para controlar el delay en OBS Studio. Gestiona la cámara virtual, escenas y sources con precisión para streamings en directo. Desarrollado por @STANIXDOR.",
  keywords: [
    "OBS",
    "OBS Studio",
    "delay",
    "streaming",
    "broadcast",
    "cámara virtual",
    "virtual camera",
    "escenas",
    "scenes",
    "control remoto",
    "websocket",
    "directo",
    "live streaming",
    "Twitch",
    "YouTube",
    "producción audiovisual",
  ],
  authors: [{ name: "STANIXDOR", url: "https://x.com/STANIXDOR" }],
  creator: "STANIXDOR",
  publisher: "STANIXDOR",

  // Favicon and icons
  icons: {
    icon: [
      { url: "/favicon.ico", sizes: "any" },
      { url: "/icon.svg", type: "image/svg+xml" },
    ],
    apple: [{ url: "/apple-touch-icon.png", sizes: "180x180" }],
  },

  // Manifest for PWA
  manifest: "/manifest.json",

  // Open Graph
  openGraph: {
    type: "website",
    locale: "es_ES",
    url: siteUrl,
    siteName: "OBS Delay Controller",
    title: "OBS Delay Controller - Control de Delay Profesional para Streaming",
    description:
      "Herramienta profesional para controlar el delay en OBS Studio. Gestiona la cámara virtual, escenas y sources con precisión para streamings en directo.",
    images: [
      {
        url: `${siteUrl}/og-image.png`,
        width: 1200,
        height: 630,
        alt: "OBS Delay Controller - Control de delay profesional",
      },
    ],
  },

  // Twitter Card
  twitter: {
    card: "summary_large_image",
    site: "@STANIXDOR",
    creator: "@STANIXDOR",
    title: "OBS Delay Controller - Control de Delay Profesional",
    description:
      "Herramienta profesional para controlar el delay en OBS Studio. Gestiona la cámara virtual, escenas y sources con precisión.",
    images: [`${siteUrl}/og-image.png`],
  },

  // Robots
  robots: {
    index: true,
    follow: true,
    googleBot: {
      index: true,
      follow: true,
      "max-video-preview": -1,
      "max-image-preview": "large",
      "max-snippet": -1,
    },
  },

  // Verification (add your own IDs when you have them)
  // verification: {
  //   google: "your-google-verification-code",
  //   yandex: "your-yandex-verification-code",
  //   bing: "your-bing-verification-code",
  // },

  // Alternate languages (if you add more languages later)
  alternates: {
    canonical: siteUrl,
    languages: {
      "es-ES": siteUrl,
    },
  },

  // App-specific
  applicationName: "OBS Delay Controller",
  category: "Technology",

  // Additional meta
  other: {
    "mobile-web-app-capable": "yes",
    "apple-mobile-web-app-capable": "yes",
    "apple-mobile-web-app-status-bar-style": "black-translucent",
    "apple-mobile-web-app-title": "OBS Delay",
    "format-detection": "telephone=no",
    "msapplication-TileColor": "#0d0d0d",
    "theme-color": "#0d0d0d",
  },
};

// Viewport configuration
export const viewport = {
  themeColor: [
    { media: "(prefers-color-scheme: light)", color: "#ffffff" },
    { media: "(prefers-color-scheme: dark)", color: "#0d0d0d" },
  ],
  width: "device-width",
  initialScale: 1,
  maximumScale: 5,
  userScalable: true,
  colorScheme: "dark",
};

export default function RootLayout({ children }) {
  // JSON-LD structured data
  const jsonLd = {
    "@context": "https://schema.org",
    "@type": "WebApplication",
    name: "OBS Delay Controller",
    description:
      "Herramienta profesional para controlar el delay en OBS Studio. Gestiona la cámara virtual, escenas y sources con precisión para streamings en directo.",
    url: siteUrl,
    applicationCategory: "MultimediaApplication",
    operatingSystem: "Web Browser",
    author: {
      "@type": "Person",
      name: "STANIXDOR",
      url: "https://x.com/STANIXDOR",
    },
    offers: {
      "@type": "Offer",
      price: "0",
      priceCurrency: "USD",
    },
    featureList: [
      "Control de delay para OBS Studio",
      "Gestión de cámara virtual",
      "Control de escenas",
      "Control de sources",
      "Conexión WebSocket",
    ],
  };

  return (
    <html lang="es" className="dark" suppressHydrationWarning>
      <head>
        <script
          type="application/ld+json"
          dangerouslySetInnerHTML={{ __html: JSON.stringify(jsonLd) }}
        />
      </head>
      <body
        className={`${inter.variable} ${jetbrainsMono.variable} antialiased font-sans`}
      >
        {children}
      </body>
    </html>
  );
}
