import { Gauge, Move3d } from "lucide-react";
import React from "react";

interface FlexSensorState {
  left: number | null;
  right: number | null;
  leftFingers: number[] | null;
  rightFingers: number[] | null;
  normalizedLeft: number | null;
  normalizedRight: number | null;
  normalizedLeftFingers: Array<number | null> | null;
  normalizedRightFingers: Array<number | null> | null;
}

interface ImuSensorState {
  roll: number;
  pitch: number;
  yaw: number;
}

interface Props {
  flex: FlexSensorState;
  imu: ImuSensorState;
  onZero: () => void;
  onFull: () => void;
  calibrationBusy: boolean;
  calibrationMessage: string;
}

function formatNumber(value: number | null, fractionDigits = 0) {
  if (typeof value !== "number") {
    return "--";
  }

  return value.toFixed(fractionDigits);
}

const fingerLabels = ["拇指", "食指", "中指", "无名指", "小指"];

export const SensorMonitor: React.FC<Props> = ({
  flex,
  imu,
  onZero,
  onFull,
  calibrationBusy,
  calibrationMessage
}) => (
  <section className="panel">
    <div className="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
      <div>
        <h2 className="panel-title">
          <Gauge className="text-cyan-300" size={22} />
          Sensor Monitor
        </h2>
        <p className="mt-2 text-sm text-slate-400">
          展示 FLEX 原始值、归一化值和 IMU 姿态数据，支持简单校准。
        </p>
      </div>

      <div className="flex flex-wrap gap-3">
        <button className="tab-button tab-button-idle" disabled={calibrationBusy} onClick={onZero}>
          置零
        </button>
        <button className="tab-button tab-button-active" disabled={calibrationBusy} onClick={onFull}>
          握满
        </button>
      </div>
    </div>

    <div className="mt-5 grid grid-cols-1 gap-4 xl:grid-cols-2">
      <div className="rounded-[24px] border border-cyan-300/20 bg-cyan-300/10 p-5">
        <div className="flex items-center gap-2 text-sm font-bold text-cyan-100">
          <Gauge size={16} />
          FLEX
        </div>
        <div className="mt-4 grid grid-cols-2 gap-4 text-sm text-slate-200">
          <div>
            <p className="text-slate-400">Left raw</p>
            <p className="mt-2 text-3xl font-black text-white">{formatNumber(flex.left)}</p>
          </div>
          <div>
            <p className="text-slate-400">Right raw</p>
            <p className="mt-2 text-3xl font-black text-white">{formatNumber(flex.right)}</p>
          </div>
          <div>
            <p className="text-slate-400">Left normalized</p>
            <p className="mt-2 text-2xl font-black text-white">{formatNumber(flex.normalizedLeft, 2)}</p>
          </div>
          <div>
            <p className="text-slate-400">Right normalized</p>
            <p className="mt-2 text-2xl font-black text-white">{formatNumber(flex.normalizedRight, 2)}</p>
          </div>
        </div>

        {Array.isArray(flex.leftFingers) && Array.isArray(flex.rightFingers) ? (
          <div className="mt-5 grid grid-cols-1 gap-4 xl:grid-cols-2">
            <div className="rounded-2xl border border-white/10 bg-slate-950/20 p-4">
              <p className="text-sm font-bold text-white">左手五指</p>
              <div className="mt-3 space-y-2">
                {fingerLabels.map((label, index) => (
                  <div key={`left-${label}`} className="flex items-center justify-between rounded-xl bg-white/5 px-3 py-2 text-sm">
                    <span className="text-slate-300">{label}</span>
                    <span className="font-bold text-white">
                      {formatNumber(flex.leftFingers?.[index] ?? null)} / {formatNumber(flex.normalizedLeftFingers?.[index] ?? null, 2)}
                    </span>
                  </div>
                ))}
              </div>
            </div>

            <div className="rounded-2xl border border-white/10 bg-slate-950/20 p-4">
              <p className="text-sm font-bold text-white">右手五指</p>
              <div className="mt-3 space-y-2">
                {fingerLabels.map((label, index) => (
                  <div key={`right-${label}`} className="flex items-center justify-between rounded-xl bg-white/5 px-3 py-2 text-sm">
                    <span className="text-slate-300">{label}</span>
                    <span className="font-bold text-white">
                      {formatNumber(flex.rightFingers?.[index] ?? null)} / {formatNumber(flex.normalizedRightFingers?.[index] ?? null, 2)}
                    </span>
                  </div>
                ))}
              </div>
            </div>
          </div>
        ) : null}
      </div>

      <div className="rounded-[24px] border border-fuchsia-300/20 bg-fuchsia-300/10 p-5">
        <div className="flex items-center gap-2 text-sm font-bold text-fuchsia-100">
          <Move3d size={16} />
          IMU
        </div>
        <div className="mt-4 grid grid-cols-3 gap-4 text-sm text-slate-200">
          <div>
            <p className="text-slate-400">roll</p>
            <p className="mt-2 text-3xl font-black text-white">{formatNumber(imu.roll)}</p>
          </div>
          <div>
            <p className="text-slate-400">pitch</p>
            <p className="mt-2 text-3xl font-black text-white">{formatNumber(imu.pitch)}</p>
          </div>
          <div>
            <p className="text-slate-400">yaw</p>
            <p className="mt-2 text-3xl font-black text-white">{formatNumber(imu.yaw)}</p>
          </div>
        </div>
      </div>
    </div>

    <div className="mt-4 rounded-2xl border border-white/10 bg-slate-950/30 p-4 text-sm text-slate-300">
      {calibrationMessage}
    </div>
  </section>
);
