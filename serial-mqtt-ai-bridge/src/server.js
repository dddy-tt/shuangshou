const http = require("http");
const path = require("path");
const express = require("express");
const cors = require("cors");
const dotenv = require("dotenv");
const { WebSocketServer } = require("ws");

const { nextMockMessage } = require("./mockGenerator");
const { generateAiFeedback } = require("./aiFeedback");
const { publishControl } = require("./mqttClient");
const { parseFrame } = require("./frameParser");
const { createFramePipeline } = require("./framePipeline");
const { normalizeSerialFrame, openSerialInput } = require("./serialInput");
const {
  TASK_STATES,
  createTask,
  setTaskState,
  updateTask,
  getTask,
  getAllTasks,
  subscribe
} = require("./taskStore");

dotenv.config();

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

const port = Number(process.env.PORT || 8765);
const mode = process.env.MODE || "mock";
const useReplay = String(process.env.USE_REPLAY || "false").toLowerCase() === "true";
const serialPortPath = process.env.SERIAL_PORT || "COM5";
const serialBaudRate = Number(process.env.SERIAL_BAUDRATE || process.env.SERIAL_BAUD || 9600);
const frameLogPath = path.resolve(
  __dirname,
  "..",
  process.env.FRAME_LOG_PATH || "logs/frame_log.jsonl"
);
const framePipeline = createFramePipeline({
  parseFrame,
  logPath: frameLogPath
});

let mockTimer = null;
let serialInputHandle = null;
let shuttingDown = false;

app.use(cors());
app.use(express.json());

createTask("frame_pipeline_task", {
  description: "Tracks frame pipeline activity"
});
createTask("serial_input_task", {
  description: "Tracks serial input status"
});
createTask("replay_engine_task", {
  description: "Tracks replay engine status"
});
createTask("ws_broadcast_task", {
  description: "Tracks websocket broadcast status"
});

function sendRaw(message) {
  const payload = JSON.stringify(message);

  wss.clients.forEach((client) => {
    if (client.readyState === 1) {
      client.send(payload);
    }
  });
}

function broadcast(message, { skipTaskUpdate = false } = {}) {
  if (!skipTaskUpdate) {
    setTaskState("ws_broadcast_task", TASK_STATES.RUNNING);
  }

  sendRaw(message);

  if (!skipTaskUpdate) {
    updateTask("ws_broadcast_task", {
      state: TASK_STATES.DONE,
      meta: {
        lastType: message.type || "unknown"
      }
    });
  }
}

function handleParsedFrame(parsed, { skipBroadcast = false } = {}) {
  if (!parsed) {
    return null;
  }

  if (parsed.type === "gesture" && !skipBroadcast) {
    broadcast({
      type: "gesture",
      gesture: parsed.gesture,
      confidence: parsed.confidence,
      holdMs: parsed.holdMs,
      timestamp: Date.now()
    });
  }

  return parsed;
}

function processIncomingFrame({ source, rawFrame, skipBroadcast = false, skipLog = false }) {
  setTaskState("frame_pipeline_task", TASK_STATES.RUNNING);

  try {
    const parsed = framePipeline.processFrame({
      source,
      rawFrame,
      skipLog
    });
    const handled = handleParsedFrame(parsed, { skipBroadcast });

    updateTask("frame_pipeline_task", {
      state: handled ? TASK_STATES.DONE : TASK_STATES.BLOCKED,
      meta: {
        source,
        lastFrame: typeof rawFrame === "string" ? rawFrame : "",
        parsedType: handled?.type || null
      }
    });

    return handled;
  } catch (error) {
    updateTask("frame_pipeline_task", {
      state: TASK_STATES.FAILED,
      meta: {
        source,
        error: error.message
      }
    });
    throw error;
  }
}

app.get("/health", (_req, res) => {
  res.json({
    ok: true,
    service: "serial-mqtt-ai-bridge",
    mode,
    useReplay,
    serialPort: serialPortPath,
    serialBaudRate
  });
});

app.post("/api/ai-feedback", async (req, res) => {
  const result = await generateAiFeedback(req.body);
  res.json(result);
});

app.post("/api/control", (req, res) => {
  const { device, action, source = "dashboard" } = req.body || {};

  if (!device || !action) {
    return res.status(400).json({
      ok: false,
      message: "device 和 action 不能为空"
    });
  }

  const result = publishControl(device, action, source);
  return res.json({
    ok: true,
    mode,
    ...result
  });
});

app.post("/api/parse-frame", (req, res) => {
  const parsed = processIncomingFrame({
    source: req.body?.source || "ws",
    rawFrame: req.body?.line
  });

  res.json({
    ok: true,
    parsed
  });
});

app.get("/api/tasks", (_req, res) => {
  res.json({
    ok: true,
    tasks: getAllTasks()
  });
});

app.get("/api/tasks/:id", (req, res) => {
  const task = getTask(req.params.id);

  if (!task) {
    return res.status(404).json({
      ok: false,
      message: "task not found"
    });
  }

  return res.json({
    ok: true,
    task
  });
});

subscribe((task) => {
  broadcast(
    {
      type: "task_update",
      task
    },
    { skipTaskUpdate: true }
  );
});

wss.on("connection", (socket) => {
  socket.send(
    JSON.stringify({
      type: "system",
      message: "serial-mqtt-ai-bridge mock websocket connected",
      timestamp: Date.now()
    })
  );

  socket.on("message", async (rawMessage) => {
    let payload;

    try {
      payload = JSON.parse(rawMessage.toString());
    } catch {
      socket.send(
        JSON.stringify({
          type: "system",
          message: "invalid websocket json payload",
          timestamp: Date.now()
        })
      );
      return;
    }

    if (payload?.type === "gesture") {
      const result = await generateAiFeedback(payload.data || {});

      socket.send(
        JSON.stringify({
          type: "ai_feedback",
          source: result.source,
          result: result.feedback,
          timestamp: Date.now()
        })
      );
      return;
    }

    if (payload?.type === "command") {
      socket.send(
        JSON.stringify({
          type: "system",
          message: "command message received, handler reserved",
          timestamp: Date.now()
        })
      );
      return;
    }

    socket.send(
      JSON.stringify({
        type: "system",
        message: "unsupported websocket message type",
        timestamp: Date.now()
      })
    );
  });
});

function buildMockGestureFrame(message) {
  return `GESTURE:ID=${message.gesture},CONF=${message.confidence},HOLD=${message.holdMs}`;
}

function startMockSource() {
  console.log("[bridge] input source=mock");
  updateTask("serial_input_task", {
    state: TASK_STATES.BLOCKED,
    meta: {
      portPath: serialPortPath,
      baudRate: serialBaudRate,
      reason: "mock fallback active"
    }
  });
  setTaskState("replay_engine_task", TASK_STATES.BLOCKED);

  mockTimer = setInterval(() => {
    const message = nextMockMessage();
    broadcast(message);

    if (message.type === "gesture") {
      processIncomingFrame({
        source: "mock",
        rawFrame: buildMockGestureFrame(message),
        skipBroadcast: true
      });
    }
  }, 3000);
}

function fallbackToReplayOrMock(reason) {
  console.warn(`[bridge] serial fallback: ${reason}`);

  if (useReplay) {
    void startReplaySource();
    return;
  }

  startMockSource();
}

async function startSerialSource() {
  console.log(`[bridge] input source=serial, port=${serialPortPath}, baud=${serialBaudRate}`);
  setTaskState("serial_input_task", TASK_STATES.RUNNING);
  setTaskState("replay_engine_task", TASK_STATES.BLOCKED);

  try {
    serialInputHandle = await openSerialInput({
      portPath: serialPortPath,
      baudRate: serialBaudRate,
      onOpen: () => {
        updateTask("serial_input_task", {
          state: TASK_STATES.RUNNING,
          meta: {
            portPath: serialPortPath,
            baudRate: serialBaudRate,
            connected: true
          }
        });
      },
      onLine: (line) => {
        const normalizedFrame = normalizeSerialFrame(line);

        if (!normalizedFrame) {
          return;
        }

        processIncomingFrame({
          source: "serial",
          rawFrame: normalizedFrame
        });
      },
      onError: (error) => {
        updateTask("serial_input_task", {
          state: TASK_STATES.FAILED,
          meta: {
            portPath: serialPortPath,
            baudRate: serialBaudRate,
            error: error.message
          }
        });
      },
      onClose: () => {
        updateTask("serial_input_task", {
          state: TASK_STATES.BLOCKED,
          meta: {
            portPath: serialPortPath,
            baudRate: serialBaudRate,
            connected: false
          }
        });
      }
    });
  } catch (error) {
    updateTask("serial_input_task", {
      state: TASK_STATES.BLOCKED,
      meta: {
        portPath: serialPortPath,
        baudRate: serialBaudRate,
        error: error.message
      }
    });
    fallbackToReplayOrMock(error.message);
  }
}

async function startReplaySource() {
  console.log(`[bridge] input source=replay, log=${frameLogPath}`);
  setTaskState("replay_engine_task", TASK_STATES.RUNNING);
  setTaskState("serial_input_task", TASK_STATES.BLOCKED);
  const replayedCount = await framePipeline.replayFrames({
    shouldContinue: () => !shuttingDown,
    onParsed: (parsed) => {
      handleParsedFrame(parsed);
    }
  });

  updateTask("replay_engine_task", {
    state: TASK_STATES.DONE,
    meta: {
      replayedCount
    }
  });
  console.log(`[bridge] replay finished, frames=${replayedCount}`);
}

function startInputSource() {
  if (mode === "serial") {
    void startSerialSource();
    return;
  }

  if (useReplay) {
    void startReplaySource();
    return;
  }

  startMockSource();
}

server.listen(port, () => {
  console.log(`[bridge] listening on http://localhost:${port}`);
  console.log(`[bridge] websocket ready at ws://localhost:${port}`);
  console.log(`[bridge] mode=${mode}`);
  console.log(`[bridge] useReplay=${useReplay}`);
  console.log(`[bridge] frameLog=${frameLogPath}`);

  startInputSource();
});

function shutdown() {
  shuttingDown = true;

  if (mockTimer) {
    clearInterval(mockTimer);
  }

  const closeSerial = serialInputHandle?.close ? serialInputHandle.close() : Promise.resolve();

  Promise.resolve(closeSerial)
    .catch(() => null)
    .finally(() => {
  wss.close(() => {
    server.close(() => {
      process.exit(0);
    });
  });
    });
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
