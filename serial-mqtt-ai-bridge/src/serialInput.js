function normalizeGestureFrame(parts) {
  const gesture = parts[1];
  const confidenceValue = Number(parts[2] || 0);
  const holdMs = Number(parts[3] || 0);
  const confidence = Number.isFinite(confidenceValue)
    ? Math.round(confidenceValue <= 1 ? confidenceValue * 100 : confidenceValue)
    : 0;

  if (!gesture) {
    return null;
  }

  return `GESTURE:ID=${gesture},CONF=${confidence},HOLD=${holdMs}`;
}

function normalizeBringupFrame(parts) {
  const status = parts[1];

  if (!status) {
    return null;
  }

  return `BRINGUP:STATUS=${status}`;
}

function normalizeControlFrame(parts) {
  const device = parts[1];
  const action = parts[2];

  if (device && action) {
    return `CTRL:DEV=${device},ACT=${action}`;
  }

  if (!device) {
    return null;
  }

  const separatorIndex = device.indexOf("_");

  if (separatorIndex <= 0 || separatorIndex >= device.length - 1) {
    return null;
  }

  const normalizedDevice = device.slice(0, separatorIndex);
  const normalizedAction = device.slice(separatorIndex + 1);

  return `CTRL:DEV=${normalizedDevice},ACT=${normalizedAction}`;
}

function normalizeSerialFrame(rawLine) {
  if (typeof rawLine !== "string") {
    return null;
  }

  const line = rawLine.trim();

  if (!line) {
    return null;
  }

  if (
    line.startsWith("BRINGUP:") ||
    line.startsWith("GESTURE:") ||
    line.startsWith("CTRL:")
  ) {
    return line;
  }

  const parts = line.split("|").map((part) => part.trim());
  const frameType = parts[0];

  if (frameType === "GESTURE") {
    return normalizeGestureFrame(parts);
  }

  if (frameType === "BRINGUP") {
    return normalizeBringupFrame(parts);
  }

  if (frameType === "CTRL") {
    return normalizeControlFrame(parts);
  }

  return null;
}

async function openSerialInput({
  portPath,
  baudRate,
  onLine,
  onOpen,
  onError,
  onClose
}) {
  const { SerialPort } = require("serialport");

  return new Promise((resolve, reject) => {
    const port = new SerialPort({
      path: portPath,
      baudRate,
      autoOpen: false
    });

    let buffer = "";

    port.on("data", (chunk) => {
      buffer += chunk.toString("utf8");

      const segments = buffer.split(/\r?\n/);
      buffer = segments.pop() || "";

      segments.forEach((line) => {
        if (onLine) {
          onLine(line);
        }
      });
    });

    port.on("error", (error) => {
      if (onError) {
        onError(error);
      }
    });

    port.on("close", () => {
      if (onClose) {
        onClose();
      }
    });

    port.open((error) => {
      if (error) {
        reject(error);
        return;
      }

      if (onOpen) {
        onOpen();
      }

      resolve({
        close: async () => {
          if (!port.isOpen) {
            return;
          }

          await new Promise((closeResolve, closeReject) => {
            port.close((closeError) => {
              if (closeError) {
                closeReject(closeError);
                return;
              }

              closeResolve();
            });
          });
        }
      });
    });
  });
}

module.exports = {
  normalizeSerialFrame,
  openSerialInput
};
