#!/usr/bin/env node
// server.mjs — 把 marketplace/ 的三个应用真的跑起来。
//
//   node server.mjs                 # 默认 127.0.0.1:8790
//   node server.mjs --port 9000
//
// 端点与 packages/*/invoke.url 的路径后缀一致：
//   POST /nodelio/dsh/task     { instruction, workspace? }
//   GET  /nodelio/dsh/status?workspace=&session=
//   POST /nodelio/dsh/answer   { session, notice_id, action? }
//
// 返回的全部是 Protocol v1 Content Envelope，可直接喂 validate.mjs 的 schema 校验。
// 设计约束：三个端点都必须在 30 秒内返回（协议 invoke.timeout_ms 上限），
// 所以 task 只投递不同步等待。

import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import crypto from 'node:crypto';

import { connect } from '../lib/rpc.mjs';
import { ATTENTION, decodeSessionLog, foldSession, sessionLogPath, speakTask } from '../lib/tasks.mjs';

const DSH_HOME = process.env.DSH_HOME ?? path.join(os.homedir(), '.dsh');
const PORT = Number(readFlag('--port') ?? 8790);
const HOST = readFlag('--host') ?? '127.0.0.1';

function readFlag(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

/** 协议要求 request_id 匹配 ^[A-Za-z0-9_-]{1,64}$。 */
const newRequestId = () => 'req_' + crypto.randomBytes(8).toString('hex');

/** 协议要求 spoken.text 是给 TTS 的短句；截断到设备友好长度。 */
const speak = (text) => ({ text: text.slice(0, 160), language: 'zh-CN' });

function errorEnvelope(appId, code, message, spokenText) {
  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: appId,
    status: 'error',
    error: { code, message: message.slice(0, 512), retryable: code !== 'config_missing' },
    spoken: speak(spokenText ?? message),
  };
}

/** 把一个 DSH 会话折叠成一行 list_item。 */
function taskRow(session, state, folded) {
  const running = state?.status === 'running';
  const bits = [];
  bits.push(state?.status ?? 'unknown');
  if (folded) {
    bits.push(`turn ${folded.turn} / step ${folded.step}`);
    if (folded.lastTool) bits.push(folded.lastTool);
  }
  return {
    type: 'list_item',
    id: session.externalSessionId.slice(0, 128),
    title: (session.title ?? '(untitled)').slice(0, 120),
    subtitle: bits.join(' · ').slice(0, 200),
  };
}

/** 读一个会话的落盘日志并折叠；文件不存在时返回 null。 */
function foldFromDisk(session) {
  const file = sessionLogPath(DSH_HOME, session.cwd, session.externalSessionId);
  if (!fs.existsSync(file)) return null;
  return foldSession(decodeSessionLog(file), { cwd: session.cwd });
}

async function readTask(client, session) {
  const [state, notices] = await Promise.all([
    client.call('session.getState', { externalSessionId: session.externalSessionId }).catch(() => null),
    client.call('session.getNotices', { externalSessionId: session.externalSessionId }).catch(() => ({ notices: [] })),
  ]);
  return {
    session,
    state,
    folded: foldFromDisk(session),
    notices: notices?.notices ?? [],
  };
}

// ---------------------------------------------------------------- /status

async function handleStatus(client, query) {
  const workspace = query.get('workspace') ?? '';
  const reference = query.get('session') ?? '';

  const { sessions } = await client.call('session.list', {});
  let pool = sessions;
  if (workspace) pool = pool.filter((s) => s.cwd === workspace);
  if (reference) {
    pool = pool.filter(
      (s) =>
        s.externalSessionId.includes(reference) ||
        s.sessionId.includes(reference) ||
        (s.title ?? '').includes(reference),
    );
  }

  if (pool.length === 0) {
    return {
      schema_version: 1,
      request_id: newRequestId(),
      app_id: 'dsh.status',
      status: 'ok',
      content: [],
      display: {
        card_type: 'list',
        title: workspace || 'DSH 任务',
        preferred_surface: 'card',
        density: 'compact',
        max_items: 8,
        ttl_sec: 10,
      },
      spoken: speak(reference ? '没找到这个任务。' : '这个工作区没有任务。'),
    };
  }

  const detailed = [];
  for (const session of pool.slice(0, 8)) {
    detailed.push(await readTask(client, session));
  }

  const running = detailed.find((t) => t.state?.status === 'running');
  const needsYou = detailed.find((t) => ATTENTION.has(t.state?.status ?? ''));
  const focus = needsYou ?? running ?? detailed[0];

  const spoken = needsYou
    ? `${focus.session.title} 卡住了，等你确认。`
    : running
      ? speakTask({ ...running.folded, running: true, needsYou: false })
      : detailed.length === 1
        ? speakTask({ ...focus.folded, running: false, needsYou: false })
        : `共 ${detailed.length} 个任务，都没有在跑。`;

  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: 'dsh.status',
    status: 'ok',
    content: detailed.map((t) => taskRow(t.session, t.state, t.folded)),
    display: {
      card_type: 'list',
      title: (workspace || 'DSH 任务').slice(0, 60),
      preferred_surface: 'card',
      density: 'compact',
      max_items: 8,
      ttl_sec: 10,
    },
    spoken: speak(spoken),
  };
}

// ---------------------------------------------------------------- /task

async function handleTask(client, body) {
  const instruction = typeof body.instruction === 'string' ? body.instruction.trim() : '';
  if (!instruction) {
    return errorEnvelope('dsh.task', 'config_missing', '缺少 instruction 参数。', '没听清要做什么。');
  }
  const workspace = typeof body.workspace === 'string' && body.workspace ? body.workspace : null;

  const { sessions } = await client.call('session.list', {});

  // 选目标：指定工作区的空闲会话 → 任意空闲会话 → 新建。
  let target = null;
  if (workspace) {
    for (const session of sessions.filter((s) => s.cwd === workspace)) {
      const state = await client.call('session.getState', { externalSessionId: session.externalSessionId }).catch(() => null);
      if (state?.status === 'idle') {
        target = session;
        break;
      }
    }
  }

  let created = false;
  let result;
  if (target) {
    result = await client.call('session.startTurn', {
      sessionId: target.sessionId,
      externalSessionId: target.externalSessionId,
      content: instruction,
      clientMessageId: crypto.randomUUID(),
    });
  } else {
    if (!workspace) {
      return errorEnvelope('dsh.task', 'config_missing', '没有可用工作区，请指定 workspace。', '不知道投到哪个工作区。');
    }
    created = true;
    result = await client.call('session.createAndStart', {
      sessionId: crypto.randomUUID(),
      content: instruction,
      cwd: workspace,
      clientMessageId: crypto.randomUUID(),
    });
  }

  // 立即返回清单，不等执行完成——这是协议 timeout_ms ≤ 30000 的硬约束决定的。
  const { sessions: after } = await client.call('session.list', {});
  const sameWorkspace = after.filter((s) => s.cwd === (workspace ?? target?.cwd ?? ''));

  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: 'dsh.task',
    status: 'ok',
    content: sameWorkspace.slice(0, 8).map((s) => ({
      type: 'list_item',
      id: s.externalSessionId.slice(0, 128),
      title: (s.title ?? '(untitled)').slice(0, 120),
      subtitle: (s.metadata?.live ? 'live' : 'cold') + ' · 已投递',
    })),
    display: {
      card_type: 'list',
      title: (workspace ?? target?.cwd ?? 'DSH').slice(0, 60),
      preferred_surface: 'card',
      density: 'compact',
      max_items: 8,
      ttl_sec: 30,
    },
    spoken: speak(created ? '已新建任务并开始。' : '已派活。'),
  };
}

// ---------------------------------------------------------------- /answer

async function handleAnswer(client, body) {
  const sessionRef = typeof body.session === 'string' ? body.session : '';
  const noticeId = typeof body.notice_id === 'string' ? body.notice_id : '';
  const action = body.action === 'deny' ? 'deny' : 'allow';

  if (!sessionRef || !noticeId) {
    return errorEnvelope('dsh.answer', 'config_missing', '需要 session 与 notice_id。', '缺少确认信息。');
  }

  const { sessions } = await client.call('session.list', {});
  const session = sessions.find(
    (s) => s.externalSessionId.includes(sessionRef) || s.sessionId.includes(sessionRef),
  );
  if (!session) {
    return errorEnvelope('dsh.answer', 'not_installed', '找不到该任务。', '找不到这个任务。');
  }

  await client.call('session.respondInteraction', {
    sessionId: session.sessionId,
    noticeId,
    actionId: action,
  });

  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: 'dsh.answer',
    status: 'ok',
    content: [
      {
        type: 'text',
        id: 'ack',
        text: action === 'allow' ? '已允许。' : '已拒绝。',
        markdown: false,
      },
    ],
    display: {
      card_type: 'plain_result',
      title: 'DSH 确认',
      preferred_surface: 'card',
      density: 'compact',
      max_items: 1,
    },
    spoken: speak(action === 'allow' ? '已允许。' : '已拒绝。'),
  };
}

// ---------------------------------------------------------------- server

function readBody(request) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    request.on('data', (chunk) => {
      size += chunk.length;
      if (size > 64 * 1024) {
        reject(new Error('body too large'));
        request.destroy();
        return;
      }
      chunks.push(chunk);
    });
    request.on('end', () => {
      const text = Buffer.concat(chunks).toString('utf8');
      if (!text.trim()) return resolve({});
      try {
        resolve(JSON.parse(text));
      } catch {
        reject(new Error('invalid JSON body'));
      }
    });
    request.on('error', reject);
  });
}

function send(response, status, payload) {
  const body = JSON.stringify(payload, null, 2);
  response.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(body),
  });
  response.end(body);
}

const server = http.createServer(async (request, response) => {
  const url = new URL(request.url, `http://${request.headers.host ?? 'localhost'}`);
  const route = `${request.method} ${url.pathname}`;
  console.log(`${new Date().toISOString()}  ${route}`);

  let client;
  try {
    client = await connect({ namespace: 'nodelio-market' });

    if (route === 'GET /nodelio/dsh/status') {
      send(response, 200, await handleStatus(client, url.searchParams));
    } else if (route === 'POST /nodelio/dsh/task') {
      send(response, 200, await handleTask(client, await readBody(request)));
    } else if (route === 'POST /nodelio/dsh/answer') {
      send(response, 200, await handleAnswer(client, await readBody(request)));
    } else {
      send(response, 404, errorEnvelope('dsh.status', 'internal', `no route: ${route}`, '路径不对。'));
    }
  } catch (error) {
    // 协议 03 §3.1：即使失败也必须返回合法 Envelope，不能只回 HTTP 5xx + 文本。
    const code = /ENOENT|ECONNREFUSED|endpoint not found/i.test(error.message)
      ? 'upstream_error'
      : 'internal';
    send(response, 200, errorEnvelope('dsh.status', code, error.message, 'DSH 没响应，可能工作台没开。'));
  } finally {
    client?.close();
  }
});

server.listen(PORT, HOST, () => {
  console.log(`nodelio market bridge listening on http://${HOST}:${PORT}`);
  console.log('  GET  /nodelio/dsh/status?workspace=&session=');
  console.log('  POST /nodelio/dsh/task    {"instruction":"...","workspace":"..."}');
  console.log('  POST /nodelio/dsh/answer  {"session":"...","notice_id":"...","action":"allow"}');
});
