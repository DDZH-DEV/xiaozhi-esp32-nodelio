/**
 * 出站连公网 Host：/ws/dsh-bridge
 * invoke 回环本机 LAN HTTP（task/status/answer），避免复制业务逻辑。
 */
import { randomUUID } from 'node:crypto';
import { WebSocket } from 'ws';

/**
 * @param {object} opts
 * @param {string} opts.wsUrl  wss://host/ws/dsh-bridge 或 ws://...
 * @param {string} opts.token  BRIDGE_TOKEN
 * @param {number} opts.lanPort 本机桥 HTTP 端口
 * @param {string} [opts.bridgeId]
 * @param {string} [opts.panelDeviceId]
 * @param {string} [opts.version]
 * @param {() => void} [opts.onReady]
 * @param {() => void} [opts.onClose]
 */
export function startHostWs(opts) {
  const {
    wsUrl,
    token,
    lanPort,
    bridgeId = `pc-${randomUUID().slice(0, 8)}`,
    panelDeviceId = '',
    version = '1.0.0',
    onReady,
    onClose,
  } = opts;
  if (!wsUrl || !token) {
    console.warn('[host-ws] skip: need NODELIO_HOST_WS + BRIDGE_TOKEN');
    return { stop() {} };
  }

  let ws;
  let stopped = false;
  let pingTimer;
  let reconnectTimer;
  let backoffMs = 1000;

  const lanBase = `http://127.0.0.1:${lanPort}`;

  function send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  async function localInvoke(appId, params) {
    const body = params && typeof params === 'object' ? params : {};
    let path = '/nodelio/dsh/status';
    let method = 'POST';
    if (appId === 'dsh.task') {
      path = '/nodelio/dsh/task';
    } else if (appId === 'dsh.answer') {
      path = '/nodelio/dsh/answer';
    } else if (appId === 'dsh.status') {
      path = '/nodelio/dsh/status';
    } else {
      return {
        status_code: 200,
        body: {
          schema_version: 1,
          app_id: appId || 'dsh.status',
          status: 'error',
          content: [],
          display: { card_type: 'error', title: '错误' },
          error: { code: 'internal', message: `unknown app_id: ${appId}`, retryable: false },
          spoken: { text: '不支持的技能。', language: 'zh-CN' },
        },
      };
    }
    const res = await fetch(`${lanBase}${path}`, {
      method,
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(body ?? {}),
      signal: AbortSignal.timeout(28000),
    });
    const text = await res.text();
    let parsed;
    try {
      parsed = JSON.parse(text);
    } catch {
      parsed = {
        schema_version: 1,
        app_id: appId,
        status: 'error',
        content: [],
        error: { code: 'upstream_error', message: text.slice(0, 200), retryable: true },
        spoken: { text: '桥本地回包不是 JSON。', language: 'zh-CN' },
      };
    }
    return { status_code: res.status || 200, body: parsed };
  }

  function connect() {
    if (stopped) return;
    const url = new URL(wsUrl);
    url.searchParams.set('token', token);
    console.log(`[host-ws] connecting ${url.origin}${url.pathname} …`);
    ws = new WebSocket(url.toString(), {
      headers: { 'X-Bridge-Token': token },
    });

    ws.on('open', () => {
      backoffMs = 1000;
      console.log('[host-ws] connected, register');
      send({
        type: 'register',
        bridge_id: bridgeId,
        version,
        panel_device_id: panelDeviceId || undefined,
      });
      clearInterval(pingTimer);
      pingTimer = setInterval(() => send({ type: 'ping' }), 25000);
      if (typeof onReady === 'function') onReady();
    });

    ws.on('message', (data) => {
      let frame;
      try {
        frame = JSON.parse(String(data));
      } catch {
        return;
      }
      const type = String(frame.type || '').toLowerCase();
      if (type === 'registered') {
        console.log(`[host-ws] registered bridge_id=${frame.bridge_id || bridgeId}`);
        return;
      }
      if (type === 'pong') return;
      if (type === 'invoke') {
        const requestId = frame.request_id;
        const appId = frame.app_id;
        let params = frame.params;
        if (typeof params === 'string') {
          try {
            params = JSON.parse(params);
          } catch {
            params = {};
          }
        }
        if (!params || typeof params !== 'object') params = {};
        console.log(`[host-ws] invoke ${appId} req=${requestId}`);
        localInvoke(appId, params)
          .then((r) => {
            send({
              type: 'result',
              request_id: requestId,
              status_code: r.status_code,
              body: r.body,
            });
          })
          .catch((e) => {
            send({
              type: 'result',
              request_id: requestId,
              status_code: 200,
              body: {
                schema_version: 1,
                app_id: appId || 'dsh.status',
                status: 'error',
                content: [],
                error: { code: 'upstream_error', message: e.message, retryable: true },
                spoken: { text: '家里桥执行失败。', language: 'zh-CN' },
              },
            });
          });
      }
    });

    ws.on('close', () => {
      clearInterval(pingTimer);
      if (typeof onClose === 'function') onClose();
      if (stopped) return;
      console.warn(`[host-ws] closed; retry in ${backoffMs}ms`);
      reconnectTimer = setTimeout(connect, backoffMs);
      backoffMs = Math.min(backoffMs * 2, 30000);
    });

    ws.on('error', (e) => {
      console.warn(`[host-ws] error: ${e.message}`);
    });
  }

  connect();

  return {
    /** 经 WS 上报 skill_hint（替代 HTTP POST） */
    sendHint(reason, sessionId, extra = {}) {
      send({
        type: 'hint',
        app_id: extra.app_id || 'dsh.status',
        instance_id: extra.instance_id || undefined,
        reason,
        session: sessionId || undefined,
        panel_device_id: panelDeviceId || undefined,
      });
    },
    stop() {
      stopped = true;
      clearInterval(pingTimer);
      clearTimeout(reconnectTimer);
      try {
        ws?.close();
      } catch {
        /* ignore */
      }
    },
  };
}
