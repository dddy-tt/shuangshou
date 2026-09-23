// 复制为 guardian.local.js 后填写微信云开发环境 ID。
// 这里只允许放环境标识和非敏感运行参数，不能放 AppSecret、MQTT 密码或用户凭证。
module.exports = {
  envId: '',
  functionName: 'guardian',
  pollIntervalMs: 15000,
  offlineAfterMs: 60000,
  eventPageSize: 50,
  outboxMaxItems: 50,
  outboxMaxAttemptsPerFlush: 8,
  outboxBaseDelayMs: 2000,
  outboxMaxDelayMs: 60000,
  alarmAckTimeoutMs: 1500,
  alarmAckMaxAttempts: 3
};
