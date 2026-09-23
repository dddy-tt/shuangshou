const assert = require('assert');
const { createAlarmAudio, createAlarmWav } = require('../services/alarm-audio');

function tick() {
  return new Promise((resolve) => setImmediate(resolve));
}

async function main() {
  const wav = createAlarmWav();
  const header = Buffer.from(wav);
  assert.strictEqual(header.toString('ascii', 0, 4), 'RIFF');
  assert.strictEqual(header.toString('ascii', 8, 12), 'WAVE');
  assert.strictEqual(header.toString('ascii', 36, 40), 'data');

  const unavailable = createAlarmAudio({ wx: null });
  const unavailableResult = await unavailable.play();
  assert.strictEqual(unavailableResult.reason, 'audio-unavailable');

  const writes = [];
  const contexts = [];
  let playCalls = 0;
  const fakeWx = {
    env: { USER_DATA_PATH: '/mock-user-data' },
    getFileSystemManager() {
      return {
        writeFile(request) { writes.push(request); }
      };
    },
    createInnerAudioContext() {
      let errorHandler = null;
      const context = {
        loop: false,
        onError(handler) { errorHandler = handler; },
        play() { playCalls += 1; },
        stop() { context.stopped = true; },
        destroy() { context.destroyed = true; },
        triggerError(error) { if (errorHandler) errorHandler(error); }
      };
      contexts.push(context);
      return context;
    }
  };
  const audio = createAlarmAudio({ wx: fakeWx });
  const stoppedBeforeReady = audio.play();
  await tick();
  assert.strictEqual(writes.length, 1, '第一次 play 应只发起一个本地文件写入');
  audio.stop();
  writes[0].success();
  const cancelled = await stoppedBeforeReady;
  assert.strictEqual(cancelled.reason, 'cancelled', 'stop 必须取消尚未完成的 ensureFile/play 链路');
  assert.strictEqual(contexts.length, 0, '取消后不能迟到创建并播放音频 context');

  let retryWrite;
  const retryContexts = [];
  const immediateRetryWx = {
    env: { USER_DATA_PATH: '/mock-immediate-retry' },
    getFileSystemManager() {
      return { writeFile(request) { retryWrite = request; } };
    },
    createInnerAudioContext() {
      const context = { onError() {}, play() {}, stop() {}, destroy() {} };
      retryContexts.push(context);
      return context;
    }
  };
  const immediateRetryAudio = createAlarmAudio({ wx: immediateRetryWx });
  const oldPlay = immediateRetryAudio.play();
  await tick();
  immediateRetryAudio.stop();
  const newPlay = immediateRetryAudio.play();
  await tick();
  assert.strictEqual(retryContexts.length, 0, '第二次 play 必须等待第一次慢写入成功后再创建音频 context');
  retryWrite.success();
  assert.strictEqual((await oldPlay).reason, 'cancelled');
  assert.strictEqual((await newPlay).ok, true, 'stop 后立即再次 play 不能被旧 single-flight 阻塞');

  const firstPlay = audio.play();
  const secondPlay = audio.play();
  assert.strictEqual(firstPlay, secondPlay, '并发 play 必须共享 single-flight Promise');
  const started = await firstPlay;
  assert.strictEqual(started.ok, true);
  assert.strictEqual(contexts.length, 1, '并发 play 只能创建一个 context');
  assert.strictEqual(playCalls, 1);
  assert.strictEqual((await audio.play()).reused, true);

  contexts[0].triggerError({ errMsg: 'mock playback failed' });
  assert.strictEqual(audio.isPlaying(), false, '播放失败必须清除活动 context');
  assert.strictEqual(contexts[0].destroyed, true, '播放失败必须销毁 context');
  assert.strictEqual(audio.getLastError().reason, 'playback-failed');

  let failureWrites = 0;
  const retryWx = {
    env: { USER_DATA_PATH: '/mock-retry' },
    getFileSystemManager() {
      return {
        writeFile(request) {
          failureWrites += 1;
          if (failureWrites === 1) request.fail(new Error('disk full'));
          else request.success();
        }
      };
    },
    createInnerAudioContext() { return { play() {}, stop() {}, destroy() {} }; }
  };
  const retryAudio = createAlarmAudio({ wx: retryWx });
  await assert.rejects(() => retryAudio.ensureFile(), /disk full/);
  await retryAudio.ensureFile();
  assert.strictEqual(failureWrites, 2, '本地文件失败后必须清除失败缓存并允许重试');

  console.log('alarm audio tests passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
