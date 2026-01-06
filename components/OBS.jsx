// "use client";

// import { useState, useRef, useEffect } from "react";
// import OBSWebSocket, { EventSubscription } from "obs-websocket-js";
// import { Input } from "@/components/ui/input";
// import { Button } from "@/components/ui/button";
// import {
//   Select,
//   SelectTrigger,
//   SelectContent,
//   SelectItem,
//   SelectValue,
// } from "@/components/ui/select";
// import { Slider } from "@/components/ui/slider";
// import { AlertCircle, CheckCircle, Timer } from "lucide-react";
// import { motion } from "framer-motion";

// /**
//  * Hook de estado persistente en localStorage
//  */
// function usePersistentState(key, defaultValue) {
//   const [value, setValue] = useState(() => {
//     if (typeof window === "undefined") return defaultValue;
//     try {
//       const saved = window.localStorage.getItem(key);
//       return saved !== null ? JSON.parse(saved) : defaultValue;
//     } catch (_err) {
//       return defaultValue;
//     }
//   });

//   useEffect(() => {
//     if (typeof window === "undefined") return;
//     try {
//       window.localStorage.setItem(key, JSON.stringify(value));
//     } catch (_err) {
//       /* modo incógnito – ignorar */
//     }
//   }, [key, value]);

//   return [value, setValue];
// }

// /**
//  * ObsControllerDelay – versión extendida con Escena Delay
//  * ---------------------------------------------------------------------------
//  * 1. Selector "Escena Delay" para elegir la escena contenedora.
//  * 2. Botón "Sincronizar Escena Delay" →
//  *    · Borra todos los sources de la escena salvo los elegidos en los selects
//  *      "video delay" y "video mientras se pone delay".
//  *    · Añade TODAS las demás escenas del proyecto como sources (tipo escena).
//  * 3. Nuevo flujo `enableDelay` / `disableDelay` basado en grabación + muting.
//  * 4. Listener en tiempo real que, cuando no hay delay, replica la escena
//  *    program actual dentro de Escena Delay activando su source correspondiente.
//  */
// export default function ObsControllerDelay() {
//   const obsRef = useRef(null);

//   /* -------------------- Estado persistente -------------------- */
//   const [port, setPort] = usePersistentState("obs:port", "4455");
//   const [password, setPassword] = usePersistentState("obs:password", "");
//   const [delayInput, setDelayInput] = usePersistentState("obs:delayInput", "");
//   const [bridgeInput, setBridgeInput] = usePersistentState("obs:bridgeInput", "");
//   const [delayScene, setDelayScene] = usePersistentState("obs:delayScene", "");
//   const [delaySceneFinal, setDelaySceneFinal] = usePersistentState("obs:delaySceneFinal", "");
//   const [delaySec, setDelaySec] = usePersistentState("obs:delaySec", 30);

//   /* -------------------- Estado no persistente ----------------- */
//   const [status, setStatus] = useState("disconnected");
//   const [errorMsg, setErrorMsg] = useState("");
//   const [inputs, setInputs] = useState([]); // lista de fuentes
//   const [scenes, setScenes] = useState([]); // lista de nombres de escena
//   const [delayActive, setDelayActive] = useState(false); // ¿hay delay ahora?
//   const delayActiveRef = useRef(delayActive);
//   useEffect(() =>
//   {
//     delayActiveRef.current = delayActive;
//   }, [delayActive]);

//   /* ------------------- Helpers generales ---------------------- */
//   const obs = () => obsRef.current;
//   const ensureObs = () => {
//     if (!obs()) throw new Error("OBS no conectado");
//   };

//   /* -------------- Conectar con OBS ---------------------------- */
//   const connect = async () => {
//     setStatus("connecting");
//     setErrorMsg("");
//     try {
//       const instance = new OBSWebSocket();
//       await instance.connect(`ws://localhost:${port}`, password || undefined, {
//         rpcVersion: 1,
//         // Necesitamos eventos de escenas y de salidas
//         eventSubscriptions:
//           EventSubscription.General |
//           EventSubscription.Outputs |
//           EventSubscription.Scenes,
//       });
//       obsRef.current = instance;
//       setStatus("connected");
//       instance.on("ConnectionClosed", () => setStatus("disconnected"));

//       /* ---------- Cargar listas de fuentes y escenas --------- */
//       const [{ inputs: inputList }, { scenes: sceneList }] = await Promise.all([
//         instance.call("GetInputList", {}),
//         instance.call("GetSceneList", {}),
//       ]);
//       setInputs(inputList.map((i) => i.inputName));
//       const sceneNames = sceneList.map((s) => s.sceneName);
//       setScenes(sceneNames);

//       // Valores por defecto si no había persistencia
//       setDelayInput((v) => v || inputList[0]?.inputName || "");
//       setBridgeInput((v) => v || inputList[1]?.inputName || "");
//       setDelayScene((v) => v || sceneNames.find((n) => n !== "Escena Delay" && n !== "Escena Delay Final") || sceneNames[0] || "");
//       setDelaySceneFinal((v) => v || sceneNames.find((n) => n === "Escena Delay Final") || sceneNames[0] || "");

//       /* ----------- Listener de cambio de escena program -------- */
//       instance.on("CurrentProgramSceneChanged", async ({ sceneName }) => {
//         if (!delayActiveRef.current) {
//           console.log("Cambiando escena program dentro de Delay:", sceneName);
//           await mirrorProgramScene(sceneName).catch(console.error);
//         }
//       });

//       // Sincronizar estado inicial si no hay delay activo
//       const { currentProgramSceneName } = await instance.call("GetCurrentProgramScene", {});
//       await mirrorProgramScene(currentProgramSceneName).catch(console.error);
//     } catch (err) {
//       setStatus("error");
//       setErrorMsg(String(err?.message || err));
//     }
//   };

//   /* ----------------- Utilidades de escena --------------------- */
//   // Activa/desactiva y (des)mutea un solo item dentro de una escena.
//   const setSceneItemEnabledInScene = async (sceneName, sourceName, enabled) => {
//     if (!sceneName || !sourceName) return;
//     ensureObs();
//     try {
//       const { sceneItems } = await obs().call("GetSceneItemList", { sceneName });
//       const matches = sceneItems.filter((it) => it.sourceName === sourceName);
//       for (const item of matches) {
//         await obs().call("SetSceneItemEnabled", {
//           sceneName,
//           sceneItemId: item.sceneItemId,
//           sceneItemEnabled: enabled,
//         });
//       }
//       await obs().call("SetInputMute", {
//         inputName: sourceName,
//         inputMuted: !enabled,
//       });
//     } catch (e) {
//       console.warn(`No se pudo actualizar ${sourceName} en ${sceneName}`, e);
//     }
//   };

//   // Desactiva + mutea TODOS los items de una escena excepto los indicados
//   const disableAllExcept = async (sceneName, keep = []) => {
//     ensureObs();
//     const { sceneItems } = await obs().call("GetSceneItemList", { sceneName });
//     for (const item of sceneItems) {
//       const shouldEnable = keep.includes(item.sourceName);
//       await setSceneItemEnabledInScene(sceneName, item.sourceName, shouldEnable);
//     }
//   };

//   // Elimina un source de una escena
//   const removeSourceFromScene = async (sceneName, sourceName) => {
//     if (!sceneName || !sourceName) return;
//     ensureObs();
//     try {
//       const { sceneItems } = await obs().call("GetSceneItemList", { sceneName });
//       const matches = sceneItems.filter((it) => it.sourceName === sourceName);
//       for (const item of matches) {
//         await obs().call("RemoveSceneItem", {
//           sceneName,
//           sceneItemId: item.sceneItemId,
//         });
//       }
//     } catch (e) {
//       console.warn(`No se pudo eliminar ${sourceName} de ${sceneName}`, e);
//     }
//   };

//   // Añade un source a una escena si no existe
//   const addSourceToScene = async (sceneName, sourceName, enabled = true) => {
//     if (!sceneName || !sourceName) return;
//     ensureObs();
//     try {
//       // Verificar si ya existe
//       const { sceneItems } = await obs().call("GetSceneItemList", { sceneName });
//       const exists = sceneItems.some((it) => it.sourceName === sourceName);
//       if (exists) {
//         // Si existe, solo habilitarlo
//         await setSceneItemEnabledInScene(sceneName, sourceName, enabled);
//         return;
//       }
//       // Crear el item
//       await obs().call("CreateSceneItem", {
//         sceneName,
//         sourceName,
//         sceneItemEnabled: enabled,
//       });
//     } catch (e) {
//       console.warn(`No se pudo añadir ${sourceName} a ${sceneName}`, e);
//     }
//   };

//   const pulseSource = async (sourceName, sleepTime = 0) =>
//   {
//     if (!sourceName) return;
//     const obs = obsRef.current;
//     if (sleepTime > 0) await new Promise((r) => setTimeout(r, sleepTime));

//     const { inputSettings } = await obs.call("GetInputSettings", { inputName: sourceName });
//     const pathKey =
//       inputSettings.local_file !== undefined
//         ? "local_file"
//         : inputSettings.file !== undefined
//           ? "file"
//           : inputSettings.path !== undefined
//             ? "path"
//             : null;
//     if (!pathKey) return;

//     const originalPath = inputSettings[pathKey];
//     const setPath = async (value) =>
//       obs.call("SetInputSettings", {
//         inputName: sourceName,
//         inputSettings: { ...inputSettings, [pathKey]: value },
//         overlay: false,
//       });

//     await setPath("");
//     await new Promise((r) => setTimeout(r, 300));
//     await setPath(originalPath);
//   };

//   /* ------------- Botón: Sincronizar Escena Delay -------------- */
//   const syncDelayScene = async () => {
//     if (!delayScene || !delaySceneFinal) return;
//     setErrorMsg("");
//     try {
//       ensureObs();

//       // 1. Borrar items que no sean delayInput o bridgeInput
//       const { sceneItems } = await obs().call("GetSceneItemList", { sceneName: delayScene });
//       for (const item of sceneItems) {
//         // if (![delayInput, bridgeInput].includes(item.sourceName)) {
//         //   await obs().call("RemoveSceneItem", {
//         //     sceneName: delayScene,
//         //     sceneItemId: item.sceneItemId,
//         //   });
//         // }
//         await obs().call("RemoveSceneItem", {
//           sceneName: delayScene,
//           sceneItemId: item.sceneItemId,
//         });
//       }

//       // 2. Añadir todas las escenas como sources (tipo escena)
//       for (const sceneName of scenes) {
//         console.log("Añadiendo escena:", sceneName);
//         // No añadir la escena delay ni la escena delay final
//         if (sceneName === delayScene || sceneName === delaySceneFinal) continue;
//         // Evitar duplicados
//         // const already = sceneItems.find((it) => it.sourceName === sceneName);
//         // if (already) continue;

//         await obs().call("CreateSceneItem", {
//           sceneName: delayScene,
//           sourceName: sceneName, // fuente de tipo "Scene"
//           sceneItemEnabled: false,
//         });
//       }

//       // 3. Añadir Escena Delay en Escena Delay Final si no está presente
//       await addSourceToScene(delaySceneFinal, delayScene, true);
//     } catch (err) {
//       setErrorMsg(err?.message || "Error al sincronizar escena delay");
//     }
//   };

//   /* ---------------------- enableDelay ------------------------- */
//   const enableDelay = async () => {
//     if (!obs() || status !== "connected" || !delayScene || !delaySceneFinal) return;
//     setErrorMsg("");
//     try {
//       // Iniciar grabación (ignoramos error si ya estuviera grabando)
//       await obs().call("StartVirtualCam").catch(() => {});

//       // Borramos el source de Escena Delay de Escena Delay Final
//       await removeSourceFromScene(delaySceneFinal, delayScene);

//       // Paso 1: activamos puente y desactivamos resto en escena final
//       await disableAllExcept(delaySceneFinal, [bridgeInput]);
//       await pulseSource(bridgeInput);

//       setDelayActive(true);

//       setTimeout(async () => {
//         try {
//           await pulseSource(delayInput); // pulso para evitar problemas de sincronización
//         } catch (e) {
//           console.error(e);
//         }
//       }, (delaySec * 1000) - 300);


//       // Paso 2: tras X segundos, cambiar a video delay en escena final
//       setTimeout(async () => {
//         try {
//           await setSceneItemEnabledInScene(delaySceneFinal, bridgeInput, false);
//           await setSceneItemEnabledInScene(delaySceneFinal, delayInput, true);
//         } catch (e) {
//           console.error(e);
//         }
//       }, delaySec * 1000);
//     } catch (err) {
//       setErrorMsg(err?.message || "Error al activar delay");
//     }
//   };

//   /* --------------------- disableDelay ------------------------- */
//   const disableDelay = async () => {
//     if (!obs() || status !== "connected" || !delayScene || !delaySceneFinal) return;
//     setErrorMsg("");
//     try {
//       await obs().call("StopVirtualCam").catch(() => {});
//       // Ocultamos/muteamos video delay en escena final
//       await setSceneItemEnabledInScene(delaySceneFinal, delayInput, false);

//       // Añadimos Escena Delay dentro de Escena Delay Final
//       await addSourceToScene(delaySceneFinal, delayScene, true);

//       // Activamos la escena actual dentro de Delay
//       const { currentProgramSceneName } = await obs().call("GetCurrentProgramScene", {});
//       await mirrorProgramScene(currentProgramSceneName);

//       setDelayActive(false);
//     } catch (err) {
//       setErrorMsg(err?.message || "Error al quitar delay");
//     }
//   };

//   /* ------ Sincronizar Escena Delay con la program actual ------ */
//   const mirrorProgramScene = async (programSceneName) => {
//     if (!delayScene || !programSceneName) return;
//     // Activar solo el source que coincide con la escena program
//     await disableAllExcept(delayScene, [programSceneName]);
//   };

//   /* ---------------------- UI helpers -------------------------- */
//   const color = {
//     disconnected: "text-gray-500",
//     connecting: "text-yellow-500",
//     connected: "text-green-500",
//     error: "text-red-500",
//   }[status];

//   return (
//     <motion.div
//       initial={{ opacity: 0, y: 8 }}
//       animate={{ opacity: 1, y: 0 }}
//       className="max-w-md mx-auto rounded-2xl shadow p-6 space-y-6"
//     >
//       {/* STATUS */}
//       <div className="flex items-center space-x-2">
//         {status === "connected" ? (
//           <CheckCircle className={color} />
//         ) : (
//           <AlertCircle className={color} />
//         )}
//         <span className={`font-semibold ${color}`}>
//           {status === "connected"
//             ? "OBS conectado"
//             : status === "connecting"
//             ? "Conectando…"
//             : status === "error"
//             ? "Error"
//             : "Desconectado"}
//         </span>
//       </div>

//       {/* CONEXIÓN */}
//       <div className="grid grid-cols-1 gap-4">
//         <div className="space-y-1">
//           <label className="text-sm font-medium">Puerto</label>
//           <Input value={port} onChange={(e) => setPort(e.target.value)} />
//         </div>
//         <div className="space-y-1">
//           <label className="text-sm font-medium">Contraseña</label>
//           <Input type="password" value={password} onChange={(e) => setPassword(e.target.value)} />
//         </div>
//         <Button onClick={connect} disabled={status === "connecting"}>
//           Conectar
//         </Button>
//       </div>

//       {/* SELECTORES */}
//       {status === "connected" && (
//         <div className="space-y-4 pt-6 border-t">
//           <div className="space-y-1">
//             <label className="text-sm font-medium">Escena Delay</label>
//             <Select value={delayScene} onValueChange={setDelayScene}>
//               <SelectTrigger className="w-full">
//                 <SelectValue placeholder="Escena" />
//               </SelectTrigger>
//               <SelectContent>
//                 {scenes.map((n) => (
//                   <SelectItem key={n} value={n}>
//                     {n}
//                   </SelectItem>
//                 ))}
//               </SelectContent>
//             </Select>
//           </div>

//           <div className="space-y-1">
//             <label className="text-sm font-medium">Escena Delay Final</label>
//             <Select value={delaySceneFinal} onValueChange={setDelaySceneFinal}>
//               <SelectTrigger className="w-full">
//                 <SelectValue placeholder="Escena Final" />
//               </SelectTrigger>
//               <SelectContent>
//                 {scenes.map((n) => (
//                   <SelectItem key={n} value={n}>
//                     {n}
//                   </SelectItem>
//                 ))}
//               </SelectContent>
//             </Select>
//           </div>

//           <div className="space-y-1">
//             <label className="text-sm font-medium">video delay</label>
//             <Select value={delayInput} onValueChange={setDelayInput}>
//               <SelectTrigger className="w-full">
//                 <SelectValue placeholder="Input" />
//               </SelectTrigger>
//               <SelectContent>
//                 {inputs.map((n) => (
//                   <SelectItem key={n} value={n}>
//                     {n}
//                   </SelectItem>
//                 ))}
//               </SelectContent>
//             </Select>
//           </div>
//           <div className="space-y-1">
//             <label className="text-sm font-medium">video mientras se pone delay</label>
//             <Select value={bridgeInput} onValueChange={setBridgeInput}>
//               <SelectTrigger className="w-full">
//                 <SelectValue placeholder="Input" />
//               </SelectTrigger>
//               <SelectContent>
//                 {inputs.map((n) => (
//                   <SelectItem key={n} value={n}>
//                     {n}
//                   </SelectItem>
//                 ))}
//               </SelectContent>
//             </Select>
//           </div>

//         </div>
//       )}

//       {/* BOTÓN DE SINCRONIZAR */}
//       {status === "connected" && (
//         <div className="pt-4 flex justify-end">
//           <Button onClick={syncDelayScene} variant="outline">
//             Sincronizar Escena Delay
//           </Button>
//         </div>
//       )}

//       {/* SLIDER + BOTONES DELAY */}
//       {status === "connected" && (
//         <div className="space-y-4 pt-6 border-t">
//           <label className="text-sm font-medium flex items-center gap-2">
//             <Timer className="w-4 h-4" /> Delay ({delaySec}s)
//           </label>
//           <Slider value={[delaySec]} min={5} max={300} step={5} onValueChange={(v) => setDelaySec(v[0])} />
//           <div className="flex gap-2 flex-wrap">
//             <Button onClick={enableDelay} variant="secondary">
//               Poner delay
//             </Button>
//             <Button onClick={disableDelay} variant="destructive">
//               Quitar delay
//             </Button>
//           </div>
//         </div>
//       )}

//       {errorMsg && <p className="text-sm text-red-600 whitespace-pre-wrap">{errorMsg}</p>}
//     </motion.div>
//   );
// }
