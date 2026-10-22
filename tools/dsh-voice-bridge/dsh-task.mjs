#!/usr/bin/env node
// dsh-task — read and drive DSH tasks from a voice front end.
//
//   dsh-task list                       # every session, grouped by workspace
//   dsh-task show <sessionId|title>     # one task, folded to 8 fields
//   dsh-task send <workspace> "<text>"  # start a turn (creates session if none)
//   dsh-task interrupt <sessionId>      # stop the current turn
//   dsh-task answer <sessionId> <action># resolve a pending approval/question
//
// The transport is the JSON-RPC bridge that DSH Desktop already runs.

import { connect, readEndpoint, RpcError } from './lib/rpc.mjs';
import { resolveCreateParams } from './lib/dsh-create.mjs';
import { getConfig, loadConfig } from './lib/bridge-config.mjs';
import {
  ATTENTION,
  decodeSessionLog,
  displayTaskSubtitle,
  displayTaskTitle,
  foldSession,
  sessionLogPath,
  speakTask,
} from './lib/tasks.mjs';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';

const DSH_HOME = process.env.DSH_HOME ?? path.join(os.homedir(), '.dsh');

function usage(code = 2) {
  process.stderr.write(
    [
      'usage:',
      '  dsh-task list [--json]',
      '  dsh-task show <sessionId|title> [--json] [--speak]',
      '  dsh-task send <workspace> <text> [--wait] [--timeout=ms] [--json]',
      '  dsh-task wait <sessionId|title> [--after=N] [--timeout=ms] [--json]',
      '  dsh-task interrupt <sessionId>',
      '  dsh-task answer <sessionId> <noticeId> [actionId]',
      '',
      'notes:',
      '  --wait on send records the turn counter BEFORE sending, so a fast turn',
      '  cannot be missed. A bare `wait` starts counting at the current turn and',
      '  will block until the NEXT one, so prefer `send --wait` or pass --after.',
      '',
    ].join('\n'),
  );
  process.exit(code);
}

function parseArgs(argv) {
  const flags = new Set();
  const values = new Map();
  const positional = [];
  for (const arg of argv) {
    if (arg.startsWith('--')) {
      const body = arg.slice(2);
      const eq = body.indexOf('=');
      if (eq >= 0) values.set(body.slice(0, eq), body.slice(eq + 1));
      else flags.add(body);
    } else {
      positional.push(arg);
    }
  }
  return { flags, values, positional };
}

/** Pick a session from a human-ish reference: exact id, suffix, or title. */
function pickSession(sessions, reference) {
  const exact = sessions.find(
    (s) => s.sessionId === reference || s.externalSessionId === reference,
  );
  if (exact) return exact;

  const bySuffix = sessions.filter(
    (s) => s.sessionId.includes(reference) || s.externalSessionId.includes(reference),
  );
  if (bySuffix.length === 1) return bySuffix[0];

  const byTitle = sessions.filter((s) => (s.title ?? '').includes(reference));
  if (byTitle.length === 1) return byTitle[0];
  if (byTitle.length > 1) {
    throw new Error(
      `"${reference}" matches ${byTitle.length} sessions; be more specific:\n` +
        byTitle.map((s) => `  ${s.externalSessionId}  ${s.title}`).join('\n'),
    );
  }
  throw new Error(`no session matches "${reference}"`);
}

async function listTasks(client, asJson) {
  const { sessions } = await client.call('session.list', {});
  const rows = [];
  for (const session of sessions) {
    let live = null;
    try {
      live = await client.call('session.getState', {
        externalSessionId: session.externalSessionId,
      });
    } catch {
      // A cold session still lists; live state is optional.
    }
    const folded = foldFromDisk(session);
    const status = live?.status ?? 'unknown';
    const needsYou = ATTENTION.has(status);
    rows.push({
      workspace: session.cwd,
      sessionId: session.externalSessionId,
      title: displayTaskTitle(session, folded),
      subtitle: displayTaskSubtitle(status, folded, needsYou),
      live: session.metadata?.live ?? false,
      status,
      turn: folded?.turn ?? null,
      step: folded?.step ?? null,
      lastTool: folded?.lastTool ?? null,
      toolCalls: folded?.toolCalls ?? null,
      needsYou,
      speak: live ? null : '会话未激活，只能看到标题。',
    });
  }
  if (asJson) {
    process.stdout.write(JSON.stringify(rows, null, 2) + '\n');
    return;
  }
  const byWorkspace = new Map();
  for (const row of rows) {
    if (!byWorkspace.has(row.workspace)) byWorkspace.set(row.workspace, []);
    byWorkspace.get(row.workspace).push(row);
  }
  for (const [workspace, group] of byWorkspace) {
    process.stdout.write(`${workspace}\n`);
    for (const row of group) {
      const mark = row.needsYou ? '!' : row.status === 'running' ? '*' : ' ';
      process.stdout.write(`  ${mark} [${row.status}] ${row.title}\n`);
      process.stdout.write(`      ${row.subtitle}\n`);
      process.stdout.write(`      ${row.sessionId}\n`);
    }
  }
}

/** Fold the on-disk log; this is the only place turn/step/tool detail lives. */
function foldFromDisk(session) {
  const file = sessionLogPath(DSH_HOME, session.cwd, session.externalSessionId);
  if (!fs.existsSync(file)) return null;
  return foldSession(decodeSessionLog(file), { cwd: session.cwd });
}

async function showTask(client, reference, flags) {
  const { sessions } = await client.call('session.list', {});
  const session = pickSession(sessions, reference);

  const [state, notices] = await Promise.all([
    client.call('session.getState', { externalSessionId: session.externalSessionId }),
    client.call('session.getNotices', { externalSessionId: session.externalSessionId }),
  ]);
  const folded = foldFromDisk(session);

  const needsYou = ATTENTION.has(state.status);
  const task = {
    workspace: session.cwd,
    sessionId: session.externalSessionId,
    title: displayTaskTitle(session, folded),
    subtitle: displayTaskSubtitle(state.status, folded, needsYou),
    status: state.status,
    turn: folded?.turn ?? null,
    step: folded?.step ?? null,
    lastTool: folded?.lastTool ?? null,
    lastToolStatus: folded?.lastToolStatus ?? null,
    toolCalls: folded?.toolCalls ?? null,
    needsYou,
    pendingNotices: (notices.notices ?? []).map((n) => ({
      noticeId: n.noticeId,
      kind: n.kind,
      title: n.title,
    })),
    model: state.metadata?.modelSelection ?? null,
    permission: state.metadata?.permissionPreset?.id ?? null,
    lastEndReason: folded?.lastEndReason ?? null,
    failures: folded?.failures?.slice(-3) ?? [],
    lastAssistantText: folded?.lastAssistantText?.slice(0, 400) ?? null,
    speak: null,
  };
  task.speak = speakTask({ ...folded, running: state.status === 'running', needsYou: task.needsYou });

  if (flags.has('json')) {
    process.stdout.write(JSON.stringify(task, null, 2) + '\n');
  } else if (flags.has('speak')) {
    process.stdout.write(task.speak + '\n');
  } else {
    process.stdout.write(
      [
        `workspace   ${task.workspace}`,
        `session     ${task.sessionId}`,
        `title       ${task.title}`,
        `subtitle    ${task.subtitle}`,
        `status      ${task.status}${task.needsYou ? '  (需要你确认)' : ''}`,
        `progress    turn ${task.turn} / step ${task.step}`,
        `tools       ${task.toolCalls} calls, last=${task.lastTool ?? '-'} (${task.lastToolStatus ?? '-'})`,
        `model       ${task.model?.model ?? '-'} / ${task.model?.reasoningEffort ?? '-'}`,
        `permission  ${task.permission ?? '-'}`,
        task.pendingNotices.length
          ? `notices     ${task.pendingNotices.map((n) => n.title ?? n.kind).join(', ')}`
          : null,
        task.lastEndReason ? `ended       ${task.lastEndReason}` : null,
        `speak       ${task.speak}`,
      ]
        .filter(Boolean)
        .join('\n') + '\n',
    );
  }
  return task;
}

/** Find an idle session in the workspace, or make one. */async function resolveWorkspaceSession(client, workspace) {
  const { sessions } = await client.call('session.list', {});
  for (const session of sessions.filter((s) => s.cwd === workspace)) {
    try {
      const state = await client.call('session.getState', {
        externalSessionId: session.externalSessionId,
      });
      if (state.status === 'idle') return { session, created: false };
    } catch {
      // fall through
    }
  }
  return { created: true };
}

async function sendTask(client, workspace, text, flags, values) {
  const target = await resolveWorkspaceSession(client, workspace);
  const clientMessageId = crypto.randomUUID();

  // Record the turn counter BEFORE the message lands, otherwise a fast turn can
  // complete between `send` and a follow-up `wait`, and the waiter then blocks
  // forever on a turn number that has already passed.
  const baseline = target.created
    ? 0
    : currentTurn(target.session.cwd, target.session.externalSessionId);

  let result;
  if (target.created) {
    loadConfig();
    const createOpts = await resolveCreateParams(client, getConfig());
    result = await client.call('session.createAndStart', {
      sessionId: crypto.randomUUID(),
      content: text,
      cwd: workspace,
      clientMessageId,
      selections: createOpts.selections,
      agentPreset: createOpts.agentPreset,
    });
  } else {
    result = await client.call('session.startTurn', {
      sessionId: target.session.sessionId,
      externalSessionId: target.session.externalSessionId,
      content: text,
      clientMessageId,
    });
  }

  const sessionRef = target.created
    ? (result?.sessionId ?? result?.externalSessionId ?? null)
    : target.session.externalSessionId;

  if (flags.has('json')) {
    process.stdout.write(
      JSON.stringify({ created: target.created, workspace, text, baselineTurn: baseline, sessionRef, result }, null, 2) + '\n',
    );
    return;
  }
  process.stdout.write(
    target.created
      ? `created a new session in ${workspace} and started the turn\n`
      : `queued into ${target.session.externalSessionId} (${target.session.title})\n`,
  );

  if (flags.has('wait')) {
    const task = await waitForTurn(sessionRef ?? target.session.externalSessionId, {
      timeoutMs: Number(values.get('timeout') ?? 180_000),
      afterTurn: baseline,
    });
    process.stdout.write(task.reply + '\n');
  }
}

/** Current turn counter for a session, straight off the on-disk log. */
function currentTurn(cwd, externalSessionId) {
  const file = sessionLogPath(DSH_HOME, cwd, externalSessionId);
  if (!fs.existsSync(file)) return 0;
  return foldSession(decodeSessionLog(file), { cwd }).turn;
}

/**
 * Block until the session finishes its next turn, then return the reply.
 * Voice needs a single sentence back, so we poll the on-disk log rather than
 * streaming: the log is the only place the final assistant text lives.
 */
async function waitForTurn(reference, { timeoutMs, afterTurn }) {
  const client = await connect({ namespace: 'dsh-task-wait' });
  try {
    const { sessions } = await client.call('session.list', {});
    const session = pickSession(sessions, reference);
    const file = sessionLogPath(DSH_HOME, session.cwd, session.externalSessionId);
    const baseline = afterTurn ?? foldSession(decodeSessionLog(file), { cwd: session.cwd }).turn;

    const deadline = Date.now() + timeoutMs;
    let lastSeen = baseline;
    while (Date.now() < deadline) {
      const state = await client.call('session.getState', {
        externalSessionId: session.externalSessionId,
      });
      const folded = foldSession(decodeSessionLog(file), { cwd: session.cwd });
      lastSeen = Math.max(lastSeen, folded.turn);
      if (lastSeen > baseline && !folded.running) {
        return {
          sessionId: session.externalSessionId,
          turn: folded.turn,
          status: state.status,
          reply: folded.lastAssistantText ?? '(no text)',
          failures: folded.turnFailures,
          toolCalls: folded.turnToolCalls,
        };
      }
      await new Promise((resolve) => setTimeout(resolve, 700));
    }
    throw new Error(
      `timed out after ${timeoutMs}ms waiting for a turn after ${baseline} ` +
        `(last seen turn ${lastSeen})`,
    );
  } finally {
    client.close();
  }
}

async function main() {
  const { flags, values, positional } = parseArgs(process.argv.slice(2));
  const [command, ...rest] = positional;
  if (!command) usage();

  if (command === 'wait') {
    if (!rest[0]) usage();
    const task = await waitForTurn(rest[0], {
      timeoutMs: Number(values.get('timeout') ?? 180_000),
      afterTurn: values.has('after') ? Number(values.get('after')) : undefined,
    });    process.stdout.write(
      flags.has('json') ? JSON.stringify(task, null, 2) + '\n' : task.reply + '\n',
    );
    return;
  }

  const client = await connect({ namespace: 'dsh-task-cli' });
  try {
    switch (command) {
      case 'list':
        await listTasks(client, flags.has('json'));
        break;
      case 'show':
        if (!rest[0]) usage();
        await showTask(client, rest[0], flags);
        break;
      case 'send':
        if (!rest[0] || !rest[1]) usage();
        await sendTask(client, rest[0], rest.slice(1).join(' '), flags, values);
        break;
      case 'interrupt': {
        if (!rest[0]) usage();
        const { sessions } = await client.call('session.list', {});
        const session = pickSession(sessions, rest[0]);
        const result = await client.call('session.interrupt', {
          sessionId: session.sessionId,
          reason: 'voice-request',
        });
        process.stdout.write(JSON.stringify(result) + '\n');
        break;
      }
      case 'answer': {
        if (!rest[0] || !rest[1]) usage();
        const { sessions } = await client.call('session.list', {});
        const session = pickSession(sessions, rest[0]);
        const result = await client.call('session.respondInteraction', {
          sessionId: session.sessionId,
          noticeId: rest[1],
          actionId: rest[2] ?? 'allow',
        });
        process.stdout.write(JSON.stringify(result) + '\n');
        break;
      }
      default:
        usage();
    }
  } finally {
    client.close();
  }
}

main().catch((error) => {
  if (error instanceof RpcError) {
    process.stderr.write(`bridge error ${error.code}: ${error.message}\n`);
  } else {
    process.stderr.write(`${error.message}\n`);
  }
  process.exit(1);
});
