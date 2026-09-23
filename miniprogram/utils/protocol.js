const FLEX_CHANNEL_COUNT = 5;
const UINT32_MAX = 0xFFFFFFFF;
const MAX_TEXT_BUFFER_LENGTH = 2048;
const FRAME_PREFIXES = Object.freeze([
  'ALARM_STATE|', 'BRINGUP:', 'ALARM|', 'FLEX|', 'IMU|',
  'CARE|', 'PPG|', 'ACC|', 'JY|', 'BOOT:'
]);

function bytesToUtf8(arrayBuffer) {
  if (typeof TextDecoder !== 'undefined') {
    return new TextDecoder('utf-8').decode(arrayBuffer);
  }

  const bytes = new Uint8Array(arrayBuffer);
  let encoded = '';
  for (let i = 0; i < bytes.length; i += 1) {
    encoded += `%${bytes[i].toString(16).padStart(2, '0')}`;
  }
  return decodeURIComponent(encoded);
}

function parseNumber(value, label) {
  const number = Number(String(value).trim());
  if (!Number.isFinite(number)) {
    throw new Error(`${label} 无效数字`);
  }
  return number;
}

function parseFields(text, separator) {
  const fields = {};
  text.split(separator).forEach((item) => {
    const index = item.indexOf('=');
    if (index <= 0) {
      throw new Error(`字段格式错误: ${item}`);
    }

    const key = item.slice(0, index).trim();
    const value = item.slice(index + 1).trim();
    if (!key || value === '') {
      throw new Error(`字段格式错误: ${item}`);
    }
    fields[key] = parseNumber(value, key);
  });
  return fields;
}

function requireFields(fields, keys, frameType) {
  keys.forEach((key) => {
    if (!Object.prototype.hasOwnProperty.call(fields, key)) {
      throw new Error(`${frameType} 缺少字段 ${key}`);
    }
  });
}

function parseBoot(line) {
  const name = line.slice('BOOT:'.length).trim();
  if (!name) {
    throw new Error('BOOT 缺少名称');
  }
  return { type: 'boot', name };
}

function parseFlex(line) {
  const fields = parseFields(line.slice('FLEX|'.length), '|');
  const leftKeys = Array.from({ length: FLEX_CHANNEL_COUNT }, (_, index) => `L${index + 1}`);
  const rightKeys = Array.from({ length: FLEX_CHANNEL_COUNT }, (_, index) => `R${index + 1}`);
  requireFields(fields, [...leftKeys, ...rightKeys], 'FLEX');

  const values = [...leftKeys, ...rightKeys].map((key) => fields[key]);
  if (values.some((value) => value < 0 || value > 100)) {
    throw new Error('FLEX 数值必须在 0 到 100 之间');
  }

  return {
    type: 'flex',
    left: leftKeys.map((key) => fields[key]),
    right: rightKeys.map((key) => fields[key])
  };
}

function parseImu(line) {
  const fields = parseFields(line.slice('IMU|'.length), '|');
  requireFields(fields, ['R', 'P', 'Y'], 'IMU');
  return {
    type: 'imu',
    roll: fields.R,
    pitch: fields.P,
    yaw: fields.Y
  };
}

function parseBringup(line) {
  const fields = parseFields(line.slice('BRINGUP:'.length).trim(), ',');
  requireFields(fields, ['ADC1', 'ADC2'], 'BRINGUP');
  const hasLegacyJY = Object.prototype.hasOwnProperty.call(fields, 'JY')
    || Object.prototype.hasOwnProperty.call(fields, 'JY_RET');
  const hasDualJY = ['JY_R', 'JY_L', 'JY_R_RET', 'JY_L_RET'].some((key) => Object.prototype.hasOwnProperty.call(fields, key));
  if (!hasLegacyJY && !hasDualJY && !Object.prototype.hasOwnProperty.call(fields, 'DEG')) {
    throw new Error('BRINGUP 缺少 JY 诊断字段');
  }

  const result = {
    type: 'bringup',
    adc1: fields.ADC1,
    adc2: fields.ADC2
  };
  if (hasLegacyJY) {
    requireFields(fields, ['JY', 'JY_RET'], 'BRINGUP');
    result.jy = fields.JY;
    result.jyRet = fields.JY_RET;
  }
  if (Object.prototype.hasOwnProperty.call(fields, 'BEEP')) result.beep = fields.BEEP;
  if (hasDualJY) {
    requireFields(fields, ['JY_R', 'JY_L', 'JY_R_RET', 'JY_L_RET'], 'BRINGUP');
    result.jyRight = fields.JY_R;
    result.jyLeft = fields.JY_L;
    result.jyRightRet = fields.JY_R_RET;
    result.jyLeftRet = fields.JY_L_RET;
    // 保留旧页面可读的聚合别名，同时保留左右原始诊断字段。
    result.jy = fields.JY_R === 1 && fields.JY_L === 1 ? 1 : 0;
    result.jyRet = fields.JY_R_RET !== 0 ? fields.JY_R_RET : fields.JY_L_RET;
  }
  if (Object.prototype.hasOwnProperty.call(fields, 'JY_VALID')) {
    if (![0, 1].includes(fields.JY_VALID)) throw new Error('BRINGUP JY_VALID 必须为 0 或 1');
    result.jyValid = fields.JY_VALID === 1;
  }
  if (Object.prototype.hasOwnProperty.call(fields, 'ACC_VALID')) {
    if (![0, 1].includes(fields.ACC_VALID)) throw new Error('BRINGUP ACC_VALID 必须为 0 或 1');
    result.accValid = fields.ACC_VALID === 1;
  }
  const hasDynamicJY = ['JY_ES', 'JY_ERR', 'JY_AGE', 'JY_AV']
    .some((key) => Object.prototype.hasOwnProperty.call(fields, key));
  if (hasDynamicJY) {
    requireFields(fields, ['JY_ES', 'JY_ERR', 'JY_AGE', 'JY_AV'], 'BRINGUP');
    ['JY_ES', 'JY_ERR', 'JY_AGE'].forEach((key) => {
      if (!Number.isInteger(fields[key]) || fields[key] < 0 || fields[key] > UINT32_MAX) {
        throw new Error(`BRINGUP ${key} 必须是 uint32`);
      }
    });
    if (![0, 1].includes(fields.JY_AV)) throw new Error('BRINGUP JY_AV 必须为 0 或 1');
    if (hasLegacyJY) result.jyOnline = fields.JY === 1;
    else if (hasDualJY) result.jyOnline = result.jy === 1;
    result.jyErrorStreak = fields.JY_ES;
    result.jyLastError = fields.JY_ERR;
    result.jySampleAgeMs = fields.JY_AGE;
    result.jyAccValid = fields.JY_AV === 1;
  }
  ['max', 'maxRet', 'maxPart', 'maxHalErr', 'degraded'].forEach((name) => {
    const key = { max: 'MAX', maxRet: 'MAX_RET', maxPart: 'MAX_PART', maxHalErr: 'MAX_HAL_ERR', degraded: 'DEG' }[name];
    if (Object.prototype.hasOwnProperty.call(fields, key)) result[name] = fields[key];
  });
  return result;
}

function parseJY(line) {
  const fields = parseFields(line.slice('JY|'.length), '|');
  requireFields(fields, ['ONLINE', 'ERR', 'LAST', 'AGE'], 'JY');
  if (![0, 1].includes(fields.ONLINE)) throw new Error('JY ONLINE 必须为 0 或 1');
  ['ERR', 'LAST', 'AGE'].forEach((key) => {
    if (!Number.isInteger(fields[key]) || fields[key] < 0 || fields[key] > UINT32_MAX) {
      throw new Error(`JY ${key} 必须是 uint32`);
    }
  });
  return {
    type: 'jy',
    jyOnline: fields.ONLINE === 1,
    jyErrorStreak: fields.ERR,
    jyLastError: fields.LAST,
    jySampleAgeMs: fields.AGE
  };
}

function parseCare(line) {
  const fields = parseFields(line.slice('CARE|'.length), '|');
  requireFields(fields, ['HR', 'SPO2', 'FALL', 'SOS'], 'CARE');
  return {
    type: 'care',
    hr: fields.HR,
    spo2: fields.SPO2,
    fall: fields.FALL > 0,
    sos: fields.SOS > 0
  };
}

function parsePpg(line) {
  const fields = parseFields(line.slice('PPG|'.length), '|');
  requireFields(fields, ['IR', 'RED', 'VALID'], 'PPG');
  return {
    type: 'ppg',
    ir: fields.IR,
    red: fields.RED,
    valid: fields.VALID > 0,
    hr: Object.prototype.hasOwnProperty.call(fields, 'HR') ? fields.HR : null,
    spo2: Object.prototype.hasOwnProperty.call(fields, 'SPO2') ? fields.SPO2 : null
  };
}

function parseAcc(line) {
  const fields = parseFields(line.slice('ACC|'.length), '|');
  requireFields(fields, ['X', 'Y', 'Z', 'VALID'], 'ACC');
  if (![fields.X, fields.Y, fields.Z].every((value) => Number.isFinite(value))) {
    throw new Error('ACC 三轴数据无效');
  }
  if (![0, 1].includes(fields.VALID)) {
    throw new Error('ACC VALID 必须为 0 或 1');
  }
  return {
    type: 'acc',
    x: fields.X,
    y: fields.Y,
    z: fields.Z,
    valid: fields.VALID === 1,
    unit: 'g'
  };
}

function parseAlarm(line) {
  const fields = parseFields(line.slice('ALARM|'.length), '|');
  requireFields(fields, ['BOOT', 'ID', 'TYPE', 'ACTIVE'], 'ALARM');
  const uint32Fields = ['BOOT', 'ID'];
  uint32Fields.forEach((key) => {
    if (!Number.isInteger(fields[key]) || fields[key] < 0 || fields[key] > 0xFFFFFFFF) {
      throw new Error(`ALARM ${key} 必须是 uint32`);
    }
  });
  if (!Number.isInteger(fields.TYPE) || ![1, 2].includes(fields.TYPE)) {
    throw new Error('ALARM TYPE 只能是 1（疑似跌倒）或 2（异常抖动）');
  }
  if (!Number.isInteger(fields.ACTIVE) || ![0, 1].includes(fields.ACTIVE)) {
    throw new Error('ALARM ACTIVE 必须为 0 或 1');
  }
  return {
    type: 'alarm',
    boot: fields.BOOT,
    id: fields.ID,
    alarmType: fields.TYPE,
    active: fields.ACTIVE === 1
  };
}

function parseAlarmState(line) {
  const fields = parseFields(line.slice('ALARM_STATE|'.length), '|');
  requireFields(fields, ['BOOT', 'ACTIVE', 'ID', 'TYPE'], 'ALARM_STATE');
  ['BOOT', 'ID'].forEach((key) => {
    if (!Number.isInteger(fields[key]) || fields[key] < 0 || fields[key] > UINT32_MAX) {
      throw new Error(`ALARM_STATE ${key} 必须是 uint32`);
    }
  });
  if (![0, 1].includes(fields.ACTIVE)) throw new Error('ALARM_STATE ACTIVE 必须为 0 或 1');
  if (fields.ACTIVE === 0) {
    if (fields.ID !== 0 || fields.TYPE !== 0) throw new Error('ALARM_STATE 空闲状态的 ID/TYPE 必须为 0');
  } else if (!Number.isInteger(fields.TYPE) || ![1, 2].includes(fields.TYPE) || fields.ID === 0) {
    throw new Error('ALARM_STATE 活动状态的 ID/TYPE 无效');
  }
  return {
    type: 'alarmState',
    boot: fields.BOOT,
    active: fields.ACTIVE === 1,
    id: fields.ID,
    alarmType: fields.TYPE
  };
}

function parseLine(line) {
  const trimmed = line.trim();
  if (!trimmed) {
    return null;
  }

  if (trimmed.startsWith('BOOT:')) {
    return parseBoot(trimmed);
  }
  if (trimmed.startsWith('ALARM_STATE|')) {
    return parseAlarmState(trimmed);
  }
  if (trimmed.startsWith('FLEX|')) {
    return parseFlex(trimmed);
  }
  if (trimmed.startsWith('IMU|')) {
    return parseImu(trimmed);
  }
  if (trimmed.startsWith('BRINGUP:')) {
    return parseBringup(trimmed);
  }
  if (trimmed.startsWith('JY|')) {
    return parseJY(trimmed);
  }
  if (trimmed.startsWith('CARE|')) {
    return parseCare(trimmed);
  }
  if (trimmed.startsWith('PPG|')) {
    return parsePpg(trimmed);
  }
  if (trimmed.startsWith('ACC|')) {
    return parseAcc(trimmed);
  }
  if (trimmed.startsWith('ALARM|')) {
    return parseAlarm(trimmed);
  }

  throw new Error(`不支持的帧: ${trimmed}`);
}

function formatAlarmAck(boot, id) {
  [boot, id].forEach((value, index) => {
    if (!Number.isInteger(Number(value)) || Number(value) < 0 || Number(value) > 0xFFFFFFFF) {
      throw new Error(`${index === 0 ? 'BOOT' : 'ID'} 必须是 uint32`);
    }
  });
  return `ALARM_ACK:${Number(boot)}:${Number(id)}\n`;
}

function createProtocolParser(callbacks = {}) {
  let textBuffer = '';
  const onFrame = callbacks.onFrame || function noop() {};
  const onRawFrame = callbacks.onRawFrame || function noop() {};
  const onError = callbacks.onError || function noop() {};

  function splitRecoveredFrames(record) {
    const starts = [];
    FRAME_PREFIXES.forEach((prefix) => {
      let index = record.indexOf(prefix);
      while (index >= 0) {
        starts.push(index);
        index = record.indexOf(prefix, index + prefix.length);
      }
    });
    const ordered = Array.from(new Set(starts)).sort((left, right) => left - right);
    return ordered.map((start, index) => record.slice(start, ordered[index + 1]));
  }

  function trimOversizedBuffer() {
    if (textBuffer.length <= MAX_TEXT_BUFFER_LENGTH) return;
    const starts = FRAME_PREFIXES
      .map((prefix) => textBuffer.lastIndexOf(prefix))
      .filter((index) => index >= 0);
    textBuffer = starts.length ? textBuffer.slice(Math.max(...starts)) : '';
    onError(new Error('协议缓冲过长，已从最新帧头重新同步'));
  }

  function processRecord(record) {
    const recovered = splitRecoveredFrames(record);
    recovered.forEach((candidate) => {
      try {
        const frame = parseLine(candidate);
        if (frame) {
          onRawFrame(candidate.trim(), frame);
          onFrame(frame);
        }
      } catch (error) {
        onError(error);
      }
    });
  }

  return {
    push(arrayBuffer) {
      try {
        textBuffer += bytesToUtf8(arrayBuffer);
      } catch (error) {
        onError(new Error(`UTF-8 解码失败: ${error.message}`));
        return;
      }

      trimOversizedBuffer();

      const lines = textBuffer.split('\n');
      textBuffer = lines.pop() || '';

      lines.forEach(processRecord);
    },

    reset() {
      textBuffer = '';
    }
  };
}

module.exports = {
  FLEX_CHANNEL_COUNT,
  bytesToUtf8,
  createProtocolParser,
  formatAlarmAck,
  parseJY,
  parseLine,
  parseAcc,
  parseAlarm,
  parseAlarmState,
  MAX_TEXT_BUFFER_LENGTH
};
