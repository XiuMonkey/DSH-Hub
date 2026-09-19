// dsh-session-scan.mjs — 扫描全部会话日志，列出每次会话"实际下发了几个工具 / 花了多少输入 token"
//
// 用法（用内嵌 node，它带 zstdDecompressSync）：
//   resources\server\node.exe tools\dsh-session-scan.mjs [outFile] [根目录...]
// 默认根目录（两份安装都扫）：
//   <exeDir>\resources\server\harness\sessions      ← DSH Hub 用的
//   %USERPROFILE%\.dsh\sessions                     ← 独立安装 DSH 用的
//
// 用来回答两类问题，不必再手工翻日志：
//   1. 工具过滤有没有真的生效？→ 看 tools 数量列（全开 28，全隐藏 1）
//   2. 这次会话花了多少？→ 看 usage：计费输入 = inputTokens + cacheReadTokens
//      （inputTokens = 未命中缓存；cacheReadTokens = 命中读回；同一个前缀的第一次必然全价）
//
// 输出按工具数升序，便于"全隐藏 → 全开"直接对照。
// 同时会打印每个会话实际下发的工具名，配置写错一眼能看出来。

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import zlib from 'node:zlib';

const MAGIC = [0x28, 0xb5, 0x2f, 0xfd];

function decodeAll(buf) {
  const starts = [];
  for (let i = 0; i + 4 <= buf.length; i++) {
    if (buf[i] === MAGIC[0] && buf[i + 1] === MAGIC[1] && buf[i + 2] === MAGIC[2] && buf[i + 3] === MAGIC[3]) starts.push(i);
  }
  let text = '';
  for (let k = 0; k < starts.length; k++) {
    const s = starts[k];
    const e = k + 1 < starts.length ? starts[k + 1] : buf.length;
    try { text += zlib.zstdDecompressSync(buf.subarray(s, e)).toString('utf8'); } catch { /* 跳过坏帧 */ }
  }
  return text;
}

function walk(dir, out = []) {
  let ents = [];
  try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch { return out; }
  for (const e of ents) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (e.name.endsWith('.jsonl.zstd')) out.push(p);
  }
  return out;
}

const args = process.argv.slice(2);
const outFile = args[0] || 'dsh-session-scan.txt';
const roots = args.length > 1 ? args.slice(1) : [
  path.join(process.cwd(), 'x64', 'Debug', 'resources', 'server', 'harness', 'sessions'),
  path.join(os.homedir(), '.dsh', 'sessions'),
];

const files = roots.flatMap((r) => walk(r));
const rows = [];
for (const f of files) {
  const evs = decodeAll(fs.readFileSync(f))
    .split('\n').filter((l) => l.trim())
    .map((l) => { try { return JSON.parse(l); } catch { return null; } })
    .filter(Boolean);
  const hdr = evs.find((e) => e.type === 'request/header');
  const tools = hdr?.data?.header?.tools;
  const usage = evs.find((e) => e.data?.usage)?.data?.usage || null;
  const billed = usage ? usage.inputTokens + usage.cacheReadTokens : null;
  rows.push({
    file: f,
    title: evs.find((e) => e.type === 'session/title')?.data?.title || '(无标题)',
    cwd: evs.find((e) => e.type === 'session')?.data?.cwd || '',
    n: Array.isArray(tools) ? tools.length : (hdr ? '缺失' : '无请求'),
    names: Array.isArray(tools) ? tools.map((t) => t.name).sort() : [],
    usage,
    billed,
  });
}

const L = [];
L.push(`扫描 ${files.length} 个会话日志`);
L.push(`根目录：`);
for (const r of roots) L.push(`  ${r}  (存在=${fs.existsSync(r)})`);
L.push('');
L.push('说明：计费输入 = inputTokens(未命中) + cacheReadTokens(命中读回)。同一前缀的第一次必然全价。');
L.push('');
for (const r of rows.sort((a, b) => (typeof a.n === 'number' ? a.n : 99) - (typeof b.n === 'number' ? b.n : 99))) {
  L.push('='.repeat(88));
  L.push(path.basename(path.dirname(r.file)));
  L.push(`  标题        ${r.title}`);
  L.push(`  cwd         ${r.cwd}`);
  L.push(`  工具数      ${r.n}`);
  if (r.billed !== null) {
    L.push(`  计费输入    ${r.billed}  (未命中 ${r.usage.inputTokens} + 缓存读 ${r.usage.cacheReadTokens})`);
    L.push(`  输出        ${r.usage.outputTokens}   推理 ${r.usage.reasoningTokens}`);
  } else {
    L.push(`  计费输入    —（没有 request/header，可能未发出过请求）`);
  }
  if (r.names.length) L.push(`  工具名单    ${r.names.join(', ')}`);
  else if (r.n === '缺失') L.push(`  工具名单    (request/header 里没有 tools 键 = 本次未下发任何工具)`);
  L.push('');
}
fs.writeFileSync(outFile, L.join('\n'), 'utf8');
console.log(`已写入 ${outFile}（${rows.length} 个会话）`);
