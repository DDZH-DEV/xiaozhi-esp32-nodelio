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

function fromEnv() {
  return {
    mode: String(process.env.DSH_BRIDGE_MODE ?? DEFAULTS.mode).toLowerCase(),
    listen_host: process.env.DSH_BRIDGE_HOST ?? DEFAULTS.listen_host,
    listen_port: Number(process.env.DSH_BRIDGE_PORT ?? DEFAULTS.listen_port) || 8790,
    watch: envBool(process.env.DSH_BRIDGE_WATCH, DEFAULTS.watch),
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
}

function sanitize(raw) {
  const e = fromEnv();
  const o = { ...DEFAULTS, ...e, ...(raw && typeof raw === 'object' ? raw : {}) };
  let mode = String(o.mode || 'both').toLowerCase();
  if (mode !== 'lan' && mode !== 'host_ws' && mode !== 'both') mode = 'both';
  const port = Number(o.listen_port);
  const watchMs = Number(o.watch_ms);
  return {
    mode,
    listen_host: String(o.listen_host || '0.0.0.0').trim() || '0.0.0.0',
    listen_port: Number.isFinite(port) && port > 0 && port < 65536 ? Math.floor(port) : 8790,
    watch: o.watch === true || o.watch === 1 || o.watch === '1' || o.watch === 'true',
    watch_ms: Number.isFinite(watchMs) && watchMs >= 2000 ? Math.floor(watchMs) : 8000,
    host_ws: String(o.host_ws || '').trim(),
    bridge_token: String(o.bridge_token || '').trim(),
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
  current = sanitize(file);
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
