import type {MetadataRoute} from "next";

export default function manifest(): MetadataRoute.Manifest {
  return {
    name: "OBS Dynamic Delay",
    short_name: "OBS Delay",
    description:
      "Variable delay and multi-RTMP for OBS · Delay variable y multi-RTMP para OBS.",
    start_url: "/",
    display: "standalone",
    background_color: "#070909",
    theme_color: "#d9ff6a",
    icons: [
      {
        src: "/icon.svg",
        sizes: "any",
        type: "image/svg+xml",
      },
    ],
  };
}
