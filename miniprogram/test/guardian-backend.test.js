const assert = require('assert');
const crypto = require('crypto');
const Module = require('module');
const path = require('path');

const INDEX_PATH = path.resolve(__dirname, '../cloudfunctions/guardian/index.js');

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function matches(document, criteria) {
  return Object.keys(criteria || {}).every((key) => document[key] === criteria[key]);
}

class FakeDocumentRef {
  constructor(store, collectionName, id) {
    this.store = store;
    this.collectionName = collectionName;
    this.id = id;
  }

  async get() {
    const document = this.store.get(this.collectionName, this.id);
    if (!document) {
      const error = new Error('document not found');
      error.code = 'DOCUMENT_NOT_FOUND';
      throw error;
    }
    return { data: clone(document) };
  }

  async set({ data }) {
    this.store.put(this.collectionName, this.id, { ...clone(data), _id: this.id });
    return { _id: this.id };
  }

  async update({ data }) {
    const current = this.store.get(this.collectionName, this.id);
    if (!current) {
      const error = new Error('document not found');
      error.code = 'DOCUMENT_NOT_FOUND';
      throw error;
    }
    this.store.put(this.collectionName, this.id, { ...current, ...clone(data), _id: this.id });
    return { _id: this.id };
  }
}

class FakeQuery {
  constructor(store, collectionName, criteria = {}) {
    this.store = store;
    this.collectionName = collectionName;
    this.criteria = { ...criteria };
    this.order = null;
    this.max = null;
  }

  where(criteria) {
    this.criteria = { ...this.criteria, ...criteria };
    return this;
  }

  orderBy(field, direction) {
    this.order = { field, direction };
    return this;
  }

  limit(max) {
    this.max = max;
    return this;
  }

  async get() {
    let documents = this.store.list(this.collectionName).filter((item) => matches(item, this.criteria));
    if (this.order) {
      const { field, direction } = this.order;
      documents.sort((left, right) => {
        const a = Number(left[field]) || 0;
        const b = Number(right[field]) || 0;
        return direction === 'asc' ? a - b : b - a;
      });
    }
    if (this.max !== null) documents = documents.slice(0, this.max);
    return { data: clone(documents) };
  }
}

class FakeCollection {
  constructor(store, name) {
    this.store = store;
    this.name = name;
  }

  where(criteria) {
    return new FakeQuery(this.store, this.name, criteria);
  }

  orderBy(field, direction) {
    return new FakeQuery(this.store, this.name).orderBy(field, direction);
  }

  limit(max) {
    return new FakeQuery(this.store, this.name).limit(max);
  }

  doc(id) {
    return new FakeDocumentRef(this.store, this.name, id);
  }

  async add({ data }) {
    const id = `auto-${++this.store.nextId}`;
    this.store.put(this.name, id, { ...clone(data), _id: id });
    return { _id: id };
  }
}

class FakeDatabase {
  constructor() {
    this.collections = new Map();
    this.nextId = 0;
    this.transactionTail = Promise.resolve();
  }

  collection(name) {
    return new FakeCollection(this, name);
  }

  get(collectionName, id) {
    const collection = this.collections.get(collectionName);
    const document = collection && collection.get(id);
    return document ? clone(document) : null;
  }

  put(collectionName, id, document) {
    if (!this.collections.has(collectionName)) this.collections.set(collectionName, new Map());
    this.collections.get(collectionName).set(id, clone(document));
  }

  list(collectionName) {
    const collection = this.collections.get(collectionName);
    return collection ? Array.from(collection.values()).map(clone) : [];
  }

  seed(collectionName, id, document) {
    this.put(collectionName, id, { ...document, _id: id });
  }

  snapshot() {
    const snapshot = {};
    for (const [name, collection] of this.collections.entries()) {
      snapshot[name] = Object.fromEntries(collection.entries());
    }
    return clone(snapshot);
  }

  restore(snapshot) {
    this.collections = new Map();
    for (const [name, documents] of Object.entries(snapshot)) {
      this.collections.set(name, new Map(Object.entries(documents)));
    }
  }

  async runTransaction(handler) {
    const previous = this.transactionTail;
    let release;
    this.transactionTail = new Promise((resolve) => { release = resolve; });
    await previous;
    const snapshot = this.snapshot();
    try {
      return await handler(this);
    } catch (error) {
      this.restore(snapshot);
      throw error;
    } finally {
      release();
    }
  }
}

function createRuntime() {
  const db = new FakeDatabase();
  let openid = '';
  const fakeSdk = {
    DYNAMIC_CURRENT_ENV: 'fake-env',
    init() {},
    database() { return db; },
    getWXContext() { return { OPENID: openid }; }
  };
  const originalLoad = Module._load;
  Module._load = function load(request, parent, isMain) {
    if (request === 'wx-server-sdk') return fakeSdk;
    return originalLoad.call(this, request, parent, isMain);
  };
  delete require.cache[INDEX_PATH];
  let guardian;
  try {
    guardian = require(INDEX_PATH);
  } finally {
    Module._load = originalLoad;
  }

  return {
    db,
    call(user, request) {
      openid = user;
      return guardian.main(request);
    }
  };
}

async function expectCode(promise, code) {
  const result = await promise;
  assert.strictEqual(result.ok, false, `expected ${code}, got ${JSON.stringify(result)}`);
  assert.strictEqual(result.code, code, `expected ${code}, got ${JSON.stringify(result)}`);
  return result;
}

async function bind(runtime, wearer, guardian, wearableId) {
  const created = await runtime.call(wearer, { action: 'createInvite', wearableId });
  assert.strictEqual(created.ok, true, JSON.stringify(created));
  const accepted = await runtime.call(guardian, { action: 'acceptInvite', code: created.invite.code });
  assert.strictEqual(accepted.ok, true, JSON.stringify(accepted));
  return { invite: created.invite, binding: accepted.binding };
}

function event(wearableId, boot, id, active, alarmType = 1, origin = 'device') {
  return {
    eventKey: `${wearableId}/${boot}/${id}`,
    wearableId,
    boot,
    id,
    alarmType,
    active,
    origin
  };
}

function namespacedEventId(bindingId, eventKey) {
  return crypto.createHash('sha256').update(`${bindingId}/${eventKey}`).digest('hex').slice(0, 32);
}

async function testCrossTenantEventsAndStatus() {
  const runtime = createRuntime();
  const tenantA = await bind(runtime, 'wearer-a', 'guardian-a', 'shared-wearable');
  const tenantB = await bind(runtime, 'wearer-b', 'guardian-b', 'shared-wearable');
  const shared = event('shared-wearable', 17, 4, true);

  const publishedA = await runtime.call('wearer-a', { action: 'publishEvent', event: shared });
  const publishedB = await runtime.call('wearer-b', {
    action: 'publishEvent',
    event: { ...shared, active: false }
  });
  assert.strictEqual(publishedA.ok, true, JSON.stringify(publishedA));
  assert.strictEqual(publishedB.ok, true, JSON.stringify(publishedB));
  assert.strictEqual(Object.prototype.hasOwnProperty.call(publishedA.event, 'wearerOpenId'), false);
  assert.strictEqual(Object.prototype.hasOwnProperty.call(publishedA.event, 'bindingId'), false);

  const listA = await runtime.call('guardian-a', { action: 'listEvents', limit: 10 });
  const listB = await runtime.call('guardian-b', { action: 'listEvents', limit: 10 });
  assert.strictEqual(listA.events.length, 1);
  assert.strictEqual(listB.events.length, 1);
  assert.strictEqual(listA.events[0].eventKey, shared.eventKey);
  assert.strictEqual(listA.events[0].active, true, 'tenant A event was changed by tenant B');
  assert.strictEqual(listB.events[0].active, false);

  const statusA = await runtime.call('wearer-a', {
    action: 'touchStatus',
    wearableId: 'shared-wearable',
    online: true
  });
  assert.strictEqual(statusA.ok, true, JSON.stringify(statusA));
  const statusB = await runtime.call('guardian-b', { action: 'getStatus' });
  assert.strictEqual(statusB.ok, true, JSON.stringify(statusB));
  assert.strictEqual(statusB.status.online, false, 'tenant B read tenant A status');

  const poisonedId = namespacedEventId(tenantB.binding.bindingId, 'shared-wearable/17/5');
  runtime.db.seed('guardian_events', poisonedId, {
    eventKey: 'shared-wearable/17/5',
    wearerOpenId: 'wearer-a',
    wearableId: 'shared-wearable',
    bindingId: tenantA.binding.bindingId,
    boot: 17,
    id: 5,
    alarmType: 1,
    active: true,
    origin: 'device',
    deviceConfirmed: true,
    occurredAt: Date.now(),
    acknowledgedBy: []
  });
  const ownership = await runtime.call('wearer-b', {
    action: 'publishEvent',
    event: event('shared-wearable', 17, 5, false)
  });
  assert.strictEqual(ownership.ok, false);
  assert.strictEqual(ownership.code, 'OWNERSHIP_FORBIDDEN');
}

async function testDifferentInvitesCannotBindOneAccountConcurrently() {
  const runtime = createRuntime();
  const inviteOne = await runtime.call('wearer-c', { action: 'createInvite', wearableId: 'wearable-c' });
  const inviteTwo = await runtime.call('wearer-c', { action: 'createInvite', wearableId: 'wearable-c' });
  assert.strictEqual(inviteOne.ok, true);
  assert.strictEqual(inviteTwo.ok, true);

  const results = await Promise.all([
    runtime.call('guardian-c', { action: 'acceptInvite', code: inviteOne.invite.code }),
    runtime.call('guardian-c', { action: 'acceptInvite', code: inviteTwo.invite.code })
  ]);
  assert.strictEqual(results.filter((result) => result.ok).length, 1, JSON.stringify(results));
  assert.strictEqual(results.filter((result) => result.code === 'ALREADY_BOUND').length, 1, JSON.stringify(results));

  const successful = results.find((result) => result.ok);
  const retry = await runtime.call('guardian-c', {
    action: 'acceptInvite',
    code: successful === results[0] ? inviteOne.invite.code : inviteTwo.invite.code
  });
  assert.strictEqual(retry.ok, true, JSON.stringify(retry));
  assert.strictEqual(retry.binding.bindingId, successful.binding.bindingId);

  const duplicateByOtherAccount = await runtime.call('guardian-d', {
    action: 'acceptInvite',
    code: successful === results[0] ? inviteOne.invite.code : inviteTwo.invite.code
  });
  assert.strictEqual(duplicateByOtherAccount.ok, false);
  assert.strictEqual(duplicateByOtherAccount.code, 'INVITE_INVALID');

  const bindings = runtime.db.list('guardian_bindings').filter((item) => item.wearerOpenId === 'wearer-c');
  assert.strictEqual(bindings.length, 1, 'concurrent accepts created multiple binding documents');
}

async function testResolvedEventCannotBeReactivated() {
  const runtime = createRuntime();
  await bind(runtime, 'wearer-e', 'guardian-e', 'wearable-e');
  const active = event('wearable-e', 21, 8, true, 2);
  const resolved = event('wearable-e', 21, 8, false, 2);

  assert.strictEqual((await runtime.call('wearer-e', { action: 'publishEvent', event: active })).ok, true);
  const concurrent = await Promise.all([
    runtime.call('wearer-e', { action: 'publishEvent', event: resolved }),
    runtime.call('wearer-e', { action: 'publishEvent', event: active })
  ]);
  assert.ok(concurrent.every((result) => result.ok), JSON.stringify(concurrent));
  const afterRace = await runtime.call('guardian-e', { action: 'listEvents', limit: 10 });
  assert.strictEqual(afterRace.events[0].active, false, 'stale ACTIVE won the publish race');

  const staleRetry = await runtime.call('wearer-e', { action: 'publishEvent', event: active });
  assert.strictEqual(staleRetry.ok, true, JSON.stringify(staleRetry));
  assert.strictEqual(staleRetry.event.active, false);
}

async function testNonOwnerAckAndDeviceBindingChecks() {
  const runtime = createRuntime();
  await bind(runtime, 'wearer-f', 'guardian-f', 'wearable-f');
  await bind(runtime, 'wearer-g', 'guardian-g', 'wearable-g');
  const alarm = event('wearable-f', 31, 2, true);
  const published = await runtime.call('wearer-f', { action: 'publishEvent', event: alarm });
  assert.strictEqual(published.ok, true, JSON.stringify(published));

  const nonOwnerAck = await runtime.call('guardian-g', {
    action: 'acknowledgeEvent',
    eventKey: alarm.eventKey
  });
  assert.strictEqual(nonOwnerAck.ok, false);
  assert.strictEqual(nonOwnerAck.code, 'EVENT_NOT_FOUND');

  const wrongDeviceStatus = await runtime.call('wearer-f', {
    action: 'touchStatus',
    wearableId: 'wearable-g',
    online: true
  });
  assert.strictEqual(wrongDeviceStatus.ok, false);
  assert.strictEqual(wrongDeviceStatus.code, 'OWNERSHIP_FORBIDDEN');

  const wrongListDevice = await runtime.call('guardian-f', {
    action: 'listEvents',
    wearableId: 'wearable-g'
  });
  assert.strictEqual(wrongListDevice.ok, false);
  assert.strictEqual(wrongListDevice.code, 'OWNERSHIP_FORBIDDEN');

  const spoofedOrigin = await runtime.call('guardian-f', {
    action: 'publishEvent',
    event: { ...alarm, origin: 'device' }
  });
  assert.strictEqual(spoofedOrigin.ok, false);
  assert.strictEqual(spoofedOrigin.code, 'ROLE_FORBIDDEN');
}

async function testInviteExpiryDuplicateRetryAndDeviceImmutability() {
  const runtime = createRuntime();
  const originalNow = Date.now;
  let now = 10_000_000;
  Date.now = () => now;
  try {
    const expired = await runtime.call('wearer-h', {
      action: 'createInvite',
      wearableId: 'wearable-h',
      ttlMs: 60 * 1000
    });
    assert.strictEqual(expired.ok, true, JSON.stringify(expired));
    now += 60 * 1000 + 1;
    await expectCode(runtime.call('guardian-h', {
      action: 'acceptInvite',
      code: expired.invite.code
    }), 'INVITE_INVALID');

    const valid = await runtime.call('wearer-h', {
      action: 'createInvite',
      wearableId: 'wearable-h',
      ttlMs: 60 * 1000
    });
    const accepted = await runtime.call('guardian-h', {
      action: 'acceptInvite',
      code: valid.invite.code
    });
    assert.strictEqual(accepted.ok, true, JSON.stringify(accepted));
    const retry = await runtime.call('guardian-h', {
      action: 'acceptInvite',
      code: valid.invite.code
    });
    assert.strictEqual(retry.ok, true, JSON.stringify(retry));
    assert.strictEqual(retry.binding.bindingId, accepted.binding.bindingId);

    const repeatedByOther = await runtime.call('guardian-i', {
      action: 'acceptInvite',
      code: valid.invite.code
    });
    assert.strictEqual(repeatedByOther.ok, false);
    assert.strictEqual(repeatedByOther.code, 'INVITE_INVALID');

    await expectCode(runtime.call('wearer-h', {
      action: 'createInvite',
      wearableId: 'another-wearable'
    }), 'WEARABLE_MISMATCH');
  } finally {
    Date.now = originalNow;
  }
}

async function run() {
  const tests = [
    ['cross-tenant event/status ownership', testCrossTenantEventsAndStatus],
    ['concurrent different invites bind one account once', testDifferentInvitesCannotBindOneAccountConcurrently],
    ['resolved events reject stale ACTIVE', testResolvedEventCannotBeReactivated],
    ['non-owner acknowledgement and device checks', testNonOwnerAckAndDeviceBindingChecks],
    ['invite expiry, duplicate, retry, and device immutability', testInviteExpiryDuplicateRetryAndDeviceImmutability]
  ];
  let failures = 0;
  for (const [name, test] of tests) {
    try {
      await test();
      console.log(`ok - ${name}`);
    } catch (error) {
      failures += 1;
      console.error(`not ok - ${name}`);
      console.error(error && error.stack || error);
    }
  }
  if (failures) throw new Error(`${failures} guardian backend regression test(s) failed`);
  console.log('guardian backend tests passed');
}

run().catch((error) => {
  console.error(error && error.stack || error);
  process.exitCode = 1;
});
