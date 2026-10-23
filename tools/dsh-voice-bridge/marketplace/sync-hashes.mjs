#!/usr/bin/env node
/** 按 packages 下各 1.0.0.json 刷新 marketplace.json / _hashes.json（尾部 CR/LF 不计入，与 Host 校验一致） */
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const apps = ['dsh.task', 'dsh.status', 'dsh.answer'];

function hashMaterial(buf) {
  let end = buf.length;
  while (end > 0 && (buf[end - 1] === 0x0a || buf[end - 1] === 0x0d)) end -= 1;
  return buf.subarray(0, end);
}

const hashes = {};
for (const app of apps) {
  const buf = fs.readFileSync(path.join(HERE, 'packages', app, '1.0.0.json'));
  hashes[app] = crypto.createHash('sha256').update(hashMaterial(buf)).digest('hex');
  console.log(`${app}  ${hashes[app]}`);
}
fs.writeFileSync(path.join(HERE, '_hashes.json'), `${JSON.stringify(hashes, null, 2)}\n`, 'utf8');
const mpPath = path.join(HERE, 'marketplace.json');
const mp = JSON.parse(fs.readFileSync(mpPath, 'utf8'));
for (const app of mp.apps ?? []) {
  const h = hashes[app.app_id];
  if (h) app.package_sha256 = h;
}
fs.writeFileSync(mpPath, `${JSON.stringify(mp, null, 2)}\n`, 'utf8');
console.log('updated marketplace.json + _hashes.json');
