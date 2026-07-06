import { Archive, Trash2 } from "lucide-react";
import React from "react";
import { CustomGestureCategory, CustomGestureItem, CustomGestureMatchMessage } from "../types";

interface Props {
  gestureName: string;
  actionText: string;
  category: CustomGestureCategory;
  items: CustomGestureItem[];
  latestMatch: CustomGestureMatchMessage | null;
  busy: boolean;
  message: string;
  onNameChange: (value: string) => void;
  onActionChange: (value: string) => void;
  onCategoryChange: (value: CustomGestureCategory) => void;
  onCapture: () => void;
  onDelete: (id: string) => void;
}

const categoryOptions: Array<{ value: CustomGestureCategory; label: string }> = [
  { value: "translation", label: "翻译文本" },
  { value: "control", label: "家电控制" },
  { value: "training", label: "训练目标" }
];

const categoryLabels: Record<CustomGestureCategory, string> = {
  translation: "翻译文本",
  control: "家电控制",
  training: "训练目标"
};

export const CustomGesturePanel: React.FC<Props> = ({
  gestureName,
  actionText,
  category,
  items,
  latestMatch,
  busy,
  message,
  onNameChange,
  onActionChange,
  onCategoryChange,
  onCapture,
  onDelete
}) => (
  <section className="panel">
    <div className="flex items-center gap-3 border-b border-white/10 pb-3">
      <Archive className="text-amber-300" size={20} />
      <div>
        <h2 className="text-lg font-bold text-white">自定义手势管理</h2>
        <p className="mt-1 text-sm text-slate-400">记录当前十指和 IMU 快照，保存为可配置的自定义手势模板。</p>
      </div>
    </div>

    <div className="mt-5 grid grid-cols-1 gap-4 xl:grid-cols-[0.95fr_1.05fr]">
      <div className="rounded-2xl border border-white/10 bg-slate-950/25 p-4">
        <div className="grid grid-cols-1 gap-4">
          <label className="text-sm text-slate-300">
            <span className="mb-2 block font-medium text-white">手势名称</span>
            <input
              className="w-full rounded-2xl border border-white/10 bg-white/5 px-4 py-3 text-white outline-none transition focus:border-sky-300/50"
              placeholder="例如：喝水手势 A"
              value={gestureName}
              onChange={(event) => onNameChange(event.target.value)}
            />
          </label>

          <label className="text-sm text-slate-300">
            <span className="mb-2 block font-medium text-white">用途分类</span>
            <select
              className="w-full rounded-2xl border border-white/10 bg-white/5 px-4 py-3 text-white outline-none transition focus:border-sky-300/50"
              value={category}
              onChange={(event) => onCategoryChange(event.target.value as CustomGestureCategory)}
            >
              {categoryOptions.map((option) => (
                <option key={option.value} value={option.value} className="bg-slate-900">
                  {option.label}
                </option>
              ))}
            </select>
          </label>

          <label className="text-sm text-slate-300">
            <span className="mb-2 block font-medium text-white">动作内容</span>
            <input
              className="w-full rounded-2xl border border-white/10 bg-white/5 px-4 py-3 text-white outline-none transition focus:border-sky-300/50"
              placeholder="例如：我想喝水 / LIGHT_ON / 张手训练"
              value={actionText}
              onChange={(event) => onActionChange(event.target.value)}
            />
          </label>

          <button className="primary-button w-full" disabled={busy} onClick={onCapture}>
            保存当前姿态为自定义手势
          </button>
        </div>

        <div className="mt-4 rounded-2xl border border-white/10 bg-slate-900/40 p-4 text-sm text-slate-300">
          {message}
        </div>

        <div className="mt-4 rounded-2xl border border-emerald-300/15 bg-emerald-300/10 p-4 text-sm text-slate-200">
          {latestMatch ? (
            <div className="space-y-2">
              <p className="font-bold text-emerald-100">最近命中手势：{latestMatch.item.name}</p>
              <p>用途：{categoryLabels[latestMatch.item.category]} / 动作：{latestMatch.item.action}</p>
              <p>匹配分数：{latestMatch.score}</p>
            </div>
          ) : (
            <p>当前还没有命中的自定义手势。</p>
          )}
        </div>
      </div>

      <div className="rounded-2xl border border-white/10 bg-slate-950/25 p-4">
        <div className="flex items-center justify-between">
          <h3 className="text-base font-bold text-white">已保存手势</h3>
          <span className="text-sm text-slate-400">{items.length} 条</span>
        </div>

        <div className="custom-scrollbar mt-4 max-h-[420px] space-y-3 overflow-y-auto pr-2">
          {items.length === 0 ? (
            <div className="rounded-2xl border border-dashed border-white/10 bg-white/5 p-4 text-sm text-slate-400">
              还没有保存的自定义手势。
            </div>
          ) : (
            items.map((item) => (
              <div key={item.id} className="rounded-2xl border border-white/10 bg-white/5 p-4">
                <div className="flex items-start justify-between gap-3">
                  <div>
                    <div className="flex items-center gap-2">
                      <p className="text-base font-bold text-white">{item.name}</p>
                      <span className="rounded-full border border-sky-300/20 bg-sky-300/10 px-2 py-1 text-xs text-sky-100">
                        {categoryLabels[item.category]}
                      </span>
                    </div>
                    <p className="mt-2 text-sm text-slate-300">{item.action}</p>
                    <p className="mt-2 text-xs text-slate-500">
                      {new Date(item.createdAt).toLocaleString("zh-CN", { hour12: false })}
                    </p>
                  </div>

                  <button
                    className="rounded-xl border border-rose-300/20 bg-rose-300/10 p-2 text-rose-200 transition hover:bg-rose-300/20"
                    onClick={() => onDelete(item.id)}
                    title="删除"
                  >
                    <Trash2 size={16} />
                  </button>
                </div>

                <div className="mt-3 grid grid-cols-1 gap-2 text-xs text-slate-400 sm:grid-cols-2">
                  <div className="rounded-xl bg-slate-950/30 px-3 py-2">
                    Left: {item.snapshot.leftFingers.join(", ")}
                  </div>
                  <div className="rounded-xl bg-slate-950/30 px-3 py-2">
                    Right: {item.snapshot.rightFingers.join(", ")}
                  </div>
                  <div className="rounded-xl bg-slate-950/30 px-3 py-2">
                    roll / pitch / yaw
                  </div>
                  <div className="rounded-xl bg-slate-950/30 px-3 py-2">
                    {item.snapshot.roll} / {item.snapshot.pitch} / {item.snapshot.yaw}
                  </div>
                </div>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  </section>
);
