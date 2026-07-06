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

function parseKeyValuePart(part) {
  const [key, rawValue] = part.split("=");

  if (!key || typeof rawValue === "undefined") {
    return null;
  }

  const value = Number(rawValue.trim());

  if (Number.isNaN(value)) {
    return null;
  }

  return {
    key: key.trim(),
    value
  };
}

function parseFlexPayload(parts) {
  const fingerValues = {
    left: Array(5).fill(null),
    right: Array(5).fill(null)
  };
  let matchedFingerCount = 0;

  for (let index = 1; index < parts.length; index += 1) {
    const parsedPart = parseKeyValuePart(parts[index] || "");

    if (!parsedPart) {
      continue;
    }

    const leftMatch = /^L([1-5])$/.exec(parsedPart.key);
    const rightMatch = /^R([1-5])$/.exec(parsedPart.key);

    if (leftMatch) {
      fingerValues.left[Number(leftMatch[1]) - 1] = parsedPart.value;
      matchedFingerCount += 1;
      continue;
    }

    if (rightMatch) {
      fingerValues.right[Number(rightMatch[1]) - 1] = parsedPart.value;
      matchedFingerCount += 1;
    }
  }

  if (
    matchedFingerCount === 10 &&
    fingerValues.left.every((value) => typeof value === "number") &&
    fingerValues.right.every((value) => typeof value === "number")
  ) {
    return {
      type: "sensor_raw",
      sensor: "flex",
      left: fingerValues.left.reduce((sum, value) => sum + value, 0),
      right: fingerValues.right.reduce((sum, value) => sum + value, 0),
      leftFingers: fingerValues.left,
      rightFingers: fingerValues.right
    };
  }

  const leftPart = parseKeyValuePart(parts[1] || "");
  const rightPart = parseKeyValuePart(parts[2] || "");

  if (leftPart?.key !== "L" || rightPart?.key !== "R") {
    return null;
  }

  return {
    type: "sensor_raw",
    sensor: "flex",
    left: leftPart.value,
    right: rightPart.value,
    leftFingers: null,
    rightFingers: null
  };
}

function parseImuPayload(parts) {
  const rollPart = parseKeyValuePart(parts[1] || "");
  const pitchPart = parseKeyValuePart(parts[2] || "");
  const yawPart = parseKeyValuePart(parts[3] || "");

  if (rollPart?.key !== "R" || pitchPart?.key !== "P" || yawPart?.key !== "Y") {
    return null;
  }

  return {
    type: "sensor_raw",
    sensor: "imu",
    roll: rollPart.value,
    pitch: pitchPart.value,
    yaw: yawPart.value
  };
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

function parseSensorPayload(rawLine) {
  if (typeof rawLine !== "string") {
    return null;
  }

  const line = rawLine.trim();

  if (!line) {
    return null;
  }

  const parts = line.split("|").map((part) => part.trim());
  const frameType = parts[0];

  if (frameType === "FLEX") {
    return parseFlexPayload(parts);
  }

  if (frameType === "IMU") {
    return parseImuPayload(parts);
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
  parseSensorPayload,
  openSerialInput
};
