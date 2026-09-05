import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";

const readJson = (path) => JSON.parse(readFileSync(new URL(path, import.meta.url), "utf8"));
const pkg = readJson("../package.json");
const lock = readJson("../package-lock.json");
const build = readJson("../../buildspec.json");

test("website, lockfile and native deliverables use one release version", () => {
  assert.match(pkg.version, /^\d+\.\d+\.\d+$/);
  assert.equal(pkg.version, build.version);
  assert.equal(lock.version, pkg.version);
  assert.equal(lock.packages[""].version, pkg.version);
});

test("release notes include bilingual installation and all platform downloads", () => {
  const notes = readFileSync(new URL(`../../docs/releases/${pkg.version}.md`, import.meta.url), "utf8");
  assert.ok(notes.includes("## English"));
  assert.ok(notes.includes("## Español"));
  for (const platform of ["macos-universal.zip", "windows-x64.zip", "x86_64-linux-gnu.deb"]) {
    assert.ok(notes.includes(`${build.name}-${pkg.version}-${platform}`), platform);
  }
  assert.ok(notes.includes("SHA256SUMS.txt"));
});

test("English and Spanish have matching message structure and placeholders", () => {
  const en = readJson("../messages/en.json");
  const es = readJson("../messages/es.json");
  function compare(left, right, path = "messages") {
    assert.equal(typeof right, typeof left, path);
    if (typeof left === "string") {
      const placeholders = (value) => [...value.matchAll(/\{([a-zA-Z]\w*)\}/g)].map((match) => match[1]).sort();
      assert.ok(right.length > 0, path);
      assert.deepEqual(placeholders(right), placeholders(left), path);
    } else {
      assert.deepEqual(Object.keys(right).sort(), Object.keys(left).sort(), path);
      for (const key of Object.keys(left)) compare(left[key], right[key], `${path}.${key}`);
    }
  }
  compare(en, es);
});
