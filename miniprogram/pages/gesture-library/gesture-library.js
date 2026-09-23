const { createGestureStore } = require('../../services/gesture-store');
Page({
  data: { query: '', items: [] },
  onLoad() { this.store = createGestureStore(); }, onShow() { this.refresh(); },
  refresh() { const query = this.data.query.trim().toLowerCase(); const items = this.store.list().filter((item) => !query || `${item.name} ${item.action}`.toLowerCase().includes(query)).map((item) => ({ ...item, fingers: Array.from({ length: 10 }, (_, i) => Number.isFinite(item.fingers[i]) ? item.fingers[i].toFixed(1) : '—'), categoryLabel: item.category === 'training' ? '训练' : item.category === 'control' ? '控制' : '翻译', time: new Date(item.updatedAt).toLocaleString('zh-CN', { hour12: false }) })); this.setData({ items }); },
  search(e) { this.setData({ query: e.detail.value }); this.refresh(); },
  toggle(e) { const id = e.currentTarget.dataset.id; const item = this.store.list().find((entry) => entry.id === id); if (item) { this.store.update(id, { enabled: !item.enabled }); this.refresh(); } },
  edit(e) { const id = e.currentTarget.dataset.id; const item = this.store.list().find((entry) => entry.id === id); if (!item) return; wx.showModal({ title: `修改「${item.name}」的文字`, editable: true, content: item.action, success: (result) => { if (result.confirm && String(result.content || '').trim()) { this.store.update(id, { text: result.content, action: result.content }); this.refresh(); } } }); },
  remove(e) { const id = e.currentTarget.dataset.id; wx.showModal({ title: '删除手势', content: '删除后无法恢复，确定删除吗？', success: (result) => { if (result.confirm) { this.store.remove(id); this.refresh(); } } }); },
  add() { wx.navigateTo({ url: '/pages/gesture-train/gesture-train' }); },
  exportLibrary() { const text = this.store.exportJson(); wx.setClipboardData({ data: text, success: () => wx.showToast({ title: '已复制导出数据', icon: 'success' }) }); },
  importLibrary() { wx.chooseMessageFile({ count: 1, type: 'file', extension: ['json'], success: (result) => { const file = result.tempFiles && result.tempFiles[0]; if (!file) return; wx.getFileSystemManager().readFile({ filePath: file.path, encoding: 'utf8', success: (read) => { try { this.store.importJson(read.data, 'merge'); this.refresh(); wx.showToast({ title: '导入完成', icon: 'success' }); } catch (error) { wx.showModal({ title: '导入失败', content: error.message || '文件格式不正确', showCancel: false }); } }, fail: () => wx.showToast({ title: '无法读取文件', icon: 'none' }) }); } }); }
});
