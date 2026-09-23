const assert = require('assert');
const {
  createProtocolParser,
  formatAlarmAck,
  MAX_TEXT_BUFFER_LENGTH,
  parseLine
} = require('../utils/protocol');

function collectFrames(chunks) {
  const frames = [];
  const errors = [];
  const parser = createProtocolParser({
    onFrame: (frame) => frames.push(frame),
    onError: (error) => errors.push(error.message)
  });

  chunks.forEach((chunk) => parser.push(Buffer.from(chunk, 'utf8')));
  return { frames, errors };
}

const result = collectFrames([
  'BOOT:STM32F407_BASE\r\nFLEX|L1=10|L2=20|L3=30|L4=40|L5=50|R1=',
  '60|R2=70|R3=80|R4=90|R5=100\r\nIMU|R=1.25|P=-2.50|Y=90.00\r\n',
  'BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1\r\nJY|ONLINE=1|ERR=3|LAST=1|AGE=87\r\nACC|X=0.01|Y=-0.02|Z=0.98|VALID=1\r\nALARM|BOOT=4294967295|ID=7|TYPE=1|ACTIVE=1\r\n'
]);

assert.deepStrictEqual(result.errors, []);
assert.strictEqual(result.frames.length, 7);
assert.deepStrictEqual(result.frames[0], {
  type: 'boot',
  name: 'STM32F407_BASE'
});
assert.deepStrictEqual(result.frames[1], {
  type: 'flex',
  left: [10, 20, 30, 40, 50],
  right: [60, 70, 80, 90, 100]
});
assert.deepStrictEqual(result.frames[2], {
  type: 'imu',
  roll: 1.25,
  pitch: -2.5,
  yaw: 90
});
assert.deepStrictEqual(result.frames[3], {
  type: 'bringup',
  jy: 1,
  jyRet: 0,
  adc1: 1,
  adc2: 1,
  beep: 1
});
assert.deepStrictEqual(result.frames[4], {
  type: 'jy',
  jyOnline: true,
  jyErrorStreak: 3,
  jyLastError: 1,
  jySampleAgeMs: 87
});
assert.deepStrictEqual(result.frames[5], {
  type: 'acc', x: 0.01, y: -0.02, z: 0.98, valid: true, unit: 'g'
});
assert.deepStrictEqual(result.frames[6], {
  type: 'alarm', boot: 4294967295, id: 7, alarmType: 1, active: true
});

const recovered = collectFrames([
  'discarded-prefixJY|ONLINE=1|ERR=0|LAST=0|AGE=12IMU|R=1.25|P=-2.50|Y=3.75\r\n',
  'ACC|X=0.1|Y=0.2|Z=1.0|VALID=1FLEX|L1=1|L2=2|L3=3|L4=4|L5=5|R1=6|R2=7|R3=8|R4=9|R5=10\r\n'
]);
assert.deepStrictEqual(recovered.errors, []);
assert.deepStrictEqual(recovered.frames.map((frame) => frame.type), ['jy', 'imu', 'acc', 'flex']);

const overflowRecovered = collectFrames([
  `${'x'.repeat(MAX_TEXT_BUFFER_LENGTH + 32)}FLEX|L1=1|L2=2|L3=3|L4=4|L5=5|R1=6|R2=7|R3=8|R4=9|R5=10`,
  '\r\n'
]);
assert.deepStrictEqual(overflowRecovered.frames, [{
  type: 'flex', left: [1, 2, 3, 4, 5], right: [6, 7, 8, 9, 10]
}]);
assert.deepStrictEqual(overflowRecovered.errors, ['协议缓冲过长，已从最新帧头重新同步']);

assert.deepStrictEqual(
  parseLine('ALARM_STATE|BOOT=42|ACTIVE=0|ID=0|TYPE=0'),
  { type: 'alarmState', boot: 42, active: false, id: 0, alarmType: 0 }
);
assert.deepStrictEqual(
  parseLine('ALARM_STATE|BOOT=42|ACTIVE=1|ID=7|TYPE=2'),
  { type: 'alarmState', boot: 42, active: true, id: 7, alarmType: 2 }
);

assert.deepStrictEqual(
  parseLine('BRINGUP: JY_R=1,JY_L=1,JY_R_RET=0,JY_L_RET=0,MAX=1,MAX_RET=0,MAX_PART=0x57,MAX_HAL_ERR=0,ADC1=1,ADC2=1,DEG=0'),
  {
    type: 'bringup',
    adc1: 1,
    adc2: 1,
    jyRight: 1,
    jyLeft: 1,
    jyRightRet: 0,
    jyLeftRet: 0,
    jy: 1,
    jyRet: 0,
    max: 1,
    maxRet: 0,
    maxPart: 0x57,
    maxHalErr: 0,
    degraded: 0
  }
);

assert.deepStrictEqual(
  parseLine('BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1,JY_ES=3,JY_ERR=1,JY_AGE=87,JY_AV=0,MAX=1,DEG=0'),
  {
    type: 'bringup',
    adc1: 1,
    adc2: 1,
    jy: 1,
    jyRet: 0,
    jyOnline: true,
    jyErrorStreak: 3,
    jyLastError: 1,
    jySampleAgeMs: 87,
    jyAccValid: false,
    beep: 1,
    max: 1,
    degraded: 0
  }
);

assert.deepStrictEqual(
  parseLine('JY|ONLINE=0|ERR=4294967295|LAST=2|AGE=4294967295'),
  {
    type: 'jy',
    jyOnline: false,
    jyErrorStreak: 0xFFFFFFFF,
    jyLastError: 2,
    jySampleAgeMs: 0xFFFFFFFF
  }
);

assert.deepStrictEqual(
  parseLine('CARE|HR=78|SPO2=98|FALL=0|SOS=1'),
  { type: 'care', hr: 78, spo2: 98, fall: false, sos: true }
);

assert.deepStrictEqual(
  parseLine('PPG|IR=12345|RED=6789|VALID=1|HR=76|SPO2=97'),
  { type: 'ppg', ir: 12345, red: 6789, valid: true, hr: 76, spo2: 97 }
);

assert.throws(
  () => parseLine('FLEX|L1=10|R1=not-a-number'),
  /无效数字/
);
assert.strictEqual(formatAlarmAck(12, 34), 'ALARM_ACK:12:34\n');
assert.throws(() => parseLine('ALARM|BOOT=1.5|ID=7|TYPE=1|ACTIVE=1'), /uint32/);
assert.throws(() => parseLine('ACC|X=0|Y=0|Z=1|VALID=2'), /VALID/);
assert.throws(() => parseLine('JY|ONLINE=1|ERR=0|LAST=0|AGE=4294967296'), /uint32/);

console.log('protocol tests passed');
