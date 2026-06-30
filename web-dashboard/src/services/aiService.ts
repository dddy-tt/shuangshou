import { GestureType, GESTURE_MAP } from "../types";

export interface AiFeedbackRequest {
  targetGesture: GestureType;
  actualGesture: GestureType;
  isCorrect: boolean;
  confidence: number;
}

export const fetchAiFeedback = async (data: AiFeedbackRequest): Promise<string> =>
  new Promise((resolve) => {
    window.setTimeout(() => {
      if (data.isCorrect) {
        const praises = [
          "本次动作完成得很稳，姿态保持和节奏控制都比较好，可以继续保持当前训练强度。",
          "这次识别结果很准确，说明你的动作边界已经比较清晰，接下来可以继续拉长保持时长。",
          "训练表现不错，动作收放比较到位，建议下一轮继续保持专注，把稳定性再提高一点。"
        ];

        resolve(praises[Math.floor(Math.random() * praises.length)]);
        return;
      }

      resolve(
        `本轮目标动作是“${GESTURE_MAP[data.targetGesture]}”，系统识别为“${GESTURE_MAP[data.actualGesture]}”。建议先放慢动作切换速度，注意手指展开或握合的一致性，再进行下一次训练采集。当前置信度约为 ${data.confidence}%。`
      );
    }, 1200);
  });
