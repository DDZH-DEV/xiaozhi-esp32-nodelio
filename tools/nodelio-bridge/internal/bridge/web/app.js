'use strict';
const $ = id => document.getElementById(id);
const fragment = new URLSearchParams(location.hash.slice(1));
if (fragment.has('token')) { sessionStorage.setItem('nodelio-token', fragment.get('token')); history.replaceState(null, '', '/'); }
const token = sessionStorage.getItem('nodelio-token') || '';
let config, activeWorkspace = "", clearSecrets = new Set();
function message(text, error=false) { $('message').textContent=text; $('message').className=error?'error':''; }
async function api(path, method='GET', body) {
  const res=await fetch(path,{method,headers:{Authorization:`Bearer ${token}`,...(body?{'Content-Type':'application/json'}:{})},...(body?{body:JSON.stringify(body)}:{})});
  if (!res.ok) { const text=await res.text(); throw new Error(res.status===401?'请通过程序打开配置页，或打开本机 config-url.txt 中的地址。':text); }
  return res.json();
}
function val(id){return $(id).value.trim();}
function connectionFields(raw) {
  let doc;
  try { doc=JSON.parse(raw); } catch (_) { throw new Error('配置格式不正确，请复制 APP 的完整“连接配置”。'); }
  if(!doc || doc.type!=='nodelio-bridge-connection' || doc.version!==1) throw new Error('请使用 APP 的“复制连接配置”，单独的地址或凭据请填写到对应输入框。');
  if(typeof doc.host_ws!=='string' || typeof doc.bridge_token!=='string' || typeof doc.panel_device_id!=='string') throw new Error('连接配置缺少必要字段，请重新复制。');
  let url;
  try { url=new URL(doc.host_ws.trim()); } catch (_) { throw new Error('服务器连接地址格式不正确。'); }
  if(!['ws:','wss:'].includes(url.protocol) || !url.hostname || url.username || url.password || url.search || url.hash) throw new Error('服务器连接地址必须是 ws:// 或 wss:// 地址。');
  const credential=doc.bridge_token.trim(), panel=doc.panel_device_id.trim();
  if(!/^[a-f0-9]{64}$/i.test(credential)) throw new Error('桥凭据格式不正确，请重新从 APP 复制。');
  if(panel.length>128 || /[\x00-\x1f\x7f]/.test(panel)) throw new Error('墙板绑定信息格式不正确。');
  return {host:doc.host_ws.trim(),token:credential,panel,name:typeof doc.name==='string'?doc.name.slice(0,40):''};
}
$('import-connection').onclick=()=>{
  try {
    const fields=connectionFields(val('connection-json'));
    $('host-ws').value=fields.host;$('host-token').value=fields.token;$('panel-id').value=fields.panel;
    if(fields.name)$('computer-name').value=fields.name;clearSecrets.delete('host_token');$('connection-json').value='';$('connection-import').open=false;
    $('import-result').textContent='';message('连接信息已填入。请确认服务器地址，再点击“保存并应用”。');$('host-ws').focus();
  } catch(e) { $('import-result').textContent=e.message; }
};
async function load() {
  activeWorkspace="";
  const doc=await api('/api/config');config=doc.config; const p=config.providers;
  $('computer-name').value=config.name||'';$('host-ws').value=config.host_ws; $('panel-id').value=config.panel_device_id; $('default-provider').value=config.default_provider;
  for(const id of ['codex','dsh','openai','workbuddy']) $(id+'-enabled').checked=p[id].enabled;
  for(const [id,key] of [['dsh-workspace','workspace'],['dsh-model','model'],['dsh-permission','permission'],['dsh-endpoint','endpoint_file'],['dsh-home','dsh_home'],['dsh-agent','agent_preset']]) $(id).value=p.dsh[key]||'';
  for(const [id,key] of [['codex-executable','executable'],['codex-home','codex_home'],['codex-socket','control_socket']]) $(id).value=p.codex[key]||'';
  $('codex-mode').value=p.codex.mode||'auto';
  $('openai-model').value=p.openai.model||''; $('openai-base').value=p.openai.base_url;
  $('workbuddy-mode').value=p.workbuddy.mode; $('workbuddy-base').value=p.workbuddy.base_url;
  for(const [id,key] of [['host-token','host_token'],['openai-key','openai_key'],['workbuddy-token','workbuddy_token']]) {$(id).value='';$(id).placeholder=doc.secrets_set[key]?'已保存；留空保留':'尚未配置';}
  $('version').textContent='v'+doc.version; $('test-provider').value=config.default_provider; $('test-provider').onchange();
  if(doc.restart_required)message('监听地址已修改，重启程序后生效。');
}
async function health(){const h=await api('/api/health');$('host-state').textContent=h.host_online?'Host 已连接':'Host 未连接';for(const [id,s] of Object.entries(h.providers)){const el=$(id+'-state');el.textContent=s.message;el.className='state '+s.state;}}
async function startup(){const s=await api('/api/startup');$('startup').checked=s.enabled;$('startup').disabled=!s.supported;$('startup-note').textContent=s.message||`${s.mechanism} · ${s.enabled?'已启用，下次登录生效':'未启用'}`;}
$('settings').addEventListener('submit',async event=>{event.preventDefault();const button=event.submitter;button.disabled=true;try{
  if(val('host-ws') && !/^wss?:\/\//i.test(val('host-ws')))throw new Error('服务器连接地址应以 ws:// 或 wss:// 开头；生成的凭据请填到“桥凭据（Token）”。');
  config.name=val('computer-name');config.host_ws=val('host-ws');config.panel_device_id=val('panel-id');config.default_provider=val('default-provider');
  for(const id of ['codex','dsh','openai','workbuddy'])config.providers[id].enabled=$(id+'-enabled').checked;
  Object.assign(config.providers.dsh,{workspace:val('dsh-workspace'),model:val('dsh-model'),permission:val('dsh-permission'),endpoint_file:val('dsh-endpoint'),dsh_home:val('dsh-home'),agent_preset:val('dsh-agent')||'standard'});
  Object.assign(config.providers.codex,{executable:val('codex-executable'),codex_home:val('codex-home'),control_socket:val('codex-socket'),mode:val('codex-mode')});
  Object.assign(config.providers.openai,{model:val('openai-model'),base_url:val('openai-base')});Object.assign(config.providers.workbuddy,{mode:val('workbuddy-mode'),base_url:val('workbuddy-base')});
  await api('/api/config','PUT',{config,secrets:{host_token:val('host-token'),openai_key:val('openai-key'),workbuddy_token:val('workbuddy-token')},clear_secrets:[...clearSecrets]});clearSecrets.clear();await load();message('配置已保存。正在检查连接…');await health();message('配置已应用。');
}catch(e){message(e.message,true);}finally{button.disabled=false;}});
for(const b of document.querySelectorAll('[data-clear]')) b.onclick=()=>{clearSecrets.add(b.dataset.clear);message('已标记清除凭证，保存后生效。');};
$('refresh').onclick=async()=>{try{await health();message('连接检查完成。');}catch(e){message(e.message,true);}};
$('startup').onchange=async()=>{const enabled=$('startup').checked;$('startup').disabled=true;try{await api('/api/startup','PUT',{enabled});await startup();message(enabled?'已注册登录启动。当前程序请保持在固定目录。':'已取消下次登录启动，当前程序继续运行。');}catch(e){$('startup').checked=!enabled;message(e.message,true);}finally{$('startup').disabled=false;}};
function render(doc){
  if(doc.status==='error')throw new Error(doc.error.message);
  activeWorkspace=(doc.display?.filters||[]).find(f=>f.selected)?.workspace||'';
  const selected=val('test-session');$('tasks').replaceChildren();$('test-session').replaceChildren(new Option(val('test-provider')==='codex'?'请先选择已有会话':'新建／默认',''));
  for(const row of doc.content||[]) {if(row.type!=='list_item')continue;if(!row.id.startsWith('__'))$('test-session').add(new Option(row.title,row.id));const card=document.createElement('article');card.className='task';const title=document.createElement('h3');title.textContent=row.title;const sub=document.createElement('small');sub.textContent=row.subtitle;const body=document.createElement('pre');body.textContent=row.body||'';card.append(title,sub,body);
    for(const action of row.actions||[]){const b=document.createElement('button');b.textContent=action.title;b.onclick=async()=>{if(action.invoke.params?.arm){$('test-session').value=action.invoke.params.session||'';$('instruction').focus();message('请输入指令，确认后点击发送。');return;}b.disabled=true;try{render(await api('/api/invoke','POST',{app_id:action.invoke.app_id,params:action.invoke.params}));}catch(e){message(e.message,true);}finally{b.disabled=false;}};card.append(b);}
    $('tasks').append(card);
  }
  if([...$('test-session').options].some(o=>o.value===selected))$('test-session').value=selected;
}
$('test-provider').onchange=()=>{activeWorkspace='';$('test-session').replaceChildren(new Option(val('test-provider')==='codex'?'请先选择已有会话':'新建／默认',''));$('tasks').replaceChildren();};
$('list').onclick=async()=>{try{render(await api('/api/invoke','POST',{app_id:val('test-provider')+'.status',params:{workspace:activeWorkspace}}));}catch(e){message(e.message,true);}};
$('send').onclick=async()=>{const instruction=val('instruction');if(!instruction){message('请先输入指令。',true);return;}$('send').disabled=true;try{const session=val('test-session');if(val('test-provider')==='codex'&&!session)throw new Error('请先刷新工作台，选择一个已有会话。');const doc=await api('/api/invoke','POST',{app_id:val('test-provider')+'.task',params:{instruction,session,request_id:crypto.randomUUID()}});render(doc);if(doc.content?.[0]?.id)$('test-session').value=doc.content[0].id;$('instruction').value='';message(doc.spoken?.text||'指令已提交。稍后刷新任务查看结果；不要重复发送。');}catch(e){message(e.message,true);}finally{$('send').disabled=false;}};
$('logs').onclick=async()=>{try{$('log-output').textContent=(await api('/api/logs')).text;$('log-output').hidden=false;}catch(e){message(e.message,true);}};
$('stop').onclick=async()=>{try{await api('/api/stop','POST',{});message('连接桥已退出。重新打开程序可继续使用。');for(const b of document.querySelectorAll('button'))b.disabled=true;}catch(e){message(e.message,true);}};
(async()=>{try{await load();await Promise.all([health(),startup()]);}catch(e){message(e.message,true);}})();
