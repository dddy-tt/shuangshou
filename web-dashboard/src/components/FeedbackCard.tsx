import { BrainCircuit, Sparkles } from "lucide-react";
import React from "react";

interface Props {
  feedback: string;
  isLoading: boolean;
}

export const FeedbackCard: React.FC<Props> = ({ feedback, isLoading }) => (
  <section className="panel min-h-[320px]">
    <div className="flex items-start justify-between gap-3 border-b border-white/10 pb-3">
      <h2 className="panel-title border-b-0 pb-0">
        <BrainCircuit className="text-fuchsia-300" size={22} />
        AI 康复训练辅助反馈
      </h2>
      <span className="rounded-full border border-fuchsia-300/20 bg-fuchsia-300/10 px-3 py-1 text-xs font-bold text-fuchsia-100">
        AI 康复训练建议
      </span>
    </div>

    <div className="mt-5 flex min-h-[220px] items-center rounded-[24px] border border-fuchsia-300/10 bg-gradient-to-br from-fuchsia-300/10 via-transparent to-sky-300/10 p-5">
      {isLoading ? (
        <div className="mx-auto text-center">
          <div className="mx-auto h-12 w-12 rounded-full border-4 border-fuchsia-200/20 border-t-fuchsia-300 animate-spin" />
          <p className="mt-4 font-bold text-fuchsia-100">正在生成模拟反馈...</p>
        </div>
      ) : feedback ? (
        <div className="relative">
          <Sparkles className="absolute -top-2 -left-1 text-fuchsia-200/40" size={28} />
          <p className="pl-6 text-base leading-8 text-slate-100">{feedback}</p>
        </div>
      ) : (
        <div className="w-full text-center">
          <p className="text-lg font-bold text-white">等待训练结果</p>
          <p className="mt-2 text-sm text-slate-400">完成一次训练采集后，这里会显示前端模拟的训练反馈。</p>
        </div>
      )}
    </div>
  </section>
);
