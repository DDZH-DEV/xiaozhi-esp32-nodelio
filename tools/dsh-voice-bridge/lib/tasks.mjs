// Task vocabulary: how a spoken instruction maps onto a DSH session, and how a
// session's state is read back as one short sentence.
//
// Design constraint: the panel is 480x480 with TTS in/out. A "task" is therefore
//   { workspace, sessionId, title, status, step, lastTool, needsYou, summary }
// and never a raw event dump.

import path from 'node:path';
import fs from 'node:fs';
import { zstdDecompressSync } from 'node:zlib';

/** DSH session status values that mean "a human has to act". */
export const ATTENTION = new Set(['waiting_approval', 'waiting', 'pending']);

/** Turn-level terminal reasons worth surfacing on a panel. */
const FAILURE_REASONS = new Set(['error', 'aborted', 'interrupted', 'cancelled']);

/**
 * Locate the session log for an external session id.
 * DSH stores `<dshHome>/sessions/--<slug>--/<session-id>/session.v4.jsonl.zstd`
 * where the slug runs of separators (':' and '\\' and '/') collapse to one '-':
 *   D:\xz25          -> --D-xz25--
 *   D:\SITES\NodeLio -> --D-SITES-NodeLio--
 */
export function sessionLogPath(dshHome, cwd, externalSessionId) {
  const slug = '--' + cwd.replace(/[\\/:]+/g, '-') + '--';
  return path.join(dshHome, 'sessions', slug, externalSessionId, 'session.v4.jsonl.zstd');
}

/**
 * DSH writes one zstd frame per append, so a log is a concatenation of frames.
 * `zstdDecompressSync` only reads the first frame; split on the magic number.
 */
export function decodeSessionLog(file) {
  const bytes = fs.readFileSync(file);
  const MAGIC = Buffer.from([0x28, 0xb5, 0x2f, 0xfd]);
  const offsets = [];
  let index = 0;
  while ((index = bytes.indexOf(MAGIC, index)) !== -1) {
    offsets.push(index);
    index += MAGIC.length;
  }
  let text = '';
  for (let i = 0; i < offsets.length; i += 1) {
    const end = i + 1 < offsets.length ? offsets[i + 1] : bytes.length;
    try {
      text += zstdDecompressSync(bytes.subarray(offsets[i], end)).toString('utf8');
    } catch {
      // A torn trailing frame (writer still appending) is expected; skip it.
    }
  }
  return text
    .split('\n')
    .filter(Boolean)
    .map((line) => {
      try {
        return JSON.parse(line);
      } catch {
        return null;
      }
    })
    .filter(Boolean);
}

/**
 * Fold a session's event stream into the one object a voice UI can speak.
 * Everything here is derived from real events, not guessed.
 */
export function foldSession(events, { cwd } = {}) {
  const header = events.find((event) => event.type === 'session') ?? {};
  const state = {
    sessionId: header.id,
    cwd: cwd ?? header.cwd ?? null,
    title: null,
    turn: 0,
    step: 0,
    running: false,
    lastTool: null,
    lastToolStatus: null,
    lastAssistantText: null,
    lastEndReason: null,
    toolCalls: 0,
    turnToolCalls: 0,
    // Failures are scoped to the current turn: an error in turn 3 says nothing
    // about turn 6, and a voice UI must not report a stale failure as current.
    failures: [],
    turnFailures: [],
  };

  for (const event of events) {
    const data = event.data ?? {};
    switch (event.type) {
      case 'session/title':
        state.title = data.title ?? state.title;
        break;
      case 'turn/start':
        state.turn = data.turn ?? state.turn;
        state.running = true;
        state.lastEndReason = null;
        state.turnToolCalls = 0;
        state.turnFailures = [];
        break;
      case 'turn/end':
        state.running = false;
        state.lastEndReason = data.reason?.kind ?? null;
        if (FAILURE_REASONS.has(state.lastEndReason)) {
          state.failures.push({ turn: state.turn, reason: state.lastEndReason });
        }
        break;
      case 'step/start':
        state.step = data.step ?? state.step;
        break;
      case 'tool/call':
        state.toolCalls += 1;
        state.turnToolCalls += 1;
        state.lastTool = data.name ?? state.lastTool;
        state.lastToolStatus = 'running';
        break;
      case 'tool/result': {
        const text = (data.message?.content ?? [])
          .filter((part) => part.type === 'text')
          .map((part) => part.text)
          .join('\n');
        const failed = /^Error:|\[exit code: [1-9]/.test(text.trim());
        state.lastToolStatus = failed ? 'failed' : 'ok';
        if (failed) {
          const failure = {
            turn: data.turn,
            step: data.step,
            tool: state.lastTool,
            detail: text.trim().slice(0, 160),
          };
          state.failures.push(failure);
          state.turnFailures.push(failure);
        }
        break;
      }
      case 'assistant/message': {
        const parts = data.message?.content ?? [];
        const text = parts
          .filter((part) => part.type === 'text')
          .map((part) => part.text)
          .join('\n')
          .trim();
        if (text) state.lastAssistantText = text;
        break;
      }
      default:
        break;
    }
  }

  state.needsYou = false; // filled in from live RPC state, not the log
  return state;
}

/** One short sentence, safe to hand to TTS. */
export function speakTask(state) {
  if (state.needsYou) return '有一项操作需要你确认。';
  if (state.running) {
    const where = state.lastTool ? `正在用 ${state.lastTool} 干活` : '正在思考';
    return `第 ${state.step} 步，${where}。`;
  }
  if (state.lastEndReason && FAILURE_REASONS.has(state.lastEndReason)) {
    return `第 ${state.turn} 轮中断了，原因 ${state.lastEndReason}。`;
  }
  const currentTurnFailures = state.turnFailures ?? [];
  if (currentTurnFailures.length) {
    const last = currentTurnFailures[currentTurnFailures.length - 1];
    return `第 ${state.turn} 轮做完了，但 ${last.tool ?? '某一步'} 报错了。`;
  }
  if (state.turn > 0) {
    return `第 ${state.turn} 轮已完成，共调用 ${state.turnToolCalls ?? 0} 次工具。`;
  }
  return '没有正在进行的任务。';
}
