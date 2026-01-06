"use client";

import { useState, useRef, useEffect } from "react";
import OBSWebSocket, { EventSubscription } from "obs-websocket-js";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import {
  Select,
  SelectTrigger,
  SelectContent,
  SelectItem,
  SelectValue,
} from "@/components/ui/select";
import { Slider } from "@/components/ui/slider";
import {
  Wifi,
  WifiOff,
  Loader2,
  Clock,
  Play,
  Square,
  Monitor,
  Video,
  Film,
  Clapperboard,
} from "lucide-react";
import { motion, AnimatePresence } from "framer-motion";

/**
 * Hook de estado persistente en localStorage
 */
function usePersistentState(key, defaultValue) {
  const [value, setValue] = useState(() => {
    if (typeof window === "undefined") return defaultValue;
    try {
      const saved = window.localStorage.getItem(key);
      return saved !== null ? JSON.parse(saved) : defaultValue;
    } catch (_err) {
      return defaultValue;
    }
  });

  useEffect(() => {
    if (typeof window === "undefined") return;
    try {
      window.localStorage.setItem(key, JSON.stringify(value));
    } catch (_err) {
      /* modo incógnito – ignorar */
    }
  }, [key, value]);

  return [value, setValue];
}

/**
 * ObsControllerDelay – Control de delay con cámara virtual
 */
export default function ObsControllerDelay() {
  const obsRef = useRef(null);

  /* -------------------- Estado persistente -------------------- */
  const [port, setPort] = usePersistentState("obs:port", "4455");
  const [password, setPassword] = usePersistentState("obs:password", "");
  const [delayInput, setDelayInput] = usePersistentState("obs:delayInput", "");
  const [bridgeInput, setBridgeInput] = usePersistentState("obs:bridgeInput", "");
  const [recordScene, setRecordScene] = usePersistentState("obs:recordScene", "");
  const [delayScene, setDelayScene] = usePersistentState("obs:delayScene", "");
  const [delaySec, setDelaySec] = usePersistentState("obs:delaySec", 30);

  /* -------------------- Estado no persistente ----------------- */
  const [status, setStatus] = useState("disconnected");
  const [errorMsg, setErrorMsg] = useState("");
  const [inputs, setInputs] = useState([]);
  const [scenes, setScenes] = useState([]);
  const [delayActive, setDelayActive] = useState(false);

  /* ------------------- Helpers generales ---------------------- */
  const obs = () => obsRef.current;
  const ensureObs = () => {
    if (!obs()) throw new Error("OBS no conectado");
  };

  /* -------------- Conectar con OBS ---------------------------- */
  const connect = async () => {
    setStatus("connecting");
    setErrorMsg("");
    try {
      const instance = new OBSWebSocket();
      await instance.connect(`ws://localhost:${port}`, password || undefined, {
        rpcVersion: 1,
        eventSubscriptions:
          EventSubscription.General |
          EventSubscription.Outputs |
          EventSubscription.Scenes,
      });
      obsRef.current = instance;
      setStatus("connected");
      instance.on("ConnectionClosed", () => setStatus("disconnected"));

      const [{ inputs: inputList }, { scenes: sceneList }] = await Promise.all([
        instance.call("GetInputList", {}),
        instance.call("GetSceneList", {}),
      ]);
      setInputs(inputList.map((i) => i.inputName));
      const sceneNames = sceneList.map((s) => s.sceneName);
      setScenes(sceneNames);

      setDelayInput((v) => v || inputList[0]?.inputName || "");
      setBridgeInput((v) => v || inputList[1]?.inputName || "");
      setRecordScene((v) => v || sceneNames[0] || "");
      setDelayScene((v) => v || sceneNames[1] || sceneNames[0] || "");
    } catch (err) {
      setStatus("error");
      setErrorMsg(String(err?.message || err));
    }
  };

  /* ----------------- Utilidades de escena --------------------- */
  const setSceneItemEnabled = async (sceneName, sourceName, enabled) => {
    if (!sceneName || !sourceName) return;
    ensureObs();
    try {
      const { sceneItems } = await obs().call("GetSceneItemList", { sceneName });
      const matches = sceneItems.filter((it) => it.sourceName === sourceName);
      for (const item of matches) {
        await obs().call("SetSceneItemEnabled", {
          sceneName,
          sceneItemId: item.sceneItemId,
          sceneItemEnabled: enabled,
        });
      }
    } catch (e) {
      console.warn(`No se pudo actualizar ${sourceName} en ${sceneName}`, e);
    }
  };

  const isSourceEnabled = async (sceneName, sourceName) => {
    if (!sceneName || !sourceName) return false;
    ensureObs();
    try {
      const { sceneItems } = await obs().call("GetSceneItemList", { sceneName });
      const item = sceneItems.find((it) => it.sourceName === sourceName);
      return item?.sceneItemEnabled ?? false;
    } catch (e) {
      console.warn(`No se pudo verificar ${sourceName} en ${sceneName}`, e);
      return false;
    }
  };

  const pulseSource = async (sourceName) => {
    if (!sourceName) return;
    ensureObs();
    try {
      const { inputSettings } = await obs().call("GetInputSettings", { inputName: sourceName });
      const pathKey =
        inputSettings.local_file !== undefined
          ? "local_file"
          : inputSettings.file !== undefined
            ? "file"
            : inputSettings.path !== undefined
              ? "path"
              : null;
      if (!pathKey) return;

      const originalPath = inputSettings[pathKey];
      const setPath = async (value) =>
        obs().call("SetInputSettings", {
          inputName: sourceName,
          inputSettings: { ...inputSettings, [pathKey]: value },
          overlay: false,
        });

      await setPath("");
      await new Promise((r) => setTimeout(r, 300));
      await setPath(originalPath);
    } catch (e) {
      console.warn(`No se pudo pulsar ${sourceName}`, e);
    }
  };

  /* ---------------------- enableDelay ------------------------- */
  const enableDelay = async () => {
    if (!obs() || status !== "connected" || !delayScene) return;
    setErrorMsg("");
    try {
      await obs().call("StartVirtualCam").catch(() => {});

      const bridgeWasActive = await isSourceEnabled(delayScene, bridgeInput);
      if (bridgeWasActive) {
        await pulseSource(bridgeInput);
      } else {
        await setSceneItemEnabled(delayScene, bridgeInput, true);
      }

      const delayWasActive = await isSourceEnabled(delayScene, delayInput);
      if (delayWasActive) {
        await setSceneItemEnabled(delayScene, delayInput, false);
      }

      await obs().call("SetCurrentProgramScene", { sceneName: delayScene });

      setDelayActive(true);

      setTimeout(async () => {
        try {
          await setSceneItemEnabled(delayScene, delayInput, true);
          await setSceneItemEnabled(delayScene, bridgeInput, false);
        } catch (e) {
          console.error("Error al cambiar sources tras delay:", e);
        }
      }, delaySec * 1000);
    } catch (err) {
      setErrorMsg(err?.message || "Error al activar delay");
    }
  };

  /* --------------------- disableDelay ------------------------- */
  const disableDelay = async () => {
    if (!obs() || status !== "connected") return;
    setErrorMsg("");
    try {
      await obs().call("StopVirtualCam").catch(() => {});

      if (recordScene) {
        await obs().call("SetCurrentProgramScene", { sceneName: recordScene });
      }

      await setSceneItemEnabled(delayScene, delayInput, false);

      setDelayActive(false);
    } catch (err) {
      setErrorMsg(err?.message || "Error al quitar delay");
    }
  };

  /* ---------------------- Formatear tiempo -------------------- */
  const formatTime = (seconds) => {
    const mins = Math.floor(seconds / 60);
    const secs = seconds % 60;
    if (mins > 0) {
      return `${mins}m ${secs}s`;
    }
    return `${secs}s`;
  };

  /* ---------------------- Status Badge ------------------------ */
  const StatusBadge = () => {
    const config = {
      disconnected: {
        icon: WifiOff,
        text: "Desconectado",
        bg: "bg-zinc-800/50",
        border: "border-zinc-700/50",
        text_color: "text-zinc-400",
        dot: "bg-zinc-500",
      },
      connecting: {
        icon: Loader2,
        text: "Conectando...",
        bg: "bg-amber-950/30",
        border: "border-amber-800/30",
        text_color: "text-amber-400",
        dot: "bg-amber-500",
        spin: true,
      },
      connected: {
        icon: Wifi,
        text: "Conectado",
        bg: "bg-emerald-950/30",
        border: "border-emerald-800/30",
        text_color: "text-emerald-400",
        dot: "bg-emerald-500",
      },
      error: {
        icon: WifiOff,
        text: "Error",
        bg: "bg-red-950/30",
        border: "border-red-800/30",
        text_color: "text-red-400",
        dot: "bg-red-500",
      },
    }[status];

    const Icon = config.icon;

    return (
      <motion.div
        initial={{ opacity: 0, scale: 0.95 }}
        animate={{ opacity: 1, scale: 1 }}
        className={`inline-flex items-center gap-2.5 px-4 py-2 rounded-full ${config.bg} ${config.border} border`}
      >
        <span className={`relative flex h-2 w-2`}>
          <span
            className={`animate-ping absolute inline-flex h-full w-full rounded-full ${config.dot} opacity-75`}
          />
          <span className={`relative inline-flex rounded-full h-2 w-2 ${config.dot}`} />
        </span>
        <Icon className={`w-4 h-4 ${config.text_color} ${config.spin ? "animate-spin" : ""}`} />
        <span className={`text-sm font-medium ${config.text_color}`}>{config.text}</span>
      </motion.div>
    );
  };

  /* ---------------------- Section Header ---------------------- */
  const SectionHeader = ({ icon: Icon, title }) => (
    <div className="flex items-center gap-3 mb-4">
      <div className="p-2 rounded-lg bg-primary/10 text-primary">
        <Icon className="w-4 h-4" />
      </div>
      <h3 className="text-sm font-semibold text-foreground/90 uppercase tracking-wider">{title}</h3>
    </div>
  );

  /* ---------------------- Field Label ------------------------- */
  const FieldLabel = ({ children }) => (
    <label className="block text-xs font-medium text-muted-foreground mb-1.5 uppercase tracking-wide">
      {children}
    </label>
  );

  return (
    <div className="min-h-screen flex items-center justify-center p-4 sm:p-8">
      <motion.div
        initial={{ opacity: 0, y: 20 }}
        animate={{ opacity: 1, y: 0 }}
        transition={{ duration: 0.5, ease: [0.23, 1, 0.32, 1] }}
        className="relative w-full max-w-lg"
      >
        {/* Main Card */}
        <div className="relative overflow-hidden rounded-2xl bg-card/80 backdrop-blur-xl border border-border/50 shadow-2xl shadow-black/20">
          {/* Gradient accent line */}
          <div className="absolute top-0 left-0 right-0 h-px bg-gradient-to-r from-transparent via-primary/50 to-transparent" />

          {/* Header */}
          <div className="px-6 py-5 border-b border-border/50">
            <div className="flex items-center justify-between">
              <div className="flex items-center gap-3">
                <div className="p-2.5 rounded-xl bg-gradient-to-br from-primary/20 to-primary/5 border border-primary/20">
                  <Monitor className="w-5 h-5 text-primary" />
                </div>
                <div>
                  <h1 className="text-lg font-bold text-foreground">OBS Delay Controller</h1>
                  <p className="text-xs text-muted-foreground">Control de delay dinámico</p>
                </div>
              </div>
              <StatusBadge />
            </div>
          </div>

          {/* Content */}
          <div className="p-6 space-y-6">
            {/* Connection Section */}
            <section>
              <SectionHeader icon={Wifi} title="Conexión" />
              <div className="grid grid-cols-2 gap-3">
                <div>
                  <FieldLabel>Puerto</FieldLabel>
                  <Input
                    value={port}
                    onChange={(e) => setPort(e.target.value)}
                    placeholder="4455"
                    className="bg-background/50 border-border/50 focus:border-primary/50 focus:ring-primary/20 h-10 font-mono text-sm"
                  />
                </div>
                <div>
                  <FieldLabel>Contraseña</FieldLabel>
                  <Input
                    type="password"
                    value={password}
                    onChange={(e) => setPassword(e.target.value)}
                    placeholder="••••••••"
                    className="bg-background/50 border-border/50 focus:border-primary/50 focus:ring-primary/20 h-10 text-sm"
                  />
                </div>
              </div>
              <Button
                onClick={connect}
                disabled={status === "connecting"}
                className="w-full mt-3 h-11 bg-primary hover:bg-primary/90 text-primary-foreground font-semibold transition-all duration-200"
              >
                {status === "connecting" ? (
                  <>
                    <Loader2 className="w-4 h-4 mr-2 animate-spin" />
                    Conectando...
                  </>
                ) : (
                  "Conectar a OBS"
                )}
              </Button>
            </section>

            {/* Configuration Section */}
            <AnimatePresence>
              {status === "connected" && (
                <motion.section
                  initial={{ opacity: 0, height: 0 }}
                  animate={{ opacity: 1, height: "auto" }}
                  exit={{ opacity: 0, height: 0 }}
                  transition={{ duration: 0.3 }}
                >
                  <div className="pt-4 border-t border-border/30">
                    <SectionHeader icon={Clapperboard} title="Escenas" />
                    <div className="grid grid-cols-2 gap-3">
                      <div>
                        <FieldLabel>Escena a grabar</FieldLabel>
                        <Select value={recordScene} onValueChange={setRecordScene}>
                          <SelectTrigger className="bg-background/50 border-border/50 h-10">
                            <SelectValue placeholder="Seleccionar..." />
                          </SelectTrigger>
                          <SelectContent className="bg-popover border-border">
                            {scenes.map((n) => (
                              <SelectItem key={n} value={n}>
                                {n}
                              </SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                      </div>
                      <div>
                        <FieldLabel>Escena Delay</FieldLabel>
                        <Select value={delayScene} onValueChange={setDelayScene}>
                          <SelectTrigger className="bg-background/50 border-border/50 h-10">
                            <SelectValue placeholder="Seleccionar..." />
                          </SelectTrigger>
                          <SelectContent className="bg-popover border-border">
                            {scenes.map((n) => (
                              <SelectItem key={n} value={n}>
                                {n}
                              </SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                      </div>
                    </div>
                  </div>

                  <div className="pt-5">
                    <SectionHeader icon={Video} title="Sources" />
                    <div className="grid grid-cols-1 gap-3">
                      <div>
                        <FieldLabel>Video delay</FieldLabel>
                        <Select value={delayInput} onValueChange={setDelayInput}>
                          <SelectTrigger className="bg-background/50 border-border/50 h-10">
                            <Film className="w-4 h-4 mr-2 text-muted-foreground" />
                            <SelectValue placeholder="Seleccionar..." />
                          </SelectTrigger>
                          <SelectContent className="bg-popover border-border">
                            {inputs.map((n) => (
                              <SelectItem key={n} value={n}>
                                {n}
                              </SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                      </div>
                      <div>
                        <FieldLabel>Video mientras se pone delay</FieldLabel>
                        <Select value={bridgeInput} onValueChange={setBridgeInput}>
                          <SelectTrigger className="bg-background/50 border-border/50 h-10">
                            <Film className="w-4 h-4 mr-2 text-muted-foreground" />
                            <SelectValue placeholder="Seleccionar..." />
                          </SelectTrigger>
                          <SelectContent className="bg-popover border-border">
                            {inputs.map((n) => (
                              <SelectItem key={n} value={n}>
                                {n}
                              </SelectItem>
                            ))}
                          </SelectContent>
                        </Select>
                      </div>
                    </div>
                  </div>
                </motion.section>
              )}
            </AnimatePresence>

            {/* Delay Control Section */}
            <AnimatePresence>
              {status === "connected" && (
                <motion.section
                  initial={{ opacity: 0, height: 0 }}
                  animate={{ opacity: 1, height: "auto" }}
                  exit={{ opacity: 0, height: 0 }}
                  transition={{ duration: 0.3, delay: 0.1 }}
                  className="pt-4 border-t border-border/30"
                >
                  <SectionHeader icon={Clock} title="Control de Delay" />

                  {/* Delay Time Display */}
                  <div className="mb-4 p-4 rounded-xl bg-background/30 border border-border/30">
                    <div className="flex items-center justify-between mb-3">
                      <span className="text-xs font-medium text-muted-foreground uppercase tracking-wide">
                        Tiempo de delay
                      </span>
                      <span className="text-2xl font-bold font-mono text-primary">
                        {formatTime(delaySec)}
                      </span>
                    </div>
                    <Slider
                      value={[delaySec]}
                      min={5}
                      max={300}
                      step={5}
                      onValueChange={(v) => setDelaySec(v[0])}
                      className="w-full"
                    />
                    <div className="flex justify-between mt-2 text-xs text-muted-foreground">
                      <span>5s</span>
                      <span>5min</span>
                    </div>
                  </div>

                  {/* Action Buttons */}
                  <div className="grid grid-cols-2 gap-3">
                    <Button
                      onClick={enableDelay}
                      disabled={delayActive}
                      className={`h-12 font-semibold transition-all duration-300 ${
                        delayActive
                          ? "bg-muted text-muted-foreground cursor-not-allowed"
                          : "bg-emerald-600 hover:bg-emerald-500 text-white shadow-lg shadow-emerald-900/30"
                      }`}
                    >
                      <Play className="w-4 h-4 mr-2" />
                      Poner Delay
                    </Button>
                    <Button
                      onClick={disableDelay}
                      disabled={!delayActive}
                      className={`h-12 font-semibold transition-all duration-300 ${
                        !delayActive
                          ? "bg-muted text-muted-foreground cursor-not-allowed"
                          : "bg-red-600 hover:bg-red-500 text-white shadow-lg shadow-red-900/30"
                      }`}
                    >
                      <Square className="w-4 h-4 mr-2" />
                      Quitar Delay
                    </Button>
                  </div>

                  {/* Active Indicator */}
                  <AnimatePresence>
                    {delayActive && (
                      <motion.div
                        initial={{ opacity: 0, y: -10 }}
                        animate={{ opacity: 1, y: 0 }}
                        exit={{ opacity: 0, y: -10 }}
                        className="mt-4 p-3 rounded-lg bg-emerald-950/30 border border-emerald-800/30 flex items-center gap-3"
                      >
                        <span className="relative flex h-3 w-3">
                          <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-emerald-400 opacity-75" />
                          <span className="relative inline-flex rounded-full h-3 w-3 bg-emerald-500" />
                        </span>
                        <span className="text-sm font-medium text-emerald-400">
                          Delay activo — Cámara virtual en funcionamiento
                        </span>
                      </motion.div>
                    )}
                  </AnimatePresence>
                </motion.section>
              )}
            </AnimatePresence>

            {/* Error Message */}
            <AnimatePresence>
              {errorMsg && (
                <motion.div
                  initial={{ opacity: 0, y: -10 }}
                  animate={{ opacity: 1, y: 0 }}
                  exit={{ opacity: 0, y: -10 }}
                  className="p-3 rounded-lg bg-red-950/30 border border-red-800/30"
                >
                  <p className="text-sm text-red-400 font-medium">{errorMsg}</p>
                </motion.div>
              )}
            </AnimatePresence>
          </div>

          {/* Footer gradient */}
          <div className="absolute bottom-0 left-0 right-0 h-px bg-gradient-to-r from-transparent via-border/50 to-transparent" />
        </div>

        {/* Subtle shadow beneath */}
        <div className="absolute inset-x-4 -bottom-4 h-8 bg-primary/5 blur-2xl rounded-full -z-10" />

        {/* Developer Credit */}
        <div className="mt-4 flex justify-center">
          <a
            href="https://x.com/STANIXDOR"
            target="_blank"
            rel="noopener noreferrer"
            className="group flex items-center gap-2 px-3 py-1.5 rounded-full text-xs text-muted-foreground hover:text-foreground transition-colors duration-200"
          >
            <span className="opacity-60 group-hover:opacity-100 transition-opacity">desarrollado por</span>
            <span className="font-semibold text-primary group-hover:text-primary/80 transition-colors">@STANIXDOR</span>
            <svg
              viewBox="0 0 24 24"
              className="w-3.5 h-3.5 fill-current opacity-60 group-hover:opacity-100 transition-opacity"
              aria-hidden="true"
            >
              <path d="M18.244 2.25h3.308l-7.227 8.26 8.502 11.24H16.17l-5.214-6.817L4.99 21.75H1.68l7.73-8.835L1.254 2.25H8.08l4.713 6.231zm-1.161 17.52h1.833L7.084 4.126H5.117z" />
            </svg>
          </a>
        </div>
      </motion.div>
    </div>
  );
}
