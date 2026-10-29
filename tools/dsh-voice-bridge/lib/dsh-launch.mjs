/**
 * 语音桥常驻时，DSH Desktop 可能没开。墙板点「打开」后由本模块拉起本机工作台。
 * 找不到安装时返回明确错误，不瞎 spawn。
 *
 * 常见坏态：DSH NEXT.exe 还在，但 Agents Anywhere 端点是旧 pid → ECONNREFUSED。
 * 此时必须清僵死 endpoint + 重启 GUI，否则会一直「电脑 DSH 没开」。
 */
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { connect, purgeStaleEndpoints } from './rpc.mjs';
import { getConfig } from './bridge-config.mjs';

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

export async function isDshReachable() {
  let client;
  try {
    client = await connect({ namespace: 'nodelio-launch' });
    return true;
  } catch {
    return false;
  } finally {
    try {
      await client?.close?.();
    } catch {
      /* ignore */
    }
  }
}

function existsFile(p) {
  try {
    return Boolean(p) && fs.existsSync(p) && fs.statSync(p).isFile();
  } catch {
    return false;
  }
}

function walkDir(dir, depth, visit) {
  if (depth < 0) return;
  let ents;
  try {
    ents = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const e of ents) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walkDir(p, depth - 1, visit);
    else visit(p, e.name);
  }
}

function programFilesRoots() {
  const roots = new Set();
  for (const k of ['ProgramFiles', 'ProgramFiles(x86)', 'ProgramW6432']) {
    const v = process.env[k];
    if (v) roots.add(v);
  }
  for (const drive of ['C:', 'D:', 'E:', 'F:']) {
    roots.add(path.join(drive, 'Program Files'));
    roots.add(path.join(drive, 'Program Files (x86)'));
  }
  return [...roots];
}

function startMenuLnks() {
  const roots = [
    path.join(process.env.APPDATA || '', 'Microsoft', 'Windows', 'Start Menu', 'Programs'),
    path.join(process.env.ProgramData || 'C:\\ProgramData', 'Microsoft', 'Windows', 'Start Menu', 'Programs'),
  ];
  const hits = [];
  for (const root of roots) {
    walkDir(root, 3, (p, name) => {
      if (/\.lnk$/i.test(name) && /dsh/i.test(name)) hits.push(p);
    });
  }
  hits.sort((a, b) => {
    const score = (p) => {
      const n = path.basename(p).toLowerCase();
      if (/next/.test(n)) return 0;
      if (/desktop/.test(n)) return 1;
      if (/cli|terminal|cmd|shell|console/.test(n)) return 9;
      return 5;
    };
    return score(a) - score(b);
  });
  return hits;
}

function pushExe(out, seen, full) {
  if (!existsFile(full)) return;
  const key = full.toLowerCase();
  if (seen.has(key)) return;
  seen.add(key);
  out.push(full);
}

function scoreExe(p) {
  const n = path.basename(p).toLowerCase();
  const d = path.dirname(p).toLowerCase();
  if (/cli|terminal|cmd|shell|console|unins/.test(n)) return 90;
  if (/dsh next\.exe$/.test(n) || /\\dsh next\\/.test(d)) return 0;
  if (/dsh desktop\.exe$/.test(n)) return 1;
  if (/^dsh\.exe$/.test(n)) return 2;
  if (/dsh/.test(n) && /\.exe$/.test(n)) return 5;
  return 50;
}

function windowsExeCandidates(configured) {
  const home = os.homedir();
  const local = process.env.LOCALAPPDATA || path.join(home, 'AppData', 'Local');
  const named = [
    configured,
    process.env.DSH_DESKTOP_EXE,
    path.join(local, 'Programs', 'dsh', 'DSH.exe'),
    path.join(local, 'Programs', 'DSH', 'DSH.exe'),
    path.join(local, 'Programs', 'DSH Desktop', 'DSH Desktop.exe'),
    path.join(local, 'Programs', 'DSH NEXT', 'DSH NEXT.exe'),
  ].filter(Boolean);
  for (const root of programFilesRoots()) {
    named.push(
      path.join(root, 'DSH NEXT', 'DSH NEXT.exe'),
      path.join(root, 'DSH', 'DSH.exe'),
      path.join(root, 'DSH Desktop', 'DSH Desktop.exe'),
    );
  }
  const seen = new Set();
  const out = [];
  for (const p of named) pushExe(out, seen, p);

  const scanDirs = [path.join(local, 'Programs'), ...programFilesRoots()];
  for (const prog of scanDirs) {
    let ents;
    try {
      ents = fs.readdirSync(prog, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const ent of ents) {
      if (!ent.isDirectory() || !/dsh/i.test(ent.name)) continue;
      const dir = path.join(prog, ent.name);
      try {
        for (const f of fs.readdirSync(dir)) {
          if (/\.exe$/i.test(f) && /dsh/i.test(f)) pushExe(out, seen, path.join(dir, f));
        }
      } catch {
        /* skip */
      }
    }
  }
  out.sort((a, b) => scoreExe(a) - scoreExe(b));
  return out;
}

function resolveTarget() {
  const configured = String(getConfig().dsh_desktop_exe || '').trim();
  if (process.platform === 'win32') {
    const exes = windowsExeCandidates(configured);
    if (exes[0]) return { kind: 'exe', path: exes[0] };
    const lnk = startMenuLnks()[0];
    if (lnk) return { kind: 'lnk', path: lnk };
    return null;
  }
  if (configured && existsFile(configured)) return { kind: 'exe', path: configured };
  return { kind: 'app', path: configured || 'DSH' };
}

function isDshProcessRunning() {
  if (process.platform !== 'win32') return false;
  try {
    const out = execFileSync('tasklist', ['/FO', 'CSV', '/NH'], {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 5000,
    });
    return /dsh.*\.exe/i.test(out);
  } catch {
    return false;
  }
}

/** 丢掉 bridge endpoint / manager-lock，避免连上已死端口。 */
function clearBridgeFiles() {
  const home = os.homedir();
  const files = [
    path.join(home, '.agents-anywhere', 'dsh-bridge', 'endpoint.json'),
    path.join(home, '.agents-anywhere', 'dsh-bridge', '.manager-lock.json'),
    path.join(home, '.agents-anywhere', 'dsh-bridge-next', 'endpoint.json'),
    path.join(home, '.agents-anywhere', 'dsh-bridge-next', '.manager-lock.json'),
    path.join(home, '.dsh', 'agents-anywhere', 'bridge', 'endpoint.json'),
    path.join(home, '.dsh', 'agents-anywhere', 'bridge', '.manager-lock.json'),
  ];
  for (const f of files) {
    try {
      if (fs.existsSync(f)) {
        fs.unlinkSync(f);
        console.log(`[dsh-launch] removed ${f}`);
      }
    } catch (e) {
      console.warn(`[dsh-launch] remove fail ${f}: ${e.message}`);
    }
  }
}

function activeRunPath() {
  return path.join(os.homedir(), '.dsh', 'logs', 'active-run.json');
}

/** @returns {{ runId?: string, pid?: number, clean?: boolean, started?: string } | null} */
function readActiveRun() {
  try {
    return JSON.parse(fs.readFileSync(activeRunPath(), 'utf8'));
  } catch {
    return null;
  }
}

function clearActiveRun() {
  const p = activeRunPath();
  try {
    if (fs.existsSync(p)) {
      fs.unlinkSync(p);
      console.log(`[dsh-launch] removed ${p}`);
    }
  } catch (e) {
    console.warn(`[dsh-launch] active-run clear fail: ${e.message}`);
  }
}

/**
 * 恢复模式特征：active-run.clean===false 且 pid 仍活着。
 * 此时 GUI 已是「DSH 终端」恢复页，写不出 Agents Anywhere endpoint。
 */
function isRecoveryUiUp() {
  const run = readActiveRun();
  if (!run || run.clean !== false) return false;
  if (run.pid == null) return isDshProcessRunning();
  try {
    process.kill(Number(run.pid), 0);
    return true;
  } catch (e) {
    return e && e.code === 'ESRCH' ? false : isDshProcessRunning();
  }
}

/** Electron 单例/锁残留会导致 host 秒级 invalid IPC → 恢复模式。 */
function clearElectronLocks() {
  const home = os.homedir();
  const roots = [
    path.join(home, '.dsh', 'electron-user-data'),
    path.join(home, '.dsh', 'electron-user-data', 'safe-mode'),
  ];
  const names = /^(Singleton|lockfile|LOCK|DevToolsActivePort)/i;
  for (const root of roots) {
    let ents;
    try {
      ents = fs.readdirSync(root, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const e of ents) {
      if (!names.test(e.name)) continue;
      const p = path.join(root, e.name);
      try {
        fs.rmSync(p, { recursive: true, force: true });
        console.log(`[dsh-launch] cleared lock ${p}`);
      } catch (err) {
        console.warn(`[dsh-launch] lock busy ${p}: ${err.message}`);
      }
    }
  }
}

/** 杀掉所有 DSH GUI（含 Electron 子进程），并等到进程表清空。 */
async function killDshProcesses(opts = {}) {
  if (process.platform !== 'win32') return;
  const waitMs = Number(opts.waitMs) > 0 ? Number(opts.waitMs) : 8000;
  const settleMs = Number(opts.settleMs) > 0 ? Number(opts.settleMs) : 6000;
  const images = ['DSH NEXT.exe', 'DSH Desktop.exe', 'DSH.exe'];
  for (const im of images) {
    try {
      execFileSync('taskkill', ['/IM', im, '/F', '/T'], {
        encoding: 'utf8',
        windowsHide: true,
        timeout: 15000,
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      console.log(`[dsh-launch] taskkill /IM ${im}`);
    } catch {
      /* 没进程时 taskkill 非 0，忽略 */
    }
  }
  const deadline = Date.now() + waitMs;
  while (Date.now() < deadline) {
    const left = listDshProcessNames();
    if (left.length === 0) {
      console.log('[dsh-launch] all DSH processes gone');
      break;
    }
    console.log(`[dsh-launch] waiting exit… still=${left.join('|')}`);
    await sleep(400);
  }
  clearBridgeFiles();
  clearElectronLocks();
  clearActiveRun();
  purgeStaleEndpoints();
  /* 杀完立刻再开 → host.start.failed ~130ms invalid IPC，又进同一恢复页 */
  console.log(`[dsh-launch] settle ${settleMs}ms before respawn`);
  await sleep(settleMs);
}

function spawnDetached(cmd, args, opts = {}) {
  const child = spawn(cmd, args, {
    detached: true,
    stdio: 'ignore',
    windowsHide: opts.windowsHide !== false,
    shell: false,
  });
  child.on('error', (err) => {
    console.warn(`[dsh-launch] spawn error cmd=${cmd}: ${err.message}`);
  });
  child.unref();
  return child.pid || 0;
}

/** 本机 DSH 进程名快照（调试用）。 */
function listDshProcessNames() {
  if (process.platform !== 'win32') return [];
  try {
    const out = execFileSync('tasklist', ['/FO', 'CSV', '/NH'], {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 5000,
    });
    const names = [];
    for (const line of out.split(/\r?\n/)) {
      const m = line.match(/^"([^"]+\.exe)"/i);
      if (m && /dsh/i.test(m[1])) names.push(m[1]);
    }
    return names;
  } catch {
    return [];
  }
}

function endpointProbe() {
  const home = os.homedir();
  const paths = [
    path.join(home, '.agents-anywhere', 'dsh-bridge', 'endpoint.json'),
    path.join(home, '.agents-anywhere', 'dsh-bridge-next', 'endpoint.json'),
    path.join(home, '.dsh', 'agents-anywhere', 'bridge', 'endpoint.json'),
  ];
  const found = [];
  for (const p of paths) {
    try {
      if (!fs.existsSync(p)) continue;
      const raw = JSON.parse(fs.readFileSync(p, 'utf8'));
      found.push({
        path: p,
        port: raw?.port,
        pid: raw?.pid,
        pidAlive: (() => {
          try {
            if (!raw?.pid) return null;
            process.kill(Number(raw.pid), 0);
            return true;
          } catch (e) {
            return e && e.code === 'ESRCH' ? false : true;
          }
        })(),
      });
    } catch (e) {
      found.push({ path: p, error: String(e.message || e).slice(0, 80) });
    }
  }
  return found;
}

/**
 * Electron GUI：
 * - 勿 spawn(exe,{windowsHide:true})——无窗/不起来
 * - 勿经 Hidden powershell Start-Process——实测进程数为 0、永不写 endpoint
 * Windows 用 cmd start；其它平台 open / 直 spawn。
 * @param {{ kind: string, path: string }} target
 * @param {string[]} [extraArgs] 如 `--next-safe-mode`（DSH NEXT 官方参数）
 */
function launchTarget(target, extraArgs = []) {
  const args = Array.isArray(extraArgs) ? extraArgs.filter(Boolean) : [];
  if (process.platform === 'win32' && target.kind === 'exe') {
    /* start "" "path" [args...] —— 空标题防止带空格路径被当成窗口标题 */
    const pid = spawnDetached(
      process.env.ComSpec || 'cmd.exe',
      ['/c', 'start', '', target.path, ...args],
      { windowsHide: true },
    );
    console.log(
      `[dsh-launch] cmd start pid=${pid} path=${target.path}` +
        (args.length ? ` args=${args.join(' ')}` : ''),
    );
    return;
  }
  if (process.platform === 'win32' && target.kind === 'lnk') {
    const pid = spawnDetached(
      process.env.ComSpec || 'cmd.exe',
      ['/c', 'start', '', target.path],
      { windowsHide: true },
    );
    console.log(`[dsh-launch] cmd start lnk pid=${pid} path=${target.path}`);
    return;
  }
  if (process.platform === 'darwin') {
    const pid = spawnDetached('open', ['-a', target.path, '--args', ...args]);
    console.log(`[dsh-launch] open -a pid=${pid} app=${target.path}`);
    return;
  }
  const pid = spawnDetached(target.path, args, { windowsHide: false });
  console.log(`[dsh-launch] direct spawn pid=${pid} path=${target.path}`);
}

/** DSH NEXT：普通启动进恢复模式时，无人值守改走官方安全模式参数。 */
const DSH_SAFE_MODE_ARGS = ['--next-safe-mode'];

async function waitUntilReachable(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let tick = 0;
  while (Date.now() < deadline) {
    await sleep(500);
    tick += 1;
    if (await isDshReachable()) {
      console.log(
        `[dsh-launch] reachable after ~${tick * 500}ms endpoints=${JSON.stringify(endpointProbe())}`,
      );
      return true;
    }
    if (tick === 2 || tick === 6 || tick % 10 === 0) {
      const procs = listDshProcessNames();
      const eps = endpointProbe();
      console.log(
        `[dsh-launch] wait ${tick * 500}ms procs=${procs.length ? procs.join('|') : '(none)'} ` +
          `endpoints=${eps.length}`,
      );
    }
  }
  console.warn(
    `[dsh-launch] timeout ${timeoutMs}ms procs=${listDshProcessNames().join('|') || '(none)'} ` +
      `endpoints=${JSON.stringify(endpointProbe())}`,
  );
  return false;
}

/** 并发 dsh.status 会叠两次 spawn + taskkill，容易把 Desktop 打进「恢复模式」。 */
let s_launchInflight = null;
let s_lastLaunchAt = 0;
/** 自动救火失败后冷却（落盘，抗 --watch 热重载清空内存）。 */
const GIVEUP_PATH = path.join(os.homedir(), '.dsh', 'nodelio-bridge-launch-cooldown.json');
const GIVEUP_MS = 10 * 60 * 1000;

function readGiveUpUntil() {
  try {
    const j = JSON.parse(fs.readFileSync(GIVEUP_PATH, 'utf8'));
    const t = Number(j?.until);
    return Number.isFinite(t) ? t : 0;
  } catch {
    return 0;
  }
}

function writeGiveUpUntil(until) {
  try {
    fs.mkdirSync(path.dirname(GIVEUP_PATH), { recursive: true });
    fs.writeFileSync(
      GIVEUP_PATH,
      JSON.stringify({ until, reason: 'invalid_ipc_or_recovery', at: new Date().toISOString() }),
      'utf8',
    );
  } catch (e) {
    console.warn(`[dsh-launch] cooldown write fail: ${e.message}`);
  }
}

function clearGiveUp() {
  try {
    if (fs.existsSync(GIVEUP_PATH)) fs.unlinkSync(GIVEUP_PATH);
  } catch {
    /* ignore */
  }
}

/** 近 N 分钟日志里已有 invalid IPC → 别再自动开，交给本机恢复页。 */
function recentInvalidIpc(withinMs = 30 * 60 * 1000) {
  try {
    const logs = path.join(os.homedir(), '.dsh', 'logs');
    const day = new Date().toISOString().slice(0, 10);
    const file = path.join(logs, `desktop-next-${day}.0.jsonl`);
    if (!fs.existsSync(file)) return false;
    const text = fs.readFileSync(file, 'utf8');
    const lines = text.split(/\n/).filter(Boolean).slice(-80);
    const cutoff = Date.now() - withinMs;
    for (const line of lines) {
      try {
        const j = JSON.parse(line);
        const t = Date.parse(j.time || j.ts || '');
        if (!Number.isFinite(t) || t < cutoff) continue;
        const msg = String(j.error?.message || j.message || j.event || '');
        if (/invalid IPC/i.test(msg) || j.event === 'host.start.failed') return true;
      } catch {
        if (/invalid IPC/i.test(line)) return true;
      }
    }
  } catch {
    /* ignore */
  }
  return false;
}

const RECOVERY_HINT =
  'DSH Host 向壳发了非法 IPC（官方校验：仅 ready/fatal/platform-session 等合法帧）。常见于 profile/插件/账号会话坏态，不是墙板桥能修的。请在恢复页点「进入安全模式」或「插件管理」禁用最近插件；仍不行就「切换 Profile」/「重置」。也可临时用：npx @deepseek-ai/dsh web。成功进主界面后再刷新墙板。';

/**
 * @param {{ timeoutMs?: number }} [opts]
 * @returns {Promise<{ ok: boolean, launched: boolean, waiting?: boolean, message: string, spoken: string }>}
 */
export async function launchDshDesktop(opts = {}) {
  if (s_launchInflight) {
    console.log('[dsh-launch] join in-flight launch');
    return s_launchInflight;
  }
  s_launchInflight = launchDshDesktopExclusive(opts).finally(() => {
    s_launchInflight = null;
  });
  return s_launchInflight;
}

/**
 * 拉起一次并等到 RPC 或超时。
 * @returns {Promise<'ok'|'no_proc'|'timeout'|'recovery'>}
 */
async function spawnAndWait(target, extraArgs, timeoutMs) {
  console.log(
    `[dsh-launch] spawn kind=${target.kind} path=${target.path}` +
      (extraArgs.length ? ` args=${extraArgs.join(' ')}` : ''),
  );
  launchTarget(target, extraArgs);
  s_lastLaunchAt = Date.now();
  await sleep(2000);
  const earlyProcs = listDshProcessNames();
  console.log(
    `[dsh-launch] +2s procs=${earlyProcs.length ? earlyProcs.join('|') : '(none)'} ` +
      `endpoints=${endpointProbe().length}`,
  );
  if (earlyProcs.length === 0) return 'no_proc';
  /* 恢复模式也会留下进程，但 ~130ms 就 IPC 失败；多等一会看是否写出 endpoint */
  if (await waitUntilReachable(timeoutMs)) return 'ok';
  if (isRecoveryUiUp()) return 'recovery';
  return 'timeout';
}

async function launchDshDesktopExclusive(opts = {}) {
  const timeoutMs = Number(opts.timeoutMs) > 0 ? Number(opts.timeoutMs) : 30000;
  purgeStaleEndpoints();
  if (await isDshReachable()) {
    return { ok: true, launched: false, message: '', spoken: '' };
  }

  const target = resolveTarget();
  if (!target) {
    return {
      ok: false,
      launched: false,
      message:
        '电脑上找不到 DSH 安装。请在桥配置页填写 dsh_desktop_exe（如 D:\\Program Files\\DSH NEXT\\DSH NEXT.exe），或先手动打开一次。',
      spoken: '找不到 DSH 安装，请在电脑上手动打开。',
    };
  }

  const procsUp = isDshProcessRunning();
  const recoveryUp = isRecoveryUiUp();
  console.log(
    `[dsh-launch] before kind=${target.kind} path=${target.path} procsUp=${procsUp ? 1 : 0} ` +
      `recovery=${recoveryUp ? 1 : 0} procs=${listDshProcessNames().join('|') || '(none)'} ` +
      `endpoints=${endpointProbe().length} activeRun=${JSON.stringify(readActiveRun())}`,
  );

  const giveUpUntil = readGiveUpUntil();
  /* 冷却期内 / 近期日志已 invalid IPC：别再自动开，否则永远同一恢复页 */
  if (Date.now() < giveUpUntil || recentInvalidIpc()) {
    const left = Math.max(0, Math.ceil((Math.max(giveUpUntil, Date.now()) - Date.now()) / 1000));
    if (Date.now() >= giveUpUntil && recentInvalidIpc()) {
      writeGiveUpUntil(Date.now() + GIVEUP_MS);
    }
    console.log(`[dsh-launch] cooldown/recent-ipc — skip auto launch (left≈${left}s)`);
    return {
      ok: false,
      launched: false,
      waiting: false,
      message: left > 0 ? `${RECOVERY_HINT}（桥冷却 ${left}s）` : RECOVERY_HINT,
      spoken: 'DSH 进了恢复模式，请在电脑上点进入安全模式。',
    };
  }

  /*
   * 日志：健康启动 durationMs≈3.5s；失败 ≈130–290ms invalid IPC。
   * 恢复页已是失败态——再 taskkill+立刻开 = 同一失败。
   * 策略：最多一轮「长冷却杀干净 → 普通开 →（仍失败）再杀+安全模式」；然后冷却 3 分钟。
   */
  if (procsUp) {
    const recentlyLaunched = s_lastLaunchAt > 0 && Date.now() - s_lastLaunchAt < 45000;
    if (recentlyLaunched && !recoveryUp) {
      console.log('[dsh-launch] process up after recent launch — wait before kill');
      if (await waitUntilReachable(Math.min(timeoutMs, 12000))) {
        return { ok: true, launched: false, message: '', spoken: '' };
      }
    }
    if (recoveryUp) {
      console.log('[dsh-launch] recovery UI detected → one careful clean + safe-mode');
    } else {
      console.log('[dsh-launch] stuck without endpoint → clean restart');
    }
    await killDshProcesses({ waitMs: 12000, settleMs: 7000 });
  } else {
    clearBridgeFiles();
    clearElectronLocks();
    clearActiveRun();
  }

  try {
    let result = await spawnAndWait(target, [], timeoutMs);
    if (result === 'ok') {
      clearGiveUp();
      return { ok: true, launched: true, message: '', spoken: '' };
    }
    if (result === 'no_proc') {
      return {
        ok: false,
        launched: false,
        message: `已尝试启动但未见 DSH 进程：${target.path}。请手动打开 DSH NEXT。`,
        spoken: '电脑上的 DSH 没启动起来，请手动打开。',
      };
    }

    console.log('[dsh-launch] still no endpoint → clean + --next-safe-mode once');
    await killDshProcesses({ waitMs: 12000, settleMs: 8000 });
    result = await spawnAndWait(target, DSH_SAFE_MODE_ARGS, Math.max(timeoutMs, 40000));
    if (result === 'ok') {
      clearGiveUp();
      return { ok: true, launched: true, message: '', spoken: '' };
    }
  } catch (e) {
    console.warn(`[dsh-launch] launch threw: ${e.message || e}`);
    writeGiveUpUntil(Date.now() + GIVEUP_MS);
    return {
      ok: false,
      launched: false,
      message: `无法启动 DSH：${String(e.message || e).slice(0, 160)}`,
      spoken: '没法打开电脑上的 DSH。',
    };
  }

  writeGiveUpUntil(Date.now() + GIVEUP_MS);
  console.warn(`[dsh-launch] give up ${GIVEUP_MS / 1000}s — leave recovery UI alone`);
  return {
    ok: false,
    launched: true,
    waiting: false,
    message: RECOVERY_HINT,
    spoken: 'DSH 进了恢复模式，请在电脑上点进入安全模式。',
  };
}
