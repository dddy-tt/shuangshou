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
  CareMonitoringState,
  DashboardTab,
  GestureType,
  SIGN_TRANSLATION_MAP,
  SignGestureType,
  SignTranslationRecord,
  TrainingRecord,
  TrainingStats
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
  const { isConnected, lastMessage, simulateWebSocketMessage } = useWebSocket();
  const [activeTab, setActiveTab] = useState<DashboardTab>("translation");
  const [targetGesture, setTargetGesture] = useState<GestureType>("RIGHT_OPEN");
  const [aiFeedback, setAiFeedback] = useState("");
  const [isAiLoading, setIsAiLoading] = useState(false);
  const [stats, setStats] = useState<TrainingStats>({ total: 0, correct: 0, streak: 0, history: [] });
  const [translationRecords, setTranslationRecords] = useState<SignTranslationRecord[]>(() => [createSignRecord(0)]);
  const [careIndex, setCareIndex] = useState(0);

  useEffect(() => {
    if (!lastMessage) {
      return;
    }

    const isCorrect = lastMessage.gesture === targetGesture;

    setStats((previous) => {
      const newRecord: TrainingRecord = {
        id: `${lastMessage.timestamp}`,
        target: targetGesture,
        actual: lastMessage.gesture,
        isCorrect,
        confidence: lastMessage.confidence,
        time: new Date(lastMessage.timestamp).toLocaleTimeString("zh-CN", {
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
      actualGesture: lastMessage.gesture,
      isCorrect,
      confidence: lastMessage.confidence
    }).then((feedback) => {
      setAiFeedback(feedback);
      setIsAiLoading(false);
    });
  }, [lastMessage, targetGesture]);

  useEffect(() => {
    const timer = window.setInterval(() => {
      setCareIndex((previous) => (previous + 1) % careScenarios.length);
    }, 4500);

    return () => window.clearInterval(timer);
  }, []);

  const currentTranslation = translationRecords[0];
  const currentCare = careScenarios[careIndex];

  const handleSimulateTranslation = () => {
    setTranslationRecords((previous) => {
      const nextRecord = createSignRecord(previous.length);
      return [nextRecord, ...previous].slice(0, 8);
    });
  };

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
          <StatusCard wsConnected={isConnected} />
        </div>
      );
    }

    return (
      <div className="grid grid-cols-1 gap-6 xl:grid-cols-[1.05fr_0.95fr]">
        <div className="space-y-6">
          <TaskCard target={targetGesture} setTarget={setTargetGesture} onStart={() => simulateWebSocketMessage(targetGesture)} />
          <ResultCard data={lastMessage} isCorrect={lastMessage?.gesture === targetGesture} />
        </div>

        <div className="space-y-6">
          <StatusCard wsConnected={isConnected} />
          <FeedbackCard feedback={aiFeedback} isLoading={isAiLoading} />
        </div>

        <div className="xl:col-span-2">
          <StatsCard stats={stats} />
        </div>
      </div>
    );
  }, [activeTab, aiFeedback, currentCare, currentTranslation, isAiLoading, isConnected, lastMessage, simulateWebSocketMessage, stats, targetGesture, translationRecords]);

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
                将手语翻译、AI 康复训练、护理监测和家电远控整合到同一套前端 Mock 看板中，
                用于后续串口、MQTT 和 AI 服务接入前的联调预演。
              </p>
            </div>

            <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
              <TopStatusCard title="模式" value="无硬件 Mock" />
              <TopStatusCard title="端口" value="localhost:3000" />
              <TopStatusCard title="状态" value="前端演示就绪" />
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
