import { Activity, ChevronRight, HeartPulse, Home, Languages, Radar } from "lucide-react";
import React, { useEffect, useMemo, useState } from "react";
import { CareMonitoringPanel } from "./components/CareMonitoringPanel";
import { FeedbackCard } from "./components/FeedbackCard";
import { IotCard } from "./components/IotCard";
import { ResultCard } from "./components/ResultCard";
import { SignTranslationPanel } from "./components/SignTranslationPanel";
import { StatsCard } from "./components/StatsCard";
import { StatusCard } from "./components/StatusCard";
import { TaskCard } from "./components/TaskCard";
import { useWebSocket } from "./hooks/useWebSocket";
import { fetchAiFeedback } from "./services/aiService";
import {
  AiFeedbackSource,
  CareMonitoringState,
  DashboardTab,
  GestureType,
  SIGN_TRANSLATION_MAP,
  SignGestureType,
  SignTranslationRecord,
  TrainingRecord,
  TrainingStats,
  WSMessage
} from "./types";

const signGestureOrder: SignGestureType[] = ["HELP", "DRINK", "PAIN"];

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

function App() {
  const {
    bridgeStatus,
    lastCareMessage,
    lastGestureMessage,
    lastSignMessage,
    lastSystemMessage,
    simulateWebSocketMessage
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

    setIsAiLoading(true);
    fetchAiFeedback({
      targetGesture,
      actualGesture: lastGestureMessage.gesture,
      isCorrect,
      confidence: lastGestureMessage.confidence
    }).then((result) => {
      setAiFeedback(result.feedback);
      setAiSource(result.source);
      setIsAiLoading(false);
    });

    setWaitingBridgeGesture(false);
  }, [bridgeOnline, lastGestureMessage, targetGesture, waitingBridgeGesture]);

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
    if (!bridgeOnline) {
      setWaitingBridgeGesture(false);
    }
  }, [bridgeOnline]);

  const currentTranslation = translationRecords[0];
  const currentCare = careState;

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

  const modeValue =
    bridgeStatus === "online"
      ? "Bridge 在线"
      : bridgeStatus === "connecting"
        ? "尝试连接 Bridge"
        : "Bridge 离线 / Mock";
  const aiValue = aiSource === "bridge" ? "AI 接口在线" : "AI 本地回退";
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
          <IotCard />
          <StatusCard bridgeStatus={bridgeStatus} aiSource={aiSource} lastSystemMessage={bridgeNote} />
        </div>
      );
    }

    return (
      <div className="grid grid-cols-1 gap-6 xl:grid-cols-[1.05fr_0.95fr]">
        <div className="space-y-6">
          <TaskCard target={targetGesture} setTarget={setTargetGesture} onStart={handleStartTraining} />
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
  }, [activeTab, aiFeedback, aiSource, bridgeNote, bridgeStatus, currentCare, currentTranslation, isAiLoading, stats, targetGesture, trainingResult, translationRecords]);

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

export default App;
