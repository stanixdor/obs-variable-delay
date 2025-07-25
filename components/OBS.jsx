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
import { AlertCircle, CheckCircle, Timer } from "lucide-react";
import { motion } from "framer-motion";

/**
 * Hook de estado persistente en localStorage
 * Retorna [valor, setValor] igual que useState,
 * pero sincroniza automáticamente con localStorage.
 */
function usePersistentState(key, defaultValue) {
  const [value, setValue] = useState(() => {
    if (typeof window === "undefined") return defaultValue;
    try {
      const saved = window.localStorage.getItem(key);
      return saved !== null ? JSON.parse(saved) : defaultValue;
    } catch (_err) {
      // localStorage inaccesible o parseo fallido
      return defaultValue;
    }
  });

  useEffect(() => {
    if (typeof window === "undefined") return;
    try {
      window.localStorage.setItem(key, JSON.stringify(value));
    } catch (_err) {
      /* Silencioso – p.ej. usuario en modo incógnito */
    }
  }, [key, value]);

  return [value, setValue];
}

/**
 * ObsController – Video‑Delay con persistencia local
 * ------------------------------------------------------------------------------
 * Extiende el componente original añadiendo almacenamiento en localStorage para
 * todas las variables de entrada (puerto, contraseña, selecciones y delay).
 * Los datos se restauran automáticamente tras un F5 y se guardan al volar en
 * cada cambio de estado.
 */
export default function ObsController() {
  const obsRef = useRef(null);

  /* ------------------ Estado persistente ------------------ */
  const [port, setPort] = usePersistentState("obs:port", "4455");
  const [password, setPassword] = usePersistentState("obs:password", "");
  const [delayInput, setDelayInput] = usePersistentState("obs:delayInput", "");
  const [bridgeInput, setBridgeInput] = usePersistentState("obs:bridgeInput", "");
  const [delaySec, setDelaySec] = usePersistentState("obs:delaySec", 30);

  /* -------------- Estado no persistente ------------------- */
  const [status, setStatus] = useState("disconnected");
  const [errorMsg, setErrorMsg] = useState("");
  const [inputs, setInputs] = useState([]);

  // Ref para acceder a delayInput dentro de callbacks
  const delayInputRef = useRef(delayInput);
  useEffect(() => {
    delayInputRef.current = delayInput;
  }, [delayInput]);

  /* ------------------ Conectar OBS ------------------ */
  const connect = async () => {
    setStatus("connecting");
    setErrorMsg("");
    try {
      const obs = new OBSWebSocket();
      await obs.connect(`ws://localhost:${port}`, password || undefined, {
        rpcVersion: 1,
        eventSubscriptions: EventSubscription.General | EventSubscription.Outputs,
      });
      obsRef.current = obs;
      setStatus("connected");
      obs.on("ConnectionClosed", () => setStatus("disconnected"));

      // Listener para inicio de grabación
      obs.on("RecordStateChanged", async ({ outputState }) => {
        if (outputState === "OBS_WEBSOCKET_OUTPUT_STARTED") {
          await pulseSource(delayInputRef.current, 5000);
        }
      });

      // Lista inputs
      const { inputs: list } = await obs.call("GetInputList", {});
      const names = list.map((i) => i.inputName);
      setInputs(names);

      // Si no hay valor persistido usamos los primeros nombres disponibles
      setDelayInput((prev) => (prev || names[0] || ""));
      setBridgeInput((prev) => (prev || names[1] || ""));
    } catch (err) {
      setStatus("error");
      setErrorMsg(String(err?.message || err));
    }
  };

  /* ---------- Filtros util ---------- */
  const removeDelayFilters = async (sourceName) => {
    const obs = obsRef.current;
    const { filters } = await obs.call("GetSourceFilterList", { sourceName });
    for (const f of filters.filter((f) => f.filterKind === "async_delay_filter")) {
      await obs.call("RemoveSourceFilter", {
        sourceName,
        filterName: f.filterName,
      });
    }
  };

  const MAX_MS_PER_FILTER = 20000;
  const createDelayFilters = async (sourceName, totalMs) => {
    const obs = obsRef.current;
    let remaining = totalMs;
    let idx = 1;
    while (remaining > 0) {
      const chunk = Math.min(MAX_MS_PER_FILTER, remaining);
      await obs.call("CreateSourceFilter", {
        sourceName,
        filterName: `delay_${Date.now()}_${idx}`,
        filterKind: "async_delay_filter",
        filterSettings: { delay_ms: chunk, delay: chunk },
      });
      remaining -= chunk;
      idx += 1;
    }
  };

  /* ---------- Helpers scene item ---------- */
  const setSceneItemEnabled = async (sourceName, enabled) => {
    if (!sourceName) return;
    const obs = obsRef.current;
    const { scenes } = await obs.call("GetSceneList", {});

    for (const { sceneName } of scenes) {
      try {
        const { sceneItems } = await obs.call("GetSceneItemList", { sceneName });
        const matchingItems = sceneItems.filter((it) => it.sourceName === sourceName);
        for (const item of matchingItems) {
          await obs.call("SetSceneItemEnabled", {
            sceneName,
            sceneItemId: item.sceneItemId,
            sceneItemEnabled: enabled,
          });
        }
      } catch (_err) {
        console.warn(`No se pudo procesar la escena ${sceneName}`, _err);
      }
    }

    try {
      await obs.call("SetInputMute", {
        inputName: sourceName,
        inputMuted: !enabled,
      });
    } catch (_err) {
      console.warn(`No se pudo (des)mutear ${sourceName}`, _err);
    }
  };

  const pulseSource = async (sourceName, sleepTime = 0) => {
    if (!sourceName) return;
    const obs = obsRef.current;
    if (sleepTime > 0) await new Promise((r) => setTimeout(r, sleepTime));

    const { inputSettings } = await obs.call("GetInputSettings", { inputName: sourceName });
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
      obs.call("SetInputSettings", {
        inputName: sourceName,
        inputSettings: { ...inputSettings, [pathKey]: value },
        overlay: false,
      });

    await setPath("");
    await new Promise((r) => setTimeout(r, 100));
    await setPath(originalPath);
  };

  /* ---------------- Poner delay ---------------- */
  const enableDelay = async () => {
    if (!obsRef.current || status !== "connected" || !delayInput) return;
    setErrorMsg("");
    try {
      await setSceneItemEnabled(bridgeInput, true);
      await removeDelayFilters(delayInput);
      await createDelayFilters(delayInput, delaySec * 1000);
      setTimeout(() => {
        setSceneItemEnabled(bridgeInput, false).catch(console.error);
      }, delaySec * 1000);
    } catch (err) {
      setErrorMsg(err?.message || "Error al poner delay");
    }
  };

  /* --------------- Quitar delay ---------------- */
  const disableDelay = async () => {
    if (!obsRef.current || status !== "connected" || !delayInput) return;
    setErrorMsg("");
    try {
      await setSceneItemEnabled(bridgeInput, false);
      await removeDelayFilters(delayInput);
    } catch (err) {
      setErrorMsg(err?.message || "Error al quitar delay");
    }
  };

  /* ------------------ UI helpers ------------------ */
  const color = {
    disconnected: "text-gray-500",
    connecting: "text-yellow-500",
    connected: "text-green-500",
    error: "text-red-500",
  }[status];

  return (
    <motion.div
      initial={{ opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      className="max-w-md mx-auto rounded-2xl shadow p-6 space-y-6"
    >
      {/* STATUS */}
      <div className="flex items-center space-x-2">
        {status === "connected" ? (
          <CheckCircle className={color} />
        ) : (
          <AlertCircle className={color} />
        )}
        <span className={`font-semibold ${color}`}>
          {status === "connected"
            ? "OBS conectado"
            : status === "connecting"
            ? "Conectando…"
            : status === "error"
            ? "Error"
            : "Desconectado"}
        </span>
      </div>

      {/* CONEXION */}
      <div className="grid grid-cols-1 gap-4">
        <div className="space-y-1">
          <label className="text-sm font-medium">Puerto</label>
          <Input value={port} onChange={(e) => setPort(e.target.value)} />
        </div>
        <div className="space-y-1">
          <label className="text-sm font-medium">Contraseña</label>
          <Input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
          />
        </div>
        <Button onClick={connect} disabled={status === "connecting"}>
          Conectar
        </Button>
      </div>

      {/* SELECTORES */}
      {status === "connected" && (
        <div className="space-y-4 pt-6 border-t">
          <div className="space-y-1">
            <label className="text-sm font-medium">video delay</label>
            <Select value={delayInput} onValueChange={setDelayInput}>
              <SelectTrigger className="w-full">
                <SelectValue placeholder="Input" />
              </SelectTrigger>
              <SelectContent>
                {inputs.map((n) => (
                  <SelectItem key={n} value={n}>
                    {n}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
          <div className="space-y-1">
            <label className="text-sm font-medium">
              video mientras se pone delay
            </label>
            <Select value={bridgeInput} onValueChange={setBridgeInput}>
              <SelectTrigger className="w-full">
                <SelectValue placeholder="Input" />
              </SelectTrigger>
              <SelectContent>
                {inputs.map((n) => (
                  <SelectItem key={n} value={n}>
                    {n}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>
        </div>
      )}

      {/* SLIDER + BOTONES */}
      {status === "connected" && (
        <div className="space-y-4 pt-6 border-t">
          <label className="text-sm font-medium flex items-center gap-2">
            <Timer className="w-4 h-4" /> Delay ({delaySec}s)
          </label>
          <Slider
            value={[delaySec]}
            min={5}
            max={300}
            step={5}
            onValueChange={(v) => setDelaySec(v[0])}
          />
          <div className="flex gap-2 flex-wrap">
            <Button onClick={enableDelay} variant="secondary">
              Poner delay
            </Button>
            <Button onClick={disableDelay} variant="destructive">
              Quitar delay
            </Button>
          </div>
        </div>
      )}

      {errorMsg && (
        <p className="text-sm text-red-600 whitespace-pre-wrap">{errorMsg}</p>
      )}
    </motion.div>
  );
}
