import type {MetadataRoute} from "next";

export default function manifest(): MetadataRoute.Manifest {
  return {
    name: "OBS Dynamic Delay",
    short_name: "OBS Delay",
    description:
      "Variable delay for OBS without restarting the output · Delay variable para OBS sin reiniciar la salida.",
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
