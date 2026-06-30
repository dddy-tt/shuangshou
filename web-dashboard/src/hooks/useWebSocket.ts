import { useCallback, useEffect, useState } from "react";
import { GESTURE_MAP, GestureType, WSMessage } from "../types";

const allGestures = Object.keys(GESTURE_MAP) as GestureType[];

export const useWebSocket = () => {
  const [isConnected, setIsConnected] = useState(false);
  const [lastMessage, setLastMessage] = useState<WSMessage | null>(null);

  useEffect(() => {
    const timer = window.setTimeout(() => setIsConnected(true), 900);
    return () => window.clearTimeout(timer);
  }, []);

  const simulateWebSocketMessage = useCallback((targetGesture: GestureType) => {
    const isCorrect = Math.random() >= 0.3;
    let actualGesture = targetGesture;

    if (!isCorrect) {
      const wrongGestures = allGestures.filter((gesture) => gesture !== targetGesture);
      actualGesture = wrongGestures[Math.floor(Math.random() * wrongGestures.length)];
    }

    const mockData: WSMessage = {
      type: "gesture",
      gesture: actualGesture,
      confidence: isCorrect ? 85 + Math.floor(Math.random() * 14) : 52 + Math.floor(Math.random() * 28),
      holdMs: 600 + Math.floor(Math.random() * 1500),
      timestamp: Date.now()
    };

    setLastMessage(mockData);
  }, []);

  return {
    isConnected,
    lastMessage,
    simulateWebSocketMessage
  };
};
