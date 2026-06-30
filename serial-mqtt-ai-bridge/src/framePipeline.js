const fs = require("fs");
const path = require("path");

function sleep(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

function ensureLogFile(logPath) {
  fs.mkdirSync(path.dirname(logPath), { recursive: true });

  if (!fs.existsSync(logPath)) {
    fs.writeFileSync(logPath, "", "utf8");
  }
}

function appendLogEntry(logPath, entry) {
  fs.appendFileSync(logPath, `${JSON.stringify(entry)}\n`, "utf8");
}

function readLogEntries(logPath) {
  if (!fs.existsSync(logPath)) {
    return [];
  }

  const content = fs.readFileSync(logPath, "utf8");

  return content
    .split(/\r?\n/)
    .filter(Boolean)
    .map((line) => {
      try {
        return JSON.parse(line);
      } catch {
        return null;
      }
    })
    .filter(
      (entry) =>
        entry &&
        typeof entry.timestamp === "number" &&
        typeof entry.source === "string" &&
        typeof entry.rawFrame === "string"
    );
}

function createFramePipeline({ parseFrame, logPath }) {
  ensureLogFile(logPath);

  function processFrame({ source, rawFrame, timestamp = Date.now(), skipLog = false }) {
    if (typeof rawFrame !== "string") {
      return null;
    }

    const normalizedFrame = rawFrame.trim();

    if (!normalizedFrame) {
      return null;
    }

    if (!skipLog) {
      appendLogEntry(logPath, {
        timestamp,
        source,
        rawFrame: normalizedFrame
      });
    }

    return parseFrame(normalizedFrame);
  }

  async function replayFrames({ onParsed, shouldContinue } = {}) {
    const entries = readLogEntries(logPath);

    for (let index = 0; index < entries.length; index += 1) {
      if (shouldContinue && !shouldContinue()) {
        break;
      }

      if (index > 0) {
        const delay = Math.max(0, entries[index].timestamp - entries[index - 1].timestamp);
        await sleep(delay);
      }

      const entry = entries[index];
      const parsed = processFrame({
        source: entry.source,
        rawFrame: entry.rawFrame,
        timestamp: entry.timestamp,
        skipLog: true
      });

      if (onParsed) {
        await onParsed(parsed, entry);
      }
    }

    return entries.length;
  }

  return {
    logPath,
    processFrame,
    replayFrames
  };
}

module.exports = {
  createFramePipeline
};
