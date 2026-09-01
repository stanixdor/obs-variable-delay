import type {MetadataRoute} from "next";

const siteUrl = "https://www.obsdelay.com";
const releaseDate = new Date("2026-09-01T00:00:00.000Z");
const languageAlternates = {
  en: `${siteUrl}/en`,
  es: `${siteUrl}/es`,
  "x-default": siteUrl,
};

export default function sitemap(): MetadataRoute.Sitemap {
  return [
    {
      url: `${siteUrl}/en`,
      lastModified: releaseDate,
      changeFrequency: "monthly",
      priority: 1,
      alternates: {languages: languageAlternates},
    },
    {
      url: `${siteUrl}/es`,
      lastModified: releaseDate,
      changeFrequency: "monthly",
      priority: 1,
      alternates: {languages: languageAlternates},
    },
  ];
}
