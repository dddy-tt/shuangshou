const fs = require("fs");
const path = require("path");
const { spawn } = require("child_process");
const WebSocket = require("ws");

const ROOT_DIR = path.resolve(__dirname, "..");
const DASHBOARD_DIR = path.resolve(ROOT_DIR, "..", "web-dashboard");
const BRIDGE_PORT = 8765;
const DASHBOARD_PORT = 3000;
const BRIDGE_HTTP_URL = `http://localhost:${BRIDGE_PORT}`;
const DASHBOARD_URL = `http://localhost:${DASHBOARD_PORT}`;
const BRIDGE_WS_URL = `ws://localhost:${BRIDGE_PORT}`;
const STARTUP_TIMEOUT_MS = 30000;
const WS_TIMEOUT_MS = 10000;
const FRAME_WAIT_MS = 5000;
const logPath = path.resolve(ROOT_DIR, "logs", "frame_log.jsonl");

const childProcesses = [];

function sleep(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

function prefixPipe(stream, prefix) {
  stream.setEncoding("utf8");
  stream.on("data", (chunk) => {
    chunk
      .split(/\r?\n/)
      .filter(Boolean)
      .forEach((line) => {
        const normalizedLine = line.startsWith(prefix) ? line.slice(prefix.length).trimStart() : line;
        console.log(`${prefix} ${normalizedLine}`);
      });
  });
}

function spawnProcess(command, args, options) {
  const env = {
    ...process.env,
    ...options.env
  };
  let child;

  if (process.platform === "win32") {
    const commandLine = [command, ...args].join(" ");
    child = spawn("cmd.exe", ["/d", "/s", "/c", commandLine], {
      cwd: options.cwd,
      env
    });
  } else {
    child = spawn(command, args, {
      cwd: options.cwd,
      env
    });
  }

  childProcesses.push(child);

  if (child.stdout) {
    prefixPipe(child.stdout, options.prefix);
  }

  if (child.stderr) {
    prefixPipe(child.stderr, `${options.prefix}[err]`);
  }

  child.on("exit", (code) => {
    console.log(`${options.prefix} exited with code ${code}`);
  });

  return child;
}

async function waitFor(predicate, timeoutMs, errorMessage) {
  const start = Date.now();

  while (Date.now() - start < timeoutMs) {
    const result = await predicate();

    if (result) {
      return result;
    }

    await sleep(500);
  }

  throw new Error(errorMessage);
}

async function safeFetch(url) {
  try {
    const response = await fetch(url);
    return response;
  } catch {
    return null;
  }
}

async function waitForBridgeReady() {
  return waitFor(async () => {
    const response = await safeFetch(`${BRIDGE_HTTP_URL}/health`);

    if (!response || !response.ok) {
      return null;
    }

    const json = await response.json();

    if (json?.service !== "serial-mqtt-ai-bridge") {
      return null;
    }

    return json;
  }, STARTUP_TIMEOUT_MS, "bridge health check timeout");
}

async function waitForDashboardReady() {
  return waitFor(async () => {
    const response = await safeFetch(DASHBOARD_URL);
    return response && response.ok ? true : null;
  }, STARTUP_TIMEOUT_MS, "dashboard health check timeout");
}

function readLogSnapshot() {
  if (!fs.existsSync(logPath)) {
    return [];
  }

  return fs
    .readFileSync(logPath, "utf8")
    .split(/\r?\n/)
    .filter(Boolean);
}

function parseFrameTypes(lines) {
  return new Set(
    lines
      .map((line) => {
        try {
          return JSON.parse(line);
        } catch {
          return null;
        }
      })
      .filter(Boolean)
      .map((entry) => entry.rawFrame || "")
      .map((rawFrame) => {
        if (rawFrame.startsWith("GESTURE")) {
          return "GESTURE";
        }

        if (rawFrame.startsWith("BRINGUP")) {
          return "BRINGUP";
        }

        if (rawFrame.startsWith("CTRL")) {
          return "CTRL";
        }

        return null;
      })
      .filter(Boolean)
  );
}

async function verifyWebSocket() {
  return new Promise((resolve) => {
    const types = new Set();
    let connected = false;
    let finished = false;

    const socket = new WebSocket(BRIDGE_WS_URL);

    function finish() {
      if (finished) {
        return;
      }

      finished = true;

      if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
        socket.close();
      }

      resolve({
        connected,
        types: Array.from(types)
      });
    }

    const timeout = setTimeout(finish, WS_TIMEOUT_MS);

    socket.on("open", () => {
      connected = true;
      socket.send(
        JSON.stringify({
          type: "gesture",
          data: {
            targetGesture: "RIGHT_OPEN",
            actualGesture: "RIGHT_OPEN",
            isCorrect: true,
            confidence: 90,
            holdMs: 1000,
            userLevel: "beginner"
          }
        })
      );
    });

    socket.on("message", (rawMessage) => {
      try {
        const message = JSON.parse(rawMessage.toString());

        if (typeof message.type === "string") {
          types.add(message.type);
        }
      } catch {
        // Ignore malformed messages in demo check.
      }
    });

    socket.on("error", () => {
      clearTimeout(timeout);
      finish();
    });

    socket.on("close", () => {
      clearTimeout(timeout);
      finish();
    });
  });
}

async function verifyFrameFlow() {
  const before = readLogSnapshot();
  await sleep(FRAME_WAIT_MS);
  const after = readLogSnapshot();
  const newLines = after.slice(before.length);
  const frameTypes = parseFrameTypes(newLines);

  return {
    ok: frameTypes.size > 0,
    frameTypes: Array.from(frameTypes)
  };
}

function openBrowser(url) {
  if (process.env.SKIP_BROWSER === "true") {
    return;
  }

  if (process.platform === "win32") {
    spawn("cmd.exe", ["/c", "start", "", url], {
      detached: true,
      stdio: "ignore"
    }).unref();
    return;
  }

  if (process.platform === "darwin") {
    spawn("open", [url], {
      detached: true,
      stdio: "ignore"
    }).unref();
    return;
  }

  spawn("xdg-open", [url], {
    detached: true,
    stdio: "ignore"
  }).unref();
}

function inferMode(health) {
  if (health.mode === "serial") {
    return "serial";
  }

  if (health.mode === "replay" || health.useReplay) {
    return "replay";
  }

  return "mock";
}

function printReport({ bridgeOk, dashboardOk, websocketOk, frameFlowOk, mode }) {
  console.log("\n==== DEMO START REPORT ====\n");
  console.log(`[bridge] ${bridgeOk ? "OK" : "FAIL"}`);
  console.log(`[dashboard] ${dashboardOk ? "OK" : "FAIL"}`);
  console.log(`[websocket] ${websocketOk ? "OK" : "FAIL"}`);
  console.log(`[frame flow] ${frameFlowOk ? "OK" : "FAIL"}`);
  console.log("");
  console.log("PIPELINE:");
  console.log("mock/replay/serial -> framePipeline -> ws -> dashboard");
  console.log("");
  console.log(`MODE: ${mode}`);
  console.log("");
  console.log(`STATUS: ${bridgeOk && dashboardOk && websocketOk && frameFlowOk ? "READY FOR DEMO" : "FAIL"}`);
}

function shutdownChildren() {
  childProcesses.forEach((child) => {
    if (!child.killed) {
      child.kill();
    }
  });
}

async function main() {
  const bridgeChild = spawnProcess("node", ["src/server.js"], {
    cwd: ROOT_DIR,
    prefix: "[bridge]"
  });

  const dashboardChild = spawnProcess("npm", ["run", "dev"], {
    cwd: DASHBOARD_DIR,
    prefix: "[dashboard]"
  });

  const bridgeHealth = await waitForBridgeReady();
  await waitForDashboardReady();
  await sleep(8000);

  const websocketResult = await verifyWebSocket();
  const frameFlowResult = await verifyFrameFlow();

  const bridgeOk = Boolean(bridgeHealth?.ok);
  const dashboardOk = true;
  const websocketOk =
    websocketResult.connected &&
    websocketResult.types.includes("gesture") &&
    websocketResult.types.includes("sign") &&
    websocketResult.types.includes("care") &&
    websocketResult.types.includes("ai_feedback");
  const frameFlowOk = frameFlowResult.ok;
  const mode = inferMode(bridgeHealth);

  openBrowser(DASHBOARD_URL);
  printReport({
    bridgeOk,
    dashboardOk,
    websocketOk,
    frameFlowOk,
    mode
  });

  console.log("\nPress Ctrl+C to stop bridge and dashboard.\n");

  bridgeChild.on("exit", () => {
    dashboardChild.kill();
    process.exit(1);
  });

  dashboardChild.on("exit", () => {
    bridgeChild.kill();
    process.exit(1);
  });
}

process.on("SIGINT", () => {
  shutdownChildren();
  process.exit(0);
});

process.on("SIGTERM", () => {
  shutdownChildren();
  process.exit(0);
});

main().catch((error) => {
  console.error(`\n[demo] failed: ${error.message}\n`);
  shutdownChildren();
  process.exit(1);
});
