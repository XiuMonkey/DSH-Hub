import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { opendir, readFile, stat, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { homedir } from "node:os";

/**
 * ToolsFilterPlugin — a per-session view of which tools enter the model prompt.
 *
 * The filter runs on the `system-prompt/assemble` waterfall, so it changes ONLY
 * what a request announces. `ctx.tools` still resolves every hidden name at
 * dispatch time, which is what makes "hidden from the prompt, still callable"
 * work: hiding is a prompt-side projection, not a capability grant.
 *
 * A hidden tool withholds two things:
 *
 *   1. its schema in the request's `tools` field;
 *   2. its own guidance section — every tool plugin contributes one under the
 *      stable name `tool:<toolName>` — unless the config sets
 *      `DropGuidance: false`.
 *
 * Because (2) is withheld too, the model is not told the tool exists; it can
 * still call it when the conversation says so. Set `DropGuidance: false` to keep
 * the guidance line (the model keeps knowing the tool exists) at its token cost.
 *
 * Hiding never removes a tool from a registry view: `ctx.tools.restrict()` is
 * the seam for "must not run at all".
 *
 * Config lives beside the session artifact as `ToolsFilterConfig.json`, so the
 * config file is the single source of truth for both the filter below and the
 * DSH Hub UI that writes it. The HTTP endpoint reads and writes that same file
 * because DSH exposes no `tools.*` RPC — without a scope, `ctx.tools.schemas()`
 * answers from the global layer, which in a preset-composed deployment (every
 * web profile) is empty.
 */

const CONFIG_FILE_NAME = "ToolsFilterConfig.json";
const DISCOVERY_TOOL_NAME = "gettools";

// Single source for gettools' own description: the tool schema and the catalog
// context (see discoveryCatalogText) must not drift apart.
const GETTOOLS_DESCRIPTION = 'Load tools that are missing from your current tool list. Call with no arguments, or directory:"all", to load every registered tool. Pass a directory name (for example "File" or a plugin name) to load only that group. Hidden tools are restored on your next turn, so call gettools again when you need one of them.';

// Name of the context unit that advertises gettools and the session's directory
// groups (see discoveryCatalogText / the assemble handler).
const DISCOVERY_CATALOG_CONTEXT_NAME = "toolsfilter:catalog";

export const name = "ToolsFilterPlugin";
export const inject = ["tools", "systemPrompt"];

// ── session directory resolution ────────────────────────────────────────────

function resolveSessionsRoot() {
  const home = process.env.DSH_HOME;
  if (home) return join(home, "sessions");
  return join(homedir(), ".dsh", "sessions");
}

const DSH_SESSIONS_ROOT = resolveSessionsRoot();

/** Percent-free encoding of one path segment; `/`, `.` and `..` cannot escape. */
function encodeSegment(raw) {
  if (raw.length === 0) throw new Error("cannot encode an empty path segment");
  if (raw === ".") return "~002E";
  if (raw === "..") return "~002E~002E";
  let out = "";
  for (let i = 0; i < raw.length; i++) {
    const code = raw.charCodeAt(i);
    const ch = String.fromCharCode(code);
    if (ch !== "~" && /^[A-Za-z0-9._-]$/.test(ch)) out += ch;
    else out += "~" + code.toString(16).toUpperCase().padStart(4, "0");
  }
  return out;
}

/**
 * Cached session directory per id. A miss is retried after
 * {@link MISSING_DIR_RETRY_MS}: a session that has never been persisted has no
 * directory yet, and it must start filtering once the store materializes it.
 */
const sessionDirCache = new Map();
const MISSING_DIR_RETRY_MS = 5000;

async function findSessionDir(sessionId) {
  const cached = sessionDirCache.get(sessionId);
  if (cached !== undefined && (cached.dir !== null || Date.now() - cached.at < MISSING_DIR_RETRY_MS)) {
    return cached.dir;
  }

  const encodedId = encodeSegment(sessionId);
  const prefixedId = "session-" + encodedId;
  let found = null;
  try {
    const rootHandle = await opendir(DSH_SESSIONS_ROOT);
    for await (const projectEntry of rootHandle) {
      if (!projectEntry.isDirectory()) continue;
      for (const candidate of [encodedId, prefixedId]) {
        const sessionDirPath = join(DSH_SESSIONS_ROOT, projectEntry.name, candidate);
        try {
          const sessionHandle = await opendir(sessionDirPath);
          await sessionHandle.close();
          found = sessionDirPath;
          break;
        } catch { /* not this one */ }
      }
      if (found !== null) break;
    }
  } catch { /* sessions root missing */ }

  sessionDirCache.set(sessionId, { dir: found, at: Date.now() });
  return found;
}

// ── the hub-authored config ─────────────────────────────────────────────────

/** Markers a UI may write for a false flag; anything else reads as true. */
const FALSEY = new Set(["FALSE", "0", "NO", "OFF"]);

function flagIsFalse(value) {
  return FALSEY.has(String(value).toUpperCase());
}

function isPlainObject(value) {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/**
 * Compile one `FilterList` into a rule map. The two folder states are the
 * contract with the UI:
 *
 *   - `IsExpanded: false` → the whole folder is hidden, whatever its rows say;
 *   - otherwise → each row decides, and a row hides only on an explicit false
 *     marker. A row with no `IsVisible` stays visible, because silently hiding
 *     on a malformed row is the dangerous direction of this switch.
 */
function compileFilterList(filterList) {
  const rules = new Map();
  if (!Array.isArray(filterList)) return rules;

  for (const entry of filterList) {
    const dir = entry?.Directory;
    if (!isPlainObject(dir) || !Array.isArray(dir.ToolsList)) continue;
    const expanded = !flagIsFalse(dir.IsExpanded);

    for (const row of dir.ToolsList) {
      const toolName = typeof row === "string" ? row : row?.ToolName;
      if (typeof toolName !== "string" || toolName.length === 0) continue;
      if (!expanded) { rules.set(toolName, { hidden: true }); continue; }
      if (typeof row === "string") { rules.set(toolName, { hidden: false }); continue; }
      rules.set(toolName, { hidden: flagIsFalse(row.IsVisible) });
    }
  }
  return rules;
}

/** Compiled config keyed by session, invalidated by the file's mtime and size. */
const configCache = new Map();

function compileDocument(parsed) {
  return {
    rules: compileFilterList(parsed?.FilterList),
    dropGuidance: parsed?.DropGuidance === undefined ? true : !flagIsFalse(parsed.DropGuidance),
    // Which runtime-context units this session always withholds, by name
    // (e.g. "sandbox:policy"). Empty by default: contexts carry rules and
    // environment facts, not capability announcements, so nothing is dropped
    // unless the operator names it.
    hideContexts: new Set(Array.isArray(parsed?.HideContexts)
      ? parsed.HideContexts.filter((name) => typeof name === "string" && name.length > 0)
      : []),
    // The raw document is kept so the GET face can hand the UI the state the
    // compiled rules throw away: which groups are collapsed (IsExpanded) and
    // each row's own IsVisible, which is what a per-tool checkbox shows.
    document: isPlainObject(parsed) ? parsed : {},
  };
}

/**
 * Read and compile the session's config. The directory lookup is cached; the
 * file is re-stat'ed on every call, so an edit made by the UI is picked up on
 * the very next request instead of never.
 * @returns the compiled config, or `null` when the session has no config file.
 */
async function readConfig(sessionId) {
  const sessionDir = await findSessionDir(sessionId);
  if (sessionDir === null) return null;

  const configPath = join(sessionDir, CONFIG_FILE_NAME);
  let stamp;
  try {
    const stats = await stat(configPath);
    stamp = `${stats.mtimeMs}:${stats.size}`;
  } catch {
    configCache.delete(sessionId);
    return null;
  }

  const cached = configCache.get(sessionId);
  if (cached !== undefined && cached.stamp === stamp) return cached.config;

  try {
    const parsed = JSON.parse(await readFile(configPath, "utf8"));
    const config = compileDocument(parsed);
    configCache.set(sessionId, { stamp, config });
    return config;
  } catch {
    configCache.delete(sessionId);
    return null;
  }
}

/** Persist a document to the session's config path; the store owns the directory. */
async function writeConfig(sessionId, document) {
  const sessionDir = await findSessionDir(sessionId);
  if (sessionDir === null) {
    return { ok: false, reason: "session-directory-missing" };
  }
  const configPath = join(sessionDir, CONFIG_FILE_NAME);
  const body = JSON.stringify(document, null, 2) + "\n";
  try {
    await writeFile(configPath, body, "utf8");
  } catch (error) {
    return { ok: false, reason: "write-failed", message: String(error?.message ?? error) };
  }
  configCache.delete(sessionId);
  return { ok: true, configPath };
}

// ── registry view: which tools can this session's agent be given? ───────────

/**
 * The preset id a session actually runs under. The creation header is frozen, so
 * a session switched while blank would otherwise be read under the composition
 * it was created with.
 */
function resolveSessionPreset(record) {
  const events = record.events;
  if (Array.isArray(events)) {
    for (let index = events.length - 1; index >= 0; index -= 1) {
      const event = events[index];
      if (event?.type === "agent-preset/selected") return event.data?.agentPreset;
    }
  }
  return record.header?.agentPreset;
}

/**
 * Resolve the registry scope this session's tools resolve in: the live agent
 * itself when there is one, else its preset's STANDING key (composing the
 * plugins but starting no agent, session, or turn), else the global layer —
 * which a preset-composed deployment leaves empty, so callers must treat that
 * answer as degraded rather than as "no tools".
 */
async function resolveToolScope(ctx, sessionId) {
  const live = ctx.get("agents")?.get?.(sessionId);
  if (live !== undefined) return { scope: live, source: "agent", preset: undefined };

  let record;
  const attached = ctx.get("sessions")?.get?.(sessionId);
  if (attached !== undefined) {
    record = { header: attached.header, events: attached.events };
  } else {
    const persistence = ctx.get("sessionPersistence");
    if (persistence !== undefined) {
      try {
        const inspected = await persistence.inspect(sessionId);
        record = { header: inspected.meta, events: inspected.events };
      } catch { /* unknown or unreadable session */ }
    }
  }
  if (record !== undefined) {
    const preset = resolveSessionPreset(record);
    const presets = ctx.get("agentPresets");
    if (typeof preset === "string" && presets !== undefined) {
      try {
        return { scope: await presets.standingKeyFor(preset), source: "preset", preset };
      } catch { /* roster absent, or the preset is gone/broken */ }
    }
  }
  return { scope: undefined, source: "global", preset: undefined };
}

/** The tool name a `tool:<name>` guidance section belongs to, else undefined. */
function sectionToolName(sectionName) {
  if (typeof sectionName !== "string" || !sectionName.startsWith("tool:")) return undefined;
  const toolName = sectionName.slice("tool:".length);
  return toolName.length > 0 ? toolName : undefined;
}

/**
 * Every candidate tool name the given text mentions as a STANDALONE token.
 *
 * `\b` alone is not enough, and that was a real bug: a word boundary also
 * matches inside a hyphenated compound, so the sandbox unit's
 * "Current DSH file policy: workspace-write" read as a mention of the `write`
 * TOOL. Hiding `write` then silently withheld the session's file policy — a
 * permission fact, the exact thing this filter must never trade for tokens.
 *
 * A name only counts when neither neighbour is a word character or a joiner
 * (`-`, `/`, `.`), so `workspace-write`, `read-only` and `danger-full-access`
 * name no tool, while `job_list` still does not match inside `job_list_all`
 * (underscore is a word character) and `goal` still does not match inside
 * `create_goal`. Stricter than `\b` means FEWER mentions, so the rules below
 * drop less and keep more — the conservative direction.
 */
function mentionedToolNames(text, candidates) {
  const found = [];
  if (typeof text !== "string" || text.length === 0) return found;
  for (const name of candidates) {
    const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    if (new RegExp(`(?<![\\w./-])${escaped}(?![\\w./-])`).test(text)) found.push(name);
  }
  return found;
}

/**
 * Runtime-context units that carry PERMISSION and environment facts, not
 * capability announcements. The automatic "its tools are all hidden" rule never
 * withholds these: they are the model's ground truth about what it may write
 * and whether a call will block, and tokens saved there are paid for with wrong
 * actions. An explicit `HideContexts` entry still removes them — that is an
 * operator decision, not an inference.
 */
const PROTECTED_CONTEXT_NAMES = new Set([
  "sandbox:policy",
  "approval:policy",
  // Our own catalog unit names gettools and lists directory groups whose names
  // can coincide with hidden tool names ("Workflow" vs the workflow tool), so
  // the automatic mention rule must never treat it as stale prose. It is only
  // injected while gettools itself is visible; an explicit HideContexts entry
  // still removes it (checked separately before the injection).
  DISCOVERY_CATALOG_CONTEXT_NAME,
]);

/**
 * Split one catalog against the config's tool rules. A rule naming a tool that
 * this scope does not have changes nothing — which is why both faces also
 * report `unknownTools` instead of failing silently.
 */
function partitionTools(config, tools) {
  const hiddenNames = [];
  const kept = [];
  for (const tool of tools) {
    if (config.rules.get(tool.name)?.hidden === true) hiddenNames.push(tool.name);
    else kept.push(tool);
  }
  return { kept, hiddenNames };
}

/**
 * The `tool:*` guidance sections a hidden set withholds.
 *
 * Two rules, because a plugin's section name is NOT always its tool name:
 *
 *   1. `tool:<toolName>` — the convention most tool plugins follow (fs/search/
 *      shell/web/subagent/workflow/ralph). An exact match on a hidden tool drops
 *      the section.
 *   2. Plugin-level names — `dsh-tool-goal` registers `tool:goal` for
 *      create_goal/get_goal/update_goal and `dsh-tool-jobs` registers `tool:jobs`
 *      for job_list/job_output/job_kill. Their section name matches no tool, so
 *      rule 1 can never drop them; without rule 2 the model keeps reading a
 *      catalog of tools the request no longer announces (it then answers about
 *      tools it cannot call, or invents the ones whose sections were dropped).
 *      Here the section body decides: drop it only when it mentions hidden
 *      tools and mentions no tool that is still visible.
 *
 * Sections outside `tool:*` (plan policy, file-reference context) are left
 * alone: their tool mentions are incidental, and dropping them would remove
 * mode rules that have nothing to do with the filter.
 */
function sectionsToDrop(sections, hiddenNames, visibleNames, dropGuidance) {
  const dropped = new Set();
  if (!dropGuidance) return dropped;
  const hidden = new Set(hiddenNames);
  const candidates = [...hiddenNames, ...visibleNames];

  for (const section of sections) {
    const toolName = sectionToolName(section.name);
    if (toolName === undefined) continue;

    if (hidden.has(toolName)) {
      dropped.add(section.name);
      continue;
    }

    const mentioned = mentionedToolNames(section.text, candidates);
    if (mentioned.length > 0 && mentioned.every((name) => hidden.has(name)))
      dropped.add(section.name);
  }
  return dropped;
}

/**
 * The runtime-context units this session withholds.
 *
 * Contexts are NOT capability announcements — they carry rules and environment
 * facts (file policy, approval policy, per-surface orientation). So nothing is
 * dropped by name unless the config names it (`HideContexts`), and the only
 * automatic rule mirrors the sections rule 2: a unit whose body names tools,
 * all of them hidden, is describing tools the request no longer announces, so
 * it goes. A unit that names no tool, or names at least one still-visible tool,
 * stays — dropping `sandbox:policy`/`approval:policy` would take away the
 * model's ground truth about what it may write and whether a call will block.
 */
function contextsToDrop(contexts, hiddenNames, visibleNames, hideContexts) {
  const dropped = new Set();
  const hidden = new Set(hiddenNames);
  const candidates = [...hiddenNames, ...visibleNames];

  for (const context of contexts) {
    if (hideContexts.has(context.name)) {
      dropped.add(context.name);
      continue;
    }
    // Defence in depth behind the matcher fix above: the auto rule may not
    // withhold a permission/environment fact even if some future wording makes
    // it look like a tool mention. Only an explicit HideContexts entry can.
    if (PROTECTED_CONTEXT_NAMES.has(context.name)) continue;
    const mentioned = mentionedToolNames(context.text, candidates);
    if (mentioned.length > 0 && mentioned.every((name) => hidden.has(name)))
      dropped.add(context.name);
  }
  return dropped;
}

/**
 * The catalog context text: what gettools is for, and which directory groups
 * exist in this session, each with the Description it carries in the config
 * json — so the model can aim gettools' "directory" argument at a real group
 * instead of guessing names. Names go through directoryDisplayName, the same
 * normalization gettools itself applies when matching the argument.
 */
function discoveryCatalogText(document) {
  const lines = [
    `gettools: ${GETTOOLS_DESCRIPTION}`,
    'Directory groups you can pass to gettools (its "directory" argument; "all" loads every registered tool):',
  ];

  const filterList = Array.isArray(document?.FilterList) ? document.FilterList : [];
  const seen = new Set();
  for (const entry of filterList) {
    const directory = isPlainObject(entry) ? entry.Directory : undefined;
    if (!isPlainObject(directory)) continue;
    const name = directoryDisplayName(directory.DirectoryName);
    const key = name.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    const description = typeof directory.Description === "string" ? directory.Description.trim() : "";
    lines.push(`- ${name}${description.length > 0 ? `: ${description}` : ""}`);
  }
  if (seen.size === 0) lines.push("- (none recorded)");
  return lines.join("\n");
}

// ── HTTP endpoint (the read/write face for DSH Hub) ─────────────────────────

function sendJson(res, status, data, origin) {
  const headers = { "content-type": "application/json; charset=utf-8" };
  // Only a same-origin browser client needs this; the native client needs none.
  if (typeof origin === "string" && origin.length > 0) headers["access-control-allow-origin"] = origin;
  res.writeHead(status, headers);
  res.end(JSON.stringify(data));
}

/**
 * Reachability policy, not authentication: this endpoint changes what the model
 * is shown, so it is pinned to the loopback authority the way every other /api
 * entry is. A plain Origin/Host comparison is not enough — under DNS rebinding
 * both carry the attacker's hostname — so the authority itself must be loopback.
 */
function hostIsLoopback(req) {
  const host = req.headers.host;
  if (typeof host !== "string" || host.length === 0) return false;
  let hostname;
  try {
    hostname = new URL(`http://${host}`).hostname.toLowerCase();
  } catch {
    return false;
  }
  const bare = hostname.startsWith("[") && hostname.endsWith("]") ? hostname.slice(1, -1) : hostname;
  return bare === "localhost" || bare === "::1" || bare === "0:0:0:0:0:0:0:1" || /^127\./.test(bare);
}

function requestOrigin(req) {
  const origin = req.headers.origin;
  return typeof origin === "string" ? origin : "";
}

/** @returns true when the request was already answered with a rejection. */
function rejectUntrusted(req, res) {
  if (!hostIsLoopback(req)) {
    res.writeHead(403);
    res.end("forbidden");
    return true;
  }
  if (req.headers["sec-fetch-site"] === "cross-site") {
    res.writeHead(403);
    res.end("forbidden");
    return true;
  }
  const origin = requestOrigin(req);
  if (origin.length > 0) {
    try {
      if (new URL(origin).host !== req.headers.host) {
        res.writeHead(403);
        res.end("forbidden");
        return true;
      }
    } catch {
      res.writeHead(403);
      res.end("forbidden");
      return true;
    }
  }
  return false;
}

async function parseBody(req) {
  const chunks = [];
  let size = 0;
  for await (const chunk of req) {
    size += chunk.length;
    if (size > 1_048_576) throw new Error("request body exceeds 1 MiB");
    chunks.push(chunk);
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8"));
}

function getQuerySession(req) {
  const url = new URL(req.url, "http://localhost");
  return url.searchParams.get("session");
}

function publicSchema(schema) {
  return { name: schema.name, description: schema.description, parameters: schema.parameters };
}

// ── automatic grouping for the initial config ───────────────────────────────
//
// The first `GET /api/tools-filter` for a session writes an initial
// `ToolsFilterConfig.json` (the C++ side does the write). That document should
// not start as one flat Default folder: tools from an installed plugin belong
// under that plugin, while the bundled tools belong under functional folders
// such as File / Search / Shell / Web. The endpoint returns a `directory` field
// per tool for exactly that write. Existing configs are still authoritative;
// the UI and the filter read them as before.

const OTHER_DIRECTORY_NAME = "Other";
const FILTER_DIRECTORY_NAME = "ToolsFilterPlugin";

/**
 * Bundled (original) tools grouped by function. Keep package identity out of
 * this table: a new tool from an existing package gets a row here, and an
 * unknown name falls through to Other rather than silently becoming a plugin.
 */
const BUILTIN_TOOL_GROUPS = [
  { directory: "File", tools: ["read", "write", "edit", "read_image", "str_replace_editor"] },
  { directory: "Search", tools: ["glob", "grep"] },
  { directory: "Shell", tools: ["bash", "pwsh"] },
  { directory: "Web", tools: ["web_search", "web_fetch"] },
  {
    directory: "Task",
    tools: [
      "create_goal", "get_goal", "update_goal",
      "job_list", "job_output", "job_kill",
      "todo_write", "schedule_create", "schedule_list", "schedule_delete",
    ],
  },
  {
    directory: "Agent",
    tools: ["subagent", "subagent_fork", "list_subagent_models", "list_agents", "send_message", "interrupt_agent"],
  },
  { directory: "Plan", tools: ["exit_plan_mode"] },
  { directory: "Workflow", tools: ["workflow", "ralph"] },
  { directory: "Skill", tools: ["skill"] },
  { directory: "Interaction", tools: ["ask_user_question"] },
  { directory: "Output", tools: ["present"] },
  {
    directory: "Cordis",
    tools: [
      "cordis_inspect_list", "cordis_inspect_query", "cordis_inspect_self",
      "cordis_define", "cordis_run", "cordis_stop", "cordis_undefine",
    ],
  },
  { directory: "Code", tools: ["run_code"] },
];

const BUILTIN_TOOL_DIRECTORIES = new Map();
for (const group of BUILTIN_TOOL_GROUPS) {
  for (const toolName of group.tools) BUILTIN_TOOL_DIRECTORIES.set(toolName, group.directory);
}
// gettools is contributed by this plugin, so it belongs under the plugin's own
// directory rather than under a built-in functional group.
BUILTIN_TOOL_DIRECTORIES.set(DISCOVERY_TOOL_NAME, FILTER_DIRECTORY_NAME);

const automaticDirectoryCache = new Map();
const moduleInfoCache = new Map();
const pluginRequire = createRequire(import.meta.url);

function readJson(path) {
  try {
    return JSON.parse(readFileSync(path, "utf8"));
  } catch {
    return undefined;
  }
}

function addToolNameCandidate(names, value) {
  if (typeof value !== "string" || value.length === 0) return;
  if (!/^[A-Za-z0-9_.:/-]+$/.test(value)) return;
  names.add(value);
}

function collectToolNamesFromJson(value, names) {
  if (Array.isArray(value)) {
    for (const item of value) collectToolNamesFromJson(item, names);
    return;
  }
  if (!isPlainObject(value)) return;
  for (const [key, child] of Object.entries(value)) {
    if ((key === "name" || key === "ToolName" || key === "toolName") && typeof child === "string")
      addToolNameCandidate(names, child);
    collectToolNamesFromJson(child, names);
  }
}

function collectJsonToolNames(root, names) {
  const stack = [[root, 0]];
  let fileCount = 0;
  while (stack.length > 0 && fileCount < 160) {
    const [dir, depth] = stack.pop();
    let entries;
    try {
      entries = readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const entry of entries) {
      if (fileCount >= 160) break;
      const entryName = entry.name;
      if (entryName === "node_modules" || entryName === ".git" || entryName.startsWith(".")) continue;
      const fullPath = join(dir, entryName);
      if (entry.isDirectory()) {
        if (depth < 3) stack.push([fullPath, depth + 1]);
        continue;
      }
      if (!entry.isFile() || entryName === "package.json" || entryName === "package-lock.json"
          || !entryName.endsWith(".json")) continue;
      try {
        if (statSync(fullPath).size > 512 * 1024) continue;
      } catch {
        continue;
      }
      ++fileCount;
      const parsed = readJson(fullPath);
      if (parsed !== undefined) collectToolNamesFromJson(parsed, names);
    }
  }
}

function collectJsToolNames(filePath, names) {
  let code;
  try {
    code = readFileSync(filePath, "utf8");
  } catch {
    return;
  }
  const pattern = /name\s*:\s*["'`]([A-Za-z0-9_.:/-]+)["'`]/g;
  let match;
  while ((match = pattern.exec(code)) !== null) addToolNameCandidate(names, match[1]);
}

function findPackageRoot(filePath) {
  let current = dirname(filePath);
  while (true) {
    if (existsSync(join(current, "package.json"))) return current;
    const parent = dirname(current);
    if (parent === current) return undefined;
    current = parent;
  }
}

function loadModuleInfo(moduleName) {
  if (moduleInfoCache.has(moduleName)) return moduleInfoCache.get(moduleName);

  let info = null;
  try {
    const entryFile = pluginRequire.resolve(moduleName);
    const packageRoot = findPackageRoot(entryFile);
    if (packageRoot !== undefined) {
      const packageJson = readJson(join(packageRoot, "package.json")) ?? {};
      const pluginName = typeof packageJson.name === "string" && packageJson.name.length > 0
        ? packageJson.name
        : moduleName;
      const names = new Set();
      const entryCandidates = [entryFile, join(packageRoot, "index.js"), join(packageRoot, "lib", "index.js")];
      for (const candidate of entryCandidates) {
        if (existsSync(candidate)) collectJsToolNames(candidate, names);
      }
      collectJsonToolNames(packageRoot, names);
      info = { pluginName, names };
    }
  } catch {
    info = null;
  }

  moduleInfoCache.set(moduleName, info);
  return info;
}

function loaderEntriesOf(ctx) {
  let loader;
  try {
    loader = typeof ctx?.get === "function" ? ctx.get("loader") : undefined;
  } catch {
    loader = undefined;
  }
  loader ??= ctx?.loader;
  if (loader !== undefined && typeof loader.entries === "function") {
    try {
      return Array.from(loader.entries());
    } catch {
      return [];
    }
  }
  return [];
}

function liveDirectoryForTool(toolName) {
  if (BUILTIN_TOOL_DIRECTORIES.has(toolName)) return BUILTIN_TOOL_DIRECTORIES.get(toolName);
  if (toolName.startsWith("mcp_")) return "MCP";
  return undefined;
}

/**
 * Directory for every tool in `catalog`. Bundled tools use the function table;
 * any remaining tool is matched against tool names declared by installed plugin
 * packages. A package match wins only for a name the table does not already own.
 */
function automaticDirectoryByTool(ctx, catalog) {
  const cacheKey = catalog.map((tool) => tool.name).join("\u0000");
  if (automaticDirectoryCache.has(cacheKey)) return automaticDirectoryCache.get(cacheKey);

  const byTool = new Map();
  for (const tool of catalog) {
    byTool.set(tool.name, liveDirectoryForTool(tool.name) ?? OTHER_DIRECTORY_NAME);
  }

  const unresolved = new Set();
  for (const [toolName, directory] of byTool) {
    if (directory === OTHER_DIRECTORY_NAME) unresolved.add(toolName);
  }

  if (unresolved.size > 0) {
    for (const entry of loaderEntriesOf(ctx)) {
      if (!isPlainObject(entry) || entry.disabled) continue;
      const options = isPlainObject(entry.options) ? entry.options : {};
      if (options.group) continue;
      const moduleName = typeof options.name === "string" ? options.name : entry.moduleName;
      if (typeof moduleName !== "string" || moduleName.length === 0) continue;
      if (moduleName.startsWith("node:") || moduleName.startsWith(".") || moduleName.startsWith("/")) continue;
      // Built-in packages are represented by the table above. Scanning their
      // large libraries on every request would buy false positives, not names.
      if (moduleName.startsWith("@deepseek-ai/")) continue;

      const info = loadModuleInfo(moduleName);
      if (info === null) continue;
      for (const toolName of info.names) {
        if (!unresolved.delete(toolName)) continue;
        byTool.set(toolName, info.pluginName);
        if (unresolved.size === 0) break;
      }
      if (unresolved.size === 0) break;
    }
  }

  automaticDirectoryCache.set(cacheKey, byTool);
  return byTool;
}

function directoryDisplayName(raw) {
  const name = typeof raw === "string" ? raw.trim() : "";
  if (name.length === 0) return "Default";
  if (name.toLowerCase() === "tools") return "Default";
  return name;
}

function configuredDirectoryAssignments(document) {
  const byTool = new Map();
  const names = [];
  const seen = new Set();
  const filterList = Array.isArray(document?.FilterList) ? document.FilterList : [];

  for (const entry of filterList) {
    const directory = isPlainObject(entry) ? entry.Directory : undefined;
    if (!isPlainObject(directory)) continue;
    const name = directoryDisplayName(directory.DirectoryName);
    const key = name.toLowerCase();
    if (!seen.has(key)) {
      seen.add(key);
      names.push(name);
    }
    if (!Array.isArray(directory.ToolsList)) continue;
    for (const row of directory.ToolsList) {
      const toolName = typeof row === "string"
        ? row
        : (isPlainObject(row) ? row.ToolName : undefined);
      if (typeof toolName === "string" && toolName.length > 0 && !byTool.has(toolName))
        byTool.set(toolName, name);
    }
  }

  return { byTool, names };
}

function directoryAssignments(ctx, catalog, document) {
  const filterList = Array.isArray(document?.FilterList) ? document.FilterList : [];
  if (filterList.length > 0) return configuredDirectoryAssignments(document);

  const byTool = automaticDirectoryByTool(ctx, catalog);
  const names = [];
  const seen = new Set();
  for (const tool of catalog) {
    const name = byTool.get(tool.name) ?? OTHER_DIRECTORY_NAME;
    const key = name.toLowerCase();
    if (!seen.has(key)) {
      seen.add(key);
      names.push(name);
    }
  }
  return { byTool, names };
}

/**
 * The endpoint's own dependency is the web server, so the filter stays mounted
 * (and keeps working) in deployments that compose none.
 */
function registerEndpoint(ctx) {
  ctx.inject(["webServer"], (webCtx) => {
    webCtx.effect(() => webCtx.webServer.register({
      kind: "exact",
      path: "/api/tools-filter",
      handler: async (req, res) => {
        const origin = requestOrigin(req);
        if (rejectUntrusted(req, res)) return;

        if (req.method === "OPTIONS") {
          res.writeHead(204, {
            "access-control-allow-origin": origin.length > 0 ? origin : "*",
            "access-control-allow-methods": "GET, POST, OPTIONS",
            "access-control-allow-headers": "content-type",
          });
          res.end();
          return;
        }

        if (req.method === "GET") {
          const sessionId = getQuerySession(req);
          if (!sessionId) { sendJson(res, 400, { error: "missing session query param" }, origin); return; }
          try {
            const { scope, source, preset } = await resolveToolScope(ctx, sessionId);
            const catalog = ctx.tools.schemas(scope);
            const directoryByTool = automaticDirectoryByTool(ctx, catalog);
            const stored = await readConfig(sessionId);
            const config = stored ?? compileDocument({});
            const { kept, hiddenNames } = partitionTools(config, catalog);

            const catalogNames = new Set(catalog.map((tool) => tool.name));
            const unknownTools = [...config.rules.keys()].filter((toolName) => !catalogNames.has(toolName));

            sendJson(res, 200, {
              sessionId,
              scope: source,
              preset: preset ?? null,
              degraded: source === "global",
              configFound: stored !== null,
              dropGuidance: config.dropGuidance,
              tools: catalog.map((tool) => ({
                ...publicSchema(tool),
                // Consumed by the C++ side only when this session has no stored
                // FilterList yet; persisted configs keep their own directories.
                directory: directoryByTool.get(tool.name) ?? OTHER_DIRECTORY_NAME,
              })),
              hiddenTools: hiddenNames,
              hiddenSections: config.dropGuidance
                ? hiddenNames.map((toolName) => `tool:${toolName}`)
                : [],
              unknownTools,
              visibleCount: kept.length,
              // Runtime-context units this session withholds by name (config's
              // `HideContexts`). Contexts that name only hidden tools are dropped
              // per assembly and are not listed here — the client cannot know the
              // unit list without assembling a prompt.
              hideContexts: [...config.hideContexts],
              // The stored document as written by the UI (FilterList + DropGuidance),
              // so a client can render collapsed groups and per-tool state exactly as
              // they were saved instead of re-deriving them from the effective set.
              storedConfig: config.document,
            }, origin);
          } catch (error) {
            sendJson(res, 500, { error: String(error?.message ?? error) }, origin);
          }
          return;
        }

        if (req.method === "POST") {
          const mediaType = String(req.headers["content-type"] ?? "").split(";")[0].trim().toLowerCase();
          if (mediaType !== "application/json") {
            sendJson(res, 415, { error: "content-type must be application/json" }, origin);
            return;
          }
          let body;
          try {
            body = await parseBody(req);
          } catch {
            sendJson(res, 400, { error: "invalid JSON body" }, origin);
            return;
          }
          const sessionId = body?.sessionId;
          const document = body?.config;
          if (typeof sessionId !== "string" || sessionId.length === 0 || !Array.isArray(document?.FilterList)) {
            sendJson(res, 400, { error: "missing sessionId or config.FilterList" }, origin);
            return;
          }

          const written = await writeConfig(sessionId, document);
          if (!written.ok) {
            const message = written.reason === "session-directory-missing"
              ? "session directory not found: the session must exist in the store before its filter can be saved"
              : `cannot write config: ${written.message ?? written.reason}`;
            sendJson(res, written.reason === "session-directory-missing" ? 409 : 500, { error: message, persisted: false }, origin);
            return;
          }

          const config = compileDocument(document);
          let unknownTools = [];
          try {
            const { scope, source } = await resolveToolScope(ctx, sessionId);
            if (source !== "global") {
              const catalogNames = new Set(ctx.tools.schemas(scope).map((tool) => tool.name));
              unknownTools = [...config.rules.keys()].filter((toolName) => !catalogNames.has(toolName));
            }
          } catch { /* the write already landed; the report is best-effort */ }

          sendJson(res, 200, {
            ok: true,
            persisted: true,
            configPath: written.configPath,
            ruleCount: config.rules.size,
            hiddenCount: [...config.rules.values()].filter((rule) => rule.hidden).length,
            unknownTools,
          }, origin);
          return;
        }

        res.writeHead(405, { allow: "GET, POST, OPTIONS" });
        res.end();
      },
    }), "tools-filter: /api/tools-filter");
  });
}

// ── gettools: on-demand disclosure of the hidden catalog ────────────────────

/**
 * The disclosure door. Like every other tool it obeys the session config:
 * hiding it removes it from the announcement surface like anything else (DSH
 * Hub 2026-09-18 — the old "never withheld" special case was removed on
 * purpose, because an operator hiding gettools means exactly "the model does
 * not get to ask for more"). While it IS visible it answers the question every
 * hidden tool raises — "does this session still have a shell?" — without asking
 * the model to believe an unannounced tool is callable: the model asks for the
 * catalog, and the catalog is then ANNOUNCED for the rest of the turn.
 *
 * Disclosure is per turn, not per request: `dsh-agent-loop`'s `preStep` calls
 * `systemPrompt.assemble()` before every step, so a disclosure made in step 1 is
 * already in the `tools` array of step 2. The model can disclose AND call inside
 * one turn, with no second user message; the filter resumes on its next turn.
 */

/** sessionId -> { turn, directory, names: Set<string>, all: boolean }. */
const revealedSelections = new Map();

/**
 * The session's open turn number, or undefined when it has none (a bare
 * assemble, a diagnostics call). An undefined turn never discloses anything.
 */
function currentTurnOf(ctx, agent) {
  const projections = ctx.get("sessionProjections");
  if (agent?.session === undefined || typeof projections?.stateOf !== "function") return undefined;
  try {
    const boundary = projections.stateOf(agent.session, "turnBoundary");
    if (boundary === undefined || boundary.openTurnStartSeq === null) return undefined;
    return boundary.lastTurn;
  } catch {
    return undefined;
  }
}

/** The active gettools disclosure for this turn, else undefined. */
function activeDisclosure(ctx, agent, sessionId) {
  const turn = currentTurnOf(ctx, agent);
  if (turn === undefined) return undefined;
  const selection = revealedSelections.get(sessionId);
  if (selection === undefined) return undefined;
  if (selection.turn !== turn) {
    revealedSelections.delete(sessionId);
    return undefined;
  }
  return selection;
}

/**
 * The receipt the model reads. Names and one line each: the full signatures are
 * NOT duplicated here — they arrive through the `tools` array, which is the
 * channel that makes them callable, and repeating ~28k chars of JSON Schema in
 * a durable message would pay for them twice on every later request.
 */
function renderDisclosure(value) {
  const directory = typeof value.directory === "string" && value.directory.length > 0
    ? value.directory
    : "all";
  const subject = directory.toLowerCase() === "all"
    ? `${value.tools.length} tools`
    : `${value.tools.length} tools from directory "${directory}"`;
  return [
    `Loaded ${subject} for turn ${value.turn}. They are in your tool list now and can be called directly;`,
    "the hidden set is restored on your next turn, so call gettools again when you need one of them.",
    "",
    ...value.tools.map((tool) => `- ${tool.name}: ${tool.description}`),
  ].join("\n");
}

/** The definition, kept separate so that registration stays a one-liner. */
function discoveryToolDefinition(ctx) {
  return {
    name: DISCOVERY_TOOL_NAME,
    description: GETTOOLS_DESCRIPTION,
    parameters: {
      type: "object",
      properties: {
        directory: {
          type: "string",
          description: "Directory group to load, or \"all\" for every registered tool. Defaults to \"all\".",
        },
      },
      additionalProperties: false,
    },
    output: {
      schema: {
        type: "object",
        additionalProperties: false,
        properties: {
          turn: { type: "integer" },
          directory: { type: "string" },
          tools: {
            type: "array",
            items: {
              type: "object",
              additionalProperties: false,
              properties: {
                name: { type: "string" },
                description: { type: "string" },
              },
              required: ["name", "description"],
            },
          },
        },
        required: ["turn", "directory", "tools"],
      },
      render: (_args, value) => [{ type: "text", text: renderDisclosure(value) }],
    },
    async execute(args, exec) {
      const agent = exec.agent;
      const sessionId = agent?.session?.header?.id;
      if (typeof sessionId !== "string" || sessionId.length === 0) {
        throw new Error("gettools requires a session");
      }
      const turn = currentTurnOf(ctx, agent);
      if (turn === undefined) throw new Error("gettools requires an open turn");

      const catalog = ctx.tools.schemas(agent);
      const requested = typeof args?.directory === "string" ? args.directory.trim() : "all";
      if (requested.length === 0) {
        throw new Error('gettools directory must be "all" or a non-empty directory name');
      }

      const allRequested = requested.toLowerCase() === "all";
      let directoryName = "all";
      let selectedNames;

      if (allRequested) {
        selectedNames = new Set(catalog.map((tool) => tool.name));
      } else {
        const config = await readConfig(sessionId);
        const assignments = directoryAssignments(ctx, catalog, config?.document ?? {});
        const match = assignments.names.find((name) => name === requested)
          ?? assignments.names.find((name) => name.toLowerCase() === requested.toLowerCase());
        if (match === undefined) {
          const available = assignments.names.length > 0 ? assignments.names.join(", ") : "(none)";
          throw new Error(`unknown tools directory "${requested}"; available directories: ${available}`);
        }

        selectedNames = new Set();
        for (const tool of catalog) {
          if (assignments.byTool.get(tool.name) === match) selectedNames.add(tool.name);
        }
        if (selectedNames.size === 0)
          throw new Error(`tools directory "${match}" has no registered tools`);
        directoryName = match;
      }

      revealedSelections.set(sessionId, {
        turn,
        directory: directoryName,
        names: selectedNames,
        all: allRequested,
      });

      return {
        turn,
        directory: directoryName,
        tools: catalog
          .filter((tool) => selectedNames.has(tool.name))
          .map((tool) => ({
            name: tool.name,
            description: typeof tool.description === "string" ? tool.description : "",
          })),
      };
    },
  };
}

function registerDiscoveryTool(ctx) {
  // Disclosure is an ADDITION to the filter, so a failed registration (a name
  // clash, a deployment without the registry) must not take the filter down
  // with it: hiding keeps working, the session just keeps no ask-for-more door.
  try {
    ctx.effect(() => ctx.tools.register(discoveryToolDefinition(ctx)), "tools-filter: gettools");
  } catch (error) {
    console.warn(`[tools-filter] gettools was not registered: ${String(error?.message ?? error)}`);
  }
}

// ── the filter ──────────────────────────────────────────────────────────────

/**
 * EXPERIMENT switch, not a preference — it trades one invariant for tokens.
 *
 * When gettools has disclosed the catalog, should the PROMPT text come back as
 * well, or only the schema?
 *
 *   true  — restore the whole announcement surface: `tools` PLUS the `tool:*`
 *           guidance sections and the runtime-context units. The filter's own
 *           invariant holds: every tool the model can call has a manual.
 *   false — restore ONLY the `tools` parameter. The model can call the tool but
 *           is never told its conventions, so it will read `[exit code: 1]` as a
 *           command failure instead of an interruption and retry commands it
 *           should not retry. This deliberately breaks that invariant, and is
 *           here to measure what the guidance sections are actually worth.
 */
const DISCLOSURE_RESTORES_PROMPT_TEXT = false;

export function apply(ctx) {
  registerEndpoint(ctx);
  registerDiscoveryTool(ctx);

  ctx.on("system-prompt/assemble", async (assembly, context, next) => {
    const assembled = await next();

    const agent = context?.agent ?? context?.scope;
    const sessionId = agent?.session?.header?.id;
    if (typeof sessionId !== "string" || sessionId.length === 0) return assembled;

    const config = await readConfig(sessionId);
    if (config === null || config.rules.size === 0) return assembled;

    const disclosure = activeDisclosure(ctx, agent, sessionId);
    if (disclosure?.all && DISCLOSURE_RESTORES_PROMPT_TEXT) return assembled;

    const { kept, hiddenNames } = partitionTools(config, assembled.tools);
    if (disclosure === undefined && hiddenNames.length === 0) return assembled;

    // `all` restores every registered tool. A directory disclosure restores the
    // configured visible set plus only that directory's tools. Otherwise the
    // request gets exactly the configured visible set — gettools has no special
    // status anymore, hiding it is the operator's "no asking for more" switch.
    // (If the visible set ends up empty the provider adapter omits the whole
    // `tools` parameter, so nothing can be called: that outcome is now a
    // deliberate configuration, not a bug to guard against.)
    let announcedTools = kept;
    if (disclosure !== undefined) {
      const visibleNames = new Set(kept.map((tool) => tool.name));
      announcedTools = assembled.tools.filter((tool) =>
        disclosure.all || visibleNames.has(tool.name) || disclosure.names.has(tool.name));
    }

    // The prompt-side rules keep judging against what the CONFIG withholds:
    // under the experiment a disclosed tool is callable but still undescribed,
    // so its guidance section and context mentions stay dropped.
    const withheldNames = hiddenNames;
    const visibleNames = announcedTools.map((tool) => tool.name);

    const dropped = sectionsToDrop(assembled.sections, withheldNames, visibleNames, config.dropGuidance);
    const droppedContexts = contextsToDrop(assembled.contexts ?? [], withheldNames, visibleNames,
      config.hideContexts);
    let contexts = droppedContexts.size === 0
      ? assembled.contexts
      : (assembled.contexts ?? []).filter((context) => !droppedContexts.has(context.name));

    // Advertise the discovery door while it is open: gettools plus the session's
    // directory groups with their config-json descriptions. Injected only while
    // gettools itself is announced — a hidden gettools is the operator's
    // "no asking for more" switch, and advertising it in a context would defeat
    // that. An explicit HideContexts entry for our unit wins over the injection.
    const announcedNames = new Set(announcedTools.map((tool) => tool.name));
    if (announcedNames.has(DISCOVERY_TOOL_NAME)
      && !config.hideContexts.has(DISCOVERY_CATALOG_CONTEXT_NAME)) {
      contexts = [
        ...(contexts ?? []),
        {
          name: DISCOVERY_CATALOG_CONTEXT_NAME,
          text: discoveryCatalogText(config.document),
        },
      ];
    }

    return {
      ...assembled,
      tools: announcedTools,
      sections: dropped.size === 0
        ? assembled.sections
        : assembled.sections.filter((section) => !dropped.has(section.name)),
      contexts,
    };
  });
}
