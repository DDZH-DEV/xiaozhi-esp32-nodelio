/**
 * 出站连公网 Host：/ws/dsh-bridge
 * invoke 回环本机 LAN HTTP（task/status/answer），避免复制业务逻辑。
 * fetch 读本机 marketplace/（供云 Host 装包，不必进家里 LAN）。
 */
import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { WebSocket } from 'ws';

const MARKET_PREFIX = '/nodelio-dsh-market';

/**
 * @param {object} opts
 * @param {string} opts.wsUrl  wss://host/ws/dsh-bridge 或 ws://...
 * @param {string} opts.token  BRIDGE_TOKEN
 * @param {number} opts.lanPort 本机桥 HTTP 端口
 * @param {string} [opts.publicUrl] 面板可达基址（TTS URL / 回环改走 LAN）
 * @param {string} [opts.marketRoot] marketplace 目录（含 marketplace.json / packages）
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
    marketRoot = '',
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

  /* 回环 Host 会让 TTS publicBase 误判；优先用 opts.publicUrl（配置 public_url） */
  const lanBase = (opts.publicUrl && String(opts.publicUrl).replace(/\/$/, '')) ||
    `http://127.0.0.1:${lanPort}`;

  function send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  /**
   * result.body 必须是磁盘上的原样 JSON 字节（Host 用其算 package_sha256）。
   * 不可 JSON.stringify 整帧后再嵌 body，否则会变成紧凑重序列化，hash 对不上。
   */
  function sendResultRaw(requestId, statusCode, bodyBuf) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const buf = Buffer.isBuffer(bodyBuf) ? bodyBuf : Buffer.from(String(bodyBuf ?? ''), 'utf8');
    const head = Buffer.from(
      `{"type":"result","request_id":${JSON.stringify(String(requestId || ''))},"status_code":${Number(statusCode) || 200},"body":`,
      'utf8',
    );
    ws.send(Buffer.concat([head, buf, Buffer.from('}', 'utf8')]));
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

  /**
   * Host type=fetch → 读市场文件。
   * - marketplace.json：改写 package_url 为 nodelio-dsh:// 后下发（可重排）
   * - 应用包 JSON：原样字节（bodyRaw），保证 package_sha256
   */
  function localMarketFetch(reqPath) {
    const root = marketRoot && String(marketRoot).trim();
    if (!root) {
      return { status_code: 503, body: { error: 'market_root_unset' } };
    }
    let p = String(reqPath || '').trim();
    if (p !== MARKET_PREFIX && !p.startsWith(MARKET_PREFIX + '/')) {
      return { status_code: 403, body: { error: 'path_denied' } };
    }
    if (p.includes('\\') || p.includes('\0') || p.includes('..')) {
      return { status_code: 400, body: { error: 'bad_path' } };
    }
    let rel = p.slice(MARKET_PREFIX.length).replace(/^\/+/, '');
    if (!rel || rel.endsWith('/')) rel = (rel || '') + 'marketplace.json';
    const abs = path.resolve(root, rel);
    const rootAbs = path.resolve(root);
    if (!abs.startsWith(rootAbs + path.sep) && abs !== rootAbs) {
      return { status_code: 403, body: { error: 'forbidden' } };
    }
    if (!fs.existsSync(abs) || !fs.statSync(abs).isFile()) {
      return { status_code: 404, body: { error: 'not_found' } };
    }
    const raw = fs.readFileSync(abs);
    if (!abs.endsWith('.json')) {
      return {
        status_code: 200,
        body: { raw: raw.toString('base64'), content_type: 'application/octet-stream' },
      };
    }
    if (path.basename(abs) === 'marketplace.json') {
      let doc;
      try {
        doc = JSON.parse(raw.toString('utf8'));
      } catch {
        return { status_code: 502, body: { error: 'invalid_json' } };
      }
      if (doc && typeof doc === 'object') {
        for (const app of doc.apps ?? []) {
          const u = String(app.package_url ?? '');
          let rest = '';
          if (u.startsWith('nodelio-dsh://')) {
            continue;
          }
          if (u.startsWith('/nodelio-dsh-market/')) {
            rest = u.slice('/nodelio-dsh-market/'.length);
          } else if (u.startsWith('/static/nodelio-dsh-market/')) {
            rest = u.slice('/static/nodelio-dsh-market/'.length);
          } else if (u.startsWith('packages/') || u.startsWith('./packages/')) {
            rest = u.replace(/^\.\//, '');
          } else {
            try {
              const parsed = new URL(u);
              if (parsed.pathname.startsWith('/nodelio-dsh-market/')) {
                rest = parsed.pathname.slice('/nodelio-dsh-market/'.length);
              }
            } catch {
              /* keep */
            }
          }
          if (rest) {
            app.package_url = `nodelio-dsh://${rest.replace(/^\/+/, '')}`;
          }
        }
      }
      return {
        status_code: 200,
        bodyRaw: Buffer.from(JSON.stringify(doc, null, 2), 'utf8'),
      };
    }
    /* 包文件：原样字节，勿 parse→stringify */
    return { status_code: 200, bodyRaw: raw };
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
        return;
      }
      if (type === 'fetch') {
        const requestId = frame.request_id;
        const reqPath = frame.path;
        console.log(`[host-ws] fetch path=${reqPath} req=${requestId}`);
        try {
          const r = localMarketFetch(reqPath);
          if (r.bodyRaw) {
            sendResultRaw(requestId, r.status_code, r.bodyRaw);
          } else {
            send({
              type: 'result',
              request_id: requestId,
              status_code: r.status_code,
              body: r.body,
            });
          }
        } catch (e) {
          send({
            type: 'result',
            request_id: requestId,
            status_code: 500,
            body: { error: e.message || 'fetch_failed' },
          });
        }
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
