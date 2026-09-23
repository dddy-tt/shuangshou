const assert = require('assert');
const { createBluetoothClient } = require('../utils/bluetooth');

let connectionStateHandler;
let createAttempts = 0;

global.wx = {
  openBluetoothAdapter: ({ success }) => success({}),
  stopBluetoothDevicesDiscovery: ({ success }) => success({}),
  createBLEConnection: ({ fail }) => { createAttempts += 1; fail({ errMsg: 'forced connect failure' }); },
  closeBLEConnection: ({ deviceId, success }) => {
    if (connectionStateHandler) connectionStateHandler({ deviceId, connected: false });
    success({});
  },
  onBluetoothAdapterStateChange: () => {},
  onBluetoothDeviceFound: () => {},
  onBLECharacteristicValueChange: () => {},
  onBLEConnectionStateChange: (handler) => { connectionStateHandler = handler; }
};

async function run() {
  const client = createBluetoothClient();
  await client.initAdapter();
  await assert.rejects(() => client.connect({ deviceId: 'test-device', name: 'JDY-23' }));
  await new Promise((resolve) => setTimeout(resolve, 900));
  assert.strictEqual(createAttempts, 3, '内部失败清理不应触发额外自动重连');
  console.log('bluetooth reconnect tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
