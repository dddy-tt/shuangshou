function buildAiFeedback(payload = {}) {
  const {
    targetGesture = "UNKNOWN",
    actualGesture = "UNKNOWN",
    isCorrect = false,
    confidence = 0,
    holdMs = 0,
    userLevel = "beginner"
  } = payload;

  if (isCorrect) {
    if (confidence >= 85 && holdMs >= 1000) {
      return "这次动作完成得很稳定，识别结果和目标一致，请保持当前节奏继续训练。";
    }

    return "这次动作识别正确，建议继续保持手势姿态，并适当延长稳定保持时间。";
  }

  if (targetGesture === actualGesture) {
    return "这次动作接近目标，但稳定保持时间还不够，建议放慢节奏并多保持一会儿。";
  }

  if (userLevel === "beginner") {
    return "这次识别到的动作和目标不一致，请放慢动作，注意手指伸展幅度，再尝试一次。";
  }

  if (confidence < 70) {
    return "当前动作识别置信度偏低，建议减少多余摆动，先把目标动作做清楚再继续。";
  }

  return "这次动作和目标还有偏差，建议重新调整手型与保持时长，再进行下一次训练。";
}

module.exports = {
  buildAiFeedback
};
