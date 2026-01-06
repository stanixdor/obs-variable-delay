const siteUrl = process.env.NEXT_PUBLIC_SITE_URL || "https://obs-delay-controller.vercel.app";

export default function sitemap() {
  const lastModified = new Date();

  return [
    {
      url: siteUrl,
      lastModified,
      changeFrequency: "monthly",
      priority: 1.0,
    },
    // Add more pages here as your app grows
    // {
    //   url: `${siteUrl}/about`,
    //   lastModified,
    //   changeFrequency: "monthly",
    //   priority: 0.8,
    // },
  ];
}

