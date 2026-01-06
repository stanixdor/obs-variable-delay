import ObsControllerDelay from "@/components/OBS1scene";
import { Suspense } from "react";

export default function Home() {
  return (
    <Suspense fallback={<div className="min-h-screen" />}>
      <ObsControllerDelay />
    </Suspense>
  );
}
