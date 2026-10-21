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

function textFromParts(parts) {
  return (parts ?? [])
    .filter((part) => part.type === 'text' && typeof part.text === 'string')
    .map((part) => part.text)
    .join('\n')
    .trim();
}

function collapseWs(s) {
  return String(s ?? '')
    .replace(/\s+/g, ' ')
    .trim();
}

/** Lines that are agent/tool dumps, not a human task title. */
function isNoiseTitleLine(line) {
  const s = String(line ?? '').trim();
  if (!s) return true;
  if (/^[IEW]\s*\(\d+\)/.test(s)) return true;
  if (/^State:\s+/i.test(s)) return true;
  if (/^(listening|speaking|notifying|idle|connecting)\s*->/i.test(s)) return true;
  if (/edit\s*报错/i.test(s)) return true;
  if (/^Error:\s+/i.test(s)) return true;
  if (/^[A-Za-z]:\\/.test(s)) return true;
  if (/old_string was not found/i.test(s)) return true;
  if (/^\[stderr\]/i.test(s)) return true;
  if (/ParserError:/i.test(s)) return true;
  return false;
}

/** Strip ESP/IDF log-line noise so panel titles stay human-readable. */
export function cleanTaskTitle(raw) {
  if (raw == null) return '';
  let s = String(raw).replace(/\r\n/g, '\n').trim();
  if (!s) return '';
  const lines = s.split('\n').map((l) => l.trim()).filter(Boolean);
  let pick = '';
  for (const line of lines) {
    let cand = line.replace(/^[IEW]\s*\(\d+\)\s*[\w./-]+:\s*/g, '');
    if (isNoiseTitleLine(cand)) continue;
    pick = cand;
    break;
  }
  if (!pick) {
    pick = (lines[0] ?? '').replace(/^[IEW]\s*\(\d+\)\s*[\w./-]+:\s*/g, '');
  }
  pick = collapseWs(pick);
  if (pick.length > 64) pick = pick.slice(0, 64);
  return pick;
}

function clip(s, n) {
  const t = collapseWs(s);
  if (t.length <= n) return t;
  return t.slice(0, Math.max(0, n - 1)) + '…';
}

/**
 * 进度卡 TTS：不逐条念标题，只问选第几个（屏上已有列表）。
 * detailed[] = { session, state, folded, notices }
 */
export function speakStatusList(detailed) {
  const list = Array.isArray(detailed) ? detailed : [];
  if (list.length === 0) {
    return '当前没有任务。可以说「让它…」派活。';
  }
  if (list.length === 1) {
    return '请说第一个任务，或点选。';
  }
  return `共${list.length}个任务，请说第几个任务。`;
}

/** 工作区列表播报：不问逐条名称，只请选第几个 */
export function speakWorkspaceList(workspaces) {
  const list = Array.isArray(workspaces) ? workspaces : [];
  if (list.length === 0) {
    return '还没有工作区。请先在 APP 配置，或说「让它…」派活。';
  }
  if (list.length === 1) {
    return '请说第一个工作区，或点选。';
  }
  return `共${list.length}个工作区，请说第几个工作区。`;
}

/** 短确认句不当标题（否则「是的」盖住真实需求）。 */
function isTrivialUserAsk(text) {
  const t = String(text || '')
    .trim()
    .replace(/[。.!！？?\s]+$/g, '');
  if (!t) return true;
  if (t.length <= 2) return true;
  return /^(是的?|好的?|确认|取消|继续|可以|行|嗯|对|否|不要|不用了)$/i.test(t);
}

/**
 * 末轮助手输出 → 面板纯文本：保留换行；图/表/代码围栏收成可读文字。
 */
export function formatPlainReply(text) {
  let s = String(text ?? '');
  if (!s) return '';
  s = s.replace(/\r\n/g, '\n');
  s = s.replace(/!\[[^\]]*\]\([^)]*\)/g, '[图片]');
  s = s.replace(/<img\b[^>]*>/gi, '[图片]');
  s = s.replace(/```[\w+-]*\r?\n?([\s\S]*?)```/g, (_, code) => String(code || '').trim());
  s = s.replace(/`([^`]+)`/g, '$1');
  s = s.replace(/^\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$/gm, '');
  s = s.replace(/^\|(.+)\|\s*$/gm, (_, row) =>
    String(row)
      .split('|')
      .map((c) => c.trim())
      .filter(Boolean)
      .join(' · '),
  );
  s = s.replace(/\*\*([^*]+)\*\*/g, '$1');
  s = s.replace(/__([^_]+)__/g, '$1');
  s = s.replace(/(^|[\s(（])\*([^*\n]+)\*([\s)）]|$)/g, '$1$2$3');
  s = s.replace(/\n{3,}/g, '\n\n');
  return s.trim();
}

/** 详情正文：优先末条助手；失败轮次用错误信息。 */
export function lastReplyPlain(folded) {
  const f = folded ?? {};
  if (f.lastAssistantText) return formatPlainReply(f.lastAssistantText);
  if (f.lastEndError) return formatPlainReply(f.lastEndError);
  if (f.lastEndReason && FAILURE_REASONS.has(f.lastEndReason)) {
    return `本轮中断：${f.lastEndReason}`;
  }
  return '';
}

/**
 * Panel/CLI title：优先「最近一句有效需求」（便于追踪本轮），再回落首条 / 会话名。
 */
export function displayTaskTitle(session, folded) {
  const last = cleanTaskTitle(folded?.lastUserText);
  if (last && !isTrivialUserAsk(last)) return last;
  const first = cleanTaskTitle(folded?.firstUserText);
  if (first) return first;
  const foldT = cleanTaskTitle(folded?.title);
  if (foldT) return foldT;
  const sess = cleanTaskTitle(session?.title);
  if (sess) return sess;
  if (last) return last;
  return '(未命名)';
}

/**
 * One-line subtitle (≤120): what to do / what's happening / last result.
 * `liveStatus` from session.getState; `needsYou` when ATTENTION.
 */
export function displayTaskSubtitle(liveStatus, folded, needsYou) {
  const f = folded ?? {};
  if (needsYou) {
    return clip('等你确认 · 点允许或拒绝', 120);
  }
  const st = String(liveStatus ?? '');
  if (st === 'running' || f.running) {
    const where = f.lastTool ? `正在用 ${f.lastTool}` : '正在思考';
    const step = f.step > 0 ? ` · 第${f.step}步` : '';
    return clip(`${where}${step}`, 120);
  }
  const turnFailures = f.turnFailures ?? [];
  if (turnFailures.length) {
    const last = turnFailures[turnFailures.length - 1];
    const tool = last.tool ?? '某一步';
    const detail = last.detail ? clip(last.detail, 70) : '';
    return clip(detail ? `${tool} 报错：${detail}` : `${tool} 报错了`, 120);
  }
  if (f.lastEndReason && FAILURE_REASONS.has(f.lastEndReason)) {
    if (f.lastEndError) {
      return clip(`第${f.turn || '?'}轮失败 · ${f.lastEndError}`, 120);
    }
    return clip(`第${f.turn || '?'}轮中断：${f.lastEndReason}`, 120);
  }
  if (f.lastAssistantText) {
    return clip(`空闲 · ${f.lastAssistantText}`, 120);
  }
  if (f.turn > 0) {
    return clip(`空闲 · 第${f.turn}轮已完成`, 120);
  }
  return clip('空闲', 120);
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
    firstUserText: null,
    lastUserText: null,
    lastEndReason: null,
    lastEndError: null,
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
        state.lastEndError = null;
        state.turnToolCalls = 0;
        state.turnFailures = [];
        // Keep lastUserText / lastAssistantText across turns for panel readability.
        break;
      case 'turn/end':
        state.running = false;
        state.lastEndReason = data.reason?.kind ?? null;
        state.lastEndError = clip(
          data.reason?.error?.message || data.reason?.failure?.message || '',
          90,
        );
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
        const text = textFromParts(data.message?.content);
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
      case 'user/message': {
        // Only real user turns (not approval/system/context injects).
        const kind = data.source?.kind;
        if (kind && kind !== 'user') break;
        const text = textFromParts(data.content ?? data.message?.content);
        if (text) {
          if (!state.firstUserText) state.firstUserText = text;
          state.lastUserText = text;
        }
        break;
      }
      case 'assistant/message': {
        const text = textFromParts(data.message?.content ?? data.content);
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

/** One short sentence, safe to hand to TTS (≤160). */
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
    let base = `第 ${state.turn} 轮已完成，共调用 ${state.turnToolCalls ?? 0} 次工具。`;
    if (state.lastAssistantText) {
      const summary = clip(state.lastAssistantText, 90);
      base = clip(`${base} ${summary}`, 160);
    }
    return base;
  }
  return '没有正在进行的任务。';
}
