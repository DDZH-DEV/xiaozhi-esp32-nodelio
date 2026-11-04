/**
 * 桥日志：控制台 + 文件双写，**永不阻塞事件循环**。
 *
 * 为什么需要它（Windows 上「命令行容易卡住 → 数据不能上报」的根因之一）：
 *
 * 1) 控制台写满/暂停：
 *    Windows 控制台默认带 QuickEdit。用户点一下控制台窗口 = 选中模式 =
 *    输出暂停 → console.log 的写入在管道缓冲区满后**无限等待** →
 *    Node 事件循环被卡住 → HTTP 不回包、watch 定时器不跑、WS 心跳停、
 *    hint 帧发不出去 → 墙板看不到任何进度。
 *
 * 2) EPIPE 崩溃：
 *    start-bridge.bat 用管道输出时，若下游（如 `| more` 或被关闭的控制台）
 *    提前退出，stdout 写会抛 EPIPE。未捕获的 EPIPE 会**直接杀掉桥进程**，
 *    于是「桥没了 → 数据完全不上报」。
 *
 * 做法：
 * - 文件走 fs.WriteStream（异步追加，内核缓冲），不阻塞事件循环；
 * - 控制台写全部 try/catch 包裹，EPIPE/EBADF 一律吞掉；
 * - stdout/stderr 的 'error' 事件挂 no-op handler，杜绝 EPIPE 崩溃；
 * - 文件写失败只警告，绝不抛。
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

/** 日志文件：与桥同目录，桥崩了也能事后查。 */
export const LOG_PATH = path.join(ROOT, '_bridge_full.log');

/** 最多保留的日志体积（字节）；超过就轮转一次。 */
const MAX_LOG_BYTES = 8 * 1024 * 1024;

let s_stream = null;
let s_streamBroken = false;
let s_written = 0;

/** 打开（或复用）追加写入流。 */
function ensureStream() {
  if (s_stream || s_streamBroken) return s_stream;
  try {
    /* 轮转：超过上限就换个备份名，避免无限增长 */
    try {
      const st = fs.statSync(LOG_PATH);
      if (st.size > MAX_LOG_BYTES) {
        fs.renameSync(LOG_PATH, `${LOG_PATH}.1`);
      }
    } catch {
      /* 文件不存在，正常 */
    }
    const stream = fs.createWriteStream(LOG_PATH, { flags: 'a' });
    stream.on('error', (e) => {
      s_streamBroken = true;
      s_stream = null;
      try {
        process.stderr.write(`[logger] log stream error: ${e.message}\n`);
      } catch {
        /* ignore */
      }
    });
    s_stream = stream;
  } catch (e) {
    s_streamBroken = true;
    try {
      process.stderr.write(`[logger] cannot open log file: ${e.message}\n`);
    } catch {
      /* ignore */
    }
  }
  return s_stream;
}

/**
 * 写一行到日志文件（异步，不阻塞）。
 * @param {string} line
 */
export function writeLogLine(line) {
  const stream = ensureStream();
  if (!stream) return;
  try {
    stream.write(`${line}\n`);
    s_written += 1;
  } catch {
    /* ignore */
  }
}

/**
 * 控制台是否可写。
 *
 * Node 在 Windows 上：TTY 写是**异步**的（不阻塞事件循环）；
 * 而**文件/管道写是同步的** —— 输出被重定向到文件或管道时，
 * 慢盘或慢读端会让 process.stdout.write 阻塞整个事件循环。
 * 所以非 TTY 场景只写文件（已异步落盘），不再碰 stdout。
 */
function consoleWritable() {
  try {
    return Boolean(process.stdout && process.stdout.isTTY);
  } catch {
    return false;
  }
}

/** 控制台写：吞掉 EPIPE/EBADF 等一切管道错误。 */
function safeConsoleWrite(stream, text) {
  try {
    stream.write(text);
  } catch {
    /* EPIPE / EBADF / 句柄已关闭：忽略，不能让桥崩 */
  }
}

function formatArgs(args) {
  return args
    .map((a) => {
      if (typeof a === 'string') return a;
      if (a instanceof Error) return a.stack || a.message;
      try {
        return JSON.stringify(a);
      } catch {
        return String(a);
      }
    })
    .join(' ')
    .replace(/\s+/g, ' ');
}

/**
 * 打一条带时间戳的日志：文件 + 控制台。
 * @param {...any} msgs
 */
export function log(...msgs) {
  const text = formatArgs(msgs);
  const line = `${new Date().toISOString()} [bridge] ${text}`;
  writeLogLine(line);
  if (consoleWritable()) safeConsoleWrite(process.stdout, `${line}\n`);
}

/**
 * 安装控制台捕获：把 console.log/warn/error 同时落到日志文件，
 * 并让控制台写永远不抛。
 *
 * 必须在任何业务日志之前调用。
 */
export function installConsoleCapture() {
  /* EPIPE 保护：stdout/stderr 断了也不能让进程退出 */
  for (const s of [process.stdout, process.stderr]) {
    if (!s) continue;
    try {
      s.on('error', () => {});
    } catch {
      /* ignore */
    }
  }

  const original = {
    log: console.log.bind(console),
    warn: console.warn.bind(console),
    error: console.error.bind(console),
  };

  /* 非 TTY（重定向到文件/管道）：只落文件，避免同步写阻塞事件循环 */
  const toConsole = consoleWritable();

  const patch = (level, sink) =>
    function patched(...args) {
      const text = formatArgs(args);
      writeLogLine(`${new Date().toISOString()} [${level}] ${text}`);
      if (!toConsole) return;
      try {
        sink(text + '\n');
      } catch {
        /* ignore */
      }
    };

  console.log = patch('info', (t) => safeConsoleWrite(process.stdout, t));
  console.warn = patch('warn', (t) => safeConsoleWrite(process.stderr, t));
  console.error = patch('error', (t) => safeConsoleWrite(process.stderr, t));

  return {
    toConsole,
    restore() {
      console.log = original.log;
      console.warn = original.warn;
      console.error = original.error;
    },
  };
}

/** 读日志尾部。 */
export function logTail(n = 500) {
  try {
    const st = fs.statSync(LOG_PATH);
    if (st.size === 0) return [];
    const lines = fs.readFileSync(LOG_PATH, 'utf8').split('\n').filter((l) => l.trim());
    return lines.slice(-n);
  } catch {
    return [];
  }
}

/** 日志文件状态。 */
export function logStats() {
  return {
    file: LOG_PATH,
    written: s_written,
    exists: fs.existsSync(LOG_PATH),
    size: (() => {
      try {
        return fs.statSync(LOG_PATH).size;
      } catch {
        return 0;
      }
    })(),
  };
}
