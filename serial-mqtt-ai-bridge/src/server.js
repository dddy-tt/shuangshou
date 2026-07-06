const http = require("http");
const path = require("path");
const express = require("express");
const cors = require("cors");
const dotenv = require("dotenv");
const { WebSocketServer } = require("ws");

const { nextMockMessage } = require("./mockGenerator");
const { generateAiFeedback } = require("./aiFeedback");
const { publishControl } = require("./mqttClient");
const {
  readCustomGestures,
  createCustomGesture,
  deleteCustomGesture
} = require("./customGestureStore");
const { parseFrame } = require("./frameParser");
const { createFramePipeline } = require("./framePipeline");
const { normalizeSerialFrame, parseSensorPayload, openSerialInput } = require("./serialInput");
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
let lastCustomGestureMatch = {
  id: null,
  at: 0
};
const sensorState = {
  flex: {
    left: null,
    right: null,
    leftFingers: null,
    rightFingers: null,
    normalizedLeft: null,
    normalizedRight: null,
    normalizedLeftFingers: null,
    normalizedRightFingers: null,
    timestamp: null
  },
  imu: {
    roll: 0,
    pitch: 0,
    yaw: 0,
    timestamp: null
  }
};
const calibrationState = {
  zero: null,
  full: null,
  imuZero: null
};

function stopMockSource() {
  if (mockTimer) {
    clearInterval(mockTimer);
    mockTimer = null;
  }
}

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

function normalizeFlexValue(raw, zero, full) {
  if (typeof raw !== "number" || typeof zero !== "number" || typeof full !== "number" || full === zero) {
    return null;
  }

  const normalized = (raw - zero) / (full - zero);
  return Math.max(0, Math.min(1, Number(normalized.toFixed(3))));
}

function normalizeFingerArray(rawValues, zeroValues, fullValues) {
  if (!Array.isArray(rawValues) || !Array.isArray(zeroValues) || !Array.isArray(fullValues)) {
    return null;
  }

  return rawValues.map((rawValue, index) =>
    normalizeFlexValue(rawValue, zeroValues[index], fullValues[index])
  );
}

function buildFlexSensorMessage(left, right, leftFingers = null, rightFingers = null) {
  return {
    type: "sensor_raw",
    sensor: "flex",
    left,
    right,
    leftFingers,
    rightFingers,
    normalizedLeft: normalizeFlexValue(left, calibrationState.zero?.left, calibrationState.full?.left),
    normalizedRight: normalizeFlexValue(right, calibrationState.zero?.right, calibrationState.full?.right),
    normalizedLeftFingers: normalizeFingerArray(
      leftFingers,
      calibrationState.zero?.leftFingers,
      calibrationState.full?.leftFingers
    ),
    normalizedRightFingers: normalizeFingerArray(
      rightFingers,
      calibrationState.zero?.rightFingers,
      calibrationState.full?.rightFingers
    ),
    timestamp: Date.now()
  };
}

function getCurrentSensorSnapshot() {
  if (!Array.isArray(sensorState.flex.leftFingers) || !Array.isArray(sensorState.flex.rightFingers)) {
    return null;
  }

  return {
    leftFingers: sensorState.flex.leftFingers,
    rightFingers: sensorState.flex.rightFingers,
    roll: sensorState.imu.roll,
    pitch: sensorState.imu.pitch,
    yaw: sensorState.imu.yaw
  };
}

function calculateGestureScore(snapshot, current) {
  const leftScore = snapshot.leftFingers.reduce((sum, value, index) =>
    sum + Math.abs(value - (current.leftFingers[index] || 0)), 0);
  const rightScore = snapshot.rightFingers.reduce((sum, value, index) =>
    sum + Math.abs(value - (current.rightFingers[index] || 0)), 0);
  const imuScore =
    Math.abs(snapshot.roll - current.roll) +
    Math.abs(snapshot.pitch - current.pitch) +
    Math.abs(snapshot.yaw - current.yaw);

  return Number(((leftScore + rightScore) / 10 + imuScore * 0.35).toFixed(2));
}

function maybeBroadcastCustomGestureMatch() {
  const currentSnapshot = getCurrentSensorSnapshot();

  if (!currentSnapshot) {
    return;
  }

  const items = readCustomGestures();
  if (!items.length) {
    return;
  }

  let bestMatch = null;
  let bestScore = Number.POSITIVE_INFINITY;

  items.forEach((item) => {
    const score = calculateGestureScore(item.snapshot, currentSnapshot);
    if (score < bestScore) {
      bestScore = score;
      bestMatch = item;
    }
  });

  if (!bestMatch || bestScore > 18) {
    return;
  }

  const now = Date.now();
  if (lastCustomGestureMatch.id === bestMatch.id && now - lastCustomGestureMatch.at < 2000) {
    return;
  }

  lastCustomGestureMatch = {
    id: bestMatch.id,
    at: now
  };

  broadcast({
    type: "custom_gesture_match",
    item: bestMatch,
    score: bestScore,
    timestamp: now
  });
}

function handleSensorPayload(payload) {
  if (!payload || payload.type !== "sensor_raw") {
    return;
  }

  if (payload.sensor === "flex") {
    const message = buildFlexSensorMessage(
      payload.left,
      payload.right,
      payload.leftFingers || null,
      payload.rightFingers || null
    );
    sensorState.flex = {
      left: message.left,
      right: message.right,
      leftFingers: message.leftFingers,
      rightFingers: message.rightFingers,
      normalizedLeft: message.normalizedLeft,
      normalizedRight: message.normalizedRight,
      normalizedLeftFingers: message.normalizedLeftFingers,
      normalizedRightFingers: message.normalizedRightFingers,
      timestamp: message.timestamp
    };
    broadcast(message);
    maybeBroadcastCustomGestureMatch();
    return;
  }

  if (payload.sensor === "imu") {
    const adjustedRoll = calibrationState.imuZero
      ? Number((payload.roll - calibrationState.imuZero.roll).toFixed(2))
      : payload.roll;
    const adjustedPitch = calibrationState.imuZero
      ? Number((payload.pitch - calibrationState.imuZero.pitch).toFixed(2))
      : payload.pitch;
    const adjustedYaw = calibrationState.imuZero
      ? Number((payload.yaw - calibrationState.imuZero.yaw).toFixed(2))
      : payload.yaw;

    const message = {
      type: "sensor_raw",
      sensor: "imu",
      roll: adjustedRoll,
      pitch: adjustedPitch,
      yaw: adjustedYaw,
      timestamp: Date.now()
    };
    sensorState.imu = {
      roll: message.roll,
      pitch: message.pitch,
      yaw: message.yaw,
      timestamp: message.timestamp
    };
    broadcast(message);
    maybeBroadcastCustomGestureMatch();
  }
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

app.post("/api/calibration/zero", (_req, res) => {
  if (typeof sensorState.flex.left !== "number" || typeof sensorState.flex.right !== "number") {
    return res.status(400).json({
      ok: false,
      message: "no flex data available"
    });
  }

  calibrationState.zero = {
    left: sensorState.flex.left,
    right: sensorState.flex.right,
    leftFingers: sensorState.flex.leftFingers,
    rightFingers: sensorState.flex.rightFingers
  };

  const preview = buildFlexSensorMessage(
    sensorState.flex.left,
    sensorState.flex.right,
    sensorState.flex.leftFingers,
    sensorState.flex.rightFingers
  );
  sensorState.flex.normalizedLeft = preview.normalizedLeft;
  sensorState.flex.normalizedRight = preview.normalizedRight;
  sensorState.flex.normalizedLeftFingers = preview.normalizedLeftFingers;
  sensorState.flex.normalizedRightFingers = preview.normalizedRightFingers;

  return res.json({
    ok: true,
    zero: calibrationState.zero,
    full: calibrationState.full,
    imuZero: calibrationState.imuZero
  });
});

app.post("/api/calibration/full", (_req, res) => {
  if (typeof sensorState.flex.left !== "number" || typeof sensorState.flex.right !== "number") {
    return res.status(400).json({
      ok: false,
      message: "no flex data available"
    });
  }

  calibrationState.full = {
    left: sensorState.flex.left,
    right: sensorState.flex.right,
    leftFingers: sensorState.flex.leftFingers,
    rightFingers: sensorState.flex.rightFingers
  };

  const preview = buildFlexSensorMessage(
    sensorState.flex.left,
    sensorState.flex.right,
    sensorState.flex.leftFingers,
    sensorState.flex.rightFingers
  );
  sensorState.flex.normalizedLeft = preview.normalizedLeft;
  sensorState.flex.normalizedRight = preview.normalizedRight;
  sensorState.flex.normalizedLeftFingers = preview.normalizedLeftFingers;
  sensorState.flex.normalizedRightFingers = preview.normalizedRightFingers;

  return res.json({
    ok: true,
    zero: calibrationState.zero,
    full: calibrationState.full,
    imuZero: calibrationState.imuZero
  });
});

app.post("/api/calibration/imu-zero", (_req, res) => {
  if (typeof sensorState.imu.roll !== "number" || typeof sensorState.imu.pitch !== "number" || typeof sensorState.imu.yaw !== "number") {
    return res.status(400).json({
      ok: false,
      message: "no imu data available"
    });
  }

  calibrationState.imuZero = {
    roll: sensorState.imu.roll,
    pitch: sensorState.imu.pitch,
    yaw: sensorState.imu.yaw
  };

  return res.json({
    ok: true,
    imuZero: calibrationState.imuZero
  });
});

app.get("/api/custom-gestures", (_req, res) => {
  res.json({
    ok: true,
    items: readCustomGestures()
  });
});

app.post("/api/custom-gestures", (req, res) => {
  const { name, category, action, snapshot } = req.body || {};

  if (!name || !category || !action || !snapshot) {
    return res.status(400).json({
      ok: false,
      message: "name, category, action, snapshot are required"
    });
  }

  if (!Array.isArray(snapshot.leftFingers) || snapshot.leftFingers.length !== 5 ||
      !Array.isArray(snapshot.rightFingers) || snapshot.rightFingers.length !== 5) {
    return res.status(400).json({
      ok: false,
      message: "snapshot must contain 5 left fingers and 5 right fingers"
    });
  }

  const item = createCustomGesture({
    name: String(name).trim(),
    category: String(category).trim(),
    action: String(action).trim(),
    snapshot: {
      leftFingers: snapshot.leftFingers.map((value) => Number(value) || 0),
      rightFingers: snapshot.rightFingers.map((value) => Number(value) || 0),
      roll: Number(snapshot.roll) || 0,
      pitch: Number(snapshot.pitch) || 0,
      yaw: Number(snapshot.yaw) || 0
    }
  });

  return res.json({
    ok: true,
    item
  });
});

app.delete("/api/custom-gestures/:id", (req, res) => {
  const removed = deleteCustomGesture(req.params.id);

  if (!removed) {
    return res.status(404).json({
      ok: false,
      message: "custom gesture not found"
    });
  }

  return res.json({
    ok: true
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

function buildMockSensorPayloads() {
  const tick = Math.floor(Date.now() / 1000);
  const leftFingers = [18, 31, 44, 57, 70].map((base, index) => base + ((tick + index * 2) % 16));
  const rightFingers = [24, 38, 52, 66, 80].map((base, index) => base + ((tick + index * 3) % 14));

  return [
    {
      type: "sensor_raw",
      sensor: "flex",
      left: leftFingers.reduce((sum, value) => sum + value, 0),
      right: rightFingers.reduce((sum, value) => sum + value, 0),
      leftFingers,
      rightFingers
    },
    {
      type: "sensor_raw",
      sensor: "imu",
      roll: Number((Math.sin(tick / 3) * 22).toFixed(2)),
      pitch: Number((Math.cos(tick / 4) * 16).toFixed(2)),
      yaw: Number(((tick * 9) % 360).toFixed(2))
    }
  ];
}

function startMockSource() {
  stopMockSource();
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

    buildMockSensorPayloads().forEach((payload) => {
      handleSensorPayload(payload);
    });

    if (message.type === "gesture") {
      processIncomingFrame({
        source: "mock",
        rawFrame: buildMockGestureFrame(message),
        skipBroadcast: true
      });
    }
  }, 3000);
}

function fallbackToMock(reason) {
  console.warn(`[bridge] serial fallback: ${reason}`);
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
        const sensorPayload = parseSensorPayload(line);

        if (sensorPayload) {
          handleSensorPayload(sensorPayload);
          return;
        }

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
    fallbackToMock(error.message);
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

  if (!shuttingDown) {
    startMockSource();
  }
}

function resolveInputSource() {
  if (mode === "mock" || mode === "deepseek") {
    return "mock";
  }

  if (mode === "replay" || useReplay) {
    return "replay";
  }

  if (mode === "serial") {
    return "serial";
  }

  return "mock";
}

function startInputSource() {
  const inputSource = resolveInputSource();

  if (inputSource === "serial") {
    void startSerialSource();
    return;
  }

  if (inputSource === "replay") {
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

  stopMockSource();

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
