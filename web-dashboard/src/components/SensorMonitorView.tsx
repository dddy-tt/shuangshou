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
  onImuZero: () => void;
  calibrationBusy: boolean;
  calibrationMessage: string;
}

const fingerLabels = ["Thumb", "Index", "Middle", "Ring", "Little"];

function formatNumber(value: number | null, fractionDigits = 0) {
  if (typeof value !== "number") {
    return "--";
  }

  return value.toFixed(fractionDigits);
}

function buildFingerRows(sideLabel: string, rawValues: number[] | null, normalizedValues: Array<number | null> | null) {
  return fingerLabels.map((label, index) => ({
    id: `${sideLabel}-${label}`,
    title: `${sideLabel} ${label}`,
    raw: rawValues?.[index] ?? null,
    normalized: normalizedValues?.[index] ?? null
  }));
}

export const SensorMonitorView: React.FC<Props> = ({
  flex,
  imu,
  onZero,
  onFull,
  onImuZero,
  calibrationBusy,
  calibrationMessage
}) => {
  const fingerRows = [
    ...buildFingerRows("Left", flex.leftFingers, flex.normalizedLeftFingers),
    ...buildFingerRows("Right", flex.rightFingers, flex.normalizedRightFingers)
  ];

  return (
    <section className="panel">
      <div className="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
        <div>
          <h2 className="panel-title">
            <Gauge className="text-cyan-300" size={22} />
            Sensor Monitor
          </h2>
          <p className="mt-2 text-sm text-slate-400">
            Show 10-finger FLEX values, normalized results, and IMU posture with simple zero/full calibration.
          </p>
        </div>

        <div className="flex flex-wrap gap-3">
          <button className="tab-button tab-button-idle" disabled={calibrationBusy} onClick={onZero}>
            Zero
          </button>
          <button className="tab-button tab-button-active" disabled={calibrationBusy} onClick={onFull}>
            Full
          </button>
          <button className="tab-button tab-button-idle" disabled={calibrationBusy} onClick={onImuZero}>
            IMU Zero
          </button>
        </div>
      </div>

      <div className="mt-5 grid grid-cols-1 gap-4 xl:grid-cols-[1.3fr_0.7fr]">
        <div className="rounded-[24px] border border-cyan-300/20 bg-cyan-300/10 p-5">
          <div className="flex items-center gap-2 text-sm font-bold text-cyan-100">
            <Gauge size={16} />
            FLEX 10-finger layout
          </div>

          <div className="mt-4 grid grid-cols-2 gap-4 text-sm text-slate-200 sm:grid-cols-4">
            <div>
              <p className="text-slate-400">Left total</p>
              <p className="mt-2 text-3xl font-black text-white">{formatNumber(flex.left)}</p>
            </div>
            <div>
              <p className="text-slate-400">Right total</p>
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

          <div className="mt-5 grid grid-cols-1 gap-3 sm:grid-cols-2 xl:grid-cols-5">
            {fingerRows.map((finger) => (
              <div key={finger.id} className="rounded-2xl border border-white/10 bg-slate-950/25 p-4">
                <p className="text-xs uppercase tracking-[0.18em] text-slate-500">{finger.title}</p>
                <p className="mt-3 text-2xl font-black text-white">{formatNumber(finger.raw)}</p>
                <p className="mt-2 text-sm text-slate-400">
                  normalized <span className="font-bold text-cyan-100">{formatNumber(finger.normalized, 2)}</span>
                </p>
              </div>
            ))}
          </div>
        </div>

        <div className="rounded-[24px] border border-fuchsia-300/20 bg-fuchsia-300/10 p-5">
          <div className="flex items-center gap-2 text-sm font-bold text-fuchsia-100">
            <Move3d size={16} />
            IMU
          </div>
          <div className="mt-4 grid grid-cols-3 gap-4 text-sm text-slate-200">
            <div>
              <p className="text-slate-400">roll</p>
              <p className="mt-2 text-3xl font-black text-white">{formatNumber(imu.roll, 2)}</p>
            </div>
            <div>
              <p className="text-slate-400">pitch</p>
              <p className="mt-2 text-3xl font-black text-white">{formatNumber(imu.pitch, 2)}</p>
            </div>
            <div>
              <p className="text-slate-400">yaw</p>
              <p className="mt-2 text-3xl font-black text-white">{formatNumber(imu.yaw, 2)}</p>
            </div>
          </div>
        </div>
      </div>

      <div className="mt-4 rounded-2xl border border-white/10 bg-slate-950/30 p-4 text-sm text-slate-300">
        {calibrationMessage}
      </div>
    </section>
  );
};
