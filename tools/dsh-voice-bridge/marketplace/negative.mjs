#!/usr/bin/env node
// negative.mjs — 反证：确认校验器不是「什么都通过」。
// 故意构造三类违规包，断言 schema 必须拒。

import Ajv2020 from 'ajv/dist/2020.js';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PROTOCOL_ROOT =
  process.argv[2] ??
  process.env.NODELIO_PROTOCOL_ROOT ??
  path.resolve(HERE, '..', '..', '..', '..', 'SITES', 'NodeLio', 'Protocol');
const SCHEMA_DIR = path.join(PROTOCOL_ROOT, 'schemas');

const ajv = new Ajv2020({ allErrors: true, strict: false, validateFormats: false });
const ids = new Map();
for (const name of fs.readdirSync(SCHEMA_DIR).filter((n) => n.endsWith('.schema.json'))) {
  const schema = JSON.parse(fs.readFileSync(path.join(SCHEMA_DIR, name), 'utf8'));
  ids.set(name, schema.$id);
  ajv.addSchema(schema, schema.$id);
}

const base = JSON.parse(fs.readFileSync(path.join(HERE, 'packages', 'dsh.task', '1.0.0.json'), 'utf8'));
const clone = () => JSON.parse(JSON.stringify(base));

const cases = [
  {
    name: '能力名形状非法（大写）',
    schema: 'app-package.schema.json',
    data: { ...clone(), requires: ['content.list', 'Display.Color'] },
  },
  {
    name: 'version 不是 semver',
    schema: 'app-package.schema.json',
    data: { ...clone(), version: '1.0' },
  },
  {
    name: '缺少 required 的 invoke',
    schema: 'app-package.schema.json',
    data: (() => {
      const d = clone();
      delete d.invoke;
      return d;
    })(),
  },
  {
    name: 'invoke.timeout_ms 超上限 30000',
    schema: 'app-package.schema.json',
    data: (() => {
      const d = clone();
      d.invoke.timeout_ms = 120000;
      return d;
    })(),
  },
  {
    name: 'Envelope status=error 却缺 error 字段',
    schema: 'content-envelope.schema.json',
    data: {
      schema_version: 1,
      app_id: 'dsh.status',
      status: 'error',
      content: [],
      display: { card_type: 'error' },
    },
  },
  {
    name: 'market_id 只有两段',
    schema: 'marketplace.schema.json',
    data: {
      schema_version: 1,
      market_id: 'nodelio.dsh',
      name: 'x',
      apps: [],
    },
  },
];

let unexpected = 0;
for (const testCase of cases) {
  const validate = ajv.getSchema(ids.get(testCase.schema));
  const valid = validate(testCase.data);
  if (valid) {
    unexpected += 1;
    console.log(`UNEXPECTED PASS  ${testCase.name}`);
  } else {
    const first = validate.errors?.[0];
    console.log(`rejected         ${testCase.name}`);
    console.log(`                 -> ${first?.instancePath || '/'} ${first?.message}`);
  }
}

console.log(
  unexpected === 0
    ? '\nall negative cases rejected as expected'
    : `\n${unexpected} negative case(s) wrongly accepted`,
);
process.exit(unexpected === 0 ? 0 : 1);
