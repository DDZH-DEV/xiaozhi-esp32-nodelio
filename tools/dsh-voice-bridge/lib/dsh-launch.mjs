/**
 * 语音桥常驻时，DSH Desktop 可能没开。墙板点「打开」后由本模块拉起本机工作台。
 * 找不到安装时返回明确错误，不瞎 spawn。
 */
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { connect } from './rpc.mjs';
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

function startMenuLnk() {
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
  return hits[0] || '';
}

function windowsExeCandidates(configured) {
  const home = os.homedir();
  const local = process.env.LOCALAPPDATA || path.join(home, 'AppData', 'Local');
  const pf = process.env.ProgramFiles || 'C:\\Program Files';
  const pf86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
  const named = [
    configured,
    process.env.DSH_DESKTOP_EXE,
    path.join(local, 'Programs', 'dsh', 'DSH.exe'),
    path.join(local, 'Programs', 'DSH', 'DSH.exe'),
    path.join(local, 'Programs', 'DSH Desktop', 'DSH Desktop.exe'),
    path.join(pf, 'DSH', 'DSH.exe'),
    path.join(pf, 'DSH Desktop', 'DSH Desktop.exe'),
    path.join(pf86, 'DSH', 'DSH.exe'),
  ].filter(Boolean);
  const out = [];
  for (const p of named) {
    if (existsFile(p)) out.push(p);
  }
  const prog = path.join(local, 'Programs');
  try {
    for (const ent of fs.readdirSync(prog, { withFileTypes: true })) {
      if (!ent.isDirectory() || !/dsh/i.test(ent.name)) continue;
      const dir = path.join(prog, ent.name);
      try {
        for (const f of fs.readdirSync(dir)) {
          if (/\.exe$/i.test(f) && /dsh/i.test(f)) {
            const full = path.join(dir, f);
            if (existsFile(full)) out.push(full);
          }
        }
      } catch {
        /* skip */
      }
    }
  } catch {
    /* skip */
  }
  return out;
}

function resolveTarget() {
  const configured = String(getConfig().dsh_desktop_exe || '').trim();
  if (process.platform === 'win32') {
    const exes = windowsExeCandidates(configured);
    if (exes[0]) return { kind: 'exe', path: exes[0] };
    const lnk = startMenuLnk();
    if (lnk) return { kind: 'lnk', path: lnk };
    return null;
  }
  if (configured && existsFile(configured)) return { kind: 'exe', path: configured };
  return { kind: 'app', path: configured || 'DSH' };
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

function launchTarget(target) {
  if (target.kind === 'exe') {
    spawnDetached(target.path, []);
    return;
  }
  if (target.kind === 'lnk') {
    spawnDetached('cmd.exe', ['/c', 'start', '', target.path]);
    return;
  }
  if (process.platform === 'darwin') {
    spawnDetached('open', ['-a', target.path]);
    return;
  }
  spawnDetached(target.path, []);
}

/**
 * @param {{ timeoutMs?: number }} [opts]
 * @returns {Promise<{ ok: boolean, launched: boolean, waiting?: boolean, message: string, spoken: string }>}
 */
export async function launchDshDesktop(opts = {}) {
  const timeoutMs = Number(opts.timeoutMs) > 0 ? Number(opts.timeoutMs) : 12000;
  if (await isDshReachable()) {
    return { ok: true, launched: false, message: '', spoken: '' };
  }
  const target = resolveTarget();
  if (!target) {
    return {
      ok: false,
      launched: false,
      message: '电脑上找不到 DSH 安装。请在桥配置页填写 dsh_desktop_exe，或先手动打开一次。',
      spoken: '找不到 DSH 安装，请在电脑上手动打开。',
    };
  }
  try {
    launchTarget(target);
  } catch (e) {
    return {
      ok: false,
      launched: false,
      message: `无法启动 DSH：${String(e.message || e).slice(0, 160)}`,
      spoken: '没法打开电脑上的 DSH。',
    };
  }
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    await sleep(500);
    if (await isDshReachable()) {
      return { ok: true, launched: true, message: '', spoken: '' };
    }
  }
  return {
    ok: true,
    launched: true,
    waiting: true,
    message: '已在电脑上发出打开请求，请稍候再刷新。',
    spoken: '正在打开电脑上的 DSH，请稍候刷新。',
  };
}
