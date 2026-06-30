const http = require("http");
const express = require("express");
const cors = require("cors");
const dotenv = require("dotenv");
const { WebSocketServer } = require("ws");

const { nextMockMessage } = require("./mockGenerator");
const { buildAiFeedback } = require("./aiFeedback");
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

app.post("/api/ai-feedback", (req, res) => {
  const feedback = buildAiFeedback(req.body);
  res.json({ feedback });
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
