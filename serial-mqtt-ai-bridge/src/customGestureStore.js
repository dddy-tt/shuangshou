const fs = require("fs");
const path = require("path");

const storePath = path.resolve(__dirname, "..", "data", "custom_gestures.json");

function ensureStoreFile() {
  const dirPath = path.dirname(storePath);

  if (!fs.existsSync(dirPath)) {
    fs.mkdirSync(dirPath, { recursive: true });
  }

  if (!fs.existsSync(storePath)) {
    fs.writeFileSync(storePath, "[]\n", "utf8");
  }
}

function readCustomGestures() {
  ensureStoreFile();

  try {
    const content = fs.readFileSync(storePath, "utf8").trim();
    if (!content) {
      return [];
    }

    const parsed = JSON.parse(content);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

function writeCustomGestures(items) {
  ensureStoreFile();
  fs.writeFileSync(storePath, `${JSON.stringify(items, null, 2)}\n`, "utf8");
}

function createCustomGesture(input) {
  const items = readCustomGestures();
  const nextItem = {
    id: `gesture_${Date.now()}`,
    name: input.name,
    category: input.category,
    action: input.action,
    snapshot: input.snapshot,
    createdAt: Date.now()
  };

  items.unshift(nextItem);
  writeCustomGestures(items);
  return nextItem;
}

function deleteCustomGesture(id) {
  const items = readCustomGestures();
  const nextItems = items.filter((item) => item.id !== id);
  const removed = nextItems.length !== items.length;

  if (removed) {
    writeCustomGestures(nextItems);
  }

  return removed;
}

module.exports = {
  readCustomGestures,
  createCustomGesture,
  deleteCustomGesture,
  storePath
};
