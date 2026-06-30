import { AlertOctagon, Fan, Lightbulb, PlugZap } from "lucide-react";
import type { LucideIcon } from "lucide-react";
import React, { useState } from "react";

type DeviceKey = "light" | "fan" | "socket" | "sos";

interface DeviceButtonProps {
  label: string;
  hint: string;
  active: boolean;
  icon: LucideIcon;
  onClick: () => void;
  activeClassName: string;
}

const DeviceButton: React.FC<DeviceButtonProps> = ({ label, hint, active, icon: Icon, onClick, activeClassName }) => (
  <button
    onClick={onClick}
    className={`rounded-[24px] border p-5 text-left transition-all duration-200 ${
      active ? activeClassName : "border-white/10 bg-white/5 text-slate-300 hover:border-white/20"
    }`}
  >
    <Icon size={24} />
    <p className="mt-4 text-lg font-black">{label}</p>
    <p className="mt-1 text-xs">{hint}</p>
    <p className="mt-4 text-sm font-bold">{active ? "已开启" : "已关闭"}</p>
  </button>
);

export const IotCard: React.FC = () => {
  const [devices, setDevices] = useState<Record<DeviceKey, boolean>>({
    light: false,
    fan: false,
    socket: false,
    sos: false
  });

  const toggleDevice = (key: DeviceKey) => {
    setDevices((previous) => ({
      ...previous,
      [key]: !previous[key]
    }));
  };

  return (
    <section className="panel">
      <h2 className="panel-title">
        <PlugZap className="text-teal-300" size={22} />
        物联网家电远控
      </h2>

      <div className="mt-5 grid grid-cols-1 gap-4 md:grid-cols-2">
        <DeviceButton
          label="灯开关"
          hint="本地按钮状态切换"
          active={devices.light}
          icon={Lightbulb}
          onClick={() => toggleDevice("light")}
          activeClassName="border-amber-300/30 bg-amber-300/15 text-amber-100"
        />
        <DeviceButton
          label="风扇开关"
          hint="模拟家电开合状态"
          active={devices.fan}
          icon={Fan}
          onClick={() => toggleDevice("fan")}
          activeClassName="border-sky-300/30 bg-sky-300/15 text-sky-100"
        />
        <DeviceButton
          label="插座开关"
          hint="演示远控开闭操作"
          active={devices.socket}
          icon={PlugZap}
          onClick={() => toggleDevice("socket")}
          activeClassName="border-emerald-300/30 bg-emerald-300/15 text-emerald-100"
        />
        <DeviceButton
          label="SOS 触发 / 取消"
          hint="切换本地告警状态"
          active={devices.sos}
          icon={AlertOctagon}
          onClick={() => toggleDevice("sos")}
          activeClassName="border-rose-300/30 bg-rose-400 text-white"
        />
      </div>
    </section>
  );
};
