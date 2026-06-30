import { useCallback, useEffect, useRef, useState } from "react";
import {
  AiFeedbackMessage,
  BridgeMessage,
  BridgeStatus,
  CareMessage,
  GESTURE_MAP,
  GestureMessage,
  GestureType,
  SignMessage,
  SystemMessage
} from "../types";

const allGestures = Object.keys(GESTURE_MAP) as GestureType[];
const BRIDGE_WS_URL = "ws://localhost:8765";

export const useWebSocket = () => {
  const [bridgeStatus, setBridgeStatus] = useState<BridgeStatus>("connecting");
  const [lastGestureMessage, setLastGestureMessage] = useState<GestureMessage | null>(null);
  const [lastSignMessage, setLastSignMessage] = useState<SignMessage | null>(null);
  const [lastCareMessage, setLastCareMessage] = useState<CareMessage | null>(null);
  const [lastAiFeedbackMessage, setLastAiFeedbackMessage] = useState<AiFeedbackMessage | null>(null);
  const [lastSystemMessage, setLastSystemMessage] = useState<SystemMessage | null>(null);
  const reconnectTimerRef = useRef<number | null>(null);
  const socketRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    let socket: WebSocket | null = null;
    let disposed = false;

    const scheduleReconnect = () => {
      if (disposed) {
        return;
      }

      if (reconnectTimerRef.current !== null) {
        window.clearTimeout(reconnectTimerRef.current);
      }

      reconnectTimerRef.current = window.setTimeout(() => {
        setBridgeStatus("connecting");
        connect();
      }, 3000);
    };

    const handleMessage = (message: BridgeMessage) => {
      if (message.type === "gesture") {
        setLastGestureMessage(message);
        return;
      }

      if (message.type === "sign") {
        setLastSignMessage(message);
        return;
      }

      if (message.type === "care") {
        setLastCareMessage(message);
        return;
      }

      if (message.type === "ai_feedback") {
        setLastAiFeedbackMessage(message);
        return;
      }

      setLastSystemMessage(message);
    };

    const connect = () => {
      try {
        socket = new window.WebSocket(BRIDGE_WS_URL);
        socketRef.current = socket;
      } catch {
        setBridgeStatus("offline");
        scheduleReconnect();
        return;
      }

      socket.onopen = () => {
        if (!disposed) {
          setBridgeStatus("online");
        }
      };

      socket.onmessage = (event) => {
        try {
          const parsed = JSON.parse(String(event.data)) as BridgeMessage;
          handleMessage(parsed);
        } catch {
          // Keep the dashboard alive even if one frame is malformed.
        }
      };

      socket.onerror = () => {
        if (!disposed) {
          setBridgeStatus("offline");
        }
      };

      socket.onclose = () => {
        if (disposed) {
          return;
        }

        setBridgeStatus("offline");
        socketRef.current = null;
        scheduleReconnect();
      };
    };

    connect();

    return () => {
      disposed = true;

      if (reconnectTimerRef.current !== null) {
        window.clearTimeout(reconnectTimerRef.current);
      }

      socketRef.current = null;
      socket?.close();
    };
  }, []);

  const simulateWebSocketMessage = useCallback((targetGesture: GestureType) => {
    const isCorrect = Math.random() >= 0.3;
    let actualGesture = targetGesture;

    if (!isCorrect) {
      const wrongGestures = allGestures.filter((gesture) => gesture !== targetGesture);
      actualGesture = wrongGestures[Math.floor(Math.random() * wrongGestures.length)];
    }

    const mockData: GestureMessage = {
      type: "gesture",
      gesture: actualGesture,
      confidence: isCorrect ? 85 + Math.floor(Math.random() * 14) : 52 + Math.floor(Math.random() * 28),
      holdMs: 600 + Math.floor(Math.random() * 1500),
      timestamp: Date.now()
    };

    setLastGestureMessage(mockData);
  }, []);

  const sendBridgeMessage = useCallback((message: { type: "gesture" | "command"; data: unknown }) => {
    const socket = socketRef.current;

    if (!socket || socket.readyState !== window.WebSocket.OPEN) {
      return false;
    }

    socket.send(JSON.stringify(message));
    return true;
  }, []);

  return {
    bridgeStatus,
    isConnected: bridgeStatus === "online",
    lastGestureMessage,
    lastSignMessage,
    lastCareMessage,
    lastAiFeedbackMessage,
    lastSystemMessage,
    simulateWebSocketMessage,
    sendBridgeMessage
  };
};
