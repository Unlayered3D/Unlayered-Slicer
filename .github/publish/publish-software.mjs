#!/usr/bin/env node
// publish-software.mjs - publish Unlayered Slicer installers to unlayered3d.com without a browser,
// using a software publish token (app/lib/softwarePublishToken.ts). Meant for CI:
//
//   node scripts/publish-software.mjs list [--json]
//   node scripts/publish-software.mjs publish --platform <windows|mac|linux> [--arch <arch>] --version <v>
//        [--commit <sha>] [--branch <name>] [--build-url <actions run url>] [--notes-file <md>] [--prerelease] <file>
//
//   --site <url>              default https://unlayered3d.com (or $UNLAYERED_SITE)
//   --expect-channel <name>   fail, before uploading anything, unless the token is for this channel
//
// The token is read from $UNLAYERED_SOFTWARE_PUBLISH_TOKEN and is never printed. WHICH token it is
// decides the channel: the public Unlayered-Slicer repo's token publishes stable (/software), the
// private unlayered-slicer-dev repo's publishes dev (admins only), and the staging token a release
// run in staging mode (admins only). The script asks the site which one it holds; there is no
// --channel flag to get wrong. --expect-channel only checks: a release run passes it so a secret
// holding the wrong token fails instead of publishing somewhere unexpected.
//
// Publishing the same version, filename and size that is already live is a no-op, so re-running a
// CI job is safe.
//
// Dev builds are kept (the last 30 versions) and listed on /admin/software by version, with
// --branch shown beside each, so builds from different branches can be told apart.
//
// --arch: windows x64|arm64, mac universal, linux x64|arm64. Left out, the platform's default
// (windows x64, mac universal, linux x64) -- the build /software, the update check and older
// scripts mean by the platform alone.
//
// A stable publish is also the Slicer's update: /slicer/latest.json is built from the stable record
// (app/lib/slicerUpdateManifest.ts). The script always sends the file's SHA-256 - without it the
// site announces the release but never offers it for in-app installation. --notes-file becomes the
// update dialog's release notes; --prerelease puts it in the Slicer's prerelease channel (left out,
// a version with a "-suffix" counts as one).
//
// For a person publishing by hand there is also scripts/publish-slicer-release.mjs, which signs in
// as a site admin through the browser instead of holding a token.
//
// Bytes go straight to Blob through @vercel/blob/client's upload(), exactly as the admin page's
// browser does, so a 250 MB installer works; only the small token and record requests go through
// the site's functions. Same structure as publish-printer-updates.mjs.

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { upload } from "@vercel/blob/client";

// Every failure is THROWN and reported once, at the bottom. Calling process.exit() while fetch
// sockets are still closing aborts Node on Windows (libuv "UV_HANDLE_CLOSING" assertion) and
// swallows the actual error message.
class Fail extends Error {}
function die(msg) {
  throw new Fail(msg);
}

const VALUE_FLAGS = ["--site", "--platform", "--arch", "--version", "--commit", "--branch", "--build-url", "--notes-file", "--expect-channel"];
const BOOLEAN_FLAGS = ["--prerelease", "--json"];
const argv = process.argv.slice(2);
const opts = {};
const positional = [];
for (let i = 0; i < argv.length; i++) {
  if (VALUE_FLAGS.includes(argv[i])) {
    opts[argv[i].slice(2)] = argv[++i] ?? die(`${argv[i - 1]} needs a value`);
  } else if (BOOLEAN_FLAGS.includes(argv[i])) {
    opts[argv[i].slice(2)] = true;
  } else if (argv[i].startsWith("--")) {
    die(`unknown flag ${argv[i]}`);
  } else {
    positional.push(argv[i]);
  }
}
const [command, ...rest] = positional;
const site = (opts.site || process.env.UNLAYERED_SITE || "https://unlayered3d.com").replace(/\/+$/, "");

const token = (process.env.UNLAYERED_SOFTWARE_PUBLISH_TOKEN || "").trim();
const auth = { authorization: `Bearer ${token}` };

async function api(method, route, body) {
  const res = await fetch(`${site}/api/software-publish/${route}`, {
    method,
    headers: { ...auth, ...(body ? { "content-type": "application/json" } : {}) },
    body: body ? JSON.stringify(body) : undefined,
    redirect: "manual",
  });
  // A 3xx here is proxy.ts sending us to /login: the route is not deployed on this site yet.
  if (res.status >= 300 && res.status < 400) die(`${site} redirected ${route} - the publish routes are not deployed there.`);
  const text = await res.text();
  let json;
  try {
    json = JSON.parse(text);
  } catch {
    die(`${method} ${route}: HTTP ${res.status}, not JSON: ${text.slice(0, 200)}`);
  }
  if (!res.ok) die(`${method} ${route}: HTTP ${res.status}: ${json.error ?? text.slice(0, 200)}`);
  return json;
}

// Mirrors SOFTWARE_ARCHS in app/lib/softwareInstallers.ts; the first is each platform's default.
const ARCHS = { windows: ["x64", "arm64"], mac: ["universal"], linux: ["x64", "arm64"] };

const mb = (n) => `${(n / 1e6).toFixed(2)} MB`;
const when = (ms) => new Date(ms).toISOString().replace("T", " ").slice(0, 16);

function contentTypeFor(name) {
  const n = name.toLowerCase();
  if (n.endsWith(".dmg")) return "application/x-apple-diskimage";
  if (n.endsWith(".zip")) return "application/zip";
  // .exe and .AppImage: pinned rather than guessed, the site allowlists types.
  return "application/octet-stream";
}

// The token's channel, as the site reports it, checked against --expect-channel.
function checkChannel(channel) {
  const want = opts["expect-channel"];
  if (want && channel !== want) die(`this token publishes to ${channel}, not ${want}: check which secret holds it`);
}

async function list() {
  const { channel, installers } = await api("GET", "installers");
  checkChannel(channel);
  // For CI to read, e.g. the nightly build's "is main's HEAD already published?" check.
  if (opts.json) {
    console.log(JSON.stringify({ channel, installers }));
    return;
  }
  console.log(`token publishes to: ${channel}`);
  if (installers.length === 0) console.log("(nothing published on this channel)");
  for (const i of installers) {
    console.log(`${`${i.platform}/${i.arch}`.padEnd(16)} ${i.version.padEnd(28)} ${mb(i.fileSizeBytes).padStart(10)}  ${when(i.uploadedAt)}  ${i.filename}  [${i.uploadedBy}]`);
  }
}

async function publish(file) {
  const { platform, version, commit } = opts;
  if (!ARCHS[platform]) die("--platform must be windows, mac or linux");
  const arch = opts.arch || ARCHS[platform][0];
  if (!ARCHS[platform].includes(arch)) die(`--arch for ${platform} must be ${ARCHS[platform].join(" or ")}`);
  if (!version) die("--version is required");
  if (!file) die("publish needs the installer file");
  if (!fs.existsSync(file)) die(`no such file: ${file}`);

  const data = fs.readFileSync(file);
  const filename = path.basename(file);
  const sha256 = crypto.createHash("sha256").update(data).digest("hex");
  const notesFile = opts["notes-file"];
  if (notesFile && !fs.existsSync(notesFile)) die(`no such notes file: ${notesFile}`);
  const releaseNotes = notesFile ? fs.readFileSync(notesFile, "utf8") : undefined;
  const { channel, installers } = await api("GET", "installers");
  checkChannel(channel);

  const live = installers.find((i) => i.platform === platform && i.arch === arch);
  if (live && live.version === version && live.filename === filename && live.fileSizeBytes === data.length) {
    console.log(`already live on ${channel}: ${platform}/${arch} ${version} (${filename})`);
    return;
  }

  console.log(`uploading ${filename} (${mb(data.length)}) to ${channel}/${platform}/${arch} as ${version} ...`);
  const blob = await upload(`software-installers/${channel}/${filename}`, data, {
    access: "private",
    handleUploadUrl: `${site}/api/software-publish/upload-token`,
    headers: auth,
    contentType: contentTypeFor(filename),
    multipart: data.length > 8 * 1024 * 1024,
  });

  await api("POST", "installers", {
    platform,
    arch,
    version,
    filePathname: blob.pathname,
    filename,
    fileSizeBytes: data.length,
    ...(commit ? { commit } : {}),
    ...(opts.branch ? { branch: opts.branch } : {}),
    ...(opts["build-url"] ? { buildUrl: opts["build-url"] } : {}),
    sha256,
    ...(releaseNotes ? { releaseNotes } : {}),
    ...(opts.prerelease ? { prerelease: true } : {}),
  });
  console.log(`published ${platform}/${arch} ${version} to ${channel}${live ? ` (replaced ${live.version})` : ""}`);
}

async function main() {
  if (!token) die("UNLAYERED_SOFTWARE_PUBLISH_TOKEN is not set.");
  if (command === "list") {
    await list();
  } else if (command === "publish") {
    await publish(rest[0]);
  } else {
    die("usage: list [--json] | publish --platform <p> [--arch <a>] --version <v> [--commit <sha>] [--branch <name>] [--build-url <url>] [--notes-file <md>] [--prerelease] <file>   [--site <url>]");
  }
}

try {
  await main();
} catch (err) {
  console.error(`\nerror: ${err instanceof Fail ? err.message : err?.message ?? err}`);
  process.exitCode = 1;
}
