const signSamples = [
  { gesture: "HELP", translation: "我需要帮助", voicePlayed: true },
  { gesture: "DRINK", translation: "我想喝水", voicePlayed: false },
  { gesture: "PAIN", translation: "我感觉不舒服", voicePlayed: true }
];

const gestureSamples = [
  { gesture: "RIGHT_OPEN", confidence: 86, holdMs: 1200 },
  { gesture: "RIGHT_FIST", confidence: 72, holdMs: 820 },
  { gesture: "LEFT_OPEN", confidence: 91, holdMs: 1330 }
];

const careSamples = [
  { hr: 78, spo2: 97, fall: false, sos: false, tip: "当前状态平稳，建议继续观察。" },
  { hr: 84, spo2: 98, fall: false, sos: false, tip: "当前训练节奏正常，请继续保持。" },
  { hr: 76, spo2: 96, fall: false, sos: true, tip: "已触发 SOS 状态提示，请尽快确认现场情况。" }
];

let sequence = 0;

function withTimestamp(payload) {
  return {
    ...payload,
    timestamp: Date.now()
  };
}

function nextGestureMessage() {
  const sample = gestureSamples[sequence % gestureSamples.length];
  return withTimestamp({
    type: "gesture",
    ...sample
  });
}

function nextSignMessage() {
  const sample = signSamples[sequence % signSamples.length];
  return withTimestamp({
    type: "sign",
    confidence: 88 - ((sequence % 3) * 4),
    ...sample
  });
}

function nextCareMessage() {
  const sample = careSamples[sequence % careSamples.length];
  return withTimestamp({
    type: "care",
    ...sample
  });
}

function nextMockMessage() {
  const typeIndex = sequence % 3;
  let message;

  if (typeIndex === 0) {
    message = nextGestureMessage();
  } else if (typeIndex === 1) {
    message = nextSignMessage();
  } else {
    message = nextCareMessage();
  }

  sequence += 1;
  return message;
}

module.exports = {
  nextMockMessage
};
