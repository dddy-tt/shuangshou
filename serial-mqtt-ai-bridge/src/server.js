const http = require("http");
const express = require("express");
const cors = require("cors");
const dotenv = require("dotenv");
const { WebSocketServer } = require("ws");

const { nextMockMessage } = require("./mockGenerator");
const { generateAiFeedback } = require("./aiFeedback");
const { publishControl } = require("./mqttClient");
const { parseFrame } = require("./frameParser");

dotenv.config();

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

const port = Number(process.env.PORT || 8765);
const mode = process.env.MODE || "mock";

app.use(cors());
app.use(express.json());

app.get("/health", (_req, res) => {
  res.json({
    ok: true,
    service: "serial-mqtt-ai-bridge",
    mode
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
  const parsed = parseFrame(req.body?.line);
  res.json({
    ok: true,
    parsed
  });
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

const timer = setInterval(() => {
  const message = JSON.stringify(nextMockMessage());

  wss.clients.forEach((client) => {
    if (client.readyState === 1) {
      client.send(message);
    }
  });
}, 3000);

server.listen(port, () => {
  console.log(`[bridge] listening on http://localhost:${port}`);
  console.log(`[bridge] websocket ready at ws://localhost:${port}`);
  console.log(`[bridge] mode=${mode}`);
});

function shutdown() {
  clearInterval(timer);
  wss.close(() => {
    server.close(() => {
      process.exit(0);
    });
  });
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
