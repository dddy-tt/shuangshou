const assert = require('assert');
let definition;
global.Page = value => { definition = value; };
require('../pages/rehabilitation/rehabilitation');
delete global.Page;
async function run() {
  const spoken = [];
  const page = { ...definition, data: { ...definition.data }, active: true, speechSession: 0, lastSpeech: '', lastSpeechAt: 0,
    setData(patch) { Object.assign(this.data, patch); },
    tts: { speak(text) { spoken.push(text); return Promise.resolve({ ok: true }); }, stop() {} } };
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
  console.log('rehabilitation speech tests passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });
