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
import { launchDshDesktop } from '../lib/dsh-launch.mjs';
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

/** 1/true/yes/on → true；缺省 false（详情 TTS 默认关，避免轮询卡顿）。 */
function truthyFlag(v) {
  if (v === true || v === 1) return true;
  if (typeof v === 'number') return v !== 0;
  if (typeof v === 'string') {
    const s = v.trim().toLowerCase();
    return s === '1' || s === 'true' || s === 'yes' || s === 'on' || s === 'y';
  }
  return false;
}

/** 技能配置 detail_tts：打开详情时是否合成 WAV。 */
function wantDetailTts(body = {}, query) {
  const raw =
    (body && body.detail_tts != null ? body.detail_tts : null) ??
    (query && typeof query.get === 'function' ? query.get('detail_tts') : null);
  return truthyFlag(raw);
}

/** 墙板静默轮询：只刷新正文，不合成 TTS。 */
function isSilentRefresh(body = {}, query) {
  const raw =
    (body && body.silent != null ? body.silent : null) ??
    (query && typeof query.get === 'function' ? query.get('silent') : null);
  return truthyFlag(raw);
}

/**
 * 详情 spoken：仅 detail_tts=1 且非 silent 时合成多段音频；否则纯文本（省 Edge/SAPI）。
 */
async function speakDetail(text, request, body = {}, query) {
  if (!wantDetailTts(body, query) || isSilentRefresh(body, query)) {
    return speak(text);
  }
  return speakWithAudioParts(text, request, 6);
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
      density: 'normal',
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
  if (!Array.isArray(questions)) return [];
  return questions.map((q, i) => ({
    ...q,
    id: String(q?.id || `q${i}`).slice(0, 32),
  }));
}

function questionHasOptions(q) {
  return Array.isArray(q?.options) && q.options.length > 0;
}

/** 每题都有可点选项时，墙板可逐题选完再一次性 submit */
function canPickInputRequest(notice) {
  const qs = inputRequestQuestions(notice);
  return qs.length > 0 && qs.every((q) => questionHasOptions(q) && String(q.id || ''));
}

/** session+notice → 已选 answers；多题向导用，答完才 RPC */
const inputAnswerDrafts = new Map();
const INPUT_DRAFT_TTL_MS = 30 * 60 * 1000;

function draftKey(sid, nid) {
  return `${String(sid || '').slice(0, 128)}::${String(nid || '').slice(0, 128)}`;
}

function getInputDraft(sid, nid) {
  const k = draftKey(sid, nid);
  const cur = inputAnswerDrafts.get(k);
  if (!cur) return {};
  if (Date.now() - (cur.at || 0) > INPUT_DRAFT_TTL_MS) {
    inputAnswerDrafts.delete(k);
    return {};
  }
  return { ...(cur.answers || {}) };
}

function setInputDraft(sid, nid, answers) {
  inputAnswerDrafts.set(draftKey(sid, nid), { answers: { ...answers }, at: Date.now() });
}

function clearInputDraft(sid, nid) {
  inputAnswerDrafts.delete(draftKey(sid, nid));
}

function nextPickableQuestion(notice, answers) {
  const answered = answers && typeof answers === 'object' ? answers : {};
  for (const q of inputRequestQuestions(notice)) {
    const qid = String(q.id || '');
    if (!qid || answered[qid]) continue;
    if (questionHasOptions(q)) return q;
  }
  return null;
}

/** 无法点选时：纯文本详情；可点选时详情走 choiceRows，不走本函数 */
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
    canPickInputRequest(notice)
      ? '请点开任务，在墙板上逐题点选。'
      : questions.length > 1
        ? '本题含无法点选的提问，请在电脑 DSH 弹窗里答完提交。'
        : '请在电脑 DSH 弹窗里点选提交。',
  );
  return blocks.join('\n\n').slice(0, 6000);
}

function inputRequestSpoken(notice, answers = {}) {
  const q = nextPickableQuestion(notice, answers) || inputRequestQuestions(notice)[0];
  const prompt = String(q?.prompt || q?.question || notice?.title || '').trim();
  const qs = inputRequestQuestions(notice);
  const idx = q ? qs.findIndex((x) => x.id === q.id) : 0;
  const step =
    qs.length > 1 && idx >= 0 ? `第${idx + 1}题，共${qs.length}题。` : '';
  if (prompt) return `${step}需要你回答：${prompt}`.slice(0, 400);
  return `${step}有问题需要你回答。`.slice(0, 400);
}

/**
 * 当前未答题 + 选项 → 列表行，每行「选择」→ dsh.answer submit(qid+oid)。
 * 多题：桥端草稿攒齐后再 RPC；answers 为已选草稿。
 */
function inputRequestChoiceRows(session, notice, answers = {}) {
  if (!canPickInputRequest(notice)) return null;
  const questions = inputRequestQuestions(notice);
  const q = nextPickableQuestion(notice, answers);
  if (!q) return null;
  const opts = Array.isArray(q.options) ? q.options : [];
  const sid = String(session.externalSessionId || '').slice(0, 128);
  const nid = noticeIdOf(notice);
  const qid = String(q.id || '').slice(0, 32);
  if (!nid || !qid) return null;
  const qIndex = questions.findIndex((x) => x.id === qid);
  const step =
    questions.length > 1 ? `（${Math.max(1, qIndex + 1)}/${questions.length}）` : '';
  const header = `${String(q.header || '请选择').trim()}${step}`.slice(0, 96);
  const prompt = String(q.prompt || q.question || '').trim().slice(0, 120);
  const doneN = Object.keys(answers || {}).length;
  const rows = [
    {
      type: 'list_item',
      id: `q_${qid}`.slice(0, 64),
      title: header,
      subtitle: (doneN > 0 ? `已选${doneN}题 · ` : '') + (prompt || '点下面一项作答'),
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

function noticeHasCancel(notice) {
  const actions = Array.isArray(notice?.actions) ? notice.actions : [];
  return actions.some((a) => String(a.actionId || a.id || '').toLowerCase() === 'cancel');
}

function cancelNoticeAction(sid, nid) {
  return {
    id: 'cancel_task',
    title: '取消并停止',
    run: 'invoke',
    invoke: {
      app_id: 'dsh.answer',
      params: {
        session: String(sid || '').slice(0, 128),
        notice_id: String(nid || '').slice(0, 128),
        action: 'cancel',
      },
    },
  };
}

function choiceRowsEnvelope(session, notice, answers, filters, askTitle, spokenText) {
  const choiceRows = inputRequestChoiceRows(session, notice, answers);
  if (!choiceRows) return null;
  const cwd = String(session.cwd ?? '').slice(0, 200);
  const sid = String(session.externalSessionId || '').slice(0, 128);
  const nid = noticeIdOf(notice);
  if (nid && noticeHasCancel(notice)) {
    choiceRows.push({
      type: 'list_item',
      id: 'cancel_notice',
      title: '取消并停止',
      subtitle: '关掉提问并中断本轮会话',
      actions: [cancelNoticeAction(sid, nid)],
    });
  }
  choiceRows.push({
    type: 'list_item',
    id: 'back_list',
    title: '← 返回任务列表',
    subtitle: '不答题，回到进度列表（任务仍待确认）',
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
  const qs = inputRequestQuestions(notice);
  const q = nextPickableQuestion(notice, answers);
  const qIndex = q ? qs.findIndex((x) => x.id === q.id) : 0;
  const stepTitle =
    qs.length > 1 ? `请选择 ${Math.max(1, qIndex + 1)}/${qs.length}` : '请选择';
  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: 'dsh.status',
    status: 'ok',
    content: choiceRows,
    display: {
      ...statusDisplay(stepTitle, filters, 10),
      title: String(askTitle || stepTitle).slice(0, 32),
      ttl_sec: 120,
    },
    spoken: speak(String(spokenText || inputRequestSpoken(notice, answers)).slice(0, 400)),
  };
}

/** list_item + 审批时带 run:invoke → dsh.answer；提问类不下发假「允许/拒绝」 */
function taskRow(session, state, folded, notices) {
  const st = state?.status ?? 'unknown';
  const needsYou = ATTENTION.has(st);
  const notice = primaryNotice(notices);
  const kind = needsYou ? classifyNotice(notice) : null;
  let subtitle = displayTaskSubtitle(st, folded, needsYou);
  if (kind === 'input_request') {
    subtitle = canPickInputRequest(notice)
      ? '等你确认 · 点开选择'
      : '等你确认 · 请在电脑上选择';
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
  const sid = session.externalSessionId.slice(0, 128);
  if (kind === 'approval' && nid) {
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
  } else if (kind === 'input_request' && nid && noticeHasCancel(notice)) {
    /* 列表/纯文本详情：可直接取消本次提问 */
    row.actions = [cancelNoticeAction(sid, nid)];
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

/**
 * DSH 桥连不上（端点过期/未监听）→ 专用 dsh_offline 错误码。
 * 面板拿到明确中文提示，而不是裸 ECONNREFUSED 文本。
 */
function isDshOfflineError(error) {
  const m = String(error?.message || '');
  return /ECONNREFUSED|ECONNRESET|ENOENT|endpoint not found/i.test(m);
}

const DSH_OFFLINE_SPOKEN = '电脑 DSH 没开，要帮你打开吗？';

function wantsOpenDesktop(body) {
  if (!body || typeof body !== 'object') return false;
  const v = body.open_desktop;
  return v === true || v === 1 || v === '1' || v === 'true';
}

function offlineEnvelope(appId) {
  const id = appId || 'dsh.status';
  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: id,
    status: 'ok',
    content: [
      {
        type: 'list_item',
        id: 'dsh-offline',
        title: '电脑 DSH 没开',
        subtitle: '要在电脑上打开工作台吗？',
        actions: [
          {
            id: 'open',
            title: '打开',
            run: 'invoke',
            invoke: { app_id: 'dsh.status', params: { open_desktop: true } },
          },
          {
            id: 'skip',
            title: '不用',
            run: 'invoke',
            invoke: { app_id: 'dsh.status', params: { dismiss: true } },
          },
        ],
      },
    ],
    display: {
      card_type: 'list',
      title: 'DSH',
      preferred_surface: 'card',
      density: 'comfortable',
      max_items: 3,
      ttl_sec: 60,
    },
    spoken: speak(DSH_OFFLINE_SPOKEN),
  };
}

function openingEnvelope(appId, message, spokenText) {
  return {
    schema_version: 1,
    request_id: newRequestId(),
    app_id: appId || 'dsh.status',
    status: 'ok',
    content: [
      {
        type: 'list_item',
        id: 'dsh-opening',
        title: '正在打开 DSH',
        subtitle: String(message || '请稍候再点顶栏刷新').slice(0, 120),
      },
    ],
    display: {
      card_type: 'list',
      title: 'DSH',
      preferred_surface: 'card',
      density: 'comfortable',
      max_items: 3,
      ttl_sec: 30,
    },
    spoken: speak(spokenText || '正在打开电脑上的 DSH，请稍候刷新。'),
  };
}

/** 墙板点「打开」：拉起 Desktop，就绪则交给调用方继续 RPC。 */
async function tryOpenDesktop(appId) {
  const r = await launchDshDesktop({ timeoutMs: 12000 });
  if (!r.ok) {
    return {
      env: errorEnvelope(appId, 'dsh_offline', r.message, r.spoken),
    };
  }
  if (r.waiting) {
    return { env: openingEnvelope(appId, r.message, r.spoken) };
  }
  try {
    return { client: await connect({ namespace: 'nodelio-market' }) };
  } catch (e) {
    if (isDshOfflineError(e)) {
      return { env: openingEnvelope(appId, r.message, r.spoken) };
    }
    throw e;
  }
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
    density: 'normal',
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

    /* 可点选提问（含多题）：列表点「选择」；多题由桥草稿攒齐再 RPC */
    if (nKind === 'input_request' && canPickInputRequest(notice)) {
      const sid = String(t.session.externalSessionId || '').slice(0, 128);
      const nid = noticeIdOf(notice);
      const draft = getInputDraft(sid, nid);
      const env = choiceRowsEnvelope(
        t.session,
        notice,
        draft,
        filters,
        ask,
        inputRequestSpoken(notice, draft),
      );
      if (env) {
        env.spoken = await speakDetail(inputRequestSpoken(notice, draft), request, body, query);
        return env;
      }
    }

    const row = sessionRowWithArm(t.session, t.state, t.folded, t.notices);
    let detailBody = '';
    let spokenText = '';
    if (nKind === 'input_request') {
      detailBody = formatInputRequestPlain(notice);
      spokenText = inputRequestSpoken(notice);
    } else {
      detailBody = lastReplyPlain(t.folded).slice(0, 6000);
      spokenText = detailBody
        ? `末轮：${detailBody.replace(/\s+/g, ' ').trim()}`
        : '暂无文字回复';
    }
    if (detailBody) row.body = detailBody;
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
      spoken: await speakDetail(spokenText, request, body, query),
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
      if (isDshOfflineError(e)) {
        return offlineEnvelope('dsh.task');
      }
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
        if (isDshOfflineError(e)) {
          return offlineEnvelope('dsh.task');
        }
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
        if (isDshOfflineError(e)) {
          return offlineEnvelope('dsh.task');
        }
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
  /* DSH bridge-next：提问答案在 inputData；多题先攒草稿，齐了再 RPC */
  if (action === 'submit') {
    let answers = null;
    if (body.answers && typeof body.answers === 'object') {
      answers = { ...body.answers };
    } else {
      const qid = typeof body.qid === 'string' ? body.qid.trim() : '';
      const oid = typeof body.oid === 'string' ? body.oid.trim() : '';
      if (qid && oid) {
        const draft = getInputDraft(session.externalSessionId, noticeId);
        answers = { ...draft, [qid]: { optionIds: [oid] } };
      }
    }
    if (!answers || Object.keys(answers).length === 0) {
      return errorEnvelope(
        'dsh.answer',
        'config_missing',
        '提交需要 qid+oid 或 answers。',
        '缺少选项信息。',
      );
    }

    let notice = null;
    try {
      const doc = await client.call('session.getNotices', {
        externalSessionId: session.externalSessionId,
      });
      const list = Array.isArray(doc?.notices) ? doc.notices : [];
      notice =
        list.find((n) => noticeIdOf(n) === noticeId) ||
        primaryNotice(list);
    } catch {
      notice = null;
    }

    if (notice && canPickInputRequest(notice)) {
      const nextQ = nextPickableQuestion(notice, answers);
      if (nextQ) {
        setInputDraft(session.externalSessionId, noticeId, answers);
        const wsDoc = await handleWorkspaces(client).catch(() => ({ workspaces: [] }));
        const wsList = statusWorkspaceList(wsDoc.workspaces ?? [], '');
        const cwd = String(session.cwd || '');
        const filters = workspaceFilters(wsList, cwd);
        const ask = displayTaskTitle(session, foldFromDisk(session));
        const env = choiceRowsEnvelope(
          session,
          notice,
          answers,
          filters,
          ask,
          inputRequestSpoken(notice, answers),
        );
        if (env) {
          console.log(
            `[dsh.answer] draft ${Object.keys(answers).length}/${inputRequestQuestions(notice).length} next=${nextQ.id}`,
          );
          return env;
        }
      }
    }

    clearInputDraft(session.externalSessionId, noticeId);
    payload.inputData = { answers };
  }

  if (action === 'cancel' || action === 'deny' || action === 'allow') {
    clearInputDraft(session.externalSessionId, noticeId);
  }

  /* 取消/拒绝：先应答 notice，再 interrupt，否则回合会继续跑 */
  const stopTurn = action === 'cancel' || action === 'deny';
  let respondOk = false;
  try {
    await client.call('session.respondInteraction', payload);
    respondOk = true;
  } catch (e) {
    if (isDshOfflineError(e)) {
      return offlineEnvelope('dsh.answer');
    }
    if (!stopTurn) {
      const msg = String(e?.message || e).slice(0, 200);
      return errorEnvelope('dsh.answer', 'invoke_failed', msg, msg || '提交失败。');
    }
    console.warn(
      `[dsh.answer] respondInteraction failed action=${action}: ${String(e?.message || e).slice(0, 160)}`,
    );
  }

  if (stopTurn) {
    try {
      await client.call('session.interrupt', {
        sessionId: session.sessionId,
        reason: action === 'cancel' ? 'voice-cancel' : 'voice-deny',
      });
      console.log(`[dsh.answer] interrupted session=${session.externalSessionId} after ${action}`);
    } catch (e) {
      if (isDshOfflineError(e)) {
        return offlineEnvelope('dsh.answer');
      }
      if (!respondOk) {
        const msg = String(e?.message || e).slice(0, 200);
        return errorEnvelope('dsh.answer', 'invoke_failed', msg, msg || '停止失败。');
      }
      console.warn(
        `[dsh.answer] interrupt failed after ${action}: ${String(e?.message || e).slice(0, 160)}`,
      );
    }
  }

  const ack =
    action === 'deny'
      ? '已拒绝并停止。'
      : action === 'cancel'
        ? '已取消并停止。'
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

function configRuntimePub(extra = {}) {
  const pub = getPublicConfig();
  pub.runtime = {
    host_ws_wanted: wantHostWs() && Boolean(cfg().host_ws && cfg().bridge_token),
    host_ws_connected: hostWsConnected,
    watch_running: Boolean(watchTimer),
    ...extra,
  };
  return pub;
}

/** host_ws → http(s) 基址；或规范化 host_url */
function hostHttpBase(hostUrl, hostWs) {
  const hu = String(hostUrl || '').trim().replace(/\/$/, '');
  if (hu) return hu;
  const ws = String(hostWs || '').trim();
  if (!ws) return '';
  try {
    const u = new URL(ws);
    const scheme = u.protocol === 'wss:' ? 'https:' : 'http:';
    return `${scheme}//${u.host}`;
  } catch {
    return '';
  }
}

function hostWsFromHttp(hostUrl) {
  try {
    const u = new URL(String(hostUrl || '').trim());
    const scheme = u.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${scheme}//${u.host}/ws/dsh-bridge`;
  } catch {
    return '';
  }
}

/**
 * 用 APP 登录后的 Owner 会话向 Host 申请 bridge_token（明文不落盘 Owner Token）。
 * POST { owner, owner_token, host_url?, host_ws?, panel_device_id?, label?, replace_existing? }
 */
async function handleClaimBridgeToken(request, response) {
  try {
    const body = await readBody(request);
    const owner = String(body.owner || '').trim();
    const ownerToken = String(body.owner_token || body.ownerToken || '').trim();
    if (!owner || !ownerToken) {
      send(response, 400, { error: '请填写 Owner 与 Owner Token（APP 登录后本地 storage）' });
      return;
    }
    const c = cfg();
    let hostUrl = hostHttpBase(body.host_url || body.hostUrl, body.host_ws || body.hostWs || c.host_ws);
    if (!hostUrl) {
      hostUrl = hostHttpBase(c.host_url, c.host_ws);
    }
    if (!hostUrl) {
      send(response, 400, { error: '请先填 host_ws 或 host_url（指向 NodeLio Host）' });
      return;
    }
    let hostWs = String(body.host_ws || body.hostWs || c.host_ws || '').trim();
    if (!hostWs) {
      hostWs = hostWsFromHttp(hostUrl);
    }
    const panel = String(body.panel_device_id || body.panelDeviceId || c.panel_device_id || '').trim();
    const label = String(body.label || 'home-pc').trim() || 'home-pc';
    const replace =
      body.replace_existing === true ||
      body.replace_existing === 1 ||
      body.replaceExisting === true ||
      body.replace_existing == null; /* 默认替换旧 token，避免列表堆积 */

    const claimUrl = `${hostUrl}/api/owner/bridge-token`;
    console.log(`[config] claim bridge-token host=${hostUrl} owner=${owner}`);
    const res = await fetch(claimUrl, {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'X-Owner': owner,
        'X-Owner-Token': ownerToken,
      },
      body: JSON.stringify({
        label,
        panel_device_id: panel || undefined,
        replace_existing: replace,
      }),
      signal: AbortSignal.timeout(15000),
    });
    const text = await res.text();
    let doc;
    try {
      doc = JSON.parse(text);
    } catch {
      send(response, 502, { error: `Host 回包非 JSON HTTP ${res.status}` });
      return;
    }
    if (!res.ok) {
      const msg = doc.error || doc.message || doc.code || `HTTP ${res.status}`;
      send(response, res.status >= 400 && res.status < 600 ? res.status : 502, {
        error: String(msg),
      });
      return;
    }
    const token = String(doc.bridgeToken || doc.bridge_token || '').trim();
    if (!token) {
      send(response, 502, { error: 'Host 未返回 bridgeToken' });
      return;
    }
    const patch = {
      bridge_token: token,
      host_url: hostUrl,
      host_ws: hostWs,
    };
    if (panel) patch.panel_device_id = panel;
    const { config, listen_changed } = saveConfig(patch);
    const pub = configRuntimePub();
    send(response, 200, {
      ok: true,
      listen_changed,
      bridge_token_id: doc.id,
      hint: doc.hint || '已写入 bridge_token 并热应用出站 WS',
      config: pub,
    });
  } catch (e) {
    console.warn(`[config] claim fail: ${e.message}`);
    send(response, 502, { error: e.message || String(e) });
  }
}

async function handleConfigApi(request, response, method) {
  if (method === 'GET') {
    send(response, 200, configRuntimePub());
    return;
  }
  if (method === 'PUT' || method === 'POST') {
    try {
      const body = await readBody(request);
      const { config, listen_changed } = saveConfig(body); /* emit → onConfigChange → applyRuntime */
      send(response, 200, { ok: true, listen_changed, config: configRuntimePub() });
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
    marketRoot: MARKET_ROOT,
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
      if (url.pathname === '/nodelio/dsh/claim-bridge-token' && request.method === 'POST') {
        await handleClaimBridgeToken(request, response);
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
        let postBody = {};
        if (request.method === 'POST' && url.pathname.startsWith('/nodelio/dsh/')) {
          postBody = await readBody(request);
        }
        if (wantsOpenDesktop(postBody)) {
          const opened = await tryOpenDesktop('dsh.status');
          if (opened.env) {
            send(response, 200, opened.env);
            return;
          }
          client = opened.client;
        } else {
          client = await connect({ namespace: 'nodelio-market' });
        }

        if (route === 'GET /nodelio/dsh/status') {
          const env = await handleStatus(client, url.searchParams, request);
          console.log(
            `[dsh.http] GET status → ${env.status} items=${Array.isArray(env.content) ? env.content.length : 0}`,
          );
          send(response, 200, env);
        } else if (route === 'POST /nodelio/dsh/status') {
          const body = postBody;
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
          const body = postBody;
          const env = await handleTask(client, body);
          console.log(
            `[dsh.http] POST task → ${env.status}` +
              (env.status === 'error'
                ? ` code=${env.error?.code || '?'} msg=${String(env.error?.message || '').slice(0, 160)}`
                : ` items=${Array.isArray(env.content) ? env.content.length : 0} title=${env.display?.title || '-'}`),
          );
          send(response, 200, env);
        } else if (route === 'POST /nodelio/dsh/answer') {
          send(response, 200, await handleAnswer(client, postBody));
        } else {
          send(response, 404, errorEnvelope('dsh.status', 'internal', `no route: ${route}`, '路径不对。'));
        }
      } catch (error) {
        console.warn(`[dsh.http] ${route} exception: ${error.message}`);
        if (isDshOfflineError(error)) {
          send(response, 200, offlineEnvelope('dsh.status'));
        } else {
          send(
            response,
            200,
            errorEnvelope('dsh.status', 'internal', error.message, 'DSH 调用出错，请稍后再试。'),
          );
        }
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
