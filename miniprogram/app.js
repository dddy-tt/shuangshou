const { createAppState } = require('./store/app-state');

App({
  globalData: {},

  onLaunch() {
    this.runtime = createAppState();
    const guardianApi = this.runtime.getGuardianApi();
    if (guardianApi.isConfigured() && typeof wx !== 'undefined' && wx.cloud && typeof wx.cloud.init === 'function') {
      wx.cloud.init({ env: guardianApi.config.envId, traceUser: true });
    }
  },

  onShow() {
    if (!this.runtime) this.runtime = createAppState();
    if (typeof this.runtime.onAppShow === 'function') {
      const recovery = this.runtime.onAppShow();
      if (recovery && typeof recovery.catch === 'function') recovery.catch(() => {});
    }
  },

  onHide() {
    if (this.runtime && typeof this.runtime.onAppHide === 'function') this.runtime.onAppHide();
  },

  getRuntime() {
    if (!this.runtime) {
      this.runtime = createAppState();
    }
    return this.runtime;
  }
});
