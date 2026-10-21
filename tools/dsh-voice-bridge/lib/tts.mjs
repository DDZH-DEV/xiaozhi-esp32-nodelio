// Local TTS for panel spoken: Windows SAPI → wav（面板 NotifyPlayer 整文件进 PSRAM，上限 ~512KB）。
// Fail soft: callers keep text-only spoken if this throws/returns null.

import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const TTS_DIR = path.join(ROOT, '.tts-cache');
const MAX_AGE_MS = 30 * 60 * 1000;
/** 与 NotifyPlayer kWavMax=512KB 对齐；略加长以念完「请说第几个」 */
const TTS_TEXT_MAX = 140;
const TTS_RATE = 2; /* -10..10，略快省体积 */
const TTS_SAMPLE_HZ = 16000;
const CACHE_TAG = `v3-${TTS_SAMPLE_HZ}-${TTS_RATE}`;

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
 * @param {string} text
 * @returns {Promise<string|null>} basename like `a1b2.wav` under TTS_DIR, or null
 */
export async function synthesizeSpokenWav(text) {
  const t = String(text ?? '')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, TTS_TEXT_MAX);
  if (!t) return null;

  fs.mkdirSync(TTS_DIR, { recursive: true });
  purgeOld();

  const id = crypto.createHash('sha1').update(CACHE_TAG + '\n' + t).digest('hex').slice(0, 16);
  const wavName = `${id}.wav`;
  const wavPath = path.join(TTS_DIR, wavName);
  if (fs.existsSync(wavPath) && fs.statSync(wavPath).size > 64 && fs.statSync(wavPath).size < 500 * 1024) {
    return wavName;
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
  const script = `
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
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
  if (sz > 500 * 1024) {
    try {
      fs.unlinkSync(wavPath);
    } catch {
      /* ignore */
    }
    throw new Error(`sapi wav too large ${sz}`);
  }
  return wavName;
}

/** @deprecated use synthesizeSpokenWav */
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
