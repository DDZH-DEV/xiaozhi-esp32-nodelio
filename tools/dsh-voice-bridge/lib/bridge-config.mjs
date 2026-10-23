/**
 * 桥运行时配置：bridge-config.json + 环境变量引导；保存后可热应用（除 listen）。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const CONFIG_PATH = path.join(ROOT, 'bridge-config.json');
export const EXAMPLE_PATH = path.join(ROOT, 'bridge-config.example.json');

const DEFAULTS = {
  mode: 'both',
  listen_host: '0.0.0.0',
  listen_port: 8790,
  watch: false,
  watch_ms: 8000,
  host_ws: '',
  bridge_token: '',
  panel_device_id: '',
  host_url: '',
  panel_device_key: '',
  public_url: '',
  status_app_id: 'dsh.status',
  status_instance_id: '',
  /* 新建会话（createAndStart）默认；空=目录第一个可用模型 */
  default_model: '',
  default_reasoning: 'on',
  default_permission: 'danger-full-access',
  default_agent_preset: 'standard',
};

/** @type {typeof DEFAULTS} */
let current = { ...DEFAULTS };
/** @type {Set<(cfg: typeof DEFAULTS, meta: { source: string }) => void>} */
const listeners = new Set();
let watchHandle = null;
let writing = false;

function envBool(v, fallback) {
  if (v === undefined || v === null || v === '') return fallback;
  const s = String(v).toLowerCase();
  if (s === '1' || s === 'true' || s === 'yes' || s === 'on') return true;
  if (s === '0' || s === 'false' || s === 'no' || s === 'off') return false;
  return fallback;
}

function hasHintCreds(o) {
  const wsOk = Boolean(String(o.host_ws || '').trim() && String(o.bridge_token || '').trim());
  const httpOk = Boolean(
    String(o.host_url || '').trim() &&
      String(o.panel_device_id || '').trim() &&
      String(o.panel_device_key || '').trim(),
  );
  return wsOk || httpOk;
}

function fromEnv() {
  const out = {
    mode: String(process.env.DSH_BRIDGE_MODE ?? DEFAULTS.mode).toLowerCase(),
    listen_host: process.env.DSH_BRIDGE_HOST ?? DEFAULTS.listen_host,
    listen_port: Number(process.env.DSH_BRIDGE_PORT ?? DEFAULTS.listen_port) || 8790,
    watch_ms: Number(process.env.DSH_BRIDGE_WATCH_MS ?? DEFAULTS.watch_ms) || 8000,
    host_ws: (process.env.NODELIO_HOST_WS ?? '').trim(),
    bridge_token: (process.env.BRIDGE_TOKEN ?? '').trim(),
    panel_device_id: (process.env.NODELIO_PANEL_DEVICE_ID ?? '').trim(),
    host_url: (process.env.NODELIO_HOST_URL ?? '').replace(/\/$/, ''),
    panel_device_key: (process.env.NODELIO_PANEL_DEVICE_KEY ?? '').trim(),
    public_url: (process.env.DSH_BRIDGE_PUBLIC_URL ?? '').replace(/\/$/, ''),
    status_app_id: process.env.DSH_STATUS_APP_ID ?? DEFAULTS.status_app_id,
    status_instance_id: process.env.DSH_STATUS_INSTANCE_ID ?? '',
    default_model: (process.env.DSH_DEFAULT_MODEL ?? '').trim(),
    default_reasoning: (process.env.DSH_DEFAULT_REASONING ?? DEFAULTS.default_reasoning).trim(),
    default_permission: (process.env.DSH_DEFAULT_PERMISSION ?? DEFAULTS.default_permission).trim(),
    default_agent_preset: (process.env.DSH_DEFAULT_AGENT_PRESET ?? DEFAULTS.default_agent_preset).trim(),
  };
  /* 仅当环境变量显式设置时覆盖 watch；未设则交 sanitize 按凭证默认 */
  if (process.env.DSH_BRIDGE_WATCH !== undefined && process.env.DSH_BRIDGE_WATCH !== '') {
    out.watch = envBool(process.env.DSH_BRIDGE_WATCH, false);
  }
  return out;
}

function coerceWatch(v) {
  return v === true || v === 1 || v === '1' || v === 'true';
}

/**
 * APP 一键复制常是「host_ws=…\\nbridge_token=hex」；贴进单行 input 会变成空格串，
 * 整段被当成 token → Host 401。从任意字段拆出真值。
 */
export function parseCredBlob(raw) {
  const s = String(raw || '').trim();
  if (!s) return { host_ws: '', token: '' };
  let host_ws = '';
  let token = '';
  const mWs = s.match(/host_ws\s*[=:]\s*(\S+)/i);
  if (mWs) host_ws = String(mWs[1] || '').trim().replace(/[,;]+$/, '');
  const mTok = s.match(/bridge_token\s*[=:]\s*([A-Fa-f0-9]{32,})/i);
  if (mTok) token = String(mTok[1] || '').trim();
  if (!token && /^[A-Fa-f0-9]{48,}$/.test(s.replace(/\s+/g, ''))) {
    token = s.replace(/\s+/g, '');
  }
  return { host_ws, token };
}

function looksLikeCredBlob(s) {
  const t = String(s || '');
  return /host_ws\s*[=:]/i.test(t) || /bridge_token\s*[=:]/i.test(t);
}

function peelCredFields(hostWsIn, tokenIn) {
  let host_ws = String(hostWsIn || '').trim();
  let bridge_token = String(tokenIn || '').trim();
  const fromTok = parseCredBlob(bridge_token);
  const fromWs = parseCredBlob(host_ws);
  if (fromTok.token) bridge_token = fromTok.token;
  else if (looksLikeCredBlob(bridge_token)) bridge_token = '';
  if (fromTok.host_ws) host_ws = fromTok.host_ws;
  if (fromWs.host_ws && (!host_ws || looksLikeCredBlob(host_ws))) host_ws = fromWs.host_ws;
  if (fromWs.token && (!bridge_token || looksLikeCredBlob(String(tokenIn || '')))) {
    bridge_token = fromWs.token;
  }
  return { host_ws, bridge_token };
}

function sanitize(raw) {
  const e = fromEnv();
  const file = raw && typeof raw === 'object' ? raw : null;
  const o = { ...DEFAULTS, ...e, ...(file || {}) };
  let mode = String(o.mode || 'both').toLowerCase();
  if (mode !== 'lan' && mode !== 'host_ws' && mode !== 'both') mode = 'both';
  const port = Number(o.listen_port);
  const watchMs = Number(o.watch_ms);
  const peeled = peelCredFields(o.host_ws, o.bridge_token);
  const peeledBlob =
    looksLikeCredBlob(o.bridge_token) ||
    looksLikeCredBlob(o.host_ws) ||
    (peeled.bridge_token && peeled.bridge_token !== String(o.bridge_token || '').trim());
  if (peeledBlob && peeled.bridge_token) {
    console.warn('[config] 已从粘贴串拆出 bridge_token / host_ws（勿把整段剪贴板当 token）');
  }
  const oForWatch = { ...o, ...peeled };
  let watch;
  if (Object.prototype.hasOwnProperty.call(e, 'watch')) {
    watch = coerceWatch(e.watch);
  } else if (file && Object.prototype.hasOwnProperty.call(file, 'watch')) {
    watch = coerceWatch(file.watch);
  } else {
    /* 有 Host 凭证时默认开 watch，免手设 DSH_BRIDGE_WATCH=1 */
    watch = hasHintCreds(oForWatch);
  }
  return {
    mode,
    listen_host: String(o.listen_host || '0.0.0.0').trim() || '0.0.0.0',
    listen_port: Number.isFinite(port) && port > 0 && port < 65536 ? Math.floor(port) : 8790,
    watch,
    watch_ms: Number.isFinite(watchMs) && watchMs >= 2000 ? Math.floor(watchMs) : 8000,
    host_ws: peeled.host_ws,
    bridge_token: peeled.bridge_token,
    panel_device_id: String(o.panel_device_id || '').trim(),
    host_url: String(o.host_url || '').replace(/\/$/, '').trim(),
    panel_device_key: String(o.panel_device_key || '').trim(),
    public_url: String(o.public_url || '').replace(/\/$/, '').trim(),
    status_app_id: String(o.status_app_id || 'dsh.status').trim() || 'dsh.status',
    status_instance_id: String(o.status_instance_id || '').trim(),
    default_model: String(o.default_model || '').trim(),
    default_reasoning: String(o.default_reasoning || DEFAULTS.default_reasoning).trim() || 'on',
    default_permission:
      String(o.default_permission || DEFAULTS.default_permission).trim() || 'danger-full-access',
    default_agent_preset:
      String(o.default_agent_preset || DEFAULTS.default_agent_preset).trim() || 'standard',
  };
}

function readFileSilent() {
  try {
    if (!fs.existsSync(CONFIG_PATH)) return null;
    return JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
  } catch (e) {
    console.warn(`[config] read fail: ${e.message}`);
    return null;
  }
}

export function getConfig() {
  return { ...current };
}

/** 对外展示：token / key 打码 */
export function getPublicConfig() {
  const c = getConfig();
  const mask = (s) => {
    if (!s) return '';
    if (s.length <= 8) return '••••';
    return `${s.slice(0, 4)}…${s.slice(-4)}`;
  };
  return {
    ...c,
    bridge_token: c.bridge_token ? mask(c.bridge_token) : '',
    bridge_token_set: Boolean(c.bridge_token),
    panel_device_key: c.panel_device_key ? mask(c.panel_device_key) : '',
    panel_device_key_set: Boolean(c.panel_device_key),
    config_path: CONFIG_PATH,
    listen_needs_restart: true,
  };
}

function emit(source) {
  const snap = getConfig();
  for (const fn of listeners) {
    try {
      fn(snap, { source });
    } catch (e) {
      console.warn(`[config] listener: ${e.message}`);
    }
  }
}

export function onConfigChange(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function loadConfig() {
  const file = readFileSilent();
  const prevTok = file ? String(file.bridge_token || '') : '';
  const prevWs = file ? String(file.host_ws || '') : '';
  current = sanitize(file);
  /* 磁盘上若是整段剪贴板，写回纯 token，并通知热重连 */
  if (
    file &&
    (looksLikeCredBlob(prevTok) || looksLikeCredBlob(prevWs)) &&
    current.bridge_token &&
    current.bridge_token !== prevTok
  ) {
    writing = true;
    try {
      fs.writeFileSync(CONFIG_PATH, `${JSON.stringify(current, null, 2)}\n`, 'utf8');
      console.warn(`[config] 已纠正 bridge-config.json 中的 bridge_token`);
    } catch (e) {
      console.warn(`[config] 写回纠正失败: ${e.message}`);
    } finally {
      setTimeout(() => {
        writing = false;
      }, 200);
    }
    emit('load-fix');
  }
  return getConfig();
}

/**
 * 写入磁盘并热通知。keepSecrets：若 body 里 token 为空且已有值则保留。
 */
export function saveConfig(partial, { keepSecrets = true } = {}) {
  const prev = getConfig();
  const merged = { ...prev, ...(partial && typeof partial === 'object' ? partial : {}) };
  if (keepSecrets) {
    if (!String(merged.bridge_token || '').trim() && prev.bridge_token) {
      merged.bridge_token = prev.bridge_token;
    }
    if (!String(merged.panel_device_key || '').trim() && prev.panel_device_key) {
      merged.panel_device_key = prev.panel_device_key;
    }
  }
  const next = sanitize(merged);
  const listenChanged =
    next.listen_host !== prev.listen_host || next.listen_port !== prev.listen_port;
  writing = true;
  try {
    fs.writeFileSync(CONFIG_PATH, `${JSON.stringify(next, null, 2)}\n`, 'utf8');
  } finally {
    setTimeout(() => {
      writing = false;
    }, 200);
  }
  current = next;
  emit('save');
  return { config: getConfig(), listen_changed: listenChanged };
}

export function ensureExample() {
  if (fs.existsSync(EXAMPLE_PATH)) return;
  const example = {
    ...DEFAULTS,
    watch: true,
    host_ws: 'wss://your-host.example/ws/dsh-bridge',
    bridge_token: '',
    panel_device_id: '',
    host_url: 'http://127.0.0.1:8080',
  };
  fs.writeFileSync(EXAMPLE_PATH, `${JSON.stringify(example, null, 2)}\n`, 'utf8');
}

/** 监视文件被外部编辑时热加载 */
export function watchConfigFile() {
  if (watchHandle) return;
  try {
    watchHandle = fs.watch(ROOT, { persistent: true }, (evt, name) => {
      if (name !== 'bridge-config.json') return;
      if (writing) return;
      const file = readFileSilent();
      if (!file) return;
      const next = sanitize(file);
      const prev = JSON.stringify(current);
      if (JSON.stringify(next) === prev) return;
      current = next;
      console.log('[config] reloaded from disk');
      emit('file');
    });
  } catch (e) {
    console.warn(`[config] watch fail: ${e.message}`);
  }
}

export { DEFAULTS, ROOT };
