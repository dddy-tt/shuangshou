const fs = require("fs");
const path = require("path");
const WebSocket = require("ws");

const HTTP_TIMEOUT_MS = 3000;
const WS_OBSERVE_MS = 10000;
const FRAME_OBSERVE_MS = 5000;

function sleep(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

async function fetchJson(url) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), HTTP_TIMEOUT_MS);

  try {
    const response = await fetch(url, {
      signal: controller.signal
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    return await response.json();
  } finally {
    clearTimeout(timeout);
  }
}

async function discoverHttpBaseUrl() {
  const candidates = [];

  if (process.env.BRIDGE_HTTP_URL) {
    candidates.push(process.env.BRIDGE_HTTP_URL);
  }

  candidates.push("http://localhost:8765");
  candidates.push("http://localhost:3000");

  for (const baseUrl of candidates) {
    try {
      const health = await fetchJson(`${baseUrl}/health`);

      if (health?.service !== "serial-mqtt-ai-bridge") {
        continue;
      }

      return {
        baseUrl,
        health
      };
    } catch {
      // Try next candidate.
    }
  }

  throw new Error("bridge health endpoint not reachable");
}

async function safeFetchJson(url) {
  try {
    return {
      ok: true,
      data: await fetchJson(url)
    };
  } catch (error) {
    return {
      ok: false,
      error: error.message
    };
  }
}

function getTaskById(tasks, id) {
  return tasks.find((task) => task.id === id) || null;
}

function parseFrameType(rawFrame) {
  if (typeof rawFrame !== "string") {
    return null;
  }

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
}

function readLogLines(logPath) {
  if (!fs.existsSync(logPath)) {
    return [];
  }

  return fs
    .readFileSync(logPath, "utf8")
    .split(/\r?\n/)
    .filter(Boolean);
}

function createLineSnapshot(logPath) {
  const lines = readLogLines(logPath);

  return {
    count: lines.length,
    lines
  };
}

function detectMode({ health, serialTask, replayTask }) {
  if (serialTask?.state === "RUNNING") {
    return "serial";
  }

  if (replayTask?.state === "RUNNING" || replayTask?.state === "DONE") {
    return "replay";
  }

  if (health?.mode === "serial" && serialTask?.state === "BLOCKED") {
    return replayTask?.state === "DONE" ? "replay" : "mock";
  }

  return "mock";
}

async function observeWebSocket(wsUrl) {
  return new Promise((resolve) => {
    const observedTypes = new Set();
    const observedMessages = [];
    let connected = false;
    let closed = false;

    const socket = new WebSocket(wsUrl);

    const finish = () => {
      if (closed) {
        return;
      }

      closed = true;

      if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
        socket.close();
      }

      resolve({
        connected,
        observedTypes: Array.from(observedTypes),
        observedMessages
      });
    };

    const timer = setTimeout(finish, WS_OBSERVE_MS);

    socket.on("open", () => {
      connected = true;

      socket.send(
        JSON.stringify({
          type: "gesture",
          data: {
            targetGesture: "RIGHT_OPEN",
            actualGesture: "RIGHT_OPEN",
            isCorrect: true,
            confidence: 88,
            holdMs: 1200,
            userLevel: "beginner"
          }
        })
      );
    });

    socket.on("message", (rawMessage) => {
      try {
        const message = JSON.parse(rawMessage.toString());
        observedMessages.push(message);

        if (typeof message.type === "string") {
          observedTypes.add(message.type);
        }
      } catch {
        // Ignore malformed messages during check.
      }
    });

    socket.on("error", () => {
      clearTimeout(timer);
      finish();
    });

    socket.on("close", () => {
      clearTimeout(timer);
      finish();
    });
  });
}

function buildReportLine(ok, message) {
  return `[${ok ? "OK" : "FAIL"}] ${message}`;
}

async function main() {
  const logPath = path.resolve(__dirname, "..", "logs", "frame_log.jsonl");
  const initialLogSnapshot = createLineSnapshot(logPath);

  const serverResult = await discoverHttpBaseUrl();
  const httpBaseUrl = serverResult.baseUrl;
  const wsUrl = process.env.BRIDGE_WS_URL || httpBaseUrl.replace("http://", "ws://");
  const initialTasksResult = await safeFetchJson(`${httpBaseUrl}/api/tasks`);
  const initialTasks = initialTasksResult.ok ? initialTasksResult.data.tasks || [] : [];

  const framePipelineTaskInitial = getTaskById(initialTasks, "frame_pipeline_task");
  const wsBroadcastTaskInitial = getTaskById(initialTasks, "ws_broadcast_task");

  const websocketPromise = observeWebSocket(wsUrl);
  await sleep(FRAME_OBSERVE_MS);
  const frameLogAfter = createLineSnapshot(logPath);
  const websocketResult = await websocketPromise;
  const finalTasksResult = await safeFetchJson(`${httpBaseUrl}/api/tasks`);
  const finalTasks = finalTasksResult.ok ? finalTasksResult.data.tasks || [] : [];

  const framePipelineTask = getTaskById(finalTasks, "frame_pipeline_task");
  const wsBroadcastTask = getTaskById(finalTasks, "ws_broadcast_task");
  const serialTask = getTaskById(finalTasks, "serial_input_task");
  const replayTask = getTaskById(finalTasks, "replay_engine_task");

  const validTaskStates = new Set(["PENDING", "RUNNING", "DONE"]);
  const taskApiReachable = finalTasksResult.ok;
  const taskSystemActive =
    taskApiReachable &&
    framePipelineTask &&
    wsBroadcastTask &&
    validTaskStates.has(framePipelineTask.state) &&
    validTaskStates.has(wsBroadcastTask.state);

  const newLogLines = frameLogAfter.lines.slice(initialLogSnapshot.count);
  const observedFrameKinds = new Set(
    newLogLines
      .map((line) => {
        try {
          return JSON.parse(line);
        } catch {
          return null;
        }
      })
      .filter(Boolean)
      .map((entry) => parseFrameType(entry.rawFrame))
      .filter(Boolean)
  );

  const frameFlowDetected =
    observedFrameKinds.size > 0 ||
    (framePipelineTaskInitial &&
      framePipelineTask &&
      framePipelineTask.updatedAt !== framePipelineTaskInitial.updatedAt) ||
    (wsBroadcastTaskInitial &&
      wsBroadcastTask &&
      wsBroadcastTask.updatedAt !== wsBroadcastTaskInitial.updatedAt);

  const mode = detectMode({
    health: serverResult.health,
    serialTask,
    replayTask
  });

  const websocketConnected = websocketResult.connected;
  const hasAiFeedback = websocketResult.observedTypes.includes("ai_feedback");

  const expectedMessageTypes =
    mode === "mock"
      ? ["gesture", "sign", "care", "ai_feedback"]
      : ["gesture", "ai_feedback"];
  const missingMessageTypes = expectedMessageTypes.filter(
    (type) => !websocketResult.observedTypes.includes(type)
  );

  const serialUsable = serialTask?.state === "RUNNING";
  const replayUsable = replayTask?.state === "RUNNING" || replayTask?.state === "DONE";
  const mockUsable =
    mode === "mock" ||
    serialTask?.meta?.reason === "mock fallback active" ||
    serverResult.health.mode === "mock";

  const pass =
    Boolean(serverResult.health?.ok) &&
    websocketConnected &&
    taskApiReachable &&
    taskSystemActive &&
    frameFlowDetected &&
    hasAiFeedback &&
    missingMessageTypes.length === 0;

  console.log("==== SYSTEM CHECK REPORT ====\n");
  console.log(buildReportLine(Boolean(serverResult.health?.ok), `server alive (${httpBaseUrl})`));
  console.log(buildReportLine(websocketConnected, `websocket connected (${wsUrl})`));
  console.log(
    buildReportLine(
      taskApiReachable,
      taskApiReachable ? "task API reachable" : `task API reachable (${finalTasksResult.error})`
    )
  );
  console.log(buildReportLine(taskSystemActive, "task system active"));
  console.log(buildReportLine(frameFlowDetected, "frame flow detected"));
  console.log(
    buildReportLine(
      missingMessageTypes.length === 0,
      `websocket message types observed: ${websocketResult.observedTypes.join(", ") || "none"}`
    )
  );

  if (!frameFlowDetected) {
    console.log("❌ frame pipeline no data");
  }

  console.log("");
  console.log(`MODE: ${mode}`);
  console.log("");
  console.log("PIPELINE:");
  console.log("serial -> framePipeline -> ws -> dashboard");
  console.log("");
  console.log("DETAIL:");
  console.log(`- health mode: ${serverResult.health.mode}`);
  console.log(`- task api: ${taskApiReachable ? "reachable" : finalTasksResult.error}`);
  console.log(`- serial usable: ${serialUsable ? "yes" : "no"}`);
  console.log(`- replay usable: ${replayUsable ? "yes" : "no"}`);
  console.log(`- mock usable: ${mockUsable ? "yes" : "no"}`);
  console.log(`- observed frame kinds: ${Array.from(observedFrameKinds).join(", ") || "none"}`);
  console.log(`- frame pipeline state: ${framePipelineTask?.state || "missing"}`);
  console.log(`- ws broadcast state: ${wsBroadcastTask?.state || "missing"}`);
  console.log(`- serial task state: ${serialTask?.state || "missing"}`);
  console.log(`- replay task state: ${replayTask?.state || "missing"}`);
  console.log("");
  console.log(`STATUS: ${pass ? "PASS" : "FAIL"}`);
}

main().catch((error) => {
  console.log("==== SYSTEM CHECK REPORT ====\n");
  console.log(buildReportLine(false, "server alive"));
  console.log("");
  console.log(`ERROR: ${error.message}`);
  console.log("");
  console.log("STATUS: FAIL");
  process.exitCode = 1;
});
