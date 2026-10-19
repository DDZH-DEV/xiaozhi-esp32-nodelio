#!/usr/bin/env node
// verify-live.mjs — 对真实运行中的 bridge 发请求，并把返回的 Envelope 送进官方 schema。
//
//   node server.mjs --port 8791 &
//   node verify-live.mjs --port 8791 --workspace "D:\\xz25"
//
// 与 validate.mjs 的区别：validate.mjs 校验手写样例，这个校验**运行时真实产出**。

import Ajv2020 from 'ajv/dist/2020.js';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

function flag(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const PROTOCOL_ROOT =
  process.env.NODELIO_PROTOCOL_ROOT ??
  path.resolve(HERE, '..', '..', '..', '..', 'SITES', 'NodeLio', 'Protocol');
const SCHEMA_DIR = path.join(PROTOCOL_ROOT, 'schemas');
const BASE = `http://127.0.0.1:${flag('--port', '8790')}`;
const WORKSPACE = flag('--workspace', 'D:\\xz25');

const ajv = new Ajv2020({ allErrors: true, strict: false, validateFormats: false });
const ids = new Map();
for (const name of fs.readdirSync(SCHEMA_DIR).filter((n) => n.endsWith('.schema.json'))) {
  const schema = JSON.parse(fs.readFileSync(path.join(SCHEMA_DIR, name), 'utf8'));
  ids.set(name, schema.$id);
  ajv.addSchema(schema, schema.$id);
}
const envelope = ajv.getSchema(ids.get('content-envelope.schema.json'));

let failed = 0;
function check(label, payload) {
  if (envelope(payload)) {
    console.log(`ok    ${label}  (status=${payload.status}, content=${payload.content?.length ?? 0})`);
    console.log(`      spoken: ${payload.spoken?.text ?? '-'}`);
  } else {
    failed += 1;
    console.log(`FAIL  ${label}`);
    for (const error of envelope.errors ?? []) {
      console.log(`        ${error.instancePath || '/'} ${error.message}`);
    }
  }
}

async function get(url) {
  const response = await fetch(url);
  return response.json();
}

async function post(url, body) {
  const response = await fetch(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  });
  return response.json();
}

// 1. status：列出工作区任务
check('GET /nodelio/dsh/status', await get(`${BASE}/nodelio/dsh/status?workspace=${encodeURIComponent(WORKSPACE)}`));

// 2. status：查不到的任务（空结果也要是合法 Envelope）
check(
  'GET /nodelio/dsh/status (no match)',
  await get(`${BASE}/nodelio/dsh/status?session=__no_such_session__`),
);

// 3. task：缺参数 → 必须是 status:error + error.code
check('POST /nodelio/dsh/task (missing instruction)', await post(`${BASE}/nodelio/dsh/task`, {}));

// 4. answer：缺参数 → 同上
check('POST /nodelio/dsh/answer (missing notice)', await post(`${BASE}/nodelio/dsh/answer`, {}));

// 5. 未知路由 → 也必须是合法 Envelope，不能是裸 404 文本
const notFound = await get(`${BASE}/nope`);
check('GET /nope (unknown route)', notFound);

console.log(failed === 0 ? '\nall live envelopes valid' : `\n${failed} live envelope(s) invalid`);

// Windows + undici: process.exit() 时若 keep-alive socket 仍打开，会触发 libuv
// 的 UV_HANDLE_CLOSING 断言（退出码 1，但校验其实是过的）。先关掉连接池再退。
const { getGlobalDispatcher } = await import('node:undici').catch(() => ({ getGlobalDispatcher: null }));
if (getGlobalDispatcher) await getGlobalDispatcher().close().catch(() => {});
process.exitCode = failed === 0 ? 0 : 1;
