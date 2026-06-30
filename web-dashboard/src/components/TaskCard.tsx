import { Play, Target } from "lucide-react";
import React, { useEffect, useState } from "react";
import { GESTURE_MAP, GestureType } from "../types";

interface Props {
  target: GestureType;
  setTarget: (gesture: GestureType) => void;
  onStart: () => void;
}

export const TaskCard: React.FC<Props> = ({ target, setTarget, onStart }) => {
  const [countdown, setCountdown] = useState<number | null>(null);

  useEffect(() => {
    if (countdown === null) {
      return;
    }

    if (countdown === 0) {
      onStart();
      setCountdown(null);
      return;
    }

    const timer = window.setTimeout(() => setCountdown((value) => (value === null ? null : value - 1)), 1000);
    return () => window.clearTimeout(timer);
  }, [countdown, onStart]);

  return (
    <section className="panel relative overflow-hidden">
      <div className="noise-overlay absolute inset-0" />
      <div className="relative">
        <h2 className="panel-title">
          <Target className="text-amber-300" size={22} />
          当前训练任务
        </h2>

        <div className="mt-5 space-y-4">
          <div>
            <label className="mb-2 block text-sm font-bold text-slate-300">选择目标动作</label>
            <select
              value={target}
              onChange={(event) => setTarget(event.target.value as GestureType)}
              disabled={countdown !== null}
              className="w-full rounded-2xl border border-white/10 bg-slate-950/50 px-4 py-3 text-white outline-none transition focus:border-sky-300"
            >
              {(Object.keys(GESTURE_MAP) as GestureType[]).map((gesture) => (
                <option key={gesture} value={gesture} className="text-slate-900">
                  {GESTURE_MAP[gesture]}
                </option>
              ))}
            </select>
          </div>

          <div className="rounded-[24px] border border-sky-300/20 bg-sky-300/10 p-5">
            <p className="text-sm font-medium text-sky-100">本轮目标</p>
            <p className="mt-2 text-3xl font-black tracking-wide text-white">{GESTURE_MAP[target]}</p>
          </div>

          <div className="rounded-2xl border border-white/10 bg-slate-950/30 p-4 text-center">
            {countdown !== null ? (
              <>
                <p className="text-sm text-slate-300">请准备动作</p>
                <p className="mt-2 text-5xl font-black text-amber-300">{countdown}</p>
              </>
            ) : (
              <>
                <p className="text-sm text-slate-300">准备好后开始训练采集</p>
                <p className="mt-2 text-xs uppercase tracking-[0.3em] text-slate-500">Mock Training Capture</p>
              </>
            )}
          </div>

          <button className="primary-button w-full" disabled={countdown !== null} onClick={() => setCountdown(3)}>
            <Play size={18} />
            {countdown !== null ? "训练采集中..." : "开始训练采集"}
          </button>
        </div>
      </div>
    </section>
  );
};
