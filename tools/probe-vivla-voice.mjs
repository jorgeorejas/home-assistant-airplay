#!/usr/bin/env node
// Probe the vivla voice gateway: connect, stream an utterance, record the
// reply, log every control event. Run BEFORE flashing firmware to validate
// the path device → /voice → Gemini Live → Hermes → audio reply.
//
// The gateway lives in vivla-intelligence at apps/mastra/voice-bridge/.
// Local dev: from vivla-intelligence run `pnpm --filter mastra start`
// (with GOOGLE_API_KEY and DEVICE_TOKEN in apps/mastra/.env). Then:
//
//   node tools/probe-vivla-voice.mjs \
//        --bridge ws://localhost:3002/voice \
//        --token <DEVICE_TOKEN> \
//        --in    tools/sample-16k.pcm \
//        --out   /tmp/reply-24k.pcm
//
//   ffplay -f s16le -ar 24000 -ac 1 /tmp/reply-24k.pcm   # play reply
//
// Prod: --bridge wss://api.vivla.ai/voice (once deployed).
//
// The input PCM must be 16-bit little-endian, mono, 16kHz. To make one from
// any audio file:
//   ffmpeg -i utterance.m4a -ac 1 -ar 16000 -f s16le tools/sample-16k.pcm

// Uses Node's built-in WebSocket (Node 22+). Auth via ?token=... query param
// because the WHATWG WebSocket constructor can't set custom headers.

import { writeFile, readFile } from "node:fs/promises";
import { setTimeout as sleep } from "node:timers/promises";

const args = parseArgs(process.argv.slice(2));
const baseUrl = args.bridge ?? "ws://localhost:3002/voice";
const inputPath = args.in ?? "tools/sample-16k.pcm";
const outputPath = args.out ?? "/tmp/reply-24k.pcm";
const deviceToken = args.token ?? process.env.DEVICE_TOKEN ?? "";
const chunkBytes = 640; // 20ms at 16kHz mono PCM16

const url = new URL(baseUrl);
if (deviceToken) url.searchParams.set("token", deviceToken);
const ws = new WebSocket(url);
ws.binaryType = "arraybuffer";

const replyChunks = [];
let ready = false;
let toolCalled = false;

ws.addEventListener("open", () => {
  console.log("→ bridge connected");
});

ws.addEventListener("message", async (event) => {
  const data = event.data;
  if (data instanceof ArrayBuffer) {
    replyChunks.push(Buffer.from(data));
    process.stdout.write(`audio +${data.byteLength}b `.padEnd(15));
    return;
  }
  const evt = safeJson(typeof data === "string" ? data : Buffer.from(data).toString("utf8"));
  if (!evt) return;
  switch (evt.type) {
    case "session.ready":
      console.log("\n✓ session.ready");
      ready = true;
      streamUtterance().catch((err) => fail(`stream: ${err.message}`));
      break;
    case "transcript":
      console.log(`\n  [${evt.role}${evt.final ? "/final" : ""}] ${evt.text}`);
      break;
    case "tool.call":
      console.log(`\n→ tool.call ${evt.name}`, JSON.stringify(evt.input).slice(0, 200));
      toolCalled = true;
      break;
    case "tool.result":
      console.log(
        `\n← tool.result ${evt.name} ${evt.ok ? "ok" : "FAIL"}: ${evt.preview}`,
      );
      break;
    case "interrupted":
      console.log("\n  (interrupted)");
      break;
    case "turn.complete":
      console.log("\n✓ turn.complete");
      await finish(0);
      break;
    case "error":
      fail(`bridge error (${evt.source}): ${evt.message}`);
      break;
    case "session.closed":
      fail("bridge closed session");
      break;
    default:
      console.log("\n?", evt);
  }
});

ws.addEventListener("error", (event) => {
  fail(`ws error: ${event.message ?? "unknown"}`);
});
ws.addEventListener("close", (event) => {
  if (!ready) fail(`closed before ready (${event.code} ${event.reason})`);
});

setTimeout(() => fail("timeout — no turn.complete in 60s"), 60_000).unref();

async function streamUtterance() {
  let pcm;
  try {
    pcm = await readFile(inputPath);
  } catch (err) {
    fail(`cannot read ${inputPath}: ${err.message}`);
    return;
  }
  console.log(`→ streaming ${pcm.length} bytes (${(pcm.length / 32_000).toFixed(2)}s @ 16k mono PCM16)`);
  for (let i = 0; i < pcm.length; i += chunkBytes) {
    const chunk = pcm.subarray(i, Math.min(i + chunkBytes, pcm.length));
    ws.send(chunk);
    await sleep(20);
  }
  ws.send(JSON.stringify({ type: "audio.end" }));
  console.log("→ audio.end");
}

async function finish(code) {
  const reply = Buffer.concat(replyChunks);
  await writeFile(outputPath, reply);
  console.log(
    `\nwrote ${reply.length} bytes → ${outputPath} (${(reply.length / 48_000).toFixed(2)}s @ 24k mono PCM16)`,
  );
  console.log(`tool delegated to Hermes: ${toolCalled ? "yes" : "no"}`);
  if (reply.length === 0) {
    console.error("WARNING: zero audio bytes returned");
    code = 2;
  }
  ws.close();
  process.exit(code);
}

function fail(msg) {
  console.error(`\n✗ ${msg}`);
  try { ws.close(); } catch {}
  process.exit(1);
}

function safeJson(s) {
  try { return JSON.parse(s); } catch { return null; }
}

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith("--")) {
      const k = a.slice(2);
      const v = argv[i + 1];
      if (v && !v.startsWith("--")) {
        out[k] = v;
        i++;
      } else {
        out[k] = true;
      }
    }
  }
  return out;
}
