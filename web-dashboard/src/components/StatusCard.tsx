import { Activity, Cpu, Radio, Sparkles } from "lucide-react";
import type { LucideIcon } from "lucide-react";
import React from "react";

interface Props {
  wsConnected: boolean;
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
        <span className="text-xs font-bold text-slate-300">{active ? "在线" : "离线"}</span>
      </div>
    </div>
  </div>
);

export const StatusCard: React.FC<Props> = ({ wsConnected }) => (
  <section className="panel">
    <h2 className="panel-title">
      <Activity className="text-sky-300" size={22} />
      系统状态
    </h2>

    <div className="mt-5 space-y-3">
      <StatusItem icon={Cpu} label="手套训练链路" active={wsConnected} helper="纯前端 Mock 连接状态" />
      <StatusItem icon={Radio} label="串口 / MQTT 桥接预留" active helper="当前仅做前端预演，不接真实链路" />
      <StatusItem icon={Sparkles} label="AI 反馈引擎" active helper="本地模拟反馈逻辑已启用" />
    </div>

    <div className="mt-4 rounded-2xl border border-amber-300/20 bg-amber-300/10 p-4 text-sm text-amber-100">
      当前版本：无硬件 Mock 演示版
    </div>
  </section>
);
