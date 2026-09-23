const assert = require('assert');
const { createTtsService } = require('../services/tts');

function createAudioContext() {
  const handlers = {};
  return { src: '', onPlay(handler) { handlers.play = handler; }, onError(handler) { handlers.error = handler; }, onEnded(handler) { handlers.ended = handler; }, play() { handlers.play(); }, stop() {}, destroy() {} };
}

async function run() {
  const calls = [];
  global.wx = {
    env: { USER_DATA_PATH: '/tmp' },
    createInnerAudioContext: createAudioContext,
    getFileSystemManager: () => ({ writeFile({ filePath, data, success }) { assert.ok(filePath.endsWith('.mp3')); assert.ok(data.byteLength); success(); } }),
    request({ url, data, success }) {
      calls.push({ url, data });
      if (url.includes('/token')) success({ data: { access_token: 'token-123', expires_in: 3600 } });
      else success({ header: { 'content-type': 'audio/mp3' }, data: new ArrayBuffer(8) });
    }
  };
  const tts = createTtsService({ cooldownMs: 0, config: { apiKey: 'key', secretKey: 'secret' } });
  const result = await tts.speak('你好');
  assert.deepStrictEqual(result, { ok: true, provider: 'Baidu' });
  assert.strictEqual(calls.length, 2);
  assert.strictEqual(calls[1].data.tok, 'token-123');
  assert.strictEqual(tts.isConfigured(), true);
  const missing = await createTtsService({ config: { apiKey: '', secretKey: '' } }).speak('没有密钥');
  assert.deepStrictEqual(missing, { ok: false, reason: 'credentials-missing' });
  delete global.wx;
  console.log('tts tests passed');
}

run().catch((error) => { console.error(error); process.exitCode = 1; });
