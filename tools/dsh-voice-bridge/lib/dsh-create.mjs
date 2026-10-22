/**
 * 新建 DSH 会话：createAndStart 必须带 selections + agentPreset，
 * 否则 AA 未选手动模型时会报 Choose a model / permissions / Agent mode。
 */

/**
 * @param {object} client RPC
 * @param {object} cfg bridge-config（default_model / default_permission / default_agent_preset）
 * @returns {Promise<{ selections: { model: string, permission: string }, agentPreset: string, resolved: object }>}
 */
export async function resolveCreateParams(client, cfg = {}) {
  const [modelsDoc, permsDoc, presetsDoc] = await Promise.all([
    client.call('catalog.listModels', { limit: 10000 }),
    client.call('catalog.listPermissions', { limit: 100 }),
    client.call('catalog.listAgentPresets', {}).catch(() => null),
  ]);

  const models = Array.isArray(modelsDoc?.models) ? modelsDoc.models : [];
  const permissions = Array.isArray(permsDoc?.permissions) ? permsDoc.permissions : [];
  const presets = Array.isArray(presetsDoc?.presets) ? presetsDoc.presets : [];

  const wantModel = String(cfg.default_model || '').trim();
  const wantPerm = String(cfg.default_permission || '').trim() || 'danger-full-access';
  const wantAgent = String(cfg.default_agent_preset || '').trim() || 'standard';

  const modelEntry = pickModel(models, wantModel);
  if (!modelEntry) {
    throw new Error(
      models.length
        ? `桥配置 default_model「${wantModel || '(空)'}」未匹配到可用模型，请打开 /config 重选`
        : 'DSH 无可用模型目录，请先在 Agents Anywhere / DSH 登录模型供应商',
    );
  }

  const modelSelectionId = pickReasoningSelectionId(modelEntry, cfg.default_reasoning);
  const permEntry = pickPermission(permissions, wantPerm);
  if (!permEntry?.selectionId) {
    throw new Error(`桥配置 default_permission「${wantPerm}」无效`);
  }

  let agentPreset = wantAgent;
  if (presets.length && !presets.some((p) => p.id === agentPreset && p.enabled !== false)) {
    agentPreset = presets.find((p) => p.enabled !== false)?.id || 'standard';
  }

  return {
    selections: {
      model: modelSelectionId,
      permission: permEntry.selectionId,
    },
    agentPreset,
    resolved: {
      model_title: modelEntry.title || modelEntry.metadata?.model || modelEntry.id,
      model_id: modelEntry.id,
      permission: permEntry.metadata?.preset || permEntry.title || permEntry.id,
      agent_preset: agentPreset,
    },
  };
}

function pickModel(models, want) {
  const enabled = models.filter((m) => m && m.enabled !== false);
  const pool = enabled.length ? enabled : models;
  if (!pool.length) return null;
  if (!want) return pool[0];

  const q = want.toLowerCase();
  const bySelection = pool.find(
    (m) =>
      m.selectionId === want ||
      m.id === want ||
      (m.reasoningItems || []).some((r) => r.selectionId === want || r.id === want),
  );
  if (bySelection) return bySelection;

  const byMeta = pool.find((m) => {
    const provider = String(m.metadata?.provider || '').toLowerCase();
    const model = String(m.metadata?.model || m.metadata?.modelName || '').toLowerCase();
    const title = String(m.title || '').toLowerCase();
    const combo = `${provider}/${model}`;
    return (
      model === q ||
      title === q ||
      combo === q ||
      title.includes(q) ||
      model.includes(q) ||
      `${provider}:${model}` === q
    );
  });
  if (byMeta) return byMeta;

  return null;
}

function pickReasoningSelectionId(modelEntry, wantReasoning) {
  const items = Array.isArray(modelEntry.reasoningItems) ? modelEntry.reasoningItems : [];
  const enabled = items.filter((r) => r && r.enabled !== false);
  const pool = enabled.length ? enabled : items;
  if (!pool.length) return modelEntry.selectionId || modelEntry.id;

  const want = String(wantReasoning || '').trim().toLowerCase();
  if (want) {
    const hit =
      pool.find((r) => r.selectionId === wantReasoning || r.id === wantReasoning) ||
      pool.find((r) => String(r.id).toLowerCase() === want || String(r.title).toLowerCase() === want);
    if (hit?.selectionId) return hit.selectionId;
  }

  const preferOn =
    pool.find((r) => r.id === 'on' || r.id === 'high' || /开启|high|on/i.test(String(r.title || ''))) ||
    pool[0];
  return preferOn.selectionId || modelEntry.selectionId || modelEntry.id;
}

function pickPermission(permissions, want) {
  const enabled = permissions.filter((p) => p && p.enabled !== false);
  const pool = enabled.length ? enabled : permissions;
  if (!pool.length) return null;
  const q = String(want || '').toLowerCase();
  return (
    pool.find((p) => p.selectionId === want || p.id === want) ||
    pool.find((p) => String(p.metadata?.preset || '').toLowerCase() === q) ||
    pool.find((p) => String(p.title || '').toLowerCase() === q) ||
    pool.find((p) => String(p.metadata?.preset || '').toLowerCase().includes(q)) ||
    pool.find((p) => String(p.metadata?.preset || '') === 'danger-full-access') ||
    pool.find((p) => String(p.metadata?.preset || '') === 'workspace-write') ||
    pool[0]
  );
}

/** 配置页下拉用 */
export async function listCreateCatalog(client) {
  const [modelsDoc, permsDoc, presetsDoc] = await Promise.all([
    client.call('catalog.listModels', { limit: 10000 }),
    client.call('catalog.listPermissions', { limit: 100 }),
    client.call('catalog.listAgentPresets', {}).catch(() => ({ presets: [] })),
  ]);
  const models = (modelsDoc?.models || [])
    .filter((m) => m && m.enabled !== false)
    .map((m) => ({
      id: m.id,
      selectionId: m.selectionId,
      title: m.title,
      value: m.metadata?.model
        ? `${m.metadata.provider || ''}/${m.metadata.model}`.replace(/^\//, '')
        : m.title || m.id,
      provider: m.metadata?.provider || '',
      model: m.metadata?.model || '',
      reasoning: (m.reasoningItems || [])
        .filter((r) => r && r.enabled !== false)
        .map((r) => ({ id: r.id, title: r.title, selectionId: r.selectionId })),
    }));
  const permissions = (permsDoc?.permissions || [])
    .filter((p) => p && p.enabled !== false)
    .map((p) => ({
      id: p.id,
      selectionId: p.selectionId,
      title: p.title,
      value: p.metadata?.preset || p.title || p.id,
    }));
  const agent_presets = (presetsDoc?.presets || [])
    .filter((p) => p && p.enabled !== false)
    .map((p) => ({ id: p.id, name: p.name || p.id }));
  return { models, permissions, agent_presets };
}
