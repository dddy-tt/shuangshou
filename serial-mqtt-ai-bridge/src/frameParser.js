function parseKeyValuePairs(text) {
  return text.split(",").reduce((result, item) => {
    const [rawKey, rawValue] = item.split("=");

    if (!rawKey || typeof rawValue === "undefined") {
      return result;
    }

    const key = rawKey.trim();
    const value = rawValue.trim();
    const numberValue = Number(value);

    result[key] = Number.isNaN(numberValue) ? value : numberValue;
    return result;
  }, {});
}

function parseBringup(line) {
  const payload = line.slice("BRINGUP:".length).trim();
  if (!payload) {
    return null;
  }

  const fields = parseKeyValuePairs(payload);
  if (Object.keys(fields).length === 0) {
    return null;
  }

  return {
    type: "bringup",
    ...fields
  };
}

function parseGesture(line) {
  const payload = line.slice("GESTURE:".length).trim();
  const fields = parseKeyValuePairs(payload);

  if (!fields.ID) {
    return null;
  }

  return {
    type: "gesture",
    gesture: fields.ID,
    confidence: typeof fields.CONF === "number" ? fields.CONF : 0,
    holdMs: typeof fields.HOLD === "number" ? fields.HOLD : 0
  };
}

function parseControl(line) {
  const payload = line.slice("CTRL:".length).trim();
  const fields = parseKeyValuePairs(payload);

  if (!fields.DEV || !fields.ACT) {
    return null;
  }

  return {
    type: "control",
    device: fields.DEV,
    action: fields.ACT
  };
}

function parseFrame(line) {
  if (typeof line !== "string") {
    return null;
  }

  const trimmed = line.trim();
  if (!trimmed) {
    return null;
  }

  if (trimmed.startsWith("BRINGUP:")) {
    return parseBringup(trimmed);
  }

  if (trimmed.startsWith("GESTURE:")) {
    return parseGesture(trimmed);
  }

  if (trimmed.startsWith("CTRL:")) {
    return parseControl(trimmed);
  }

  return null;
}

module.exports = {
  parseFrame
};
