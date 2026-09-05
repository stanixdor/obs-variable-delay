// Decode the loopback-only integration artifacts. This checks actual muxed
// packets/content in addition to the driver's production state assertions.
import assert from "node:assert/strict";
import {execFileSync, spawnSync} from "node:child_process";
import {existsSync} from "node:fs";
import {dirname, join} from "node:path";
import {fileURLToPath} from "node:url";

const directory = process.argv[2];
assert.ok(directory, "Usage: node tests/verify-multistream.mjs <artifact-directory>");
const run = (tool, args) => execFileSync(tool, args, {maxBuffer: 64 * 1024 * 1024});
const sharedFiles = ["fast.flv", "secondary-before-reconnect.flv", "secondary-after-reconnect.flv", "slow.flv"];
const controllerFiles = ["controller-hardware.flv", "controller-second.flv", "profile-stop.flv", "collection-stop.flv"];
let reference;
for (const name of [...sharedFiles, ...controllerFiles.filter((name) => existsSync(join(directory, name)))]) {
  const file = join(directory, name);
  const probe = JSON.parse(run("ffprobe", ["-v", "error", "-show_streams", "-show_packets",
    "-show_data_hash", "sha256", "-of", "json", file]));
  assert.deepEqual(probe.streams.map((stream) => stream.codec_name).sort(), ["aac", "h264"]);
  const previous = new Map();
  for (const packet of probe.packets) {
    const dts = Number(packet.dts);
    assert.ok(Number.isFinite(dts) && (!previous.has(packet.stream_index) || dts > previous.get(packet.stream_index)),
      `${name}: DTS increases strictly per track`);
    previous.set(packet.stream_index, dts);
  }
  const firstVideo = probe.packets.find((packet) => packet.codec_type === "video");
  assert.ok(firstVideo?.flags.includes("K"), `${name}: first video packet is a keyframe`);
  assert.equal(Number(firstVideo.dts), 0, `${name}: new connection resets its own DTS epoch`);
  const decode = spawnSync("ffmpeg", ["-v", "error", "-xerror", "-i", file, "-f", "null", "-"], {encoding: "utf8"});
  assert.equal(decode.status, 0, decode.error?.message ?? decode.stderr);
  assert.equal(decode.stderr, "", `${name}: complete decode has no errors`);
  if (name === "fast.flv") {
    reference = new Set(probe.packets.map((packet) => packet.data_hash));
  } else if (sharedFiles.includes(name)) {
    for (const packet of probe.packets)
      assert.ok(reference.has(packet.data_hash), `${name}: payload is bit-identical to the shared master output`);
  }
  console.log(`${name}: PASS (${probe.packets.length} packets, H.264/AAC, IDR-first, own epoch, monotonic DTS, decode)`);
}

const contentVerifier = join(dirname(fileURLToPath(import.meta.url)), "verify-recording.mjs");
for (const name of ["fast.flv", "controller-hardware.flv"].filter((name) => existsSync(join(directory, name))))
  execFileSync(process.execPath, [contentVerifier, join(directory, name)], {stdio: "inherit"});
console.log("PASS: loopback content and shared encoded payloads, including independent reconnects.");
