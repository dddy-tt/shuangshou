import { Activity, ChevronRight, HeartPulse, Home, Languages, Radar } from "lucide-react";
import React, { useEffect, useMemo, useState } from "react";
import { CareMonitoringPanel } from "./components/CareMonitoringPanel";
import { CustomGestureManager } from "./components/CustomGestureManager";
import { FeedbackCard } from "./components/FeedbackCard";
import { IotCard } from "./components/IotCard";
import { ResultCard } from "./components/ResultCard";
import { SensorMonitorView } from "./components/SensorMonitorView";
import { SignTranslationPanel } from "./components/SignTranslationPanel";
import { StatsCard } from "./components/StatsCard";
import { StatusCard } from "./components/StatusCard";
import { TaskCard } from "./components/TaskCard";
import { useWebSocket } from "./hooks/useWebSocket";
import {
  AiFeedbackSource,
  CareMonitoringState,
  CustomGestureCategory,
  CustomGestureItem,
  CustomGestureMatchMessage,
  DashboardTab,
  GESTURE_MAP,
  GestureType,
  SIGN_TRANSLATION_MAP,
  SignGestureType,
  SignTranslationRecord,
  TrainingRecord,
  TrainingStats,
  WSMessage
} from "./types";

const signGestureOrder: SignGestureType[] = ["HELP", "DRINK", "PAIN"];
const fingerMockBase = [18, 32, 45, 58, 71];

const careScenarios: CareMonitoringState[] = [
  { hr: 76, spo2: 98, fallDetected: false, sosActive: false, reminder: "状态平稳，建议继续保持日常观察。" },
  { hr: 82, spo2: 97, fallDetected: false, sosActive: false, reminder: "训练后可适当补水，保持舒适坐姿。" },
  { hr: 79, spo2: 99, fallDetected: true, sosActive: true, reminder: "检测到异常姿态，请尽快查看当前状态。" }
];

const tabLabelMap: Record<DashboardTab, string> = {
  translation: "手语翻译",
  rehab: "AI 康复训练",
  care: "护理监测",
  iot: "家电远控"
};

type DeviceKey = "light" | "fan" | "socket" | "sos";
type DeviceState = Record<DeviceKey, boolean>;

function App() {
  const {
    bridgeStatus,
    lastAiFeedbackMessage,
    lastCareMessage,
    lastCustomGestureMatchMessage,
    lastGestureMessage,
    lastSensorRawMessage,
    lastSignMessage,
    lastSystemMessage,
    simulateWebSocketMessage,
    sendBridgeMessage
  } = useWebSocket();
  const [activeTab, setActiveTab] = useState<DashboardTab>("translation");
  const [targetGesture, setTargetGesture] = useState<GestureType>("RIGHT_OPEN");
  const [aiFeedback, setAiFeedback] = useState("");
  const [isAiLoading, setIsAiLoading] = useState(false);
  const [aiSource, setAiSource] = useState<AiFeedbackSource>("mock");
  const [stats, setStats] = useState<TrainingStats>({ total: 0, correct: 0, streak: 0, history: [] });
  const [trainingResult, setTrainingResult] = useState<WSMessage | null>(null);
  const [waitingBridgeGesture, setWaitingBridgeGesture] = useState(false);
  const [translationRecords, setTranslationRecords] = useState<SignTranslationRecord[]>(() => [createSignRecord(0)]);
  const [careIndex, setCareIndex] = useState(0);
  const [careState, setCareState] = useState<CareMonitoringState>(careScenarios[0]);
  const [flexState, setFlexState] = useState({
    left: null as number | null,
    right: null as number | null,
    leftFingers: null as number[] | null,
    rightFingers: null as number[] | null,
    normalizedLeft: null as number | null,
    normalizedRight: null as number | null,
    normalizedLeftFingers: null as Array<number | null> | null,
    normalizedRightFingers: null as Array<number | null> | null
  });
  const [imuState, setImuState] = useState({
    roll: 0,
    pitch: 0,
    yaw: 0
  });
  const [calibrationBusy, setCalibrationBusy] = useState(false);
  const [calibrationMessage, setCalibrationMessage] = useState("串口上报后可进行 FLEX 置零、握满和 IMU 置零。");
  const [customGestureName, setCustomGestureName] = useState("");
  const [customGestureAction, setCustomGestureAction] = useState("");
  const [customGestureCategory, setCustomGestureCategory] = useState<CustomGestureCategory>("translation");
  const [customGestureItems, setCustomGestureItems] = useState<CustomGestureItem[]>([]);
  const [customGestureBusy, setCustomGestureBusy] = useState(false);
  const [customGestureMessage, setCustomGestureMessage] = useState("可将当前十指和 IMU 姿态保存为自定义手势模板。");
  const [trainingPrompt, setTrainingPrompt] = useState("点击“随机选择训练手势”后，系统会从训练手势库里抽一个动作给你练习。");
  const [devices, setDevices] = useState<DeviceState>({
    light: false,
    fan: false,
    socket: false,
    sos: false
  });

  const bridgeOnline = bridgeStatus === "online";

  useEffect(() => {
    if (!lastGestureMessage) {
      return;
    }

    if (bridgeOnline && !waitingBridgeGesture) {
      return;
    }

    const isCorrect = lastGestureMessage.gesture === targetGesture;
    setTrainingResult(lastGestureMessage);

    setStats((previous) => {
      const newRecord: TrainingRecord = {
        id: `${lastGestureMessage.timestamp}`,
        target: targetGesture,
        actual: lastGestureMessage.gesture,
        isCorrect,
        confidence: lastGestureMessage.confidence,
        time: new Date(lastGestureMessage.timestamp).toLocaleTimeString("zh-CN", {
          hour12: false,
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit"
        })
      };

      return {
        total: previous.total + 1,
        correct: previous.correct + (isCorrect ? 1 : 0),
        streak: isCorrect ? previous.streak + 1 : 0,
        history: [newRecord, ...previous.history].slice(0, 10)
      };
    });

    if (bridgeOnline) {
      setIsAiLoading(true);
      const sent = sendBridgeMessage({
        type: "gesture",
        data: {
          targetGesture,
          actualGesture: lastGestureMessage.gesture,
          isCorrect,
          confidence: lastGestureMessage.confidence,
          holdMs: lastGestureMessage.holdMs,
          userLevel: "beginner"
        }
      });

      if (!sent) {
        setAiFeedback(buildLocalAiFeedback({
          targetGesture,
          actualGesture: lastGestureMessage.gesture,
          isCorrect,
          confidence: lastGestureMessage.confidence
        }));
        setAiSource("mock");
        setIsAiLoading(false);
      }
    } else {
      setAiFeedback(buildLocalAiFeedback({
        targetGesture,
        actualGesture: lastGestureMessage.gesture,
        isCorrect,
        confidence: lastGestureMessage.confidence
      }));
      setAiSource("mock");
      setIsAiLoading(false);
    }

    setWaitingBridgeGesture(false);
  }, [bridgeOnline, lastGestureMessage, sendBridgeMessage, targetGesture, waitingBridgeGesture]);

  useEffect(() => {
    if (!lastAiFeedbackMessage) {
      return;
    }

    setAiFeedback(lastAiFeedbackMessage.result);
    setAiSource(lastAiFeedbackMessage.source);
    setIsAiLoading(false);
  }, [lastAiFeedbackMessage]);

  useEffect(() => {
    if (bridgeOnline) {
      return;
    }

    const timer = window.setInterval(() => {
      setCareIndex((previous) => (previous + 1) % careScenarios.length);
    }, 4500);

    return () => window.clearInterval(timer);
  }, [bridgeOnline]);

  useEffect(() => {
    if (!bridgeOnline) {
      setCareState(careScenarios[careIndex]);
    }
  }, [bridgeOnline, careIndex]);

  useEffect(() => {
    if (bridgeOnline) {
      return;
    }

    const updateMockSensors = () => {
      const seed = Date.now();
      const leftFingers = fingerMockBase.map((base, index) => base + ((Math.floor(seed / 700) + index * 9) % 18));
      const rightFingers = fingerMockBase.map((base, index) => base + 8 + ((Math.floor(seed / 900) + index * 7) % 20));

      setFlexState({
        left: leftFingers.reduce((sum, value) => sum + value, 0),
        right: rightFingers.reduce((sum, value) => sum + value, 0),
        leftFingers,
        rightFingers,
        normalizedLeft: Number((leftFingers.reduce((sum, value) => sum + value, 0) / 500).toFixed(2)),
        normalizedRight: Number((rightFingers.reduce((sum, value) => sum + value, 0) / 500).toFixed(2)),
        normalizedLeftFingers: leftFingers.map((value) => Number((value / 100).toFixed(2))),
        normalizedRightFingers: rightFingers.map((value) => Number((value / 100).toFixed(2)))
      });

      setImuState({
        roll: Number((Math.sin(seed / 1200) * 24).toFixed(2)),
        pitch: Number((Math.cos(seed / 1500) * 18).toFixed(2)),
        yaw: Number(((seed / 45) % 360).toFixed(2))
      });
    };

    updateMockSensors();
    const timer = window.setInterval(updateMockSensors, 2000);

    return () => window.clearInterval(timer);
  }, [bridgeOnline]);

  useEffect(() => {
    if (!lastSignMessage) {
      return;
    }

    setTranslationRecords((previous) => {
      const nextRecord: SignTranslationRecord = {
        id: `${lastSignMessage.timestamp}`,
        gesture: lastSignMessage.gesture,
        text: lastSignMessage.translation,
        confidence: lastSignMessage.confidence,
        voiceStatus: lastSignMessage.voicePlayed ? "已播报" : "待播报",
        time: new Date(lastSignMessage.timestamp).toLocaleTimeString("zh-CN", {
          hour12: false,
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit"
        })
      };

      return [nextRecord, ...previous].slice(0, 8);
    });
  }, [lastSignMessage]);

  useEffect(() => {
    if (!lastCareMessage) {
      return;
    }

    setCareState({
      hr: lastCareMessage.hr,
      spo2: lastCareMessage.spo2,
      fallDetected: lastCareMessage.fall,
      sosActive: lastCareMessage.sos,
      reminder: lastCareMessage.tip
    });
  }, [lastCareMessage]);

  useEffect(() => {
    if (!lastSensorRawMessage) {
      return;
    }

    if (lastSensorRawMessage.sensor === "flex") {
      setFlexState({
        left: lastSensorRawMessage.left,
        right: lastSensorRawMessage.right,
        leftFingers: lastSensorRawMessage.leftFingers,
        rightFingers: lastSensorRawMessage.rightFingers,
        normalizedLeft: lastSensorRawMessage.normalizedLeft,
        normalizedRight: lastSensorRawMessage.normalizedRight,
        normalizedLeftFingers: lastSensorRawMessage.normalizedLeftFingers,
        normalizedRightFingers: lastSensorRawMessage.normalizedRightFingers
      });
      return;
    }

    setImuState({
      roll: lastSensorRawMessage.roll,
      pitch: lastSensorRawMessage.pitch,
      yaw: lastSensorRawMessage.yaw
    });
  }, [lastSensorRawMessage]);

  useEffect(() => {
    if (!bridgeOnline) {
      setWaitingBridgeGesture(false);
    }
  }, [bridgeOnline]);

  useEffect(() => {
    if (!lastCustomGestureMatchMessage) {
      return;
    }

    applyCustomGestureMatch(lastCustomGestureMatchMessage, {
      setTranslationRecords,
      setDevices,
      setTrainingPrompt,
      setCustomGestureMessage
    });
  }, [lastCustomGestureMatchMessage]);

  useEffect(() => {
    const loadCustomGestures = async () => {
      try {
        const response = await window.fetch("http://localhost:8765/api/custom-gestures");
        const result = (await response.json()) as { ok?: boolean; items?: CustomGestureItem[] };

        if (!response.ok || !result.ok) {
          throw new Error("加载自定义手势失败");
        }

        setCustomGestureItems(Array.isArray(result.items) ? result.items : []);
        setCustomGestureMessage("已从 bridge 加载自定义手势列表。");
      } catch (error) {
        setCustomGestureMessage(error instanceof Error ? error.message : "bridge 未启动，暂时无法读取自定义手势。");
      }
    };

    void loadCustomGestures();
  }, []);

  const currentTranslation = translationRecords[0];
  const currentCare = careState;

  const sendCalibrationRequest = async (action: "zero" | "full" | "imu-zero") => {
    setCalibrationBusy(true);

    try {
      const response = await window.fetch(`http://localhost:8765/api/calibration/${action}`, {
        method: "POST"
      });

      const result = (await response.json()) as { ok?: boolean; message?: string };

      if (!response.ok || !result.ok) {
        throw new Error(result.message || "校准请求失败");
      }

      if (action === "zero") {
        setCalibrationMessage("置零成功，已记录当前 FLEX 原始值作为 offset。");
      } else if (action === "full") {
        setCalibrationMessage("握满成功，已记录当前 FLEX 原始值作为 full scale。");
      } else {
        setCalibrationMessage("IMU 置零成功，当前姿态已作为参考姿态。");
      }
    } catch (error) {
      setCalibrationMessage(error instanceof Error ? error.message : "校准请求失败");
    } finally {
      setCalibrationBusy(false);
    }
  };

  const handleSimulateTranslation = () => {
    if (bridgeOnline) {
      return;
    }

    setTranslationRecords((previous) => {
      const nextRecord = createSignRecord(previous.length);
      return [nextRecord, ...previous].slice(0, 8);
    });
  };

  const handleStartTraining = () => {
    if (bridgeOnline) {
      setWaitingBridgeGesture(true);
      return;
    }

    simulateWebSocketMessage(targetGesture);
  };

  const handleCaptureCustomGesture = async () => {
    if (!customGestureName.trim() || !customGestureAction.trim()) {
      setCustomGestureMessage("请先填写手势名称和动作内容。");
      return;
    }

    if (!Array.isArray(flexState.leftFingers) || !Array.isArray(flexState.rightFingers)) {
      setCustomGestureMessage("当前还没有十指数据，先等待传感器上报后再保存。");
      return;
    }

    setCustomGestureBusy(true);

    try {
      const response = await window.fetch("http://localhost:8765/api/custom-gestures", {
        method: "POST",
        headers: {
          "Content-Type": "application/json"
        },
        body: JSON.stringify({
          name: customGestureName.trim(),
          category: customGestureCategory,
          action: customGestureAction.trim(),
          snapshot: {
            leftFingers: flexState.leftFingers,
            rightFingers: flexState.rightFingers,
            roll: imuState.roll,
            pitch: imuState.pitch,
            yaw: imuState.yaw
          }
        })
      });

      const result = (await response.json()) as { ok?: boolean; item?: CustomGestureItem; message?: string };

      if (!response.ok || !result.ok || !result.item) {
        throw new Error(result.message || "保存自定义手势失败");
      }

      setCustomGestureItems((previous) => [result.item as CustomGestureItem, ...previous]);
      setCustomGestureMessage(`已保存手势“${result.item.name}”。`);
      setCustomGestureName("");
      setCustomGestureAction("");
    } catch (error) {
      setCustomGestureMessage(error instanceof Error ? error.message : "保存自定义手势失败");
    } finally {
      setCustomGestureBusy(false);
    }
  };

  const handleDeleteCustomGesture = async (id: string) => {
    setCustomGestureBusy(true);

    try {
      const response = await window.fetch(`http://localhost:8765/api/custom-gestures/${id}`, {
        method: "DELETE"
      });
      const result = (await response.json()) as { ok?: boolean; message?: string };

      if (!response.ok || !result.ok) {
        throw new Error(result.message || "删除自定义手势失败");
      }

      setCustomGestureItems((previous) => previous.filter((item) => item.id !== id));
      setCustomGestureMessage("已删除选中的自定义手势。");
    } catch (error) {
      setCustomGestureMessage(error instanceof Error ? error.message : "删除自定义手势失败");
    } finally {
      setCustomGestureBusy(false);
    }
  };

  const handleToggleDevice = (key: DeviceKey) => {
    setDevices((previous) => ({
      ...previous,
      [key]: !previous[key]
    }));
  };

  const handlePickRandomTraining = () => {
    const trainingItems = customGestureItems.filter((item) => item.category === "training");

    if (!trainingItems.length) {
      setTrainingPrompt("手势库里还没有训练类手势，先保存一个“训练目标”模板。");
      return;
    }

    const picked = trainingItems[Math.floor(Math.random() * trainingItems.length)];
    setTrainingPrompt(`随机训练任务：请完成“${picked.name}”，目标内容：${picked.action}`);
    setActiveTab("rehab");
  };

  const modeValue =
    bridgeStatus === "online"
      ? "Bridge 在线"
      : bridgeStatus === "connecting"
        ? "尝试连接 Bridge"
        : "Bridge 离线 / Mock";
  const aiValue = aiSource === "deepseek" ? "DeepSeek 在线" : "AI 本地回退";
  const bridgeNote = lastSystemMessage?.message || "Bridge 不在线时会自动回退前端 Mock。";

  const moduleContent = useMemo(() => {
    if (activeTab === "translation") {
      return (
        <SignTranslationPanel
          currentRecord={currentTranslation}
          records={translationRecords}
          onSimulate={handleSimulateTranslation}
        />
      );
    }

    if (activeTab === "care") {
      return <CareMonitoringPanel monitoring={currentCare} />;
    }

    if (activeTab === "iot") {
      return (
        <div className="grid grid-cols-1 gap-6 xl:grid-cols-[1.1fr_0.9fr]">
          <IotCard devices={devices} onToggle={handleToggleDevice} />
          <StatusCard bridgeStatus={bridgeStatus} aiSource={aiSource} lastSystemMessage={bridgeNote} />
        </div>
      );
    }

    return (
      <div className="grid grid-cols-1 gap-6 xl:grid-cols-[1.05fr_0.95fr]">
        <div className="space-y-6">
          <TaskCard target={targetGesture} setTarget={setTargetGesture} onStart={handleStartTraining} />
          <div className="rounded-2xl border border-fuchsia-300/15 bg-fuchsia-300/10 p-4 text-sm text-slate-200">
            <p className="font-bold text-fuchsia-100">训练提示</p>
            <p className="mt-2">{trainingPrompt}</p>
          </div>
          <ResultCard data={trainingResult} isCorrect={trainingResult?.gesture === targetGesture} />
        </div>

        <div className="space-y-6">
          <StatusCard bridgeStatus={bridgeStatus} aiSource={aiSource} lastSystemMessage={bridgeNote} />
          <FeedbackCard feedback={aiFeedback} isLoading={isAiLoading} source={aiSource} />
        </div>

        <div className="xl:col-span-2">
          <StatsCard stats={stats} />
        </div>
      </div>
    );
  }, [activeTab, aiFeedback, aiSource, bridgeNote, bridgeStatus, currentCare, currentTranslation, devices, isAiLoading, stats, targetGesture, trainingPrompt, trainingResult, translationRecords]);

  return (
    <div className="dashboard-shell">
      <div className="mx-auto max-w-7xl px-4 py-8 sm:px-6 lg:px-8">
        <header className="relative overflow-hidden rounded-[36px] border border-white/10 bg-slate-950/40 p-6 shadow-panel">
          <div className="noise-overlay absolute inset-0" />
          <div className="relative flex flex-col gap-6 lg:flex-row lg:items-end lg:justify-between">
            <div className="max-w-3xl">
              <div className="inline-flex items-center gap-2 rounded-full border border-sky-300/20 bg-sky-300/10 px-4 py-2 text-xs font-bold uppercase tracking-[0.35em] text-sky-100">
                <Radar size={14} />
                Mock Frontend Dashboard
              </div>
              <h1 className="mt-4 text-4xl font-black leading-tight text-white sm:text-5xl" style={{ fontFamily: '"ZCOOL XiaoWei", serif' }}>
                双手智能手语交互手套系统
              </h1>
              <p className="mt-4 max-w-2xl text-sm leading-7 text-slate-300 sm:text-base">
                将手语翻译、AI 康复训练、护理监测和家电远控整合到同一套前端看板中，
                可优先接入本地 bridge，也可在 bridge 不在线时自动回退前端 Mock。
              </p>
            </div>

            <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
              <TopStatusCard title="模式" value={modeValue} />
              <TopStatusCard title="端口" value="localhost:3000" />
              <TopStatusCard title="状态" value={aiValue} />
            </div>
          </div>
        </header>

        <div className="mt-6 flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
          <div className="flex flex-wrap items-center gap-3">
            <NavButton active={activeTab === "translation"} onClick={() => setActiveTab("translation")} icon={<Languages size={16} className="mr-2 inline-block" />} label="手语翻译" />
            <NavButton active={activeTab === "rehab"} onClick={() => setActiveTab("rehab")} icon={<Activity size={16} className="mr-2 inline-block" />} label="AI 康复训练" />
            <NavButton active={activeTab === "care"} onClick={() => setActiveTab("care")} icon={<HeartPulse size={16} className="mr-2 inline-block" />} label="护理监测" />
            <NavButton active={activeTab === "iot"} onClick={() => setActiveTab("iot")} icon={<Home size={16} className="mr-2 inline-block" />} label="家电远控" />
          </div>

          <div className="flex items-center gap-2 text-sm text-slate-400">
            <span>功能模块</span>
            <ChevronRight size={16} />
            <span className="font-bold text-slate-200">{tabLabelMap[activeTab]}</span>
          </div>
        </div>

        <main className="mt-6">{moduleContent}</main>
        <div className="mt-6">
          <SensorMonitorView
            flex={flexState}
            imu={imuState}
            onZero={() => {
              void sendCalibrationRequest("zero");
            }}
            onFull={() => {
              void sendCalibrationRequest("full");
            }}
            onImuZero={() => {
              void sendCalibrationRequest("imu-zero");
            }}
            calibrationBusy={calibrationBusy}
            calibrationMessage={calibrationMessage}
          />
        </div>
        <div className="mt-6">
          <CustomGestureManager
            gestureName={customGestureName}
            actionText={customGestureAction}
            category={customGestureCategory}
            items={customGestureItems}
            latestMatch={lastCustomGestureMatchMessage}
            busy={customGestureBusy}
            message={customGestureMessage}
            trainingPrompt={trainingPrompt}
            onNameChange={setCustomGestureName}
            onActionChange={setCustomGestureAction}
            onCategoryChange={setCustomGestureCategory}
            onCapture={() => {
              void handleCaptureCustomGesture();
            }}
            onDelete={(id) => {
              void handleDeleteCustomGesture(id);
            }}
            onPickRandomTraining={handlePickRandomTraining}
          />
        </div>
      </div>
    </div>
  );
}

function createSignRecord(seed: number): SignTranslationRecord {
  const gesture = signGestureOrder[seed % signGestureOrder.length];
  const statuses: SignTranslationRecord["voiceStatus"][] = ["播报中", "待播报", "已播报"];

  return {
    id: `${Date.now()}-${seed}`,
    gesture,
    text: SIGN_TRANSLATION_MAP[gesture],
    confidence: 86 + ((seed * 7) % 12),
    voiceStatus: statuses[seed % statuses.length],
    time: new Date().toLocaleTimeString("zh-CN", {
      hour12: false,
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit"
    })
  };
}

function NavButton({ active, onClick, icon, label }: { active: boolean; onClick: () => void; icon: React.ReactNode; label: string }) {
  return (
    <button className={`tab-button ${active ? "tab-button-active" : "tab-button-idle"}`} onClick={onClick}>
      {icon}
      {label}
    </button>
  );
}

function TopStatusCard({ title, value }: { title: string; value: string }) {
  return (
    <div className="rounded-3xl border border-white/10 bg-white/5 p-4">
      <p className="text-xs uppercase tracking-[0.25em] text-slate-500">{title}</p>
      <p className="mt-2 text-lg font-black text-white">{value}</p>
    </div>
  );
}

function buildLocalAiFeedback({
  targetGesture,
  actualGesture,
  isCorrect,
  confidence
}: {
  targetGesture: GestureType;
  actualGesture: GestureType;
  isCorrect: boolean;
  confidence: number;
}) {
  if (isCorrect) {
    const praises = [
      "本次动作完成得很稳，姿态保持和节奏控制都比较好，可以继续保持当前训练强度。",
      "这次识别结果很准确，说明你的动作边界已经比较清晰，接下来可以继续拉长保持时长。",
      "训练表现不错，动作收放比较到位，建议下一轮继续保持专注，把稳定性再提高一点。"
    ];

    return praises[Math.floor(Math.random() * praises.length)];
  }

  return `本轮目标动作是“${GESTURE_MAP[targetGesture]}”，系统识别为“${GESTURE_MAP[actualGesture]}”。建议先放慢动作切换速度，注意手指展开或握合的一致性。当前置信度约为 ${confidence}%。`;
}

function applyCustomGestureMatch(
  match: CustomGestureMatchMessage,
  handlers: {
    setTranslationRecords: React.Dispatch<React.SetStateAction<SignTranslationRecord[]>>;
    setDevices: React.Dispatch<React.SetStateAction<DeviceState>>;
    setTrainingPrompt: React.Dispatch<React.SetStateAction<string>>;
    setCustomGestureMessage: React.Dispatch<React.SetStateAction<string>>;
  }
) {
  if (match.item.category === "translation") {
    handlers.setTranslationRecords((previous) => {
      const nextRecord: SignTranslationRecord = {
        id: `${match.timestamp}-${match.item.id}`,
        gesture: "HELP",
        text: match.item.action,
        confidence: Math.max(60, 100 - Math.round(match.score)),
        voiceStatus: "待播报",
        time: new Date(match.timestamp).toLocaleTimeString("zh-CN", {
          hour12: false,
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit"
        })
      };

      return [nextRecord, ...previous].slice(0, 8);
    });
    handlers.setCustomGestureMessage(`已触发自定义翻译手势“${match.item.name}”。`);
    return;
  }

  if (match.item.category === "control") {
    const normalized = match.item.action.trim().toUpperCase();
    const controlMap: Record<string, { key: DeviceKey; value: boolean } | undefined> = {
      LIGHT_ON: { key: "light", value: true },
      LIGHT_OFF: { key: "light", value: false },
      FAN_ON: { key: "fan", value: true },
      FAN_OFF: { key: "fan", value: false },
      SOCKET_ON: { key: "socket", value: true },
      SOCKET_OFF: { key: "socket", value: false },
      SOS_ON: { key: "sos", value: true },
      SOS_OFF: { key: "sos", value: false }
    };

    const target = controlMap[normalized];
    if (target) {
      handlers.setDevices((previous) => ({
        ...previous,
        [target.key]: target.value
      }));
      handlers.setCustomGestureMessage(`已触发自定义控制手势“${match.item.name}”，动作：${match.item.action}。`);
    }
    return;
  }

  handlers.setTrainingPrompt(`命中训练手势：${match.item.name}，建议按“${match.item.action}”继续完成动作。`);
  handlers.setCustomGestureMessage(`已命中训练手势“${match.item.name}”。`);
}

export default App;
