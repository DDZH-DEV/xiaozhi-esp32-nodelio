// Minimal JSON-RPC 1.x client for the DSH "Agents Anywhere" bridge endpoint.
//
// The bridge is a newline-delimited JSON-RPC server on 127.0.0.1. It is
// already running whenever DSH Desktop is running; no extra service needed.
// Endpoint + bearer token live in ~/.agents-anywhere/dsh-bridge/endpoint.json
// (falls back to ~/.dsh/agents-anywhere/bridge/endpoint.json on older builds).

import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';

export const ENDPOINT_CANDIDATES = [
  path.join(os.homedir(), '.agents-anywhere', 'dsh-bridge', 'endpoint.json'),
  path.join(os.homedir(), '.agents-anywhere', 'dsh-bridge-next', 'endpoint.json'),
  path.join(os.homedir(), '.dsh', 'agents-anywhere', 'bridge', 'endpoint.json'),
];

/** 串行化短连接：健康检查 / watch / status 叠 initialize 会 ECONNRESET（见诊断日志 10:23:53）。 */
let s_aaChain = Promise.resolve();
let s_aaActive = 0;

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

/** pid 仍存活（Windows 上 kill(pid,0) 可用）。无 pid 字段返回 null。 */
export function isEndpointPidAlive(pid) {
  if (pid == null || pid === '') return null;
  const n = Number(pid);
  if (!Number.isFinite(n) || n <= 0) return null;
  try {
    process.kill(n, 0);
    return true;
  } catch (e) {
    if (e && e.code === 'ESRCH') return false;
    /* EPERM 等：进程多半还在 */
    return true;
  }
}

/**
 * 丢掉「写着端口但 AA 进程已死」的 endpoint.json。
 * 否则会一直 ECONNREFUSED 旧端口，墙板永远停在「电脑 DSH 没开」。
 * @returns {number} 删除个数
 */
export function purgeStaleEndpoints() {
  let n = 0;
  for (const candidate of ENDPOINT_CANDIDATES) {
    try {
      if (!fs.existsSync(candidate)) continue;
      const raw = JSON.parse(fs.readFileSync(candidate, 'utf8'));
      const alive = isEndpointPidAlive(raw?.pid);
      if (alive === false) {
        fs.unlinkSync(candidate);
        n += 1;
        console.log(`[rpc] purged stale endpoint pid=${raw.pid} port=${raw.port} ${candidate}`);
      }
    } catch {
      /* ignore */
    }
  }
  return n;
}

export function readEndpoint() {
  purgeStaleEndpoints();
  for (const candidate of ENDPOINT_CANDIDATES) {
    try {
      const raw = JSON.parse(fs.readFileSync(candidate, 'utf8'));
      if (raw && typeof raw.port === 'number' && typeof raw.token === 'string') {
        const alive = isEndpointPidAlive(raw.pid);
        if (alive === false) continue;
        return { ...raw, source: candidate };
      }
    } catch {
      // try next candidate
    }
  }
  throw new Error(
    'DSH bridge endpoint not found. Is DSH Desktop running? Checked: ' +
      ENDPOINT_CANDIDATES.join(', '),
  );
}

/**
 * One authenticated connection. Resolves after `initialize` succeeds.
 * Every method returns a promise keyed by request id.
 * 同刻只允许一条 AA 连接；close 等 FIN，并与下一连间隔，避免 ECONNRESET。
 */
export async function connect(options = {}) {
  const run = s_aaChain.then(() => connectExclusive(options));
  s_aaChain = run.then(
    () => sleep(80),
    () => sleep(80),
  );
  return run;
}

async function connectExclusive(options = {}) {
  /* 若上一连接尚未 close 完，稍等（health 与 watch 曾 3ms 内叠 initialize） */
  for (let i = 0; i < 40 && s_aaActive > 0; i += 1) {
    await sleep(50);
  }
  const endpoint = options.endpoint ?? readEndpoint();
  const namespace = options.namespace ?? 'dsh-voice-bridge';
  const host = endpoint.host || '127.0.0.1';
  const socket = net.connect(endpoint.port, host);

  let buffer = '';
  let nextId = 1;
  const pending = new Map();
  const listeners = new Set();
  let closed = false;
  s_aaActive += 1;

  socket.setNoDelay(true);

  const onLine = (line) => {
    if (!line.trim()) return;
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      return;
    }
    if (message.id !== undefined && pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) reject(new RpcError(message.error));
      else resolve(message.result);
      return;
    }
    // Server-initiated notification (runtime.sync.batch, runtime.error, ...)
    for (const listener of listeners) listener(message);
  };

  socket.on('data', (chunk) => {
    buffer += chunk.toString('utf8');
    let index;
    while ((index = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, index);
      buffer = buffer.slice(index + 1);
      onLine(line);
    }
  });

  const markClosed = () => {
    if (closed) return;
    closed = true;
    s_aaActive = Math.max(0, s_aaActive - 1);
    for (const { reject } of pending.values()) {
      reject(new Error('DSH bridge connection closed'));
    }
    pending.clear();
  };

  socket.on('close', markClosed);
  socket.on('error', (error) => {
    for (const { reject } of pending.values()) reject(error);
    pending.clear();
  });

  const call = (method, params = {}) =>
    new Promise((resolve, reject) => {
      if (closed) {
        reject(new Error('DSH bridge connection closed'));
        return;
      }
      const id = nextId++;
      pending.set(id, { resolve, reject });
      socket.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    });

  try {
    await new Promise((resolve, reject) => {
      socket.once('connect', resolve);
      socket.once('error', reject);
    });

    const identity = await call('initialize', {
      authToken: endpoint.token,
      protocolVersion: '1.0',
      runtime: 'dsh',
      sessionNamespace: namespace,
    });

    return {
      identity,
      endpoint,
      call,
      onNotification: (listener) => {
        listeners.add(listener);
        return () => listeners.delete(listener);
      },
      /** 等 TCP FIN，避免半开重置打到 AA 插件 */
      close: () =>
        new Promise((resolve) => {
          if (closed || socket.destroyed) {
            markClosed();
            resolve();
            return;
          }
          const done = () => {
            socket.off('close', done);
            socket.off('error', done);
            resolve();
          };
          socket.once('close', done);
          socket.once('error', done);
          socket.end();
          setTimeout(() => {
            if (!socket.destroyed) socket.destroy();
            done();
          }, 1500);
        }),
    };
  } catch (e) {
    markClosed();
    try {
      socket.destroy();
    } catch {
      /* ignore */
    }
    throw e;
  }
}

export class RpcError extends Error {
  constructor(error) {
    super(error.message ?? 'DSH bridge error');
    this.name = 'RpcError';
    this.code = error.data?.code ?? error.code;
    this.retryable = error.data?.retryable ?? false;
    this.raw = error;
  }
}
