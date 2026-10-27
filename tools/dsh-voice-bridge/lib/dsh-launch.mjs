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
      client?.close();
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

/** 杀掉所有 DSH GUI（含 Electron 子进程），好让 Agents Anywhere 重新写 endpoint。 */
function killDshProcesses() {
  if (process.platform !== 'win32') return;
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
}

function spawnDetached(cmd, args) {
  const child = spawn(cmd, args, {
    detached: true,
    stdio: 'ignore',
    windowsHide: true,
    shell: false,
  });
  child.unref();
}

/**
 * Electron GUI 勿用 spawn(exe,{windowsHide:true})——容易无窗/不起来。
 * 统一经 PowerShell Start-Process（隐藏的是 powershell 自己）。
 */
function launchTarget(target) {
  if (process.platform === 'win32' && (target.kind === 'exe' || target.kind === 'lnk')) {
    /* 旧版 PowerShell 无 -LiteralPath；-FilePath 通用。引号内路径即可。 */
    const quoted = JSON.stringify(target.path);
    spawnDetached('powershell.exe', [
      '-NoProfile',
      '-WindowStyle',
      'Hidden',
      '-Command',
      `Start-Process -FilePath ${quoted}`,
    ]);
    return;
  }
  if (process.platform === 'darwin') {
    spawnDetached('open', ['-a', target.path]);
    return;
  }
  spawnDetached(target.path, []);
}

async function waitUntilReachable(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    await sleep(500);
    if (await isDshReachable()) return true;
  }
  return false;
}

/**
 * @param {{ timeoutMs?: number }} [opts]
 * @returns {Promise<{ ok: boolean, launched: boolean, waiting?: boolean, message: string, spoken: string }>}
 */
export async function launchDshDesktop(opts = {}) {
  const timeoutMs = Number(opts.timeoutMs) > 0 ? Number(opts.timeoutMs) : 25000;
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

  const zombie = isDshProcessRunning();
  if (zombie) {
    /* 进程在、RPC 死：僵死 GUI。杀干净再拉，否则只会干等 ECONNREFUSED。 */
    console.log(`[dsh-launch] RPC down while process up → restart via ${target.path}`);
    killDshProcesses();
    purgeStaleEndpoints();
    await sleep(1200);
  }

  try {
    console.log(`[dsh-launch] spawn kind=${target.kind} path=${target.path}`);
    launchTarget(target);
  } catch (e) {
    return {
      ok: false,
      launched: false,
      message: `无法启动 DSH：${String(e.message || e).slice(0, 160)}`,
      spoken: '没法打开电脑上的 DSH。',
    };
  }

  if (await waitUntilReachable(timeoutMs)) {
    return { ok: true, launched: true, message: '', spoken: '' };
  }

  return {
    ok: true,
    launched: true,
    waiting: true,
    message: zombie
      ? '已重启 DSH，但远程控制仍未就绪。请在 DSH 主界面确认已登录，并开启 Agents Anywhere / 远程控制，再点刷新。'
      : '已打开 DSH，但远程控制仍未就绪。请确认主界面已出现并已登录，再点刷新。',
    spoken: 'DSH 远程控制还没就绪，请稍候再刷新。',
  };
}
