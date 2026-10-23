// Local TTS for panel spoken → wav/mp3（NotifyPlayer 整文件进 PSRAM，上限 ~512KB）。
// 默认：优先微软 Edge 神经音色（Xiaoxiao）；失败回退 Windows SAPI 中文声。
// Fail soft: callers keep text-only spoken if this throws/returns null.

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync, execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const TTS_DIR = path.join(ROOT, '.tts-cache');
const MAX_AGE_MS = 30 * 60 * 1000;
/** 单段上限（码点）；按标点优先断句，可略长于硬切 */
export const TTS_TEXT_MAX = 64;
const TTS_RATE = 3; /* SAPI -10..10 */
const TTS_SAMPLE_HZ = 8000;
const TTS_WAV_MAX = 320 * 1024;
const EDGE_VOICE = process.env.DSH_TTS_VOICE || 'zh-CN-XiaoxiaoNeural';
const CACHE_TAG = `v6-${TTS_SAMPLE_HZ}-${TTS_RATE}-${TTS_TEXT_MAX}-${EDGE_VOICE}`;

/** @type {'auto'|'edge'|'sapi'} */
function ttsEngine() {
  const e = String(process.env.DSH_TTS_ENGINE || 'auto').toLowerCase();
  if (e === 'edge' || e === 'sapi') return e;
  return 'auto';
}

let edgeCmdCache = undefined; /* undefined=未测; null=无; string=命令前缀 */

function resolveEdgeCmd() {
  if (edgeCmdCache !== undefined) return edgeCmdCache;
  try {
    execSync('edge-tts --version', { stdio: 'ignore', timeout: 8000 });
    edgeCmdCache = 'edge-tts';
    return edgeCmdCache;
  } catch {
    /* fallthrough */
  }
  try {
    execSync('python -m edge_tts --version', { stdio: 'ignore', timeout: 8000 });
    edgeCmdCache = 'python -m edge_tts';
    return edgeCmdCache;
  } catch {
    /* fallthrough */
  }
  try {
    execSync('py -m edge_tts --version', { stdio: 'ignore', timeout: 8000 });
    edgeCmdCache = 'py -m edge_tts';
    return edgeCmdCache;
  } catch {
    edgeCmdCache = null;
    return null;
  }
}

function purgeOld() {
  try {
    if (!fs.existsSync(TTS_DIR)) return;
    const now = Date.now();
    for (const name of fs.readdirSync(TTS_DIR)) {
      const p = path.join(TTS_DIR, name);
      try {
        const st = fs.statSync(p);
        if (now - st.mtimeMs > MAX_AGE_MS) fs.unlinkSync(p);
      } catch {
        /* ignore */
      }
    }
  } catch {
    /* ignore */
  }
}

/**
 * 按句读/分句标点切段：优先 。！？；!? 其次 ，、,；硬上限 maxChars。
 * @param {string} text
 * @param {number} [maxChars]
 * @param {number} [maxChunks]
 * @returns {string[]}
 */
export function chunkSpokenText(text, maxChars = TTS_TEXT_MAX, maxChunks = 6) {
  const chars = [...String(text ?? '').replace(/\s+/g, ' ').trim()];
  if (chars.length === 0) return [];

  const hard = new Set(['。', '！', '？', '；', '!', '?', ';', '\n']);
  const soft = new Set(['，', '、', ',', '：', ':']);
  const out = [];
  let buf = [];

  const flush = () => {
    const s = buf.join('').trim();
    buf = [];
    if (s) out.push(s);
  };

  for (let i = 0; i < chars.length && out.length < maxChunks; i++) {
    const ch = chars[i];
    buf.push(ch);
    const len = buf.length;
    const atHard = hard.has(ch);
    const atSoft = soft.has(ch);
    const nearEnd = len >= Math.floor(maxChars * 0.55);
    const mustCut = len >= maxChars;

    if (mustCut || (atHard && len >= 8) || (atSoft && nearEnd && len >= 16)) {
      flush();
      if (out.length >= maxChunks) break;
    }
  }
  if (out.length < maxChunks && buf.length) flush();

  /* 末段过短且还有未读完：并入上一段（若并后仍不超 maxChars*1.2） */
  if (out.length >= 2) {
    const last = out[out.length - 1];
    const prev = out[out.length - 2];
    if ([...last].length < 10 && [...prev, ...last].length <= Math.floor(maxChars * 1.15)) {
      out[out.length - 2] = prev + last;
      out.pop();
    }
  }
  return out.slice(0, maxChunks);
}

function synthSapiOnce(t) {
  const id = crypto.createHash('sha1').update(CACHE_TAG + '\nsapi\n' + t).digest('hex').slice(0, 16);
  const wavName = `${id}.wav`;
  const wavPath = path.join(TTS_DIR, wavName);
  if (fs.existsSync(wavPath) && fs.statSync(wavPath).size > 64 && fs.statSync(wavPath).size <= TTS_WAV_MAX) {
    return { name: wavName, mime: 'audio/wav' };
  }
  try {
    if (fs.existsSync(wavPath)) fs.unlinkSync(wavPath);
  } catch {
    /* ignore */
  }

  const txtPath = path.join(TTS_DIR, `${id}.txt`);
  const ps1 = path.join(TTS_DIR, `${id}.ps1`);
  fs.writeFileSync(txtPath, t, 'utf8');
  const esc = (p) => p.replace(/'/g, "''");
  /* 优先选中文女声（Huihui / Yaoyao / Xiaoxiao 若已装） */
  const script = `
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
  $prefer = @('Xiaoxiao','Huihui','Yaoyao','Xiaoyi','Kangkang','Zhiwei','Chinese')
  $voices = @($s.GetInstalledVoices() | ForEach-Object { $_.VoiceInfo })
  $pick = $null
  foreach ($key in $prefer) {
    $pick = $voices | Where-Object { $_.Name -like "*$key*" -or $_.Culture.Name -like 'zh*' } | Select-Object -First 1
    if ($pick -and ($pick.Name -like "*$key*" -or $key -eq 'Chinese')) { break }
    $pick = $null
  }
  if (-not $pick) { $pick = $voices | Where-Object { $_.Culture.Name -like 'zh*' } | Select-Object -First 1 }
  if ($pick) { [void]$s.SelectVoice($pick.Name) }
  $text = [System.IO.File]::ReadAllText('${esc(txtPath)}', [System.Text.UTF8Encoding]::new($false))
  $s.Rate = ${TTS_RATE}
  $s.Volume = 100
  $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    ${TTS_SAMPLE_HZ},
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)
  $s.SetOutputToWaveFile('${esc(wavPath)}', $fmt)
  $s.Speak($text)
} finally {
  $s.Dispose()
}
`;
  fs.writeFileSync(ps1, script, 'utf8');

  const speak = spawnSync(
    'powershell.exe',
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ps1],
    { encoding: 'utf8', timeout: 45000 },
  );
  try {
    fs.unlinkSync(ps1);
    fs.unlinkSync(txtPath);
  } catch {
    /* ignore */
  }
  if (speak.status !== 0 || !fs.existsSync(wavPath) || fs.statSync(wavPath).size < 64) {
    const err = (speak.stderr || speak.stdout || 'sapi failed').toString().slice(0, 200);
    throw new Error(`sapi: ${err}`);
  }
  const sz = fs.statSync(wavPath).size;
  if (sz > TTS_WAV_MAX) {
    try {
      fs.unlinkSync(wavPath);
    } catch {
      /* ignore */
    }
    const err = new Error(`sapi wav too large ${sz}`);
    err.code = 'WAV_TOO_LARGE';
    err.size = sz;
    throw err;
  }
  return { name: wavName, mime: 'audio/wav' };
}

function synthEdgeOnce(t) {
  const cmd = resolveEdgeCmd();
  if (!cmd) {
    const err = new Error('edge-tts not installed');
    err.code = 'NO_EDGE';
    throw err;
  }
  const id = crypto.createHash('sha1').update(CACHE_TAG + '\nedge\n' + t).digest('hex').slice(0, 16);
  const mp3Name = `${id}.mp3`;
  const mp3Path = path.join(TTS_DIR, mp3Name);
  if (fs.existsSync(mp3Path) && fs.statSync(mp3Path).size > 64 && fs.statSync(mp3Path).size <= TTS_WAV_MAX) {
    return { name: mp3Name, mime: 'audio/mpeg' };
  }
  try {
    if (fs.existsSync(mp3Path)) fs.unlinkSync(mp3Path);
  } catch {
    /* ignore */
  }

  const args = cmd.split(' ').concat([
    '--voice',
    EDGE_VOICE,
    '--text',
    t,
    '--write-media',
    mp3Path,
  ]);
  const bin = args.shift();
  const r = spawnSync(bin, args, { encoding: 'utf8', timeout: 60000 });
  if (r.status !== 0 || !fs.existsSync(mp3Path) || fs.statSync(mp3Path).size < 64) {
    const errMsg = (r.stderr || r.stdout || 'edge-tts failed').toString().slice(0, 200);
    const err = new Error(`edge: ${errMsg}`);
    err.code = 'EDGE_FAIL';
    throw err;
  }
  const sz = fs.statSync(mp3Path).size;
  if (sz > TTS_WAV_MAX) {
    try {
      fs.unlinkSync(mp3Path);
    } catch {
      /* ignore */
    }
    const err = new Error(`edge mp3 too large ${sz}`);
    err.code = 'WAV_TOO_LARGE';
    throw err;
  }
  return { name: mp3Name, mime: 'audio/mpeg' };
}

/**
 * @param {string} text
 * @returns {Promise<{name:string,mime:string}|null>}
 */
export async function synthesizeSpoken(text) {
  let t = String(text ?? '')
    .replace(/\s+/g, ' ')
    .trim();
  /* 单段输入仍裁一下，防止误传超长 */
  t = [...t].slice(0, Math.floor(TTS_TEXT_MAX * 1.2)).join('');
  if (!t) return null;

  fs.mkdirSync(TTS_DIR, { recursive: true });
  purgeOld();

  const engine = ttsEngine();
  const tryEdge = engine === 'edge' || engine === 'auto';
  const trySapi = engine === 'sapi' || engine === 'auto';

  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      if (tryEdge) {
        try {
          const r = synthEdgeOnce(t);
          if (attempt === 0) console.log(`[tts] engine=edge voice=${EDGE_VOICE}`);
          return r;
        } catch (e) {
          if (engine === 'edge') throw e;
          if (e?.code !== 'NO_EDGE' && e?.code !== 'EDGE_FAIL') throw e;
          if (attempt === 0) console.warn(`[tts] edge unavailable (${e.message}) → sapi`);
        }
      }
      if (trySapi) {
        if (attempt === 0) console.log('[tts] engine=sapi');
        return synthSapiOnce(t);
      }
      return null;
    } catch (e) {
      if (e?.code !== 'WAV_TOO_LARGE' || [...t].length <= 20) throw e;
      t = [...t].slice(0, Math.max(12, Math.floor([...t].length * 0.6))).join('');
      console.warn(`[tts] retry shorter len=${[...t].length} after ${e.message}`);
    }
  }
  return null;
}

/** @deprecated 返回 basename；新代码用 synthesizeSpoken */
export async function synthesizeSpokenWav(text) {
  const r = await synthesizeSpoken(text);
  return r?.name ?? null;
}

/** @deprecated */
export async function synthesizeSpokenMp3(text) {
  return synthesizeSpokenWav(text);
}

export function ttsFilePath(name) {
  if (!name || name.includes('..') || name.includes('/') || name.includes('\\')) {
    return null;
  }
  const p = path.join(TTS_DIR, name);
  return p.startsWith(TTS_DIR) ? p : null;
}

export { TTS_DIR };
