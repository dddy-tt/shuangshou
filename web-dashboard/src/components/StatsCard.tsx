import { BarChart3, CheckCircle2, History, Target, Trophy } from "lucide-react";
import React from "react";
import { GESTURE_MAP, TrainingStats } from "../types";

interface Props {
  stats: TrainingStats;
}

export const StatsCard: React.FC<Props> = ({ stats }) => {
  const accuracy = stats.total === 0 ? 0 : Math.round((stats.correct / stats.total) * 100);

  return (
    <section className="panel">
      <h2 className="panel-title">
        <BarChart3 className="text-emerald-300" size={22} />
        训练统计
      </h2>

      <div className="mt-5 grid grid-cols-2 gap-3 xl:grid-cols-4">
        <div className="metric-tile">
          <p className="text-xs uppercase tracking-[0.2em] text-slate-500">训练次数</p>
          <p className="mt-2 text-3xl font-black text-white">{stats.total}</p>
        </div>
        <div className="metric-tile">
          <p className="text-xs uppercase tracking-[0.2em] text-slate-500">正确次数</p>
          <p className="mt-2 text-3xl font-black text-emerald-300">{stats.correct}</p>
        </div>
        <div className="metric-tile">
          <div className="flex items-center gap-2 text-slate-500">
            <Target size={14} />
            <p className="text-xs uppercase tracking-[0.2em]">正确率</p>
          </div>
          <p className="mt-2 text-3xl font-black text-sky-300">{accuracy}%</p>
        </div>
        <div className="metric-tile">
          <div className="flex items-center gap-2 text-slate-500">
            <Trophy size={14} />
            <p className="text-xs uppercase tracking-[0.2em]">连续正确次数</p>
          </div>
          <p className="mt-2 text-3xl font-black text-amber-300">{stats.streak}</p>
        </div>
      </div>

      <div className="mt-6">
        <div className="mb-3 flex items-center gap-2 text-sm font-bold text-slate-200">
          <History size={16} />
          最近 10 条记录
        </div>

        {stats.history.length === 0 ? (
          <div className="rounded-2xl border border-dashed border-white/10 bg-slate-950/20 px-4 py-8 text-center text-sm text-slate-400">
            还没有训练记录，先完成一次训练采集。
          </div>
        ) : (
          <div className="custom-scrollbar max-h-[280px] space-y-2 overflow-y-auto pr-1">
            {stats.history.map((record) => (
              <div
                key={record.id}
                className="rounded-2xl border border-white/10 bg-slate-950/25 px-4 py-3 transition hover:border-white/20"
              >
                <div className="flex items-start justify-between gap-3">
                  <div>
                    <p className="text-sm font-bold text-white">{GESTURE_MAP[record.target]}</p>
                    <p className="mt-1 text-xs text-slate-400">
                      识别结果：{GESTURE_MAP[record.actual]} · 置信度 {record.confidence}%
                    </p>
                  </div>
                  <div className="text-right">
                    <span
                      className={`inline-flex items-center gap-1 rounded-full px-3 py-1 text-xs font-bold ${
                        record.isCorrect ? "bg-emerald-400/15 text-emerald-300" : "bg-rose-400/15 text-rose-300"
                      }`}
                    >
                      <CheckCircle2 size={12} />
                      {record.isCorrect ? "正确" : "错误"}
                    </span>
                    <p className="mt-2 text-xs text-slate-500">{record.time}</p>
                  </div>
                </div>
              </div>
            ))}
          </div>
        )}
      </div>
    </section>
  );
};
