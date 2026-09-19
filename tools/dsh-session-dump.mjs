// dsh-session-dump.mjs — 解出 DSH 会话日志里"真正发给模型的东西"
//
// 用法（用内嵌 node，它带 zstdDecompressSync）：
//   resources\server\node.exe tools\dsh-session-dump.mjs <session.v3.jsonl.zstd> [outDir]
//
// 会话日志路径：
//   <DSH_HOME>\sessions\<cwd 编码后>\session-<id>\session.v3.jsonl.zstd
//   DSH_HOME = <exeDir>\resources\server\harness
//
// 为什么需要它：token 花在哪，只能看实际 payload。而 payload 不在别处，
// 就在会话日志的 `request/header` 事件里（tools 为空时该键整体消失）。
//
// 关键坑：日志是**多个 zstd 帧拼接**的，zstdDecompressSync 只解第一帧，
// 必须按 magic(28 B5 2F FD) 切片逐帧解。
//
// 产出：
//   stdout          事件类型直方图 + tools/system 体积 + usage/token 数字
//   <outDir>/tools.json      实际下发的 tools 数组（按体积降序打印在 stdout）
//   <outDir>/system.txt      实际下发的 system 文本
//
// 经验换算：本 profile 下 ~4.0 bytes/token（英文 schema 与中文正文都成立）。
//
// 注意：主 system 提示**不叫** `system` —— 它在 `system/message` 事件的
// `data.message.content[].text` 里。而 system/tools/message 三者各占多少 token
// 的拆解只在投影缓存里（见下），会话日志里没有。
//   <harness>\storages\session_projcache\sessions\session-<id>.json
//   → record.rows.contextBreakdown.val.breakdown = {systemTokens, toolsTokens, messageTokens}
//   → record.rows.tokenUsage.val.totals        = {uncachedInputTokens, outputTokens, cacheReadTokens, ...}

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const MAGIC = [0x28, 0xb5, 0x2f, 0xfd];

function decodeAllFrames(buf) {
  const starts = [];
  for (let i = 0; i + 4 <= buf.length; i++) {
    if (buf[i] === MAGIC[0] && buf[i + 1] === MAGIC[1] && buf[i + 2] === MAGIC[2] && buf[i + 3] === MAGIC[3]) starts.push(i);
  }
  let text = '';
  const stats = [];
  for (let k = 0; k < starts.length; k++) {
    const s = starts[k];
    const e = k + 1 < starts.length ? starts[k + 1] : buf.length;
    try {
      const d = zlib.zstdDecompressSync(buf.subarray(s, e));
      text += d.toString('utf8');
      stats.push(`${e - s}->${d.length}`);
    } catch (err) {
      stats.push(`${e - s}->ERR(${err.code || err.message})`);
    }
  }
  return { text, stats };
}

function walkFind(obj, key, pathStr = '') {
  const out = [];
  if (obj && typeof obj === 'object') {
    if (obj[key] !== undefined) out.push({ path: pathStr + '.' + key, value: obj[key] });
    for (const [k, v] of Object.entries(obj)) {
      if (v && typeof v === 'object') out.push(...walkFind(v, key, pathStr + '.' + k));
    }
  }
  return out;
}

const file = process.argv[2];
const outDir = process.argv[3] || '.';
if (!file) {
  console.error('usage: node dsh-session-dump.mjs <session.v3.jsonl.zstd> [outDir]');
  process.exit(2);
}

const buf = fs.readFileSync(file);
const { text, stats } = decodeAllFrames(buf);
console.log(`file      = ${file}`);
console.log(`bytes     = ${buf.length}  frames = ${stats.length}`);
console.log(`frames    = ${stats.join(' | ')}`);
console.log(`decoded   = ${text.length} chars`);

const events = text
  .split('\n')
  .filter((l) => l.trim())
  .map((l) => { try { return JSON.parse(l); } catch { return null; } })
  .filter(Boolean);

console.log(`\n=== events (${events.length}) ===`);
const types = {};
for (const e of events) types[e.type || '<none>'] = (types[e.type || '<none>'] || 0) + 1;
for (const [k, v] of Object.entries(types).sort((a, b) => b[1] - a[1])) console.log(`${v}\t${k}`);

fs.mkdirSync(outDir, { recursive: true });

let sawTools = false;
let sawSystem = false;
for (const [i, e] of events.entries()) {
  for (const hit of walkFind(e, 'tools')) {
    if (!Array.isArray(hit.value) || !hit.value.length) continue;
    sawTools = true;
    const rows = hit.value.map((t) => {
      const name = t.name || t.function?.name || '<noname>';
      const desc = t.description || t.function?.description || '';
      return { name, bytes: JSON.stringify(t).length, descChars: desc.length };
    });
    console.log(`\n=== TOOLS @ event#${i} (${e.type}) ${hit.path}: ${rows.length} 个 ===`);
    console.log('name\tbytes\tdescChars\t~tokens(bytes/4)');
    let total = 0;
    for (const r of rows.sort((a, b) => b.bytes - a.bytes)) {
      total += r.bytes;
      console.log(`${r.name}\t${r.bytes}\t${r.descChars}\t${Math.round(r.bytes / 4)}`);
    }
    console.log(`TOTAL_TOOLS_BYTES\t${total}\t\t~${Math.round(total / 4)} tokens`);
    fs.writeFileSync(path.join(outDir, 'tools.json'), JSON.stringify(hit.value, null, 2));
  }
  for (const hit of walkFind(e, 'system')) {
    if (typeof hit.value !== 'string') continue;
    sawSystem = true;
    console.log(`\n=== SYSTEM @ event#${i} (${e.type}) ${hit.path}: ${hit.value.length} chars ≈ ${Math.round(hit.value.length / 4)} tokens (前 2000 字符) ===`);
    console.log(hit.value.slice(0, 2000));
    fs.writeFileSync(path.join(outDir, 'system.title.txt'), hit.value);
  }
  // 主 system 提示：system/message 事件的 data.message.content[].text
  if (e.type === 'system/message') {
    const parts = (e.data?.message?.content || []).filter((c) => typeof c?.text === 'string');
    if (parts.length) {
      sawSystem = true;
      const body = parts.map((c) => c.text).join('\n\n');
      console.log(`\n=== SYSTEM PROMPT @ event#${i} (system/message): ${body.length} chars ≈ ${Math.round(body.length / 4)} tokens (前 2000 字符) ===`);
      console.log(body.slice(0, 2000));
      fs.writeFileSync(path.join(outDir, 'system.txt'), body);
    }
  }
}

console.log('\n=== usage ===');
// 模型真实计费来自 assistant/message 的 data.usage：
//   inputTokens     = 本次新增(未命中缓存)的输入 —— API 的 prompt_tokens - cache_hit
//   cacheReadTokens = 命中缓存的输入（同一前缀重复出现时才有）
//   totalTokens     = inputTokens + cacheReadTokens + outputTokens
// 投影缓存里的 tokenUsage.val.totals 是它的跨步累计版（uncachedInputTokens 同义于 inputTokens）。
let sawUsage = false;
for (const [i, e] of events.entries()) {
  for (const hit of walkFind(e, 'usage')) {
    if (!hit.value || typeof hit.value !== 'object' || !('inputTokens' in hit.value)) continue;
    sawUsage = true;
    console.log(`event#${i} (${e.type}) ${hit.path}: ${JSON.stringify(hit.value)}`);
  }
  for (const hit of walkFind(e, 'totals')) {
    console.log(`event#${i} (${e.type}) ${hit.path}: ${JSON.stringify(hit.value)}`);
  }
  for (const hit of walkFind(e, 'breakdown')) {
    const v = hit.value;
    sawUsage = true;
    console.log(`event#${i} (${e.type}) ${hit.path}: ${JSON.stringify(v)}`);
    if (v && typeof v === 'object') {
      const tot = Object.values(v).reduce((a, b) => a + (typeof b === 'number' ? b : 0), 0);
      for (const [k, n] of Object.entries(v)) {
        if (typeof n === 'number') console.log(`    ${k.padEnd(16)} ${String(n).padStart(6)}  ${tot ? ((n / tot) * 100).toFixed(1) + '%' : ''}`);
      }
    }
  }
  for (const hit of walkFind(e, 'pressureTokens')) console.log(`event#${i} (${e.type}) ${hit.path}: ${hit.value}`);
}
if (!sawTools) console.log('\n(本次会话没有下发给模型的 tools —— 可能不是模型步骤，或 tools 为空时该键被省略)');
if (!sawSystem) console.log('(没有 system 字段)');
if (!sawUsage) console.log('(没有 usage 记录)');
