function publishControl(device, action, source = "glove") {
  const topic = `shuangshou/control/${String(device || "").toLowerCase()}`;
  const payload = JSON.stringify({
    action,
    source
  });

  console.log(`[MQTT MOCK] ${topic} ${payload}`);

  return {
    topic,
    payload
  };
}

module.exports = {
  publishControl
};
