#!/usr/bin/env node
// server.mjs — DSH ↔ NodeLio Envelope 桥（家庭 LAN）。
//
//   node server.mjs                         # 0.0.0.0:8790
//   node server.mjs --host 127.0.0.1         # 仅本机
//   node server.mjs --port 9000
//   node server.mjs --watch                 # 边沿监视 → Host skill_hint
//
// 端点：
//   GET  /nodelio/dsh/health
//   POST /nodelio/dsh/task     { instruction, workspace? }
//   GET  /nodelio/dsh/status?workspace=&session=
//   POST /nodelio/dsh/answer   { session, notice_id, action? }
//   GET  /nodelio-dsh-market/**          市场索引与包（免拷到 Host）
//
// 设计：三个业务端点必须在 30s 内返回；task 只投递不等执行完。

import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

import { connect, readEndpoint } from '../lib/rpc.mjs';
import {
  ATTENTION,
  decodeSessionLog,
  displayTaskSubtitle,
  displayTaskTitle,
  foldSession,
  lastReplyPlain,
  sessionLogPath,
  speakStatusList,
  speakWorkspaceList,
} from '../lib/tasks.mjs';
import { chunkSpokenText, synthesizeSpoken, ttsFilePath, TTS_TEXT_MAX } from '../lib/tts.mjs';
import { startHostWs } from '../lib/host-ws.mjs';
import {
  loadConfig,
  getConfig,
  getPublicConfig,
  saveConfig,
  onConfigChange,
  watchConfigFile,
  ensureExample,
  CONFIG_PATH,
} from '../lib/bridge-config.mjs';
import { listCreateCatalog, resolveCreateParams } from '../lib/dsh-create.mjs';

const DSH_HOME = process.env.DSH_HOME ?? path.join(os.homedir(), '.dsh');
/** 本目录即市场根（marketplace.json + packages/） */
const MARKET_ROOT = path.dirname(fileURLToPath(import.meta.url));
const MARKET_PREFIX = '/nodelio-dsh-market';
const PUBLIC_DIR = path.resolve(MARKET_ROOT, '..', 'public');

function readFlag(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

/* CLI / 环境覆盖进 process.env，再 loadConfig（文件优先于默认，env 在 sanitize 里作底） */
if (readFlag('--port')) process.env.DSH_BRIDGE_PORT = readFlag('--port');
if (readFlag('--host')) process.env.DSH_BRIDGE_HOST = readFlag('--host');
if (process.argv.includes('--watch')) process.env.DSH_BRIDGE_WATCH = '1';

ensureExample();
loadConfig();
watchConfigFile();

function cfg() {
  return getConfig();
}
function wantLan() {
  const m = cfg().mode;
  return m === 'lan' || m === 'both';
}
function wantHostWs() {
  const m = cfg().mode;
  return m === 'host_ws' || m === 'both';
}

/** @type {ReturnType<typeof startHostWs> | null} */
let hostWsClient = null;
let watchTimer = null;
let hostWsConnected = false;

const newRequestId = () => 'req_' + crypto.randomBytes(8).toString('hex');
const speak = (text) => ({ text: String(text ?? '').slice(0, 300), language: 'zh-CN' });

function listenPort() {
  return cfg().listen_port || 8790;
}

function publicBaseFromRequest(request) {
  const envBase = (cfg().public_url || process.env.DSH_BRIDGE_PUBLIC_URL || '').replace(/\/$/, '');
  if (envBase) return envBase;
  const port = listenPort();
  const host = request?.headers?.host ?? '';
  /* Host 若是本机回环，面板拉不到音频 → 改用 LAN IP */
  if (host && !/^(localhost|127\.0\.0\.1)(:|$)/i.test(host)) {
    return `http://${host}`;
  }
  const lans = lanAddresses();
  if (lans[0]) return `http://${lans[0]}:${port}`;
  if (host) return `http://${host}`;
  return `http://127.0.0.1:${port}`;
}

/** 文本 + 可选本机音频（Edge 神经声 / SAPI；面板 Idle 后 HTTP 播报）. */
async function speakWithAudio(text, request) {
  return speakWithAudioParts(text, request, 1);
}

/** 多段：按句读切分；spoken.audio.url=首段，urls=[…] 连续播。 */
async function speakWithAudioParts(text, request, maxChunks = 6) {
  const parts = chunkSpokenText(text, TTS_TEXT_MAX, maxChunks);
  const spoken = speak(parts.join('') || text);
  if (parts.length === 0) return spoken;
  const base = publicBaseFromRequest(request);
  const urls = [];
  let mime = 'audio/wav';
  try {
    for (let i = 0; i < parts.length; i++) {
      const t0 = Date.now();
      const r = await synthesizeSpoken(parts[i]);
      if (!r?.name) {
        console.warn(`[tts] part ${i} null`);
        continue;
      }
      mime = r.mime || mime;
      const url = `${base}/nodelio/dsh/tts/${r.name}`;
      urls.push(url);
      console.log(
        `[tts] ok part=${i + 1}/${parts.length} ${r.name} ${Date.now() - t0}ms len=${[...parts[i]].length}`,
      );
    }
  } catch (e) {
    console.warn(`[tts] fail: ${e.message}`);
  }
  if (urls.length > 0) {
    spoken.audio = { url: urls[0], urls, mime };
  }
  return spoken;
}

function tryServeTts(request, response, pathname) {
  if (request.method !== 'GET' || !pathname.startsWith('/nodelio/dsh/tts/')) return false;
  const name = pathname.slice('/nodelio/dsh/tts/'.length);
  const file = ttsFilePath(name);
  if (!file || !fs.existsSync(file)) {
    response.writeHead(404).end('not found');
    return true;
  }
  const body = fs.readFileSync(file);
  const ctype = name.endsWith('.mp3') ? 'audio/mpeg' : 'audio/wav';
  response.writeHead(200, {
    'content-type': ctype,
    'content-length': body.length,
    'cache-control': 'public, max-age=600',
  });
  response.end(body);
  return true;
}

function errorEnvelope(appId, code, message, spokenText) {
  const msg = String(message).slice(0, 512);
  const spoken = speak(spokenText ?? message);
  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: appId,
    status: 'error',
    error: { code, message: msg, retryable: code !== 'config_missing' },
    content: [{ type: 'error', message: msg }],
    display: {
      card_type: 'list',
      title: 'DSH',
      preferred_surface: 'card',
      density: 'comfortable',
      max_items: 3,
      ttl_sec: 8,
    },
    spoken,
  };
}

function lanAddresses() {
  const out = [];
  const ifaces = os.networkInterfaces();
  for (const list of Object.values(ifaces)) {
    for (const a of list ?? []) {
      if (a.family === 'IPv4' && !a.internal) out.push(a.address);
    }
  }
  return out;
}

function resolveWorkspace(body) {
  if (typeof body.workspace === 'string' && body.workspace.trim()) return body.workspace.trim();
  if (typeof body.default_workspace === 'string' && body.default_workspace.trim()) {
    return body.default_workspace.trim();
  }
  return null;
}

/** 首条 open notice；无则 null */
function primaryNotice(notices) {
  if (!Array.isArray(notices) || notices.length === 0) return null;
  return notices.find((n) => String(n.status || 'open') === 'open') ?? notices[0];
}

/**
 * DSH notice 两类：
 * - approval：二元允许/拒绝（actionId allow|deny|approve|reject）
 * - input_request：ask_user_question 多选/填空（submit+schema），墙板不能装成允许/拒绝
 */
function classifyNotice(notice) {
  if (!notice) return null;
  const itype = String(notice.interactionType || '');
  if (itype === 'input_request') return 'input_request';
  const actions = Array.isArray(notice.actions) ? notice.actions : [];
  const ids = actions.map((a) => String(a.actionId || a.id || '').toLowerCase());
  if (ids.some((id) => id === 'allow' || id === 'deny' || id === 'approve' || id === 'reject')) {
    return 'approval';
  }
  if (ids.includes('submit') || notice.source?.component === 'dsh.ask_user_question') {
    return 'input_request';
  }
  if (itype === 'approval' || itype === 'permission') return 'approval';
  return actions.length ? 'approval' : null;
}

function noticeIdOf(notice) {
  return String(notice?.id ?? notice?.noticeId ?? '').slice(0, 128);
}

function inputRequestQuestions(notice) {
  const submit = (notice?.actions || []).find(
    (a) => String(a.actionId || a.id || '') === 'submit' && a.input,
  );
  const questions = submit?.input?.uiSchema?.questions;
  return Array.isArray(questions) ? questions : [];
}

/** 多题 / 无选项：纯文本（详情 body）；单题有选项走列表点选 */
function formatInputRequestPlain(notice) {
  const questions = inputRequestQuestions(notice);
  if (questions.length === 0) {
    const t = String(notice?.title || '').trim();
    return t ? `${t}\n\n请在电脑 DSH 里回答。` : '';
  }
  const blocks = [];
  for (const q of questions) {
    const header = String(q.header || '').trim();
    const prompt = String(q.prompt || q.question || '').trim();
    const lines = [];
    if (header) lines.push(`【${header}】`);
    if (prompt) lines.push(prompt);
    const opts = Array.isArray(q.options) ? q.options : [];
    opts.forEach((o, i) => {
      const label = String(o.label || o.id || '').trim();
      const desc = String(o.description || '').trim();
      lines.push(desc ? `${i + 1}. ${label} — ${desc}` : `${i + 1}. ${label}`);
    });
    if (q.allowCustom) lines.push(`${opts.length + 1}. （可在电脑输入自定义回答）`);
    blocks.push(lines.join('\n'));
  }
  blocks.push(
    questions.length > 1
      ? '本题含多问，请在电脑 DSH 弹窗里答完提交。'
      : '请在电脑 DSH 弹窗里点选提交。',
  );
  return blocks.join('\n\n').slice(0, 6000);
}

function inputRequestSpoken(notice) {
  const q0 = inputRequestQuestions(notice)[0];
  const prompt = String(q0?.prompt || q0?.question || notice?.title || '').trim();
  if (prompt) return `需要你回答：${prompt}`.slice(0, 400);
  return '有问题需要你回答。';
}

/**
 * 单题 + 选项 → 列表行，每行「选择」→ dsh.answer submit。
 * 参数用 qid/oid 压缩，避免 params_json 256 上限。
 */
function inputRequestChoiceRows(session, notice) {
  const questions = inputRequestQuestions(notice);
  if (questions.length !== 1) return null;
  const q = questions[0];
  const opts = Array.isArray(q.options) ? q.options : [];
  if (opts.length === 0) return null;
  const sid = session.externalSessionId.slice(0, 128);
  const nid = noticeIdOf(notice);
  const qid = String(q.id || '').slice(0, 32);
  if (!nid || !qid) return null;
  const header = String(q.header || '请选择').trim().slice(0, 96);
  const prompt = String(q.prompt || q.question || '').trim().slice(0, 120);
  const rows = [
    {
      type: 'list_item',
      id: `q_${qid}`.slice(0, 64),
      title: header,
      subtitle: prompt || '点下面一项作答',
    },
  ];
  opts.slice(0, 8).forEach((o, i) => {
    const oid = String(o.id || `o_${i}`).slice(0, 16);
    const label = String(o.label || oid).trim();
    const desc = String(o.description || '').trim();
    rows.push({
      type: 'list_item',
      id: `${qid}_${oid}`.slice(0, 64),
      title: `${i + 1}. ${label}`.slice(0, 96),
      subtitle: (desc || '点选此项').slice(0, 120),
      actions: [
        {
          id: `pick_${oid}`.slice(0, 24),
          title: '选择',
          run: 'invoke',
          invoke: {
            app_id: 'dsh.answer',
            params: {
              session: sid,
              notice_id: nid,
              action: 'submit',
              qid,
              oid,
            },
          },
        },
      ],
    });
  });
  return rows;
}

/** list_item + 审批时带 run:invoke → dsh.answer；提问类不下发假「允许/拒绝」 */
function taskRow(session, state, folded, notices) {
  const st = state?.status ?? 'unknown';
  const needsYou = ATTENTION.has(st);
  const notice = primaryNotice(notices);
  const kind = needsYou ? classifyNotice(notice) : null;
  let subtitle = displayTaskSubtitle(st, folded, needsYou);
  if (kind === 'input_request') {
    const qs = inputRequestQuestions(notice);
    const canPick = qs.length === 1 && Array.isArray(qs[0]?.options) && qs[0].options.length > 0;
    subtitle = canPick ? '等你确认 · 点开选择' : '等你确认 · 请在电脑上选择';
  } else if (kind === 'approval') {
    subtitle = '等你确认 · 点允许或拒绝';
  }
  const row = {
    type: 'list_item',
    id: session.externalSessionId.slice(0, 128),
    title: displayTaskTitle(session, folded).slice(0, 96),
    subtitle: subtitle.slice(0, 120),
  };
  const nid = noticeIdOf(notice);
  if (kind === 'approval' && nid) {
    const sid = session.externalSessionId.slice(0, 128);
    const invokeBase = { app_id: 'dsh.answer' };
    row.actions = [
      {
        id: 'allow',
        title: '允许',
        run: 'invoke',
        invoke: { ...invokeBase, params: { session: sid, notice_id: nid, action: 'allow' } },
      },
      {
        id: 'deny',
        title: '拒绝',
        run: 'invoke',
        invoke: { ...invokeBase, params: { session: sid, notice_id: nid, action: 'deny' } },
      },
    ];
  }
  return row;
}

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

async function handleHealth() {
  let dsh = false;
  let detail = '';
  try {
    const ep = readEndpoint();
    detail = `port=${ep.port}`;
    const client = await connect({ namespace: 'nodelio-health' });
    try {
      await client.call('session.list', {});
      dsh = true;
    } finally {
      client.close();
    }
  } catch (e) {
    detail = e.message?.slice(0, 120) ?? 'unreachable';
  }
  const lans = lanAddresses();
  const port = listenPort();
  const publicHost = lans[0] ? `${lans[0]}:${port}` : `127.0.0.1:${port}`;
  return {
    ok: true,
    dsh,
    detail,
    host: cfg().listen_host || '0.0.0.0',
    port,
    lan: lans,
    market_url: `http://${publicHost}${MARKET_PREFIX}/marketplace.json`,
  };
}

function dirExists(p) {
  try {
    return Boolean(p) && fs.existsSync(p) && fs.statSync(p).isDirectory();
  } catch {
    return false;
  }
}

function pathLeaf(p) {
  const s = String(p ?? '').replace(/[\\/]+$/, '');
  return path.basename(s) || s;
}

/** Windows 路径比较：斜杠/大小写/尾分隔符统一。 */
function normPath(p) {
  return String(p ?? '')
    .trim()
    .replace(/\//g, '\\')
    .replace(/\\+$/, '')
    .toLowerCase();
}

function samePath(a, b) {
  const na = normPath(a);
  const nb = normPath(b);
  return Boolean(na) && na === nb;
}

/**
 * DSH JSON-RPC 常 resolve 业务失败体 `{ ok:false, code, message }`（不 throw）。
 * 返回可读失败信息；成功则 null。
 */
function dshResultFail(result) {
  if (result == null || typeof result !== 'object') return null;
  if (result.ok === false) {
    const code = String(result.code || result.error?.code || 'upstream_error');
    const message = String(
      result.message || result.error?.message || 'DSH 调用失败',
    ).slice(0, 400);
    return { code, message };
  }
  return null;
}

function mapCreateFailSpoken(message) {
  const m = String(message || '');
  if (/choose a model|select a model|选.*模型|permissions|Agent mode|agentPreset/i.test(m)) {
    return '新建失败：请打开桥配置页选默认模型与权限，或把 APP 默认工作区改成已有空闲会话的目录。';
  }
  if (/INVALID_PARAMS/i.test(m)) {
    return '新建任务参数无效，请换工作区或检查 DSH 设置。';
  }
  return `派活失败：${m.slice(0, 120)}`;
}

function logTask(msg, extra) {
  const tail = extra != null ? ` ${typeof extra === 'string' ? extra : JSON.stringify(extra)}` : '';
  console.log(`[dsh.task] ${msg}${tail}`);
}

/** APP 配置「默认工作区」候选：真实 session cwd + 本机存在的常见目录（win/mac/linux） */
async function handleWorkspaces(client) {
  const home = os.homedir();
  const platform = process.platform; /* win32 | darwin | linux */
  const counts = new Map();
  try {
    const { sessions } = await client.call('session.list', {});
    for (const s of sessions ?? []) {
      const cwd = typeof s.cwd === 'string' ? s.cwd.trim() : '';
      if (!cwd) continue;
      counts.set(cwd, (counts.get(cwd) ?? 0) + 1);
    }
  } catch {
    /* DSH 不可达时仍给本机目录候选 */
  }

  const fromSessions = [...counts.entries()]
    .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
    .map(([p, n]) => ({
      path: p,
      label: pathLeaf(p),
      sessions: n,
      source: 'session',
    }));

  const seen = new Set(fromSessions.map((w) => w.path));
  const candidates = [home];
  if (platform === 'win32') {
    candidates.push(
      path.join(home, 'Desktop'),
      path.join(home, 'Documents'),
      path.join(home, 'Projects'),
      path.join(home, 'source'),
      'D:\\xz25',
      'D:\\SITES\\NodeLio',
      'D:\\SITES',
      'C:\\src',
    );
  } else if (platform === 'darwin') {
    candidates.push(
      path.join(home, 'Desktop'),
      path.join(home, 'Documents'),
      path.join(home, 'Projects'),
      path.join(home, 'Developer'),
      path.join(home, 'code'),
      path.join(home, 'src'),
      path.join(home, 'xz25'),
    );
  } else {
    candidates.push(
      path.join(home, 'Desktop'),
      path.join(home, 'Documents'),
      path.join(home, 'Projects'),
      path.join(home, 'code'),
      path.join(home, 'src'),
      path.join(home, 'xz25'),
      '/opt',
      '/srv',
    );
  }

  const local = [];
  for (const p of candidates) {
    if (!p || seen.has(p) || !dirExists(p)) continue;
    seen.add(p);
    local.push({
      path: p,
      label: pathLeaf(p) || p,
      sessions: 0,
      source: 'local',
    });
  }

  return {
    platform,
    home,
    workspaces: [...fromSessions, ...local].slice(0, 20),
  };
}

/** 请求 Host → 绝对基址（APP 加源 / Host 拉包用） */
function requestBase(request) {
  const host = String(request.headers.host ?? '').trim() || `127.0.0.1:${listenPort()}`;
  return `http://${host}`;
}

/** H5(localhost) → 桥(LAN) 跨域：必须应答 OPTIONS；回显预检头（含 APP 的 x-owner 等） */
function marketCorsHeaders(request) {
  const requested = String(request.headers['access-control-request-headers'] ?? '').trim();
  return {
    'access-control-allow-origin': '*',
    'access-control-allow-methods': 'GET, HEAD, OPTIONS',
    'access-control-allow-headers':
      requested ||
      'Content-Type, Authorization, X-Requested-With, Accept, X-Token, X-Device-Key, X-Owner',
    'access-control-max-age': '86400',
  };
}

function sendMarketText(request, response, status, text, extra = {}) {
  const body = Buffer.from(String(text), 'utf8');
  response.writeHead(status, {
    ...marketCorsHeaders(request),
    'content-type': 'text/plain; charset=utf-8',
    'content-length': body.length,
    ...extra,
  });
  response.end(body);
}

/**
 * GET|HEAD|OPTIONS /nodelio-dsh-market/...
 * marketplace.json：相对 package_url 改写为指向本桥的绝对 URL。
 * 包 JSON：原样字节下发，保证 package_sha256 对得上。
 */
function tryServeMarket(request, response, pathname) {
  if (pathname !== MARKET_PREFIX && !pathname.startsWith(MARKET_PREFIX + '/')) return false;

  if (request.method === 'OPTIONS') {
    response.writeHead(204, marketCorsHeaders(request));
    response.end();
    return true;
  }
  if (request.method !== 'GET' && request.method !== 'HEAD') {
    sendMarketText(request, response, 405, 'method not allowed', { allow: 'GET, HEAD, OPTIONS' });
    return true;
  }

  let rel = pathname.slice(MARKET_PREFIX.length).replace(/^\/+/, '');
  if (!rel || rel.endsWith('/')) rel = (rel || '') + 'marketplace.json';
  if (rel.includes('\0') || path.posix.normalize(rel).startsWith('..')) {
    sendMarketText(request, response, 400, 'bad path');
    return true;
  }
  const abs = path.resolve(MARKET_ROOT, rel);
  if (!abs.startsWith(MARKET_ROOT + path.sep) && abs !== MARKET_ROOT) {
    sendMarketText(request, response, 403, 'forbidden');
    return true;
  }
  if (!fs.existsSync(abs) || !fs.statSync(abs).isFile()) {
    sendMarketText(request, response, 404, 'not found');
    return true;
  }

  const base = requestBase(request);
  let body = fs.readFileSync(abs);
  let ctype = 'application/octet-stream';
  if (abs.endsWith('.json')) {
    ctype = 'application/json; charset=utf-8';
    if (path.basename(abs) === 'marketplace.json') {
      try {
        const doc = JSON.parse(body.toString('utf8'));
        const prefix = `${base}${MARKET_PREFIX}/`;
        for (const app of doc.apps ?? []) {
          const u = String(app.package_url ?? '');
          if (u.startsWith('/nodelio-dsh-market/')) {
            app.package_url = base + u;
          } else if (u.startsWith('/static/nodelio-dsh-market/')) {
            /* 兼容旧相对路径 */
            app.package_url = base + u.replace(/^\/static/, '');
          } else if (u.startsWith('packages/') || u.startsWith('./packages/')) {
            app.package_url = prefix + u.replace(/^\.\//, '');
          }
        }
        body = Buffer.from(JSON.stringify(doc, null, 2), 'utf8');
      } catch {
        /* 坏 JSON 原样返回，由客户端报错 */
      }
    }
  }

  response.writeHead(200, {
    ...marketCorsHeaders(request),
    'content-type': ctype,
    'content-length': body.length,
    'cache-control': 'no-store',
  });
  if (request.method === 'HEAD') response.end();
  else response.end(body);
  return true;
}

function statusInvokeBase() {
  const c = cfg();
  const inv = { app_id: c.status_app_id };
  if (c.status_instance_id) inv.instance_id = c.status_instance_id;
  return inv;
}

function listDisplay(title, maxItems = 8) {
  return {
    card_type: 'list',
    title,
    preferred_surface: 'card',
    density: 'comfortable',
    max_items: maxItems,
    /* 墙板无操作超时关卡，避免常亮挡息屏；固件对 dsh 亦有 120s 兜底 */
    ttl_sec: 120,
  };
}

/**
 * 顶栏 chip：优先「有会话的工作区」，避免 Desktop/Documents 等空候选占满内存与屏宽。
 * 最多 8 个（固件 NL_ENV_FILTER_MAX）。
 */
function statusWorkspaceList(all, defaultWs) {
  const list = all ?? [];
  const withSess = list.filter((w) => (w.sessions ?? 0) > 0 || w.source === 'session');
  let out = withSess.length > 0 ? [...withSess] : [...list];
  if (defaultWs && !out.some((w) => w.path === defaultWs)) {
    const hit = list.find((w) => w.path === defaultWs);
    if (hit) out.unshift(hit);
  }
  return out.slice(0, 8);
}

/** 顶栏横滑工作区 chip：点选 → 再 invoke status(workspace=…) */
function workspaceFilters(list, selectedPath) {
  return (list ?? []).map((w) => {
    const p = String(w.path ?? '').slice(0, 160);
    return {
      id: p.slice(0, 128),
      title: String(w.label || pathLeaf(p) || p).slice(0, 16),
      selected: p === selectedPath,
      run: 'invoke',
      invoke: { ...statusInvokeBase(), params: { workspace: p } },
    };
  });
}

function pickWorkspacePath(list, requested, defaultWs) {
  const paths = (list ?? []).map((w) => String(w.path ?? '')).filter(Boolean);
  /* 显式请求优先：即使 chip 列表暂无该区（0 会话），也按请求过滤，避免误落到 xz25 等 */
  if (requested) {
    const hit = paths.find((p) => samePath(p, requested));
    if (hit) return hit;
    return requested;
  }
  if (defaultWs) {
    const hit = paths.find((p) => samePath(p, defaultWs));
    if (hit) return hit;
  }
  const withSess = (list ?? []).find((w) => (w.sessions ?? 0) > 0);
  if (withSess?.path) return String(withSess.path);
  return paths[0] ?? '';
}

function statusDisplay(title, filters, maxItems = 10) {
  return { ...listDisplay(title, maxItems), filters: filters ?? [] };
}

/** 会话行：二元审批→允许/拒绝；提问挂起不加续聊；其余→续聊 arm */
function sessionRowWithArm(session, state, folded, notices) {
  const row = taskRow(session, state, folded, notices);
  if (row.actions && row.actions.length > 0) return row;
  const st = state?.status ?? 'unknown';
  if (ATTENTION.has(st) && classifyNotice(primaryNotice(notices)) === 'input_request') {
    return row;
  }
  const sid = session.externalSessionId.slice(0, 128);
  const cwd = String(session.cwd ?? '').slice(0, 200);
  row.actions = [
    {
      id: 'arm_sess',
      title: '续聊',
      run: 'invoke',
      invoke: {
        ...statusInvokeBase(),
        params: { arm: true, workspace: cwd, session: sid },
      },
    },
  ];
  return row;
}

/**
 * 一屏：顶栏 filters=工作区；content=当前区会话。
 * 无 workspace 时自动选 default_workspace / 有会话的区 / 第一个。
 * query 与 POST body 字段均可（Host GET 会把 merged 打进 query）。
 */
async function handleStatus(client, query, request, body = {}) {
  const requested =
    (typeof body.workspace === 'string' && body.workspace.trim()) ||
    query.get('workspace') ||
    '';
  const defaultWs =
    (typeof body.default_workspace === 'string' && body.default_workspace.trim()) ||
    query.get('default_workspace') ||
    '';
  const reference =
    (typeof body.session === 'string' && body.session.trim()) || query.get('session') || '';

  const wsDoc = await handleWorkspaces(client);
  const wsList = statusWorkspaceList(wsDoc.workspaces ?? [], defaultWs);
  const workspace = pickWorkspacePath(wsList, requested, defaultWs);
  const filters = workspaceFilters(wsList, workspace);
  const leaf = pathLeaf(workspace) || 'DSH';

  if (!workspace) {
    return {
      schema_version: 1,
      request_id: newRequestId(),
      app_id: 'dsh.status',
      status: 'ok',
      content: [],
      display: statusDisplay('DSH 任务', filters, 10),
      /* 进度卡禁止 spoken.audio：墙板 NotifyPlayer 会 Suspend MQTT，再唤醒易 hello 超时 */
      spoken: speak(speakWorkspaceList(wsList)),
    };
  }

  const { sessions } = await client.call('session.list', {});
  let pool = sessions.filter((s) => samePath(s.cwd, workspace));
  console.log(
    `[dsh.status] workspace=${workspace || '-'} requested=${requested || '-'} default=${defaultWs || '-'} pool=${pool.length} filters=${filters.length}`,
  );

  /* 单会话详情（墙板点卡片）：带 body 全文纯文本 */
  if (reference) {
    const matchRef = (s) =>
      s.externalSessionId.includes(reference) ||
      s.sessionId.includes(reference) ||
      (s.title ?? '').includes(reference);
    let hit = pool.filter(matchRef);
    /* cwd 规范化不一致时，允许跨工作区按 session 命中 */
    if (hit.length === 0) {
      hit = (sessions ?? []).filter(matchRef);
    }
    pool = hit;
    if (pool.length === 0) {
      return {
        schema_version: 1,
        request_id: newRequestId(),
        app_id: 'dsh.status',
        status: 'ok',
        content: [],
        display: statusDisplay('DSH 任务', filters, 6),
        spoken: speak('没找到这个任务。'),
      };
    }
    const detailed = [await readTask(client, pool[0])];
    const t = detailed[0];
    const notice = primaryNotice(t.notices);
    const nKind = classifyNotice(notice);
    const ask = displayTaskTitle(t.session, t.folded);

    /* 单题多选：列表点「选择」提交（无 body，避免进纯文本详情） */
    if (nKind === 'input_request') {
      const choiceRows = inputRequestChoiceRows(t.session, notice);
      if (choiceRows) {
        const cwd = String(t.session.cwd ?? '').slice(0, 200);
        choiceRows.push({
          type: 'list_item',
          id: 'back_list',
          title: '← 返回任务列表',
          subtitle: '不答题，回到进度列表',
          actions: [
            {
              id: 'back',
              title: '返回',
              run: 'invoke',
              invoke: {
                ...statusInvokeBase(),
                params: { workspace: cwd },
              },
            },
          ],
        });
        return {
          schema_version: 1,
          request_id: newRequestId(),
          app_id: 'dsh.status',
          status: 'ok',
          content: choiceRows,
          display: {
            ...statusDisplay('请选择', filters, 10),
            title: String(ask || '请选择').slice(0, 32),
            ttl_sec: 120,
          },
          spoken: await speakWithAudioParts(inputRequestSpoken(notice), request, 6),
        };
      }
    }

    const row = sessionRowWithArm(t.session, t.state, t.folded, t.notices);
    let body = '';
    let spokenText = '';
    if (nKind === 'input_request') {
      body = formatInputRequestPlain(notice);
      spokenText = inputRequestSpoken(notice);
    } else {
      body = lastReplyPlain(t.folded).slice(0, 6000);
      spokenText = body
        ? `末轮：${body.replace(/\s+/g, ' ').trim()}`
        : '暂无文字回复';
    }
    if (body) row.body = body;
    else row.body = '（本会话还没有可展示的文字回复）';
    return {
      schema_version: 1,
      request_id: newRequestId(),
      app_id: 'dsh.status',
      status: 'ok',
      content: [row],
      display: {
        ...statusDisplay('会话详情', filters, 1),
        title: String(ask || '会话详情').slice(0, 32),
        ttl_sec: 120,
      },
      spoken: await speakWithAudioParts(spokenText, request, 6),
    };
  }

  if (pool.length === 0) {
    return {
      schema_version: 1,
      request_id: newRequestId(),
      app_id: 'dsh.status',
      status: 'ok',
      content: [],
      display: statusDisplay('DSH 任务', filters, 6),
      spoken: speak(`${leaf}没有任务。横滑换工作区，或说「让它…」派活。`),
    };
  }

  const detailed = [];
  for (const session of pool.slice(0, 8)) {
    detailed.push(await readTask(client, session));
  }
  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: 'dsh.status',
    status: 'ok',
    content: detailed.map((t) => sessionRowWithArm(t.session, t.state, t.folded, t.notices)),
    display: statusDisplay('DSH 任务', filters, 10),
    spoken: speak(speakStatusList(detailed)),
  };
}

/** 「新建会话…」前缀 → 强制 createAndStart，不复用 idle。 */
function stripNewSessionPrefix(raw) {
  const text = String(raw || '').trim();
  const m = text.match(/^(?:新建(?:一个)?会话|开(?:一个)?新会话|新开(?:一个)?会话)[，,：:\s]*(.*)$/i);
  if (!m) return { instruction: text, forceNew: false };
  const rest = String(m[1] || '').trim();
  return { instruction: rest || text, forceNew: true };
}

async function handleTask(client, body) {
  const rawInstruction = typeof body.instruction === 'string' ? body.instruction.trim() : '';
  if (!rawInstruction) {
    logTask('reject empty instruction');
    return errorEnvelope(
      'dsh.task',
      'config_missing',
      '请说：让它 + 要做的事（不要只说「派活」）。',
      '请说让它，再加上要做的事。',
    );
  }
  const stripped = stripNewSessionPrefix(rawInstruction);
  const instruction = stripped.instruction;
  const forceNew =
    stripped.forceNew || body.new_session === true || body.new_session === 'true';

  const sessionRef = typeof body.session === 'string' ? body.session.trim() : '';
  let workspace = resolveWorkspace(body);
  logTask('begin', {
    instruction: instruction.slice(0, 120),
    forceNew,
    workspace: workspace || null,
    session: sessionRef || null,
    default_workspace: typeof body.default_workspace === 'string' ? body.default_workspace : null,
  });

  const { sessions } = await client.call('session.list', {});
  const beforeIds = new Set((sessions ?? []).map((s) => s.externalSessionId));
  const idleInWs = (sessions ?? []).filter((s) => samePath(s.cwd, workspace)).length;
  logTask('session.list', {
    total: (sessions ?? []).length,
    idle_candidates_ws: idleInWs,
    cwds: [...new Set((sessions ?? []).map((s) => s.cwd).filter(Boolean))].slice(0, 12),
  });

  let target = null;
  let created = false;

  if (sessionRef) {
    target = sessions.find(
      (s) =>
        s.externalSessionId.includes(sessionRef) ||
        s.sessionId.includes(sessionRef) ||
        (s.title ?? '').includes(sessionRef),
    );
    if (!target) {
      logTask('session miss', { sessionRef });
      return errorEnvelope('dsh.task', 'not_installed', '找不到该会话。', '找不到这个任务。');
    }
    workspace = target.cwd || workspace;
    logTask('startTurn by session', {
      session: target.externalSessionId,
      cwd: target.cwd,
    });
    let startResult;
    try {
      startResult = await client.call('session.startTurn', {
        sessionId: target.sessionId,
        externalSessionId: target.externalSessionId,
        content: instruction,
        clientMessageId: crypto.randomUUID(),
      });
    } catch (e) {
      logTask('startTurn throw', { err: e.message, code: e.code });
      return errorEnvelope(
        'dsh.task',
        'upstream_error',
        e.message || 'startTurn 失败',
        mapCreateFailSpoken(e.message),
      );
    }
    const fail = dshResultFail(startResult);
    if (fail) {
      logTask('startTurn fail', fail);
      return errorEnvelope('dsh.task', fail.code, fail.message, mapCreateFailSpoken(fail.message));
    }
    logTask('startTurn ok', { session: target.externalSessionId });
  } else {
    if (!workspace) {
      logTask('reject no workspace');
      return errorEnvelope(
        'dsh.task',
        'config_missing',
        '未选工作区：请先「查看任务」点选工作区，或在 APP「DSH 派活」配置默认工作区。',
        '请先选工作区或配置默认工作区。',
      );
    }
    if (!forceNew) {
      for (const session of sessions.filter((s) => samePath(s.cwd, workspace))) {
        const state = await client
          .call('session.getState', { externalSessionId: session.externalSessionId })
          .catch(() => null);
        if (state?.status === 'idle') {
          target = session;
          break;
        }
      }
    } else {
      logTask('force new session — skip idle reuse');
    }
    if (target) {
      logTask('reuse idle', {
        session: target.externalSessionId,
        title: (target.title || '').slice(0, 60),
        cwd: target.cwd,
      });
      let startResult;
      try {
        startResult = await client.call('session.startTurn', {
          sessionId: target.sessionId,
          externalSessionId: target.externalSessionId,
          content: instruction,
          clientMessageId: crypto.randomUUID(),
        });
      } catch (e) {
        logTask('startTurn throw', { err: e.message, code: e.code });
        return errorEnvelope(
          'dsh.task',
          'upstream_error',
          e.message || 'startTurn 失败',
          mapCreateFailSpoken(e.message),
        );
      }
      const fail = dshResultFail(startResult);
      if (fail) {
        logTask('startTurn fail', fail);
        return errorEnvelope('dsh.task', fail.code, fail.message, mapCreateFailSpoken(fail.message));
      }
      logTask('startTurn ok', { session: target.externalSessionId });
    } else {
      created = true;
      let createOpts;
      try {
        createOpts = await resolveCreateParams(client, cfg());
      } catch (e) {
        logTask('resolveCreateParams fail', { err: e.message });
        return errorEnvelope(
          'dsh.task',
          'config_missing',
          e.message || '无法解析默认模型',
          mapCreateFailSpoken(e.message),
        );
      }
      logTask('createAndStart', {
        cwd: workspace,
        forceNew,
        model: createOpts.resolved.model_title,
        permission: createOpts.resolved.permission,
        agent: createOpts.resolved.agent_preset,
      });
      let createResult;
      try {
        createResult = await client.call('session.createAndStart', {
          sessionId: crypto.randomUUID(),
          content: instruction,
          cwd: workspace,
          clientMessageId: crypto.randomUUID(),
          selections: createOpts.selections,
          agentPreset: createOpts.agentPreset,
        });
      } catch (e) {
        logTask('createAndStart throw', { err: e.message, code: e.code });
        return errorEnvelope(
          'dsh.task',
          'upstream_error',
          e.message || 'createAndStart 失败',
          mapCreateFailSpoken(e.message),
        );
      }
      const fail = dshResultFail(createResult);
      if (fail) {
        logTask('createAndStart fail', {
          ...fail,
          nested: createResult?.result?.sourceState || null,
        });
        return errorEnvelope('dsh.task', fail.code, fail.message, mapCreateFailSpoken(fail.message));
      }
      /* create 回包常带 sessionId；再 list 对齐 cwd（有的实现 cwd 规范化不一致） */
      const { sessions: afterCreate } = await client.call('session.list', {});
      const createdId =
        createResult?.externalSessionId ||
        createResult?.sessionId ||
        createResult?.result?.externalSessionId ||
        createResult?.result?.sessionId ||
        null;
      target =
        (createdId &&
          afterCreate.find(
            (s) =>
              s.externalSessionId === createdId ||
              s.sessionId === createdId ||
              s.externalSessionId?.includes(String(createdId)) ||
              s.sessionId?.includes(String(createdId)),
          )) ||
        afterCreate.find((s) => !beforeIds.has(s.externalSessionId) && samePath(s.cwd, workspace)) ||
        afterCreate.find((s) => !beforeIds.has(s.externalSessionId)) ||
        null;
      if (!target) {
        logTask('createAndStart ghost — no session in list', {
          createdId,
          after_n: afterCreate.length,
          result_keys: createResult && typeof createResult === 'object' ? Object.keys(createResult) : [],
        });
        return errorEnvelope(
          'dsh.task',
          'upstream_error',
          'DSH 未创建可见会话（可能未选模型或工作区无效）。',
          mapCreateFailSpoken('Choose a model in AA before creating a DSH session.'),
        );
      }
      if (target?.cwd) workspace = target.cwd;
      logTask('createAndStart ok', {
        session: target.externalSessionId,
        cwd: target.cwd,
      });
    }
  }

  const { sessions: after } = await client.call('session.list', {});
  let sameWorkspace = after.filter((s) => samePath(s.cwd, workspace));
  if (sameWorkspace.length === 0 && target) {
    sameWorkspace = [target];
  }
  /* 刚投递的会话置顶，便于对照「续聊」 */
  if (target) {
    const tid = target.externalSessionId;
    sameWorkspace = [
      ...sameWorkspace.filter((s) => s.externalSessionId === tid),
      ...sameWorkspace.filter((s) => s.externalSessionId !== tid),
    ];
  }
  const leaf = pathLeaf(workspace) || '工作区';
  const targetId = target?.externalSessionId || '';
  const pool = (sameWorkspace.length ? sameWorkspace : target ? [target] : []).slice(0, 5);
  const content = [];
  for (let i = 0; i < pool.length; i += 1) {
    const s = pool[i];
    const folded = foldFromDisk(s);
    const state = await client
      .call('session.getState', { externalSessionId: s.externalSessionId })
      .catch(() => null);
    const notices = await client
      .call('session.getNotices', { externalSessionId: s.externalSessionId })
      .catch(() => ({ notices: [] }));
    const row = sessionRowWithArm(s, state, folded, notices?.notices ?? []);
    const isTarget = Boolean(targetId) && s.externalSessionId === targetId;
    if (isTarget) {
      row.title = instruction.slice(0, 96);
      row.subtitle = clipTaskSub(
        created ? `新建并投递 · ${leaf}` : `刚投递到本会话 · ${leaf}`,
      );
    }
    content.push(row);
  }

  const wsDoc = await handleWorkspaces(client);
  const wsList = statusWorkspaceList(wsDoc.workspaces ?? [], workspace);
  const filters = workspaceFilters(wsList, workspace);
  const ord = targetId ? pool.findIndex((s) => s.externalSessionId === targetId) + 1 : 0;

  logTask('done', {
    created,
    workspace,
    leaf,
    target: targetId || null,
    ord,
    items: content.length,
    first: content[0]?.title?.slice(0, 60) || null,
  });

  const spokenText = sessionRef
    ? `已发给${leaf}里选中的会话。可说续聊或查看详情。`
    : created
      ? `已在${leaf}新建第${ord || 1}个会话并开始。可说续聊或查看详情。`
      : `已投到${leaf}第${ord || 1}个会话。可说续聊或查看详情。`;

  /* 回包对齐 dsh.status 进度卡，墙板无需再点刷新换皮 */
  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: 'dsh.status',
    status: 'ok',
    content,
    display: statusDisplay(`DSH 任务`, filters, 10),
    spoken: speak(spokenText),
  };
}

function clipTaskSub(s) {
  return String(s || '').slice(0, 120);
}

async function handleAnswer(client, body) {
  const sessionRef = typeof body.session === 'string' ? body.session : '';
  const noticeId = typeof body.notice_id === 'string' ? body.notice_id : '';
  const rawAction = typeof body.action === 'string' ? body.action.trim().toLowerCase() : 'allow';
  const action =
    rawAction === 'deny' || rawAction === 'reject'
      ? 'deny'
      : rawAction === 'cancel'
        ? 'cancel'
        : rawAction === 'submit'
          ? 'submit'
          : 'allow';

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

  const payload = {
    sessionId: session.sessionId,
    noticeId,
    actionId: action,
  };
  /* DSH bridge-next：提问答案在 inputData，不是 input */
  if (action === 'submit') {
    let answers = null;
    if (body.answers && typeof body.answers === 'object') {
      answers = body.answers;
    } else {
      const qid = typeof body.qid === 'string' ? body.qid.trim() : '';
      const oid = typeof body.oid === 'string' ? body.oid.trim() : '';
      if (qid && oid) {
        answers = { [qid]: { optionIds: [oid] } };
      }
    }
    if (!answers) {
      return errorEnvelope(
        'dsh.answer',
        'config_missing',
        '提交需要 qid+oid 或 answers。',
        '缺少选项信息。',
      );
    }
    payload.inputData = { answers };
  }

  try {
    await client.call('session.respondInteraction', payload);
  } catch (e) {
    const msg = String(e?.message || e).slice(0, 200);
    return errorEnvelope('dsh.answer', 'invoke_failed', msg, msg || '提交失败。');
  }

  const ack =
    action === 'deny'
      ? '已拒绝。'
      : action === 'cancel'
        ? '已取消。'
        : action === 'submit'
          ? '已提交。'
          : '已允许。';

  /* 提交后回进度列表，墙板无需再刷 */
  try {
    const cwd = String(session.cwd || '');
    const statusEnv = await handleStatus(client, new URLSearchParams(), null, {
      workspace: cwd,
    });
    if (statusEnv && statusEnv.status === 'ok') {
      statusEnv.spoken = speak(ack);
      return statusEnv;
    }
  } catch {
    /* fall through plain ack */
  }

  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: 'dsh.answer',
    status: 'ok',
    content: [
      {
        type: 'text',
        id: 'ack',
        text: ack,
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
    spoken: speak(ack),
  };
}

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

/** 边沿：needs_you / completed → Host MQTT skill_hint */
const s_prev = new Map();
let s_watch_seeded = false;

async function postSkillHint(reason, sessionId) {
  const c = cfg();
  /* 优先经出站 WS hint（公网 Host 无需设备密钥 HTTP） */
  if (hostWsClient) {
    try {
      hostWsClient.sendHint(reason, sessionId, {
        app_id: c.status_app_id,
        instance_id: c.status_instance_id || undefined,
      });
      console.log(`[watch] skill-hint via ws reason=${reason} session=${sessionId ?? '-'}`);
      return;
    } catch (e) {
      console.warn(`[watch] skill-hint ws fail: ${e.message}`);
    }
  }
  if (!c.host_url || !c.panel_device_id || !c.panel_device_key) return;
  const url = `${c.host_url}/api/devices/${encodeURIComponent(c.panel_device_id)}/panel/skill-hint`;
  const body = JSON.stringify({
    app_id: c.status_app_id,
    instance_id: c.status_instance_id || undefined,
    reason,
    session: sessionId || undefined,
  });
  try {
    const res = await fetch(url, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'X-Device-Key': c.panel_device_key,
      },
      body,
      signal: AbortSignal.timeout(5000),
    });
    if (!res.ok) {
      console.warn(`[watch] skill-hint HTTP ${res.status}`);
    } else {
      console.log(`[watch] skill-hint reason=${reason} session=${sessionId ?? '-'}`);
    }
  } catch (e) {
    console.warn(`[watch] skill-hint fail: ${e.message}`);
  }
}

function tryServeConfigUi(request, response, pathname) {
  if (pathname === '/' || pathname === '/config' || pathname === '/config/') {
    const htmlPath = path.join(PUBLIC_DIR, 'config.html');
    if (!fs.existsSync(htmlPath)) {
      send(response, 404, { error: 'config.html missing' });
      return true;
    }
    const html = fs.readFileSync(htmlPath);
    response.writeHead(200, {
      'content-type': 'text/html; charset=utf-8',
      'content-length': html.length,
      'cache-control': 'no-store',
    });
    response.end(html);
    return true;
  }
  return false;
}

async function handleConfigApi(request, response, method) {
  if (method === 'GET') {
    const pub = getPublicConfig();
    pub.runtime = {
      host_ws_wanted: wantHostWs() && Boolean(cfg().host_ws && cfg().bridge_token),
      host_ws_connected: hostWsConnected,
      watch_running: Boolean(watchTimer),
    };
    send(response, 200, pub);
    return;
  }
  if (method === 'PUT' || method === 'POST') {
    try {
      const body = await readBody(request);
      const { config, listen_changed } = saveConfig(body); /* emit → onConfigChange → applyRuntime */
      const pub = getPublicConfig();
      pub.runtime = {
        host_ws_wanted: wantHostWs() && Boolean(config.host_ws && config.bridge_token),
        host_ws_connected: hostWsConnected,
        watch_running: Boolean(watchTimer),
      };
      send(response, 200, { ok: true, listen_changed, config: pub });
    } catch (e) {
      send(response, 400, { error: e.message });
    }
    return;
  }
  send(response, 405, { error: 'method not allowed' });
}

async function watchTick() {
  let client;
  try {
    client = await connect({ namespace: 'nodelio-watch' });
    const { sessions } = await client.call('session.list', {});
    const seeding = !s_watch_seeded;
    for (const session of sessions.slice(0, 24)) {
      const state = await client
        .call('session.getState', { externalSessionId: session.externalSessionId })
        .catch(() => null);
      const st = state?.status ?? 'unknown';
      const id = session.externalSessionId;
      const prev = s_prev.get(id);
      s_prev.set(id, st);
      if (seeding) {
        /* 基线写入后：对已在 waiting 的会话补一次 needs_you，避免启动瞬间漏推 */
        if (ATTENTION.has(st)) {
          await postSkillHint('needs_you', id);
        }
        continue;
      }
      if (!prev) continue;
      if (!ATTENTION.has(prev) && ATTENTION.has(st)) {
        await postSkillHint('needs_you', id);
      }
      if (prev === 'running' && (st === 'idle' || st === 'completed')) {
        await postSkillHint('completed', id);
      }
    }
    s_watch_seeded = true;
  } catch (e) {
    console.warn(`[watch] ${e.message}`);
  } finally {
    client?.close();
  }
}

function stopWatch() {
  if (watchTimer) {
    clearInterval(watchTimer);
    watchTimer = null;
  }
}

function restartWatch() {
  stopWatch();
  s_prev.clear();
  s_watch_seeded = false;
  const c = cfg();
  if (!c.watch) {
    console.log('[watch] off');
    return;
  }
  if (!hostWsClient && !(c.host_url && c.panel_device_id && c.panel_device_key)) {
    console.warn(
      '[watch] 需 host_ws+token，或 host_url + panel_device_id + panel_device_key',
    );
  }
  console.log(`[watch] every ${c.watch_ms}ms`);
  watchTick().catch(() => {});
  watchTimer = setInterval(() => {
    watchTick().catch(() => {});
  }, c.watch_ms);
}

function stopHostWs() {
  hostWsConnected = false;
  if (hostWsClient) {
    try {
      hostWsClient.stop();
    } catch {
      /* ignore */
    }
    hostWsClient = null;
  }
}

function restartHostWs() {
  stopHostWs();
  if (!wantHostWs()) {
    console.log('[host-ws] mode excludes host_ws');
    return;
  }
  const c = cfg();
  if (!c.host_ws || !c.bridge_token) {
    console.warn('[host-ws] 未配置 host_ws / bridge_token（可在 /config 填写）');
    return;
  }
  hostWsClient = startHostWs({
    wsUrl: c.host_ws,
    token: c.bridge_token,
    lanPort: c.listen_port,
    publicUrl: c.public_url || '',
    panelDeviceId: c.panel_device_id,
    version: '1.0.0',
    onReady: () => {
      hostWsConnected = true;
      console.log('[host-ws] ready');
    },
    onClose: () => {
      hostWsConnected = false;
    },
  });
}

/** 热应用：watch + host-ws（listen 不变） */
function applyRuntime(source) {
  console.log(`[config] apply runtime source=${source}`);
  restartHostWs();
  restartWatch();
}

onConfigChange((_c, meta) => {
  applyRuntime(meta.source);
});

function startLanHttp() {
  const c0 = cfg();
  const listenHost = c0.mode === 'host_ws' ? '127.0.0.1' : c0.listen_host;
  const PORT = c0.listen_port;
  const server = http.createServer(async (request, response) => {
    const url = new URL(request.url, `http://${request.headers.host ?? 'localhost'}`);
    const route = `${request.method} ${url.pathname}`;
    console.log(`${new Date().toISOString()}  ${route}`);
    try {
      if (tryServeConfigUi(request, response, url.pathname)) return;
      if (url.pathname === '/nodelio/dsh/config') {
        await handleConfigApi(request, response, request.method);
        return;
      }
      if (tryServeMarket(request, response, url.pathname)) return;
      if (tryServeTts(request, response, url.pathname)) return;

      if (route === 'GET /nodelio/dsh/health') {
        send(response, 200, await handleHealth());
        return;
      }

      let client;
      try {
        client = await connect({ namespace: 'nodelio-market' });

        if (route === 'GET /nodelio/dsh/status') {
          const env = await handleStatus(client, url.searchParams, request);
          console.log(
            `[dsh.http] GET status → ${env.status} items=${Array.isArray(env.content) ? env.content.length : 0}`,
          );
          send(response, 200, env);
        } else if (route === 'POST /nodelio/dsh/status') {
          const body = await readBody(request);
          const env = await handleStatus(client, url.searchParams, request, body);
          const wav = env?.spoken?.audio?.url ? String(env.spoken.audio.url).slice(0, 96) : '';
          console.log(
            `[dsh.http] POST status → ${env.status} items=${Array.isArray(env.content) ? env.content.length : 0}` +
              ` session=${body?.session ? '1' : '0'} wav=${wav ? '1' : '0'}${wav ? ` ${wav}` : ''}`,
          );
          send(response, 200, env);
        } else if (route === 'GET /nodelio/dsh/workspaces') {
          send(response, 200, await handleWorkspaces(client));
        } else if (route === 'GET /nodelio/dsh/catalog') {
          send(response, 200, await listCreateCatalog(client));
        } else if (route === 'POST /nodelio/dsh/task') {
          const body = await readBody(request);
          const env = await handleTask(client, body);
          console.log(
            `[dsh.http] POST task → ${env.status}` +
              (env.status === 'error'
                ? ` code=${env.error?.code || '?'} msg=${String(env.error?.message || '').slice(0, 160)}`
                : ` items=${Array.isArray(env.content) ? env.content.length : 0} title=${env.display?.title || '-'}`),
          );
          send(response, 200, env);
        } else if (route === 'POST /nodelio/dsh/answer') {
          send(response, 200, await handleAnswer(client, await readBody(request)));
        } else {
          send(response, 404, errorEnvelope('dsh.status', 'internal', `no route: ${route}`, '路径不对。'));
        }
      } catch (error) {
        console.warn(`[dsh.http] ${route} exception: ${error.message}`);
        const code = /ENOENT|ECONNREFUSED|endpoint not found/i.test(error.message)
          ? 'upstream_error'
          : 'internal';
        send(response, 200, errorEnvelope('dsh.status', code, error.message, 'DSH 没响应，可能工作台没开。'));
      } finally {
        client?.close();
      }
    } catch (error) {
      console.warn(`[dsh.http] ${route} fatal: ${error.message}`);
      try {
        send(response, 500, errorEnvelope('dsh.status', 'internal', error.message, '桥内部错误。'));
      } catch {
        /* ignore */
      }
    }
  });

  server.listen(PORT, listenHost, () => {
    const lans = lanAddresses();
    const mode = cfg().mode;
    console.log(`nodelio DSH bridge mode=${mode} listening on http://${listenHost}:${PORT}`);
    console.log(`  config UI  http://127.0.0.1:${PORT}/config`);
    console.log(`  config file ${CONFIG_PATH}`);
    if (listenHost === '0.0.0.0' || listenHost === '::') {
      for (const ip of lans) {
        console.log(`  LAN  http://${ip}:${PORT}`);
        console.log(`  market http://${ip}:${PORT}${MARKET_PREFIX}/marketplace.json`);
      }
    } else {
      console.log(`  market http://${listenHost}:${PORT}${MARKET_PREFIX}/marketplace.json`);
    }
    console.log('  GET  /nodelio/dsh/health');
    console.log('  GET/PUT /nodelio/dsh/config');
    console.log('  GET /nodelio/dsh/catalog  (models / permissions for create)');
    console.log('  GET  /nodelio/dsh/workspaces');
    console.log('  GET/POST /nodelio/dsh/status  workspace=&session=（空=工作区列表）');
    console.log('  POST /nodelio/dsh/task    {"instruction","workspace?","session?"}');
    console.log('  POST /nodelio/dsh/answer  {"session":"...","notice_id":"...","action":"allow"}');
    console.log(`  GET  ${MARKET_PREFIX}/marketplace.json`);
    applyRuntime('boot');
  });
}

if (wantLan() || wantHostWs()) {
  startLanHttp();
} else {
  console.error(`[dsh-bridge] unknown mode=${cfg().mode} (use lan|host_ws|both)`);
  process.exit(1);
}
