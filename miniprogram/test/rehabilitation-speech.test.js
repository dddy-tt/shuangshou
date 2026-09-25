const assert = require('assert');
let definition;
global.Page = value => { definition = value; };
require('../pages/rehabilitation/rehabilitation');
delete global.Page;
async function run() {
  const spoken = [];
  const toasts = [];
  global.wx = { showToast(options) { toasts.push(options); } };
  const page = { ...definition, data: { ...definition.data }, active: true, speechSession: 0, lastSpeech: '', lastSpeechAt: 0,
    setData(patch) { Object.assign(this.data, patch); },
    tts: { speak(text) { spoken.push(text); return Promise.resolve({ ok: true }); }, stop() {} } };
  page.store = { list: () => [{ id: 'legacy', enabled: true, needsResample: true }] };
  assert.equal(page.chooseTarget(), null, '旧无掩码手势不能进入康复随机目标');
  assert.match(toasts[0].title, /重新采样/);
  page.store = { list: () => [
    { id: 'legacy', enabled: true, needsResample: true },
    { id: 'valid', enabled: true, needsResample: false }
  ] };
  assert.equal(page.chooseTarget().id, 'valid', '康复目标只从有有效采样掩码的手势中选择');
  page.announceFeedback('动作正确');
  await Promise.resolve();
  assert.deepStrictEqual(spoken, ['动作正确']);
  page.announceFeedback('动作正确');
  assert.equal(spoken.length, 1);
  page.announceFeedback('请弯曲左食指');
  assert.ok(page.speechTimer);
  page.cancelSpeech();
  assert.equal(page.speechTimer, null);
  let resolveSpeech;
  page.tts.speak = () => new Promise(resolve => { resolveSpeech = resolve; });
  page.playFeedback();
  page.cancelSpeech();
  page.data.speechStatus = '已暂停';
  resolveSpeech({ ok: true });
  await Promise.resolve();
  assert.equal(page.data.speechStatus, '已暂停');
  delete global.wx;
  console.log('rehabilitation speech tests passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });
