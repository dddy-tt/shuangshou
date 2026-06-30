import { AlertTriangle, BellRing, HeartPulse, ShieldAlert, Waves } from "lucide-react";
import React from "react";
import { CareMonitoringState } from "../types";

interface Props {
  monitoring: CareMonitoringState;
}

export const CareMonitoringPanel: React.FC<Props> = ({ monitoring }) => (
  <div className="grid grid-cols-1 gap-6 xl:grid-cols-[1.05fr_0.95fr]">
    <section className="panel">
      <h2 className="panel-title">
        <HeartPulse className="text-rose-300" size={22} />
        护理监测
      </h2>

      <div className="mt-5 grid grid-cols-1 gap-4 md:grid-cols-2">
        <div className="rounded-[24px] border border-rose-300/20 bg-rose-300/10 p-5">
          <div className="flex items-center gap-2 text-sm font-bold text-rose-100">
            <HeartPulse size={16} />
            HR 心率
          </div>
          <p className="mt-3 text-4xl font-black text-white">{monitoring.hr}</p>
          <p className="mt-2 text-sm text-slate-300">当前 Mock 状态值</p>
        </div>

        <div className="rounded-[24px] border border-sky-300/20 bg-sky-300/10 p-5">
          <div className="flex items-center gap-2 text-sm font-bold text-sky-100">
            <Waves size={16} />
            SpO2 血氧
          </div>
          <p className="mt-3 text-4xl font-black text-white">{monitoring.spo2}%</p>
          <p className="mt-2 text-sm text-slate-300">当前 Mock 状态值</p>
        </div>
      </div>

      <div className="mt-4 grid grid-cols-1 gap-4 md:grid-cols-2">
        <div className="rounded-[24px] border border-white/10 bg-slate-950/30 p-5">
          <div className="flex items-center gap-2 text-sm font-bold text-slate-300">
            <AlertTriangle size={16} className={monitoring.fallDetected ? "text-amber-300" : "text-emerald-300"} />
            跌倒检测状态
          </div>
          <p className="mt-3 text-2xl font-black text-white">{monitoring.fallDetected ? "检测到异常姿态" : "状态平稳"}</p>
          <p className="mt-2 text-sm text-slate-400">用于演示状态提示，不做诊断判断。</p>
        </div>

        <div className="rounded-[24px] border border-white/10 bg-slate-950/30 p-5">
          <div className="flex items-center gap-2 text-sm font-bold text-slate-300">
            <ShieldAlert size={16} className={monitoring.sosActive ? "text-rose-300" : "text-slate-400"} />
            SOS 状态
          </div>
          <p className="mt-3 text-2xl font-black text-white">{monitoring.sosActive ? "SOS 已触发" : "SOS 未触发"}</p>
          <p className="mt-2 text-sm text-slate-400">当前为前端 Mock 联调展示。</p>
        </div>
      </div>
    </section>

    <section className="panel">
      <h2 className="panel-title">
        <BellRing className="text-amber-300" size={22} />
        护理辅助提醒
      </h2>

      <div className="mt-5 rounded-[24px] border border-amber-300/20 bg-amber-300/10 p-6">
        <p className="text-xs uppercase tracking-[0.25em] text-amber-100">Status Prompt</p>
        <p className="mt-4 text-2xl font-black text-white">{monitoring.reminder}</p>
      </div>

      <div className="mt-4 rounded-[24px] border border-white/10 bg-slate-950/30 p-5 text-sm leading-7 text-slate-300">
        本模块当前展示 HR、SpO2、跌倒检测状态、SOS 状态和护理辅助提醒的 Mock 数据，
        后续可接入 STM32 串口上报与桥接服务。
      </div>
    </section>
  </div>
);
