const { classifyFinger } = require('../../services/gesture-matcher');
const labels = ['拇指', '食指', '中指', '无名指', '小指'];
Component({
  properties: { fingers: { type: Array, value: [] }, enabled: { type: Array, value: [] }, connected: Boolean },
  data: { hands: [] },
  observers: {
    'fingers, enabled': function (fingers, enabled) {
      const values = Array.from({ length: 10 }, (_, index) => {
        const disabled = enabled[index] === false;
        const valid = Number.isFinite(fingers[index]);
        const value = valid ? Math.round(Math.max(0, Math.min(100, fingers[index])) * 10) / 10 : 0;
        const kind = classifyFinger(value);
        return { label: labels[index % 5], value, disabled,
          state: disabled ? '已关闭' : !valid ? '等待数据' : { straight: '平直', half: '半弯', full: '全弯' }[kind],
          tone: disabled || !valid ? 'idle' : { straight: 'success', half: 'warning', full: 'danger' }[kind] };
      });
      this.setData({ hands: [{ name: '左手', fingers: values.slice(0, 5) }, { name: '右手', fingers: values.slice(5) }] });
    }
  }
});
