const SYSTEM_PROMPT =
  "你是一个温和、专业、谨慎的手部康复训练辅助教练。你只能提供训练辅助建议，不能进行医疗诊断。"
  + "请用中文输出，不超过80字。做对时先表扬，再给一个下一步小建议。"
  + "做错时指出偏差，再给一个具体可操作纠正建议。"
  + "不要使用诊断、治疗、处方、医学结论等表述，不要夸大疗效，不要建议药物或医疗处理。"
  + "如果数据不足，就说：建议放慢动作并保持2秒后再试。";

function normalizeInput(payload = {}) {
  return {
    targetGesture: payload.targetGesture || "UNKNOWN",
    actualGesture: payload.actualGesture || "UNKNOWN",
    isCorrect: Boolean(payload.isCorrect),
    confidence: Number(payload.confidence || 0),
    holdMs: Number(payload.holdMs || 0),
    userLevel: payload.userLevel || "beginner"
  };
}

function buildPrompt(payload = {}) {
  const input = normalizeInput(payload);

  return {
    system: SYSTEM_PROMPT,
    user: [
      "请根据下面训练数据给出一句训练辅助反馈：",
      `targetGesture=${input.targetGesture}`,
      `actualGesture=${input.actualGesture}`,
      `isCorrect=${input.isCorrect}`,
      `confidence=${input.confidence}`,
      `holdMs=${input.holdMs}`,
      `userLevel=${input.userLevel}`
    ].join("\n")
  };
}

function buildMockFeedback(payload = {}) {
  const {
    targetGesture,
    actualGesture,
    isCorrect,
    confidence,
    holdMs,
    userLevel
  } = normalizeInput(payload);

  if (!targetGesture || !actualGesture) {
    return "建议放慢动作并保持2秒后再试。";
  }

  if (isCorrect) {
    if (confidence >= 85 && holdMs >= 1200) {
      return "这次动作很稳，继续保持节奏，下一轮可再多稳定1秒。";
    }

    return "这次动作完成不错，继续保持手型，下一轮把稳定时间再拉长一点。";
  }

  if (targetGesture === actualGesture) {
    return "动作接近目标，但保持时间偏短，建议放慢并稳定保持2秒。";
  }

  if (confidence < 60) {
    return "当前动作边界不够清楚，建议先放慢动作并保持2秒后再试。";
  }

  if (userLevel === "beginner") {
    return "这次动作和目标不一致，建议先放慢节奏，注意手指展开或握合的一致性。";
  }

  return "本次动作有偏差，建议重新调整手型，再稳定保持2秒后重复一次。";
}

async function callDeepSeek(payload = {}) {
  const apiKey = process.env.DEEPSEEK_API_KEY;
  if (!apiKey) {
    return null;
  }

  const baseUrl = (process.env.DEEPSEEK_BASE_URL || "https://api.deepseek.com").replace(/\/+$/, "");
  const model = process.env.DEEPSEEK_MODEL || "deepseek-v4-flash";
  const timeoutMs = Number(process.env.DEEPSEEK_TIMEOUT_MS || 12000);
  const prompt = buildPrompt(payload);
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetch(`${baseUrl}/chat/completions`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${apiKey}`
      },
      body: JSON.stringify({
        model,
        messages: [
          { role: "system", content: prompt.system },
          { role: "user", content: prompt.user }
        ],
        stream: false,
        temperature: 0.4,
        max_tokens: 120
      }),
      signal: controller.signal
    });

    if (!response.ok) {
      console.warn(`[DeepSeek] HTTP ${response.status} ${response.statusText}`);
      return null;
    }

    const data = await response.json();
    const content = data?.choices?.[0]?.message?.content;

    if (typeof content !== "string" || !content.trim()) {
      console.warn("[DeepSeek] empty content in response");
      return null;
    }

    return content.trim().slice(0, 80);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    console.warn(`[DeepSeek] request failed, fallback to mock: ${message}`);
    return null;
  } finally {
    clearTimeout(timeout);
  }
}

async function generateAiFeedback(payload = {}) {
  const feedback = await callDeepSeek(payload);

  if (feedback) {
    return {
      feedback,
      source: "deepseek"
    };
  }

  return {
    feedback: buildMockFeedback(payload),
    source: "mock"
  };
}

module.exports = {
  buildPrompt,
  buildMockFeedback,
  callDeepSeek,
  generateAiFeedback
};
