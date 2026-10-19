#!/usr/bin/env node
// validate.mjs — 用 Protocol 仓的官方 JSON Schema 校验本目录的全部样例。
//
//   node validate.mjs                       # 默认校验 ../../Protocol/schemas
//   node validate.mjs /path/to/Protocol     # 指定协议仓根目录
//
// 关键点：schema 的 $id 指向 GitHub raw URL，但互相之间用相对 $ref 引用
// （vocabulary.schema.json#/$defs/...）。所以必须先把这些文件按 $id 注册进 ajv，
// 再按 $ref 解析，否则会报 "can't resolve reference"。

import Ajv2020 from 'ajv/dist/2020.js';
import addFormats from 'ajv-formats';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

/**
 * 协议仓不在本仓库内（它是可单独克隆的对外契约），所以按以下顺序找：
 *   1. 命令行参数
 *   2. 环境变量 NODELIO_PROTOCOL_ROOT
 *   3. 常见本机位置
 */
function resolveProtocolRoot() {
  if (process.argv[2]) return path.resolve(process.argv[2]);
  if (process.env.NODELIO_PROTOCOL_ROOT) return path.resolve(process.env.NODELIO_PROTOCOL_ROOT);
  const candidates = [
    path.join(HERE, '..', '..', '..', '..', 'SITES', 'NodeLio', 'Protocol'),
    path.join(HERE, '..', '..', '..', 'Protocol'),
    path.join(HERE, 'Protocol'),
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(path.join(candidate, 'schemas'))) return path.resolve(candidate);
  }
  return null;
}

const PROTOCOL_ROOT = resolveProtocolRoot();
const SCHEMA_DIR = PROTOCOL_ROOT ? path.join(PROTOCOL_ROOT, 'schemas') : null;

if (!SCHEMA_DIR || !fs.existsSync(SCHEMA_DIR)) {
  console.error(
    [
      '找不到 NodeLio Protocol 的 schemas 目录。',
      '用法: node validate.mjs [协议仓根目录]',
      '  或: NODELIO_PROTOCOL_ROOT=D:\\SITES\\NodeLio\\Protocol node validate.mjs',
    ].join('\n'),
  );
  process.exit(2);
}

const ajv = new Ajv2020({ allErrors: true, strict: false, validateFormats: false });
addFormats(ajv);

/** 先注册全部 schema（按 $id），再编译，保证相对 $ref 能解析。 */
const schemaFiles = fs.readdirSync(SCHEMA_DIR).filter((name) => name.endsWith('.schema.json'));
const ids = new Map();
for (const name of schemaFiles) {
  const schema = JSON.parse(fs.readFileSync(path.join(SCHEMA_DIR, name), 'utf8'));
  ids.set(name, schema.$id);
  ajv.addSchema(schema, schema.$id);
}

const targets = [
  ['marketplace.json', 'marketplace.schema.json'],
  ['packages/dsh.task/1.0.0.json', 'app-package.schema.json'],
  ['packages/dsh.status/1.0.0.json', 'app-package.schema.json'],
  ['packages/dsh.answer/1.0.0.json', 'app-package.schema.json'],
  ['examples/envelope-dsh-task-list.json', 'content-envelope.schema.json'],
  ['examples/envelope-dsh-status.json', 'content-envelope.schema.json'],
  ['examples/envelope-dsh-answer.json', 'content-envelope.schema.json'],
  ['examples/envelope-dsh-error.json', 'content-envelope.schema.json'],
  ['examples/installed-app-list-panel.json', 'installed-app-list.schema.json'],
];

let failed = 0;
for (const [relative, schemaName] of targets) {
  const file = path.join(HERE, relative);
  if (!fs.existsSync(file)) {
    console.log(`SKIP  ${relative} (missing)`);
    continue;
  }
  const data = JSON.parse(fs.readFileSync(file, 'utf8'));
  const validate = ajv.getSchema(ids.get(schemaName));
  if (!validate) {
    console.log(`FAIL  ${relative} — schema ${schemaName} 未注册`);
    failed += 1;
    continue;
  }
  if (validate(data)) {
    console.log(`ok    ${relative}  [${schemaName}]`);
  } else {
    failed += 1;
    console.log(`FAIL  ${relative}  [${schemaName}]`);
    for (const error of validate.errors ?? []) {
      console.log(`        ${error.instancePath || '/'} ${error.message}` +
        (error.params?.allowedValues ? ` (allowed: ${error.params.allowedValues.join(', ')})` : ''));
    }
  }
}

console.log(failed === 0 ? '\nall schemas passed' : `\n${failed} file(s) failed`);
process.exit(failed === 0 ? 0 : 1);
