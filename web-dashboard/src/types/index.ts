export type GestureType =
  | "RIGHT_OPEN"
  | "RIGHT_FIST"
  | "LEFT_OPEN"
  | "LEFT_FIST"
  | "BOTH_OPEN"
  | "BOTH_FIST";

export type DashboardTab = "translation" | "rehab" | "care" | "iot";

export type SignGestureType = "HELP" | "DRINK" | "PAIN";

export type BridgeStatus = "connecting" | "online" | "offline";

export type AiFeedbackSource = "bridge" | "mock";

export interface GestureMessage {
  type: "gesture";
  gesture: GestureType;
  confidence: number;
  holdMs: number;
  timestamp: number;
}

export interface SignMessage {
  type: "sign";
  gesture: SignGestureType;
  translation: string;
  confidence: number;
  voicePlayed: boolean;
  timestamp: number;
}

export interface CareMessage {
  type: "care";
  hr: number;
  spo2: number;
  fall: boolean;
  sos: boolean;
  tip: string;
  timestamp: number;
}

export interface SystemMessage {
  type: "system";
  message: string;
  timestamp: number;
}

export type BridgeMessage = GestureMessage | SignMessage | CareMessage | SystemMessage;
export type WSMessage = GestureMessage;

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

export interface SignTranslationRecord {
  id: string;
  gesture: SignGestureType;
  text: string;
  confidence: number;
  voiceStatus: "播报中" | "待播报" | "已播报";
  time: string;
}

export interface CareMonitoringState {
  hr: number;
  spo2: number;
  fallDetected: boolean;
  sosActive: boolean;
  reminder: string;
}

export const GESTURE_MAP: Record<GestureType, string> = {
  RIGHT_OPEN: "右手张开",
  RIGHT_FIST: "右手握拳",
  LEFT_OPEN: "左手张开",
  LEFT_FIST: "左手握拳",
  BOTH_OPEN: "双手张开",
  BOTH_FIST: "双手握拳"
};

export const SIGN_TRANSLATION_MAP: Record<SignGestureType, string> = {
  HELP: "我需要帮助",
  DRINK: "我想喝水",
  PAIN: "我感觉不舒服"
};
