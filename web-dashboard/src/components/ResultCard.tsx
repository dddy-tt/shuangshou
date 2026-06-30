import { CheckCircle2, Clock3, Fingerprint, Percent, XCircle } from "lucide-react";
import React from "react";
import { GESTURE_MAP, WSMessage } from "../types";

interface Props {
  data: WSMessage | null;
  isCorrect: boolean;
}

export const ResultCard: React.FC<Props> = ({ data, isCorrect }) => {
  if (!data) {
    return (
      <section className="panel flex min-h-[280px] flex-col items-center justify-center text-center">
        <Fingerprint size={42} className="text-slate-500" />
        <p className="mt-4 text-lg font-bold text-white">暂无识别结果</p>
        <p className="mt-2 text-sm text-slate-400">点击开始训练采集后，这里会展示一次模拟识别结果。</p>
      </section>
    );
  }

  return (
    <section
      className={`panel min-h-[280px] border-l-4 ${
        isCorrect ? "border-l-emerald-400" : "border-l-rose-400"
      }`}
    >
      <h2 className="panel-title">
        <Fingerprint className={isCorrect ? "text-emerald-300" : "text-rose-300"} size={22} />
        实时识别结果
      </h2>

      <div className="mt-5 flex items-center justify-between gap-4">
        <div className="flex items-center gap-4">
          <div
            className={`rounded-3xl p-4 ${
              isCorrect ? "bg-emerald-400/15 text-emerald-300" : "bg-rose-400/15 text-rose-300"
            }`}
          >
            {isCorrect ? <CheckCircle2 size={34} /> : <XCircle size={34} />}
          </div>
          <div>
            <p className="text-xs uppercase tracking-[0.3em] text-slate-500">Recognition</p>
            <p className="mt-1 text-2xl font-black text-white">{GECTURE_MAP_FIX(data.gesture)}</p>
          </div>
        </div>
        <div
          className={`rounded-full px-4 py-2 text-sm font-black ${
            isCorrect ? "bg-emerald-400 text-slate-950" : "bg-rose-400 text-white"
          }`}
        >
          {isCorrect ? "正确" : "错误"}
        </div>
      </div>

      <div className="mt-5 grid grid-cols-1 gap-3 sm:grid-cols-2">
        <div className="metric-tile">
          <div className="flex items-center gap-3">
            <Percent className="text-sky-300" size={18} />
            <div>
              <p className="text-xs uppercase tracking-[0.25em] text-slate-500">Confidence</p>
              <p className="mt-1 text-xl font-black text-white">{data.confidence}%</p>
            </div>
          </div>
        </div>
        <div className="metric-tile">
          <div className="flex items-center gap-3">
            <Clock3 className="text-amber-300" size={18} />
            <div>
              <p className="text-xs uppercase tracking-[0.25em] text-slate-500">Hold Time</p>
              <p className="mt-1 text-xl font-black text-white">{data.holdMs} ms</p>
            </div>
          </div>
        </div>
      </div>
    </section>
  );
};

function GECTURE_MAP_FIX(gesture: WSMessage["gesture"]) {
  return GESTURE_MAP[gesture];
}
