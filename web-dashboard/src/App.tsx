import { Activity, ChevronRight, Home, Radar } from "lucide-react";
import React, { useEffect, useMemo, useState } from "react";
import { FeedbackCard } from "./components/FeedbackCard";
import { IotCard } from "./components/IotCard";
import { ResultCard } from "./components/ResultCard";
import { StatsCard } from "./components/StatsCard";
import { StatusCard } from "./components/StatusCard";
import { TaskCard } from "./components/TaskCard";
import { useWebSocket } from "./hooks/useWebSocket";
import { fetchAiFeedback } from "./services/aiService";
import { DashboardTab, GestureType, TrainingRecord, TrainingStats } from "./types";

function App() {
  const { isConnected, lastMessage, simulateWebSocketMessage } = useWebSocket();
  const [activeTab, setActiveTab] = useState<DashboardTab>("training");
  const [targetGesture, setTargetGesture] = useState<GestureType>("RIGHT_OPEN");
  const [aiFeedback, setAiFeedback] = useState("");
  const [isAiLoading, setIsAiLoading] = useState(false);
  const [stats, setStats] = useState<TrainingStats>({
    total: 0,
    correct: 0,
    streak: 0,
    history: []
  });

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

  const tabContent = useMemo(() => {
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
  }, [activeTab, aiFeedback, isAiLoading, isConnected, lastMessage, simulateWebSocketMessage, stats, targetGesture]);

  return (
    <div className="dashboard-shell">
      <div className="mx-auto max-w-7xl px-4 py-8 sm:px-6 lg:px-8">
        <header className="relative overflow-hidden rounded-[36px] border border-white/10 bg-slate-950/40 p-6 shadow-panel">
          <div className="noise-overlay absolute inset-0" />
          <div className="relative flex flex-col gap-6 lg:flex-row lg:items-end lg:justify-between">
            <div className="max-w-3xl">
              <div className="inline-flex items-center gap-2 rounded-full border border-sky-300/20 bg-sky-300/10 px-4 py-2 text-xs font-bold uppercase tracking-[0.35em] text-sky-100">
                <Radar size={14} />
                Mock Training Dashboard
              </div>
              <h1
                className="mt-4 text-4xl font-black leading-tight text-white sm:text-5xl"
                style={{ fontFamily: '"ZCOOL XiaoWei", serif' }}
              >
                双手手语手套训练看板
              </h1>
              <p className="mt-4 max-w-2xl text-sm leading-7 text-slate-300 sm:text-base">
                围绕“目标动作选择、训练采集、识别结果、AI 康复训练辅助反馈、历史追踪”构建的纯前端 Mock 演示页，
                用于后续串口、MQTT 和 AI 服务接入前的联调预演。
              </p>
            </div>

            <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
              <div className="rounded-3xl border border-white/10 bg-white/5 p-4">
                <p className="text-xs uppercase tracking-[0.25em] text-slate-500">模式</p>
                <p className="mt-2 text-lg font-black text-white">无硬件 Mock</p>
              </div>
              <div className="rounded-3xl border border-white/10 bg-white/5 p-4">
                <p className="text-xs uppercase tracking-[0.25em] text-slate-500">端口</p>
                <p className="mt-2 text-lg font-black text-white">localhost:3000</p>
              </div>
              <div className="rounded-3xl border border-white/10 bg-white/5 p-4">
                <p className="text-xs uppercase tracking-[0.25em] text-slate-500">状态</p>
                <p className="mt-2 text-lg font-black text-white">{isConnected ? "训练链路已就绪" : "连接中"}</p>
              </div>
            </div>
          </div>
        </header>

        <div className="mt-6 flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
          <div className="flex flex-wrap items-center gap-3">
            <button
              className={`tab-button ${activeTab === "training" ? "tab-button-active" : "tab-button-idle"}`}
              onClick={() => setActiveTab("training")}
            >
              <Activity size={16} className="mr-2 inline-block" />
              训练看板
            </button>
            <button
              className={`tab-button ${activeTab === "iot" ? "tab-button-active" : "tab-button-idle"}`}
              onClick={() => setActiveTab("iot")}
            >
              <Home size={16} className="mr-2 inline-block" />
              家电远控
            </button>
          </div>

          <div className="flex items-center gap-2 text-sm text-slate-400">
            <span>训练链路</span>
            <ChevronRight size={16} />
            <span className="font-bold text-slate-200">{activeTab === "training" ? "训练看板" : "家电远控"}</span>
          </div>
        </div>

        <main className="mt-6">{tabContent}</main>
      </div>
    </div>
  );
}

export default App;
