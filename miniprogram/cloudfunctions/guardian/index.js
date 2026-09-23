const crypto = require('crypto');
const cloud = require('wx-server-sdk');

cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV });

const db = cloud.database();
const COLLECTIONS = Object.freeze({
  accounts: 'guardian_accounts',
  bindings: 'guardian_bindings',
  events: 'guardian_events',
  invites: 'guardian_invites',
  statuses: 'guardian_statuses'
});
const ROLE = Object.freeze({ WEARER: 'wearer', GUARDIAN: 'guardian' });
const MAX_EVENT_AGE_MS = 24 * 60 * 60 * 1000;
const DEFAULT_INVITE_TTL_MS = 10 * 60 * 1000;
const OFFLINE_AFTER_MS = 60 * 1000;

function fail(code, message) {
  const error = new Error(message);
  error.code = code;
  throw error;
}

function contextOpenId() {
  const context = cloud.getWXContext();
  const openid = String(context && context.OPENID || '').trim();
  if (!openid) fail('UNAUTHENTICATED', '需要微信登录后使用监护服务。');
  return openid;
}

function text(value, label, maxLength = 128) {
  const result = String(value || '').trim();
  if (!result || result.length > maxLength) fail('INVALID_INPUT', `${label} 无效。`);
  return result;
}

function wearableId(value) {
  const result = text(value, 'wearableId', 120);
  if (!/^[A-Za-z0-9._:-]+$/.test(result)) fail('INVALID_INPUT', 'wearableId 含有不支持的字符。');
  return result;
}

function uint32(value, label) {
  if (value === null || value === undefined || (typeof value === 'string' && !value.trim())) {
    fail('INVALID_INPUT', `${label} 必须是 uint32。`);
  }
  if (typeof value === 'string' && !/^\d+$/.test(value.trim())) {
    fail('INVALID_INPUT', `${label} 必须是 uint32。`);
  }
  const result = Number(value);
  if (!Number.isSafeInteger(result) || result < 0 || result > 0xFFFFFFFF) {
    fail('INVALID_INPUT', `${label} 必须是 uint32。`);
  }
  return result;
}

function alarmType(value) {
  const result = Number(value);
  if (![1, 2].includes(result)) fail('INVALID_INPUT', 'TYPE 只能是 1 或 2。');
  return result;
}

function activeValue(value) {
  if (value === true || value === 1 || value === '1') return true;
  if (value === false || value === 0 || value === '0') return false;
  fail('INVALID_INPUT', 'ACTIVE 必须是布尔值或 0/1。');
}

function hashId(value) {
  return crypto.createHash('sha256').update(String(value)).digest('hex').slice(0, 32);
}

function accountDocumentId(openid) {
  return `account_${hashId(openid)}`;
}

function bindingDocumentId(wearerOpenId, guardianOpenId) {
  return hashId(`${wearerOpenId}/${guardianOpenId}`);
}

// eventKey 继续保持客户端的 device/boot/id 形式；数据库键另加绑定命名空间。
function eventDocumentId(binding, eventKey) {
  return hashId(`${binding.bindingId}/${eventKey}`);
}

function statusDocumentId(binding) {
  return hashId(`${binding.bindingId}/${binding.wearableId}`);
}

function isNotFound(error) {
  const code = String(error && (error.code || error.errCode) || '').toUpperCase();
  const message = String(error && (error.message || error.errMsg) || '').toLowerCase();
  return code === 'DOCUMENT_NOT_FOUND' || code === 'DOC_NOT_FOUND' || /document.*(not found|does not exist)|not found/.test(message);
}

async function getDocOrNull(ref) {
  try {
    const result = await ref.get();
    return result && result.data || null;
  } catch (error) {
    if (isNotFound(error)) return null;
    throw error;
  }
}

function eventPublic(event) {
  return {
    eventKey: event.eventKey,
    wearableId: event.wearableId,
    boot: event.boot,
    id: event.id,
    alarmType: event.alarmType,
    active: event.active,
    origin: event.origin,
    deviceConfirmed: event.deviceConfirmed === true,
    occurredAt: event.occurredAt,
    acknowledged: Array.isArray(event.acknowledgedBy) && event.acknowledgedBy.length > 0
  };
}

function bindingPublic(binding, openid) {
  return {
    bindingId: binding.bindingId,
    role: binding.wearerOpenId === openid ? ROLE.WEARER : ROLE.GUARDIAN,
    wearableId: binding.wearableId,
    status: binding.status,
    createdAt: binding.createdAt
  };
}

async function queryOne(collection, where) {
  const result = await db.collection(collection).where(where).limit(1).get();
  return result.data && result.data[0] || null;
}

async function queryUserBindings(openid) {
  const [wearerResult, guardianResult] = await Promise.all([
    db.collection(COLLECTIONS.bindings).where({ wearerOpenId: openid }).limit(10).get(),
    db.collection(COLLECTIONS.bindings).where({ guardianOpenId: openid }).limit(10).get()
  ]);
  const byId = new Map();
  for (const item of [...(wearerResult.data || []), ...(guardianResult.data || [])]) {
    if (item && item._id) byId.set(item._id, item);
  }
  const bindings = Array.from(byId.values());
  if (bindings.length > 1) fail('BINDING_STATE_CONFLICT', '当前账号存在多个绑定记录，请先由服务端处理旧数据。');
  return bindings[0] || null;
}

function bindingMatchesAccountClaim(binding, claim, openid) {
  if (!binding || !claim) return false;
  if (binding.wearerOpenId !== openid && binding.guardianOpenId !== openid) return false;
  const expectedRole = binding.wearerOpenId === openid ? ROLE.WEARER : ROLE.GUARDIAN;
  return claim.openId === openid
    && claim.role === expectedRole
    && claim.bindingId === binding.bindingId
    && claim.wearerOpenId === binding.wearerOpenId
    && claim.guardianOpenId === binding.guardianOpenId
    && claim.wearableId === binding.wearableId;
}

async function getBindingForUser(openid) {
  const claim = await getDocOrNull(db.collection(COLLECTIONS.accounts).doc(accountDocumentId(openid)));
  if (claim) {
    const binding = await getDocOrNull(db.collection(COLLECTIONS.bindings).doc(claim.bindingId));
    if (!binding || !bindingMatchesAccountClaim(binding, claim, openid)) {
      fail('BINDING_STATE_CONFLICT', '账号绑定索引与绑定记录不一致。');
    }
    return binding;
  }
  // 兼容未建立 guardian_accounts 的旧模板数据，但一旦发现多个记录就拒绝猜测归属。
  return queryUserBindings(openid);
}

async function requireBinding(openid) {
  const binding = await getBindingForUser(openid);
  if (!binding || binding.status !== 'active') fail('NOT_BOUND', '当前账号尚未完成有效绑定。');
  if (!binding.bindingId || !binding.wearerOpenId || !binding.guardianOpenId || !binding.wearableId) {
    fail('BINDING_STATE_CONFLICT', '绑定记录缺少安全归属字段。');
  }
  if (binding.wearerOpenId !== openid && binding.guardianOpenId !== openid) {
    fail('OWNERSHIP_FORBIDDEN', '当前账号不属于此绑定。');
  }
  return binding;
}

function assertWearer(binding, openid) {
  if (binding.wearerOpenId !== openid) fail('ROLE_FORBIDDEN', '只有佩戴者账号可以执行此操作。');
}

function assertDevice(binding, requestedWearableId) {
  if (requestedWearableId !== binding.wearableId) fail('OWNERSHIP_FORBIDDEN', '设备不属于当前绑定。');
}

function makeAccountClaim(binding, openid) {
  return {
    openId: openid,
    role: binding.wearerOpenId === openid ? ROLE.WEARER : ROLE.GUARDIAN,
    bindingId: binding.bindingId,
    wearerOpenId: binding.wearerOpenId,
    guardianOpenId: binding.guardianOpenId,
    wearableId: binding.wearableId,
    status: binding.status,
    createdAt: binding.createdAt
  };
}

function inviteBelongsToBinding(invite, binding, openid) {
  return Boolean(invite
    && invite.usedAt !== null
    && invite.usedAt !== undefined
    && invite.usedBy === openid
    && invite.wearerOpenId === binding.wearerOpenId
    && invite.wearableId === binding.wearableId
    && binding.guardianOpenId === openid
    && (!invite.usedBindingId || invite.usedBindingId === binding.bindingId));
}

function inviteIsUsable(invite, code, now) {
  return Boolean(invite
    && invite.code === code
    && (invite.usedAt === null || invite.usedAt === undefined)
    && Number(invite.expiresAt) > now);
}

async function createInvite(openid, input) {
  const current = await getBindingForUser(openid);
  if (current) {
    assertWearer(current, openid);
  }
  const targetWearable = wearableId(input && input.wearableId);
  if (current) {
    // 已有监护关系的佩戴者只能继续为同一受监护设备生成邀请，不能借邀请替换设备。
    if (targetWearable !== current.wearableId) {
      fail('WEARABLE_MISMATCH', '已有绑定的受监护设备不能被邀请码替换。');
    }
  }
  const requestedTtl = Number(input && input.ttlMs);
  const ttl = Math.min(60 * 60 * 1000, Math.max(60 * 1000, Number.isFinite(requestedTtl) ? requestedTtl : DEFAULT_INVITE_TTL_MS));
  const code = crypto.randomBytes(4).toString('hex').toUpperCase();
  const now = Date.now();
  await db.collection(COLLECTIONS.invites).add({ data: {
    code,
    wearerOpenId: openid,
    wearableId: targetWearable,
    createdAt: now,
    expiresAt: now + ttl,
    usedAt: null,
    usedBy: null,
    usedBindingId: null
  } });
  return { invite: { code, expiresAt: now + ttl } };
}

async function acceptInvite(openid, input) {
  const code = text(input && input.code, '邀请码', 32).toUpperCase();
  const existingForGuardian = await getBindingForUser(openid);
  const invite = await queryOne(COLLECTIONS.invites, { code });

  // 网络重试必须返回第一次已经提交的绑定；新邀请码不能覆盖已有账号绑定。
  if (existingForGuardian) {
    if (inviteBelongsToBinding(invite, existingForGuardian, openid)) {
      return { binding: bindingPublic(existingForGuardian, openid) };
    }
    fail('ALREADY_BOUND', '当前账号已有绑定，不能重复接受邀请码。');
  }
  const now = Date.now();
  if (!invite || (invite.usedAt !== null && invite.usedAt !== undefined) || Number(invite.expiresAt) <= now) {
    fail('INVITE_INVALID', '邀请码不存在、已使用或已过期。');
  }
  if (invite.wearerOpenId === openid) fail('SELF_BINDING', '佩戴者不能绑定自己为监护者。');

  // 旧数据没有 account claim 时仍先检查邀请方，避免用新记录替换已有受监护设备。
  const existingForWearer = await getBindingForUser(invite.wearerOpenId);
  if (existingForWearer) {
    if (existingForWearer.wearableId !== invite.wearableId) {
      fail('WEARABLE_MISMATCH', '已有绑定的受监护设备不能被邀请码替换。');
    }
    fail('ALREADY_BOUND', '受监护账号已有绑定，不能重复建立绑定。');
  }

  const bindingId = bindingDocumentId(invite.wearerOpenId, openid);
  const binding = {
    bindingId,
    wearerOpenId: invite.wearerOpenId,
    guardianOpenId: openid,
    wearableId: invite.wearableId,
    status: 'active',
    createdAt: now
  };

  return db.runTransaction(async (transaction) => {
    // 所有唯一性检查和写入都在同一事务中；guardian_accounts 的确定性文档键是账号唯一闸门。
    const inviteRef = transaction.collection(COLLECTIONS.invites).doc(invite._id);
    const guardianAccountRef = transaction.collection(COLLECTIONS.accounts).doc(accountDocumentId(openid));
    const wearerAccountRef = transaction.collection(COLLECTIONS.accounts).doc(accountDocumentId(invite.wearerOpenId));
    const bindingRef = transaction.collection(COLLECTIONS.bindings).doc(bindingId);
    const latestInvite = await getDocOrNull(inviteRef);
    const guardianClaim = await getDocOrNull(guardianAccountRef);
    const wearerClaim = await getDocOrNull(wearerAccountRef);
    const existingBinding = await getDocOrNull(bindingRef);

    if (!latestInvite || latestInvite.code !== code) {
      fail('INVITE_INVALID', '邀请码不存在、已使用或已过期。');
    }

    if (guardianClaim) {
      const claimedBinding = await getDocOrNull(transaction.collection(COLLECTIONS.bindings).doc(guardianClaim.bindingId));
      if (!claimedBinding || !bindingMatchesAccountClaim(claimedBinding, guardianClaim, openid)) {
        fail('BINDING_STATE_CONFLICT', '账号绑定索引与绑定记录不一致。');
      }
      if (inviteBelongsToBinding(latestInvite, claimedBinding, openid)) {
        return { binding: bindingPublic(claimedBinding, openid) };
      }
      fail('ALREADY_BOUND', '当前账号已有绑定，不能重复接受邀请码。');
    }

    if (wearerClaim) {
      const claimedBinding = await getDocOrNull(transaction.collection(COLLECTIONS.bindings).doc(wearerClaim.bindingId));
      if (!claimedBinding || !bindingMatchesAccountClaim(claimedBinding, wearerClaim, invite.wearerOpenId)) {
        fail('BINDING_STATE_CONFLICT', '账号绑定索引与绑定记录不一致。');
      }
      if (claimedBinding.wearableId !== latestInvite.wearableId) {
        fail('WEARABLE_MISMATCH', '已有绑定的受监护设备不能被邀请码替换。');
      }
      fail('ALREADY_BOUND', '受监护账号已有绑定，不能重复建立绑定。');
    }

    if (!inviteIsUsable(latestInvite, code, Date.now())) {
      fail('INVITE_INVALID', '邀请码不存在、已使用或已过期。');
    }
    if (latestInvite.wearerOpenId === openid) fail('SELF_BINDING', '佩戴者不能绑定自己为监护者。');
    if (latestInvite.wearableId !== binding.wearableId) fail('WEARABLE_MISMATCH', '邀请码设备归属已变化。');

    if (existingBinding) {
      // 确定性 bindingId 已有文档时也不能用邀请码覆盖其设备或账号。
      if (existingBinding.wearerOpenId !== binding.wearerOpenId
        || existingBinding.guardianOpenId !== binding.guardianOpenId
        || existingBinding.wearableId !== binding.wearableId) {
        fail('OWNERSHIP_FORBIDDEN', '已有绑定记录不能被替换。');
      }
      fail('ALREADY_BOUND', '绑定记录已存在。');
    }

    const usedAt = Date.now();
    await bindingRef.set({ data: binding });
    await guardianAccountRef.set({ data: makeAccountClaim(binding, openid) });
    await wearerAccountRef.set({ data: makeAccountClaim(binding, binding.wearerOpenId) });
    await inviteRef.update({ data: { usedAt, usedBy: openid, usedBindingId: binding.bindingId } });
    return { binding: bindingPublic(binding, openid) };
  });
}

async function getBinding(openid) {
  const binding = await requireBinding(openid);
  return { role: binding.wearerOpenId === openid ? ROLE.WEARER : ROLE.GUARDIAN, binding: bindingPublic(binding, openid) };
}

async function listEvents(openid, input) {
  const binding = await requireBinding(openid);
  const requestedWearable = input && (input.wearableId !== undefined ? input.wearableId : input.deviceId);
  if (requestedWearable !== undefined) assertDevice(binding, wearableId(requestedWearable));
  const limit = Math.min(50, Math.max(1, Number(input && input.limit) || 50));
  const result = await db.collection(COLLECTIONS.events)
    .where({
      bindingId: binding.bindingId,
      wearerOpenId: binding.wearerOpenId,
      wearableId: binding.wearableId
    })
    .orderBy('occurredAt', 'desc')
    .limit(limit)
    .get();
  return {
    events: (result.data || [])
      .filter((event) => event.bindingId === binding.bindingId
        && event.wearerOpenId === binding.wearerOpenId
        && event.wearableId === binding.wearableId)
      .map(eventPublic)
  };
}

function normalizeEvent(input, binding) {
  const value = input || {};
  if (value.wearableId !== undefined && value.deviceId !== undefined
    && String(value.wearableId).trim() !== String(value.deviceId).trim()) {
    fail('INVALID_INPUT', 'wearableId 与 deviceId 不一致。');
  }
  const targetWearable = wearableId(value.wearableId !== undefined ? value.wearableId : value.deviceId);
  assertDevice(binding, targetWearable);
  const boot = uint32(value.boot, 'BOOT');
  const id = uint32(value.id, 'ID');
  const type = alarmType(value.alarmType !== undefined ? value.alarmType : value.type);
  const active = activeValue(value.active);

  // origin 只是兼容字段，不是认证凭据；真正的权限来自 OPENID、active binding 和服务端设备匹配。
  if (value.origin !== undefined && String(value.origin || '').trim() !== 'device') {
    fail('INVALID_INPUT', '事件只能由设备来源上报。');
  }
  const eventKey = `${targetWearable}/${boot}/${id}`;
  if (value.eventKey && String(value.eventKey).trim() !== eventKey) {
    fail('INVALID_INPUT', 'eventKey 与设备/BOOT/ID 不一致。');
  }
  const rawTimestamp = value.timestamp !== undefined ? value.timestamp : value.occurredAt;
  const clientTimestamp = Number(rawTimestamp);
  const now = Date.now();
  const occurredAt = Number.isFinite(clientTimestamp) && Math.abs(now - clientTimestamp) <= MAX_EVENT_AGE_MS ? clientTimestamp : now;
  return {
    eventKey,
    bindingId: binding.bindingId,
    wearerOpenId: binding.wearerOpenId,
    wearableId: targetWearable,
    boot,
    id,
    alarmType: type,
    active,
    origin: 'device',
    deviceConfirmed: true,
    occurredAt,
    receivedAt: now,
    acknowledgedBy: []
  };
}

function assertExistingEventOwnership(existing, binding, next) {
  if (existing.wearerOpenId !== binding.wearerOpenId
    || existing.bindingId !== binding.bindingId
    || existing.wearableId !== binding.wearableId) {
    fail('OWNERSHIP_FORBIDDEN', '事件不属于当前绑定。');
  }
  if (existing.eventKey !== next.eventKey
    || existing.boot !== next.boot
    || existing.id !== next.id) {
    fail('EVENT_COLLISION', '事件唯一键冲突，请更换设备标识。');
  }
  if (existing.alarmType !== next.alarmType) {
    fail('EVENT_COLLISION', '同一事件唯一键不能更改报警类型。');
  }
}

async function publishEvent(openid, input) {
  const binding = await requireBinding(openid);
  assertWearer(binding, openid);
  const next = normalizeEvent(input && input.event, binding);
  const documentId = eventDocumentId(binding, next.eventKey);

  const result = await db.runTransaction(async (transaction) => {
    const ref = transaction.collection(COLLECTIONS.events).doc(documentId);
    const existing = await getDocOrNull(ref);
    if (!existing) {
      await ref.set({ data: next });
      return { event: eventPublic(next), deduplicated: false };
    }

    assertExistingEventOwnership(existing, binding, next);
    if (existing.active !== true && existing.active !== false) {
      fail('EVENT_STATE_CONFLICT', '事件活动状态无效。');
    }

    // 事务内重新读取并条件更新，保证已经由设备确认解除的事件不能被旧 ACTIVE 复活。
    if (existing.active === false && next.active === true) {
      return { event: eventPublic(existing), deduplicated: true };
    }
    const update = {
      active: existing.active === false ? false : next.active,
      occurredAt: Math.min(Number(existing.occurredAt) || next.occurredAt, next.occurredAt),
      receivedAt: next.receivedAt,
      deviceConfirmed: true
    };
    await ref.update({ data: update });
    return {
      event: eventPublic({ ...existing, ...update, acknowledgedBy: existing.acknowledgedBy || [] }),
      deduplicated: true
    };
  });
  return result;
}

function recordBelongsToBinding(record, binding) {
  return Boolean(record
    && record.bindingId === binding.bindingId
    && record.wearerOpenId === binding.wearerOpenId
    && record.wearableId === binding.wearableId);
}

async function touchStatus(openid, input) {
  const binding = await requireBinding(openid);
  assertWearer(binding, openid);
  const targetWearable = wearableId(input && input.wearableId);
  assertDevice(binding, targetWearable);
  const status = {
    bindingId: binding.bindingId,
    wearerOpenId: binding.wearerOpenId,
    wearableId: targetWearable,
    online: input && input.online !== false,
    lastSeenAt: Date.now()
  };
  await db.runTransaction(async (transaction) => {
    const ref = transaction.collection(COLLECTIONS.statuses).doc(statusDocumentId(binding));
    const existing = await getDocOrNull(ref);
    if (existing && !recordBelongsToBinding(existing, binding)) fail('OWNERSHIP_FORBIDDEN', '状态不属于当前绑定。');
    if (existing) await ref.update({ data: status });
    else await ref.set({ data: status });
  });
  return { status: { wearableId: targetWearable, online: status.online, lastSeenAt: status.lastSeenAt } };
}

async function getStatus(openid) {
  const binding = await requireBinding(openid);
  const saved = await getDocOrNull(db.collection(COLLECTIONS.statuses).doc(statusDocumentId(binding)));
  if (saved && !recordBelongsToBinding(saved, binding)) fail('OWNERSHIP_FORBIDDEN', '状态不属于当前绑定。');
  const lastSeenAt = Number(saved && saved.lastSeenAt) || 0;
  const now = Date.now();
  const online = Boolean(saved && saved.online && lastSeenAt && lastSeenAt <= now && now - lastSeenAt <= OFFLINE_AFTER_MS);
  return { status: { wearableId: binding.wearableId, online, lastSeenAt } };
}

async function acknowledgeEvent(openid, input) {
  const binding = await requireBinding(openid);
  const eventKey = text(input && input.eventKey, 'eventKey', 256);
  return db.runTransaction(async (transaction) => {
    const ref = transaction.collection(COLLECTIONS.events).doc(eventDocumentId(binding, eventKey));
    const event = await getDocOrNull(ref);
    if (!event || event.eventKey !== eventKey || !recordBelongsToBinding(event, binding)) {
      fail('EVENT_NOT_FOUND', '报警事件不存在或不属于当前绑定。');
    }
    const acknowledgedBy = Array.from(new Set([...(event.acknowledgedBy || []), openid]));
    await ref.update({ data: { acknowledgedBy } });
    return { acknowledged: true, eventKey };
  });
}

async function route(openid, request) {
  const action = String(request && request.action || '');
  if (action === 'createInvite') return createInvite(openid, request);
  if (action === 'acceptInvite') return acceptInvite(openid, request);
  if (action === 'getBinding') return getBinding(openid);
  if (action === 'listEvents') return listEvents(openid, request);
  if (action === 'publishEvent') return publishEvent(openid, request);
  if (action === 'touchStatus') return touchStatus(openid, request);
  if (action === 'getStatus') return getStatus(openid);
  if (action === 'acknowledgeEvent') return acknowledgeEvent(openid, request);
  if (action === 'resolveEvent') fail('REMOTE_RESOLVE_FORBIDDEN', '监护者不能远程解除固件报警。');
  fail('UNKNOWN_ACTION', '不支持的监护服务操作。');
}

exports.main = async (event) => {
  try {
    const openid = contextOpenId();
    const result = await route(openid, event || {});
    return { ok: true, ...result };
  } catch (error) {
    return {
      ok: false,
      code: error && error.code || 'GUARDIAN_INTERNAL_ERROR',
      message: error && error.message || '监护服务暂时不可用。'
    };
  }
};
