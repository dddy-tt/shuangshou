import { Activity, Cpu, Radio, Sparkles } from "lucide-react";
import type { LucideIcon } from "lucide-react";
import React from "react";
import { AiFeedbackSource, BridgeStatus } from "../types";

interface Props {
  bridgeStatus: BridgeStatus;
  aiSource: AiFeedbackSource;
  lastSystemMessage?: string;
}

interface StatusItemProps {
  icon: LucideIcon;
  label: string;
  active: boolean;
  helper: string;
}

const StatusItem: React.FC<StatusItemProps> = ({ icon: Icon, label, active, helper }) => (
  <div className="rounded-2xl border border-white/10 bg-slate-950/30 p-4">
    <div className="flex items-center justify-between gap-3">
      <div className="flex items-center gap-3">
        <div className={`rounded-2xl p-3 ${active ? "bg-sky-400/20 text-sky-200" : "bg-white/10 text-slate-500"}`}>
          <Icon size={20} />
        </div>
        <div>
          <p className="font-bold text-white">{label}</p>
          <p className="text-xs text-slate-400">{helper}</p>
        </div>
      </div>
      <div className="flex items-center gap-2">
        <span className={`h-2.5 w-2.5 rounded-full ${active ? "bg-emerald-400 animate-pulse" : "bg-rose-400"}`} />
        <span className="text-xs font-bold text-slate-300">{active ? "在线" : "回退"}</span>
      </div>
    </div>
  </div>
);

export const StatusCard: React.FC<Props> = ({ bridgeStatus, aiSource, lastSystemMessage }) => {
  const bridgeOnline = bridgeStatus === "online";

  return (
    <section className="panel">
      <h2 className="panel-title">
        <Activity className="text-sky-300" size={22} />
        系统状态
      </h2>

      <div className="mt-5 space-y-3">
        <StatusItem
          icon={Cpu}
          label="Bridge WebSocket"
          active={bridgeOnline}
          helper={
            bridgeOnline
              ? "Bridge 在线，正在接收 gesture / sign / care 数据"
              : bridgeStatus === "connecting"
                ? "正在尝试连接 ws://localhost:8765"
                : "Bridge 离线，已回退到前端 Mock"
          }
        />
        <StatusItem
          icon={Radio}
          label="前端 Mock 回退"
          active={!bridgeOnline}
          helper={bridgeOnline ? "当前由 bridge 数据驱动页面刷新" : "Bridge 不在线时训练、翻译、护理模块继续可用"}
        />
        <StatusItem
          icon={Sparkles}
          label="AI 反馈接口"
          active={aiSource === "bridge"}
          helper={aiSource === "bridge" ? "当前优先使用 bridge /api/ai-feedback" : "bridge 请求失败时已回退本地模拟反馈"}
        />
      </div>

      <div className="mt-4 rounded-2xl border border-amber-300/20 bg-amber-300/10 p-4 text-sm text-amber-100">
        {lastSystemMessage || "当前版本支持 bridge 优先接入，bridge 不在线时自动回退 Mock。"}
      </div>
    </section>
  );
};
