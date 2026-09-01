import type {Metadata} from "next";
import {locale as currentLocale} from "next/root-params";
import {hasLocale} from "next-intl";
import {getTranslations} from "next-intl/server";
import {notFound} from "next/navigation";

import {routing} from "@/i18n/routing";

import "../globals.css";

const siteUrl = "https://www.obsdelay.com";

type Props = LayoutProps<"/[locale]">;

export function generateStaticParams() {
  return routing.locales.map((locale) => ({locale}));
}

export async function generateMetadata({params}: Props): Promise<Metadata> {
  const {locale} = await params;

  if (!hasLocale(routing.locales, locale)) {
    notFound();
  }

  const t = await getTranslations({locale, namespace: "Metadata"});

  return {
    metadataBase: new URL(siteUrl),
    title: {
      default: t("title"),
      template: `%s · ${t("productName")}`,
    },
    description: t("description"),
    keywords: t.raw("keywords") as string[],
    applicationName: t("productName"),
    authors: [{name: "stanixdor", url: "https://github.com/stanixdor"}],
    creator: "stanixdor",
    publisher: "stanixdor",
    alternates: {
      canonical: `/${locale}`,
      languages: {
        en: "/en",
        es: "/es",
        "x-default": "/",
      },
    },
    openGraph: {
      type: "website",
      url: `/${locale}`,
      siteName: t("productName"),
      title: t("title"),
      description: t("description"),
      locale: locale === "es" ? "es_ES" : "en_US",
      alternateLocale: locale === "es" ? ["en_US"] : ["es_ES"],
      images: [
        {
          url: "/og.png",
          width: 1200,
          height: 630,
          alt: t("ogAlt"),
        },
      ],
    },
    twitter: {
      card: "summary_large_image",
      title: t("title"),
      description: t("description"),
      images: [{url: "/og.png", alt: t("ogAlt")}],
    },
    robots: {
      index: true,
      follow: true,
      googleBot: {
        index: true,
        follow: true,
        "max-image-preview": "large",
        "max-snippet": -1,
        "max-video-preview": -1,
      },
    },
    category: "technology",
  };
}

export default async function LocaleLayout({children}: Props) {
  const rootLocale = await currentLocale();
  const locale = hasLocale(routing.locales, rootLocale) ? rootLocale : routing.defaultLocale;

  return (
    <html lang={locale}>
      <body>{children}</body>
    </html>
  );
}
