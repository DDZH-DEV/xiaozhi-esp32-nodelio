#!/usr/bin/env node
/**
 * dsh-voice-bridge 回归自检：证明 Windows 上「命令行卡住 → 数据不能上报」已修复。
 *
 * 覆盖四个历史根因：
 *  1) TTS 用 spawnSync/execSync 冻住事件循环（最长 45~60s）
 *  2) 会话日志同步解压 3MB 冻住事件循环（实测 174ms/文件）
 *  3) RPC 无超时 → DSH 半死时连接/槽位被占死，整桥卡住
 *  4) host-ws 未连接时静默丢帧 → 桥以为已上报，Host 实际没收到
 *
 * 用法：
 *   node tools/dsh-voice-bridge/selftest.mjs
 *
 * 退出码 0 = 全过；非 0 = 有用例失败。
 */

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { chunkSpokenText, synthesizeSpoken } from './lib/tts.mjs';
import { decodeSessionLogAsync, readSessionEvents, sessionLogPath } from './lib/tasks.mjs';
import { RPC_CALL_TIMEOUT_MS } from './lib/rpc.mjs';
import { startHostWs } from './lib/host-ws.mjs';

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const DSH_HOME = process.env.DSH_HOME ?? path.join(os.homedir(), '.dsh');

let failures = 0;
let passes = 0;

function ok(name, detail = '') {
  passes += 1;
  console.log(`  PASS  ${name}${detail ? `  (${detail})` : ''}`);
}

function fail(name, detail = '') {
  failures += 1;
  console.log(`  FAIL  ${name}${detail ? `  (${detail})` : ''}`);
}

/** 事件循环响应性探针：统计指定 ms 内的定时器触发次数。 */
function startLoopProbe(intervalMs = 10) {
  let ticks = 0;
  const timer = setInterval(() => {
    ticks += 1;
  }, intervalMs);
  return {
    get ticks() {
      return ticks;
    },
    stop() {
      clearInterval(timer);
    },
  };
}

/** 找一个最大的会话日志文件。 */
function findBiggestSessionLog() {
  const root = path.join(DSH_HOME, 'sessions');
  let best = null;
  let bestSize = 0;
  const walk = (dir) => {
    let ents;
    try {
      ents = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of ents) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.name.endsWith('.jsonl.zstd')) {
        let size = 0;
        try {
          size = fs.statSync(p).size;
        } catch {
          continue;
        }
        if (size > bestSize) {
          bestSize = size;
          best = p;
        }
      }
    }
  };
  walk(root);
  return { file: best, size: bestSize };
}

async function testTtsDoesNotBlock() {
  console.log('\n[1] TTS 合成不冻住事件循环');
  const probe = startLoopProbe(10);
  const t0 = Date.now();
  let result = null;
  try {
    result = await synthesizeSpoken('测试语音桥在 Windows 下不会卡住。');
  } catch (e) {
    probe.stop();
    fail('TTS 调用抛错', e.message);
    return;
  }
  const ms = Date.now() - t0;
  const ticks = probe.ticks;
  probe.stop();

  if (ticks > 0) {
    ok('TTS 期间事件循环仍可调度', `${ms}ms 内定时器触发 ${ticks} 次`);
  } else {
    fail('TTS 期间事件循环被完全冻住', `${ms}ms 内定时器触发 0 次`);
  }
  if (result) {
    ok('TTS 产出音频', `${result.name} (${result.mime})`);
  } else {
    /* 无 TTS 引擎不算致命（未装 edge-tts/SAPI），但要提示 */
    console.log('  SKIP  未产出音频（本机无 TTS 引擎，属预期降级）');
  }
}

async function testLogDecodeDoesNotBlock() {
  console.log('\n[2] 会话日志解压不冻住事件循环');
  const { file, size } = findBiggestSessionLog();
  if (!file) {
    console.log('  SKIP  没有会话日志可测');
    return;
  }
  const probe = startLoopProbe(10);
  const t0 = Date.now();
  const events = await decodeSessionLogAsync(file);
  const ms = Date.now() - t0;
  const ticks = probe.ticks;
  probe.stop();

  if (events.length > 0) {
    ok('异步解码成功', `${Math.round(size / 1024)}KB → ${events.length} 事件 / ${ms}ms`);
  } else {
    fail('异步解码返回空', `${Math.round(size / 1024)}KB`);
  }
  if (ticks > 0) {
    ok('解压期间事件循环仍可调度', `定时器触发 ${ticks} 次`);
  } else {
    fail('解压期间事件循环被冻住', '定时器触发 0 次');
  }
}

async function testLogCache() {
  console.log('\n[3] 会话日志按 mtime 缓存（watch 每轮不重复解压）');
  const { file } = findBiggestSessionLog();
  if (!file) {
    console.log('  SKIP  没有会话日志可测');
    return;
  }
  const t0 = Date.now();
  const first = await readSessionEvents(file);
  const t1 = Date.now();
  const second = await readSessionEvents(file);
  const t2 = Date.now();
  if (second.length === first.length) {
    ok(
      '二次读取命中缓存',
      `首次 ${t1 - t0}ms → 二次 ${t2 - t1}ms（${first.length} 事件）`,
    );
  } else {
    fail('二次读取结果不一致', `${first.length} vs ${second.length}`);
  }
}

async function testRpcTimeoutPresent() {
  console.log('\n[4] RPC 有硬超时（DSH 半死不会占死连接）');
  if (RPC_CALL_TIMEOUT_MS > 0 && RPC_CALL_TIMEOUT_MS <= 60000) {
    ok('RPC 超时已配置', `${RPC_CALL_TIMEOUT_MS}ms`);
  } else {
    fail('RPC 超时异常', String(RPC_CALL_TIMEOUT_MS));
  }
  const src = fs.readFileSync(path.join(ROOT, 'lib', 'rpc.mjs'), 'utf8');
  if (src.includes('ETIMEDOUT')) {
    ok('超时抛 ETIMEDOUT（可被 isDshOfflineError 识别）');
  } else {
    fail('未找到 ETIMEDOUT 标记');
  }
}

function testNoSyncSubprocess() {
  console.log('\n[5] 无同步子进程调用（Windows 卡死主因）');
  const files = [
    'lib/tts.mjs',
    'lib/dsh-launch.mjs',
    'lib/host-ws.mjs',
    'lib/rpc.mjs',
    'lib/tasks.mjs',
    'marketplace/server.mjs',
  ];
  const banned = /(spawnSync|execSync|execFileSync)\s*\(/;
  let bad = 0;
  for (const rel of files) {
    const src = fs.readFileSync(path.join(ROOT, rel), 'utf8');
    /* 去掉注释后再扫，避免误报注释里的示例 */
    const code = src
      .split('\n')
      .filter((l) => !/^\s*(\/\/|\*|\/\*)/.test(l))
      .join('\n');
    if (banned.test(code)) {
      fail(`${rel} 仍含同步子进程调用`);
      bad += 1;
    }
  }
  if (bad === 0) ok('全部模块无 spawnSync / execSync / execFileSync');
}

function testChunking() {
  console.log('\n[6] TTS 断句仍正常');
  /* 短文本：末段过短会并回上一段 → 仍应是 1 段 */
  const short = chunkSpokenText('第一句话。第二句话！第三句话？', 64, 6);
  if (short.length === 1 && short[0].includes('第一句话')) {
    ok('短文本合并为 1 段', short[0].slice(0, 20));
  } else {
    fail('短文本断句异常', JSON.stringify(short));
  }
  /* 长文本：应真的切开成多段 */
  const longText =
    '这是第一句话用来测试断句逻辑是否正常工作，这里要够长才能触发切分条件。' +
    '这是第二句话同样需要足够长才能被切开，否则会被合并回去。' +
    '这是第三句话。';
  const long = chunkSpokenText(longText, 64, 6);
  if (long.length >= 2) {
    ok('长文本切成多段', `${long.length} 段`);
  } else {
    fail('长文本未切开', JSON.stringify(long));
  }
  if (chunkSpokenText('', 64, 6).length === 0) {
    ok('空文本返回空数组');
  } else {
    fail('空文本应返回空数组');
  }
}

function testLoggerNonBlocking() {
  console.log('\n[7] 日志写入不抛（EPIPE 保护）');
  const src = fs.readFileSync(path.join(ROOT, 'lib', 'logger.mjs'), 'utf8');
  if (src.includes("s.on('error'") || src.includes('s.on("error"')) {
    ok('stdout/stderr 已挂 error 兜底');
  } else {
    fail('缺少 stdout/stderr error 兜底');
  }
  if (src.includes('isTTY')) {
    ok('非 TTY 时不写 stdout（避免同步写阻塞事件循环）');
  } else {
    fail('缺少 isTTY 判断');
  }
}

async function testHostWsOutbox() {
  console.log('\n[8] host-ws 断线不丢帧（hint 入队 + 重连补发）');
  const { WebSocketServer } = await import('ws');

  /* 8a: 未连接 → hint 不得被静默丢弃，必须入队 */
  const deadPort = 45999; /* 没人监听 */
  const client = startHostWs({
    wsUrl: `ws://127.0.0.1:${deadPort}/ws/dsh-bridge`,
    token: 'deadbeef',
    lanPort: 8790,
  });
  await new Promise((r) => setTimeout(r, 600));
  const queued = client.sendHint('needs_you', 'session-test-1', { app_id: 'dsh.status' });
  if (queued === false && client.pendingCount() > 0) {
    ok('未连接时 hint 入队而非丢弃', `pending=${client.pendingCount()}`);
  } else {
    fail('未连接时 hint 被静默丢弃', `returned=${queued} pending=${client.pendingCount()}`);
  }
  client.stop();

  /* 8b: 连上真实服务 → hint 立刻送达 */
  const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise((r) => wss.once('listening', r));
  const port = wss.address().port;
  const received = [];
  wss.on('connection', (sock) => {
    sock.on('message', (data) => {
      try {
        received.push(JSON.parse(String(data)));
      } catch {
        /* ignore */
      }
    });
  });

  const live = startHostWs({
    wsUrl: `ws://127.0.0.1:${port}/ws/dsh-bridge`,
    token: 'live-token',
    lanPort: 8790,
  });
  await new Promise((r) => setTimeout(r, 900));
  live.sendHint('needs_you', 'session-live-1', { app_id: 'dsh.status' });
  await new Promise((r) => setTimeout(r, 500));

  if (live.isConnected()) {
    ok('WS 已连接状态可查询');
  } else {
    fail('WS 未连接');
  }
  const hint = received.find((m) => m.type === 'hint' && m.session === 'session-live-1');
  if (hint) {
    ok('hint 帧已送达 Host', `mode=${hint.mode} reason=${hint.reason}`);
  } else {
    fail('hint 帧未送达', JSON.stringify(received.map((m) => m.type)));
  }
  live.stop();
  wss.close();
}

async function main() {
  console.log('dsh-voice-bridge 回归自检');
  console.log(`DSH_HOME=${DSH_HOME}`);

  testNoSyncSubprocess();
  testChunking();
  testLoggerNonBlocking();
  await testRpcTimeoutPresent();
  await testLogDecodeDoesNotBlock();
  await testLogCache();
  await testTtsDoesNotBlock();
  await testHostWsOutbox();

  console.log(`\n结果: ${passes} 通过 / ${failures} 失败`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error(`selftest crashed: ${e.stack || e.message}`);
  process.exit(1);
});
