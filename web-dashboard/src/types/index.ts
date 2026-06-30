export type GestureType =
  | "RIGHT_OPEN"
  | "RIGHT_FIST"
  | "LEFT_OPEN"
  | "LEFT_FIST"
  | "BOTH_OPEN"
  | "BOTH_FIST";

export type DashboardTab = "training" | "iot";

export interface WSMessage {
  type: "gesture";
  gesture: GestureType;
  confidence: number;
  holdMs: number;
  timestamp: number;
}

export interface TrainingRecord {
  id: string;
  target: GestureType;
  actual: GestureType;
  isCorrect: boolean;
  time: string;
  confidence: number;
}

export interface TrainingStats {
  total: number;
  correct: number;
  streak: number;
  history: TrainingRecord[];
}

export const GESTURE_MAP: Record<GestureType, string> = {
  RIGHT_OPEN: "右手张开",
  RIGHT_FIST: "右手握拳",
  LEFT_OPEN: "左手张开",
  LEFT_FIST: "左手握拳",
  BOTH_OPEN: "双手张开",
  BOTH_FIST: "双手握拳"
};
