const DEFAULT_SAMPLE_RATE = 8000;
const DEFAULT_DURATION_MS = 720;
const DEFAULT_FILE_NAME = 'shuangshou-alarm-v1.wav';

function writeAscii(view, offset, value) {
  for (let index = 0; index < value.length; index += 1) view.setUint8(offset + index, value.charCodeAt(index));
}

function createAlarmWav(options = {}) {
  const sampleRate = Math.max(4000, Number(options.sampleRate) || DEFAULT_SAMPLE_RATE);
  const durationMs = Math.max(200, Number(options.durationMs) || DEFAULT_DURATION_MS);
  const sampleCount = Math.floor(sampleRate * durationMs / 1000);
  const buffer = new ArrayBuffer(44 + sampleCount * 2);
  const view = new DataView(buffer);
  writeAscii(view, 0, 'RIFF');
  view.setUint32(4, 36 + sampleCount * 2, true);
  writeAscii(view, 8, 'WAVE');
  writeAscii(view, 12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeAscii(view, 36, 'data');
  view.setUint32(40, sampleCount * 2, true);
  for (let index = 0; index < sampleCount; index += 1) {
    const timeMs = index * 1000 / sampleRate;
    const inBeep = (timeMs % 360) < 180;
    const frequency = timeMs < 360 ? 880 : 660;
    const envelope = Math.min(1, Math.min(timeMs, durationMs - timeMs) / 18);
    const sample = inBeep ? Math.sin(2 * Math.PI * frequency * index / sampleRate) * 0.35 * envelope : 0;
    view.setInt16(44 + index * 2, Math.max(-32767, Math.min(32767, Math.round(sample * 32767))), true);
  }
  return buffer;
}

function getWx(options) {
  if (options.wx) return options.wx;
  return typeof wx !== 'undefined' ? wx : null;
}

function detailOf(error) {
  return error && (error.errMsg || error.message) || String(error || '未知音频错误');
}

function createAlarmAudio(options = {}) {
  const wxApi = getWx(options);
  const fileName = options.fileName || DEFAULT_FILE_NAME;
  const suppliedFilePath = Boolean(options.audioFilePath);
  let audio = null;
  let filePath = options.audioFilePath || '';
  let writePromise = null;
  let playPromise = null;
  let cancelPendingPlay = null;
  let generation = 0;
  let lastError = null;

  function destroyContext(context) {
    if (!context) return;
    try { if (typeof context.stop === 'function') context.stop(); } catch (error) { /* 已停止 */ }
    try { if (typeof context.destroy === 'function') context.destroy(); } catch (error) { /* 已销毁 */ }
  }

  function reportFailure(reason, error) {
    const result = { ok: false, reason, detail: detailOf(error) };
    lastError = result;
    if (typeof options.onError === 'function') {
      try { options.onError({ ...result }); } catch (callbackError) { /* 反馈回调不能阻断清理 */ }
    }
    return result;
  }

  function ensureFile() {
    if (writePromise) return writePromise;
    if (filePath) return Promise.resolve(filePath);
    if (!wxApi || !wxApi.env || !wxApi.env.USER_DATA_PATH || typeof wxApi.getFileSystemManager !== 'function') {
      return Promise.reject(new Error('本地报警音文件系统不可用'));
    }
    const targetPath = `${wxApi.env.USER_DATA_PATH}/${fileName}`;
    const fs = wxApi.getFileSystemManager();
    const write = () => new Promise((resolve, reject) => fs.writeFile({
      filePath: targetPath,
      data: createAlarmWav(options),
      encoding: 'binary',
      success: () => resolve(targetPath),
      fail: reject
    }));
    const accessOrWrite = typeof fs.access === 'function'
      ? new Promise((resolve, reject) => fs.access({
        filePath: targetPath,
        success: () => resolve(targetPath),
        fail: () => write().then(resolve).catch(reject)
      }))
      : write();
    // 只有 access/write 成功后才能缓存路径；写入进行中不能让下一次 play 绕过 writePromise。
    const prepared = accessOrWrite.then((path) => {
      filePath = path;
      return path;
    });
    // 失败不能把拒绝的 Promise 留在缓存里，否则以后永远无法恢复重试。
    writePromise = prepared.catch((error) => {
      writePromise = null;
      if (!suppliedFilePath) filePath = '';
      throw error;
    });
    return writePromise;
  }

  function stop() {
    generation += 1;
    const current = audio;
    audio = null;
    destroyContext(current);
    const cancel = cancelPendingPlay;
    cancelPendingPlay = null;
    playPromise = null;
    if (cancel) cancel();
  }

  function play() {
    if (!wxApi || typeof wxApi.createInnerAudioContext !== 'function') {
      return Promise.resolve({ ok: false, reason: 'audio-unavailable' });
    }
    if (audio) return Promise.resolve({ ok: true, reused: true, local: true });
    if (playPromise) return playPromise;

    const token = generation;
    let settled = false;
    let resolveResult;
    const resultPromise = new Promise((resolve) => { resolveResult = resolve; });
    const settle = (result) => {
      if (settled) return;
      settled = true;
      resolveResult(result);
    };
    const cancelForThisPlay = () => settle({ ok: false, reason: 'cancelled' });
    cancelPendingPlay = cancelForThisPlay;
    const wrappedPromise = resultPromise.finally(() => {
      if (playPromise === wrappedPromise) playPromise = null;
      if (cancelPendingPlay === cancelForThisPlay) cancelPendingPlay = null;
    });
    playPromise = wrappedPromise;

    (async () => {
      let context = null;
      try {
        const path = await ensureFile();
        if (settled || token !== generation) {
          settle({ ok: false, reason: 'cancelled' });
          return;
        }

        context = wxApi.createInnerAudioContext();
        if (token !== generation) {
          destroyContext(context);
          settle({ ok: false, reason: 'cancelled' });
          return;
        }
        audio = context;
        let errorReported = false;
        const handleError = (error) => {
          if (errorReported) return;
          errorReported = true;
          if (audio === context) audio = null;
          destroyContext(context);
          settle(reportFailure('playback-failed', error));
        };
        if (typeof context.onError === 'function') context.onError(handleError);
        context.src = path;
        context.loop = true;
        // 不设置音量，保留系统音量；支持时跟随系统静音开关。
        if ('obeyMuteSwitch' in context) context.obeyMuteSwitch = true;
        const maybePlayPromise = context.play();
        if (maybePlayPromise && typeof maybePlayPromise.catch === 'function') maybePlayPromise.catch(handleError);
        settle({ ok: true, local: true, path });
      } catch (error) {
        if (context && audio === context) audio = null;
        if (context) destroyContext(context);
        settle(token !== generation ? { ok: false, reason: 'cancelled' } : reportFailure('audio-failed', error));
      }
    })();

    return playPromise;
  }

  return {
    ensureFile,
    isPlaying: () => Boolean(audio),
    play,
    stop,
    getLastError: () => lastError ? { ...lastError } : null,
    clearError: () => { lastError = null; }
  };
}

module.exports = { createAlarmAudio, createAlarmWav, DEFAULT_FILE_NAME };
