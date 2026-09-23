const BAIDU_TOKEN_URL = 'https://aip.baidubce.com/oauth/2.0/token';
const BAIDU_SYNTHESIS_URL = 'https://tsn.baidu.com/text2audio';

function loadLocalConfig() {
  try { return require('../config/baidu-tts.local'); } catch (error) { return {}; }
}

const TTS_CONFIG = Object.freeze({
  ...loadLocalConfig(), tokenUrl: BAIDU_TOKEN_URL, synthesisUrl: BAIDU_SYNTHESIS_URL,
  playbackTimeoutMs: 6000, maxChars: 60
});

function errorMessage(error) { return error && (error.errMsg || error.msg || error.message || String(error)) || ''; }
function isCredentialsConfigured(config) { return Boolean(String(config.apiKey || '').trim() && String(config.secretKey || '').trim()); }
function request(options) {
  if (typeof wx === 'undefined' || typeof wx.request !== 'function') return Promise.reject(new Error('微信网络能力不可用'));
  return new Promise((resolve, reject) => wx.request({ ...options, success: resolve, fail: reject }));
}

function createTtsService(options = {}) {
  const config = { ...TTS_CONFIG, ...(options.config || {}) };
  const cooldownMs = Number.isFinite(Number(options.cooldownMs)) ? Number(options.cooldownMs) : 2200;
  let audio = null;
  let token = '';
  let tokenExpiresAt = 0;
  let lastText = '';
  let lastStartedAt = 0;
  let generation = 0;

  function stop() {
    generation += 1;
    const current = audio; audio = null;
    if (!current) return;
    if (typeof current.stop === 'function') current.stop();
    if (typeof current.destroy === 'function') current.destroy();
  }

  function getToken() {
    if (token && Date.now() < tokenExpiresAt) return Promise.resolve(token);
    return request({
      url: config.tokenUrl, method: 'POST', header: { 'content-type': 'application/x-www-form-urlencoded' },
      data: { grant_type: 'client_credentials', client_id: config.apiKey, client_secret: config.secretKey }
    }).then((result) => {
      const data = result && result.data;
      if (!data || !data.access_token) throw new Error((data && (data.error_description || data.error_msg)) || '百度未返回访问令牌');
      token = data.access_token;
      tokenExpiresAt = Date.now() + Math.max(60, Number(data.expires_in) || 0) * 1000 - 5 * 60 * 1000;
      return token;
    });
  }

  function writeAudioFile(buffer) {
    if (!buffer || !buffer.byteLength || typeof wx === 'undefined' || !wx.env || !wx.env.USER_DATA_PATH || typeof wx.getFileSystemManager !== 'function') return Promise.reject(new Error('小程序文件系统不可用'));
    const filePath = `${wx.env.USER_DATA_PATH}/baidu-tts-${Date.now()}.mp3`;
    return new Promise((resolve, reject) => wx.getFileSystemManager().writeFile({ filePath, data: buffer, encoding: 'binary', success: () => resolve(filePath), fail: reject }));
  }

  function playAudio(filePath, value) {
    if (!filePath || typeof wx === 'undefined' || typeof wx.createInnerAudioContext !== 'function') return Promise.resolve({ ok: false, reason: 'audio-unavailable' });
    stop();
    return new Promise((resolve) => {
      const context = wx.createInnerAudioContext();
      const timeoutMs = Math.max(1000, Number(config.playbackTimeoutMs) || 6000);
      let settled = false;
      let timer = null;
      const finish = (result) => {
        if (settled) return;
        settled = true;
        if (timer) clearTimeout(timer);
        if (result.ok) { lastText = value; lastStartedAt = Date.now(); }
        else if (audio === context) { audio = null; if (typeof context.stop === 'function') context.stop(); if (typeof context.destroy === 'function') context.destroy(); }
        resolve(result);
      };
      audio = context;
      if (typeof context.onPlay === 'function') context.onPlay(() => finish({ ok: true, provider: 'Baidu' }));
      if (typeof context.onError === 'function') context.onError((error) => finish({ ok: false, reason: 'playback-failed', detail: errorMessage(error) }));
      if (typeof context.onEnded === 'function') context.onEnded(() => { if (audio === context) { audio = null; if (typeof context.destroy === 'function') context.destroy(); } });
      timer = setTimeout(() => finish({ ok: false, reason: 'playback-timeout' }), timeoutMs);
      try { context.src = filePath; context.play(); } catch (error) { finish({ ok: false, reason: 'playback-failed', detail: errorMessage(error) }); }
    });
  }

  function synthesize(value) {
    return getToken().then((accessToken) => request({
      url: config.synthesisUrl, method: 'POST', responseType: 'arraybuffer', header: { 'content-type': 'application/x-www-form-urlencoded' },
      data: { tex: value, tok: accessToken, cuid: config.cuid || 'signwise-glove', ctp: 1, lan: 'zh', ...config.voice }
    })).then((result) => {
      const header = result && result.header || {};
      const contentType = String(header['content-type'] || header['Content-Type'] || '').toLowerCase();
      if (!result || !result.data || contentType.includes('json')) throw new Error('百度语音合成失败，请检查接口权限或网络');
      return writeAudioFile(result.data);
    });
  }

  function speak(text) {
    const value = String(text || '').trim();
    if (!value) return Promise.resolve({ ok: false, reason: 'empty' });
    if (!isCredentialsConfigured(config)) return Promise.resolve({ ok: false, reason: 'credentials-missing' });
    if (value.length > Number(config.maxChars)) return Promise.resolve({ ok: false, reason: 'text-too-long' });
    if (value === lastText && Date.now() - lastStartedAt < cooldownMs) return Promise.resolve({ ok: false, reason: 'deduplicated' });
    const requestGeneration = ++generation;
    return synthesize(value).then((filePath) => requestGeneration === generation ? playAudio(filePath, value) : { ok: false, reason: 'cancelled' }).catch((error) => ({ ok: false, reason: 'baidu-failed', detail: errorMessage(error) }));
  }

  return { speak, stop, isConfigured: () => isCredentialsConfigured(config) };
}

module.exports = { TTS_CONFIG, createTtsService, isCredentialsConfigured };
