import { AudioLines, Languages, MessageSquareText, Sparkles } from "lucide-react";
import React from "react";
import { SignTranslationRecord } from "../types";

interface Props {
  currentRecord: SignTranslationRecord;
  records: SignTranslationRecord[];
  onSimulate: () => void;
}

export const SignTranslationPanel: React.FC<Props> = ({ currentRecord, records, onSimulate }) => (
  <div className="grid grid-cols-1 gap-6 xl:grid-cols-[1.05fr_0.95fr]">
    <section className="panel">
      <h2 className="panel-title">
        <Languages className="text-cyan-300" size={22} />
        手语翻译
      </h2>

      <div className="mt-5 grid grid-cols-1 gap-4 md:grid-cols-2">
        <div className="rounded-[24px] border border-cyan-300/20 bg-cyan-300/10 p-5">
          <p className="text-xs uppercase tracking-[0.25em] text-cyan-100">Current Gesture</p>
          <p className="mt-3 text-4xl font-black text-white">{currentRecord.gesture}</p>
          <p className="mt-2 text-sm text-slate-300">当前识别手势</p>
        </div>

        <div className="rounded-[24px] border border-white/10 bg-slate-950/30 p-5">
          <p className="text-xs uppercase tracking-[0.25em] text-slate-500">Confidence</p>
          <p className="mt-3 text-4xl font-black text-amber-300">{currentRecord.confidence}%</p>
          <p className="mt-2 text-sm text-slate-300">本次识别置信度</p>
        </div>
      </div>

      <div className="mt-4 rounded-[24px] border border-white/10 bg-slate-950/30 p-5">
        <div className="flex items-center gap-2 text-sm font-bold text-slate-300">
          <MessageSquareText size={16} className="text-sky-300" />
          翻译文本
        </div>
        <p className="mt-3 text-3xl font-black text-white">{currentRecord.text}</p>
      </div>

      <div className="mt-4 flex flex-col gap-4 rounded-[24px] border border-white/10 bg-slate-950/30 p-5 md:flex-row md:items-center md:justify-between">
        <div>
          <div className="flex items-center gap-2 text-sm font-bold text-slate-300">
            <AudioLines size={16} className="text-fuchsia-300" />
            语音播报状态
          </div>
          <p className="mt-2 text-lg font-black text-white">{currentRecord.voiceStatus}</p>
        </div>

        <button className="primary-button" onClick={onSimulate}>
          <Sparkles size={18} />
          模拟下一条识别
        </button>
      </div>
    </section>

    <section className="panel">
      <h2 className="panel-title">
        <MessageSquareText className="text-fuchsia-300" size={22} />
        最近识别记录
      </h2>

      <div className="custom-scrollbar mt-5 max-h-[420px] space-y-3 overflow-y-auto pr-1">
        {records.map((record) => (
          <div key={record.id} className="rounded-2xl border border-white/10 bg-slate-950/25 p-4">
            <div className="flex items-start justify-between gap-3">
              <div>
                <p className="text-lg font-black text-white">{record.gesture}</p>
                <p className="mt-1 text-sm text-slate-300">{record.text}</p>
              </div>
              <span className="rounded-full bg-cyan-300/15 px-3 py-1 text-xs font-bold text-cyan-200">
                {record.confidence}%
              </span>
            </div>
            <div className="mt-3 flex items-center justify-between text-xs text-slate-400">
              <span>{record.voiceStatus}</span>
              <span>{record.time}</span>
            </div>
          </div>
        ))}
      </div>
    </section>
  </div>
);
