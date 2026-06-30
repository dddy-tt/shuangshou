const TASK_STATES = {
  PENDING: "PENDING",
  RUNNING: "RUNNING",
  DONE: "DONE",
  FAILED: "FAILED",
  BLOCKED: "BLOCKED"
};

const tasks = new Map();
const listeners = new Set();

function notify(task) {
  listeners.forEach((listener) => {
    listener(task);
  });
}

function createTask(id, meta = {}) {
  const task = {
    id,
    state: TASK_STATES.PENDING,
    meta: { ...meta },
    updatedAt: Date.now()
  };

  tasks.set(id, task);
  notify(task);
  return task;
}

function setTaskState(id, state) {
  const task = tasks.get(id);

  if (!task) {
    return null;
  }

  task.state = state;
  task.updatedAt = Date.now();
  notify(task);
  return task;
}

function updateTask(id, patch = {}) {
  const task = tasks.get(id);

  if (!task) {
    return null;
  }

  Object.assign(task, patch);
  task.updatedAt = Date.now();
  notify(task);
  return task;
}

function getTask(id) {
  const task = tasks.get(id);
  return task ? { ...task, meta: { ...task.meta } } : null;
}

function getAllTasks() {
  return Array.from(tasks.values()).map((task) => ({
    ...task,
    meta: { ...task.meta }
  }));
}

function subscribe(listener) {
  listeners.add(listener);

  return () => {
    listeners.delete(listener);
  };
}

module.exports = {
  TASK_STATES,
  createTask,
  setTaskState,
  updateTask,
  getTask,
  getAllTasks,
  subscribe
};
