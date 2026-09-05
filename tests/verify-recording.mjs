// Verify the synthetic red/440 Hz, green/220 Hz, blue/880 Hz recordings
// emitted by output_session_integration using real FFmpeg decoding.
import assert from "node:assert/strict";
import {execFileSync, spawnSync} from "node:child_process";

const file = process.argv[2];
assert.ok(file, "Usage: node tests/verify-recording.mjs <integration.mp4>");
const run = (tool, args) => execFileSync(tool, args, {maxBuffer: 64 * 1024 * 1024});
const probe = JSON.parse(run("ffprobe", ["-v", "error", "-show_streams", "-show_packets", "-of", "json", file]));
const previous = new Map();
const counts = new Map();
for (const packet of probe.packets) {
  const dts = Number(packet.dts);
  assert.ok(Number.isFinite(dts), "Packet has a finite DTS");
  assert.ok(!previous.has(packet.stream_index) || dts > previous.get(packet.stream_index),
    `DTS must increase on stream ${packet.stream_index}`);
  previous.set(packet.stream_index, dts);
  counts.set(packet.stream_index, (counts.get(packet.stream_index) ?? 0) + 1);
}
assert.equal(probe.streams.length, 2, "One video and one audio stream");
// A nonzero exit or any decoder diagnostic is a test failure.
const decoded = spawnSync("ffmpeg", ["-v", "error", "-xerror", "-i", file, "-f", "null", "-"], {encoding: "utf8"});
assert.equal(decoded.status, 0, decoded.error?.message ?? decoded.stderr);
assert.equal(decoded.stderr, "", "Full decode must not report errors");
const video = run("ffmpeg", ["-v", "error", "-i", file, "-vf", "fps=30,scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]);
const segments = [];
for (let i = 0; i < video.length; i += 3) {
  const color = [video[i], video[i + 1], video[i + 2]];
  const dominant = color.indexOf(Math.max(...color));
  assert.ok(color[dominant] > 180, "Expected a fully saturated synthetic scene");
  const last = segments.at(-1);
  if (last?.color === dominant) last.end = (i / 3 + 1) / 30;
  else segments.push({color: dominant, start: i / 3 / 30, end: (i / 3 + 1) / 30});
}
assert.deepEqual(segments.map((segment) => segment.color), [0, 1, 0, 2],
  "Video shows live red, hold green, delayed red, then live blue");
const audio = run("ffmpeg", ["-v", "error", "-i", file, "-vn", "-ac", "1", "-ar", "48000", "-f", "f32le", "-"]);
const samples = new Float32Array(audio.buffer, audio.byteOffset, audio.length / 4);
const frequencies = [440, 220, 880];
function amplitude(at, hz) {
  const start = Math.round(at * 48000);
  const length = 4800;
  assert.ok(start + length <= samples.length, "Audio covers the tested video window");
  let re = 0, im = 0;
  for (let i = 0; i < length; i++) {
    const phase = 2 * Math.PI * hz * i / 48000;
    re += samples[start + i] * Math.cos(phase);
    im += samples[start + i] * Math.sin(phase);
  }
  return 2 * Math.hypot(re, im) / length;
}
for (const segment of segments) {
  const at = (segment.start + segment.end) / 2 - 0.05;
  const amplitudes = frequencies.map((hz) => amplitude(at, hz));
  assert.equal(amplitudes.indexOf(Math.max(...amplitudes)), segment.color,
    `Correct source tone accompanies color ${segment.color} at ${at.toFixed(3)} s`);
  assert.ok(amplitudes[segment.color] > 0.02, "Hold and program audio must not be silent");
  segment.toneHz = frequencies[segment.color];
  segment.toneAmplitude = amplitudes[segment.color];
}
const audioTransitions = [];
let previousTone = 0;
for (let at = 0.2; at + 0.1 < samples.length / 48000; at += 0.01) {
  const amplitudes = frequencies.map((hz) => amplitude(at, hz));
  const tone = amplitudes.indexOf(Math.max(...amplitudes));
  if (tone !== previousTone) {
    audioTransitions.push({color: tone, at: at + 0.05});
    previousTone = tone;
  }
}
assert.deepEqual(audioTransitions.map(({color}) => color), [1, 0, 2], "Audio transitions match scene order");
for (let i = 0; i < audioTransitions.length; i++) {
  const offset = audioTransitions[i].at - segments[i + 1].start;
  assert.ok(Math.abs(offset) < 0.12, `A/V transition offset is bounded: ${offset.toFixed(3)} s`);
  audioTransitions[i].offsetSeconds = offset;
}
console.log(JSON.stringify({file, packets: Object.fromEntries(counts), streams: probe.streams.map(({codec_name, duration, has_b_frames}) => ({codec_name, duration, has_b_frames})), segments, audioTransitions}, null, 2));
console.log("PASS: full decode, strictly monotonic DTS, scene order, non-silent audio, and A/V transition alignment.");
