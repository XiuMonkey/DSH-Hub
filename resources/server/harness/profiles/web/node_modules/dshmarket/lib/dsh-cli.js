/**
 * Process layer: re-invoking the dsh CLI that launched this host, spawning
 * `dsh plugin` commands with timeouts and live progress, and provisioning
 * pnpm. This is the only module that starts child processes.
 *
 * Installs run through node:child_process, not ctx.shell: the shell service is
 * the agent's sandboxed executor and denies writes to the profile directory.
 */
import { spawn } from 'node:child_process';
import { homedir } from 'node:os';
import { dirname, isAbsolute, join, resolve } from 'node:path';
import { logEvent } from './log.js';
import { createProgressTracker } from './ndjson.js';
import { pluginArgsFor } from './pnpm-compat.js';
import { profileDir } from './profile.js';
// 15 min default (slow networks + git installs), overridable for CI/tests.
// (#6 by @qichuang321.)
/**
 * macOS apps launched from Finder/Dock inherit a minimal PATH without the
 * shell profile — Homebrew/npm/corepack all vanish and every install dies
 * with ENOENT/127 (#32, #38). Append the well-known bin directories so the
 * market's children find their tools regardless of how dsh was started.
 */
function spawnEnv() {
    // pnpm v10+ blocks forever on a silent interactive prompt without a TTY;
    // CI mode forces it to act or fail instead of asking.
    if (process.platform === 'win32')
        return { ...process.env, CI: 'true' };
    const parts = (process.env.PATH ?? '').split(':').filter(part => part !== '');
    for (const bin of ['/opt/homebrew/bin', '/usr/local/bin', join(homedir(), '.local', 'bin')]) {
        if (!parts.includes(bin))
            parts.push(bin);
    }
    return { ...process.env, CI: 'true', PATH: parts.join(':') };
}
const INSTALL_TIMEOUT_MS = Number(process.env.DSH_MARKET_INSTALL_TIMEOUT_MS) || 15 * 60 * 1000;
/**
 * Windows npm/corepack/pnpm are `.cmd` shims. Node's `spawn` without a shell
 * cannot start them (ENOENT / EINVAL). Same pattern as dsh's `plugin` forwarder.
 */
export const winCmdShim = process.platform === 'win32';
/** Characters cmd.exe treats as syntax even inside a token. */
const CMD_METACHARS = /[\s"&|<>^()%!]/;
/**
 * Quote one argv token for a cmd.exe `/c` command line. cmd only groups with
 * double quotes, so a token that needs quoting gets wrapped and embedded
 * quotes are doubled.
 */
export function quoteCmdArg(arg) {
    if (!CMD_METACHARS.test(arg))
        return arg;
    return `"${arg.replace(/"/g, '""')}"`;
}
/**
 * Build a cmd.exe command line from argv. Only the Windows shim path uses
 * this: cmd re-parses the joined string, so every token is quoted before
 * joining.
 */
export function cmdCommandLine(argv) {
    return argv.map(quoteCmdArg).join(' ');
}
/** cmd.exe resolved once; the Windows shim path only. */
const COMSPEC = process.env.ComSpec ?? 'cmd.exe';
/**
 * Spawn a command, avoiding Node's deprecated `shell: true` + argv
 * combination (DEP0190). Windows `.cmd` shims cannot start without a shell,
 * so the shim path routes through `cmd.exe /d /s /c` with an explicitly
 * built, quoted command line; every other invocation spawns directly with
 * `shell: false`.
 */
function spawnShim(file, args, options) {
    const { viaShell = false, ...spawnOptions } = options;
    if (!viaShell) {
        return spawn(file, [...args], { ...spawnOptions, shell: false });
    }
    if (process.platform !== 'win32') {
        return spawn(file, [...args], { ...spawnOptions, shell: false });
    }
    return spawn(COMSPEC, ['/d', '/s', '/c', `"${cmdCommandLine([file, ...args])}"`], {
        ...spawnOptions,
        shell: false,
        windowsVerbatimArguments: true,
    });
}
/**
 * Argv re-invoking the CLI that launched this host process, so installs work
 * whether dsh runs from a global bin, a local install, or repo source
 * (`node --import tsx/esm .../bin.ts`). Falls back to a PATH `dsh`.
 */
export function dshArgv() {
    const entry = process.argv[1];
    if (entry !== undefined && /[\\/](?:bin\.(?:js|ts)|dsh)$/.test(entry)) {
        // Absolute paths are required: source launches (`pnpm dsh`) pass a
        // relative entry, which the child resolves against its OWN cwd and dies
        // with MODULE_NOT_FOUND (#13). cwd near the entry keeps execArgv imports
        // (tsx/esm) resolvable on source launches.
        const abs = resolve(entry);
        return { file: process.execPath, args: [...process.execArgv, abs], cwd: dirname(abs), viaShell: false };
    }
    // Bare `dsh` is a .cmd shim on Windows that only a shell can start (#13).
    return { file: 'dsh', args: [], cwd: undefined, viaShell: winCmdShim };
}
/**
 * Kill a spawned child and, on Windows, its whole process tree — `kill()`
 * there only terminates the wrapper, leaving pnpm children running.
 * (Contributed in #7 by @mraing.)
 */
export function killChild(child) {
    if (process.platform === 'win32' && child.pid !== undefined) {
        try {
            spawn('taskkill', ['/pid', String(child.pid), '/t', '/f'], { stdio: 'ignore' });
            return;
        }
        catch { /* fall through */ }
    }
    child.kill('SIGKILL');
}
/** The child of the operation currently running, for /dsh-market/cancel. */
let activeChild = null;
let cancelRequested = false;
let activeDesktopOperation = null;
/**
 * Kill a child and its whole tree, gracefully where the platform allows:
 * taskkill /T /F on Windows (plain kill() leaves pnpm children running),
 * SIGTERM with a 5s SIGKILL escalation elsewhere so pnpm can clean up.
 * (Cancel flow contributed in #6 by @qichuang321.)
 */
function killTree(child) {
    if (process.platform === 'win32' && child.pid !== undefined) {
        try {
            spawn('taskkill', ['/pid', String(child.pid), '/t', '/f'], { stdio: 'ignore' });
            return;
        }
        catch { /* fall through */ }
    }
    // POSIX: the dsh wrapper runs pnpm as a grandchild (spawnSync), which a
    // plain child.kill() leaves running — it keeps our stdio pipes open, so
    // the close event never fires and the market looks stuck "installing".
    // The child is spawned detached as its own process GROUP; kill the group.
    const signalTree = (signal) => {
        if (child.pid === undefined)
            return;
        try {
            process.kill(-child.pid, signal);
        }
        catch {
            try {
                child.kill(signal);
            }
            catch { /* already gone */ }
        }
    };
    signalTree('SIGTERM');
    const escalate = setTimeout(() => signalTree('SIGKILL'), 5000);
    escalate.unref?.();
}
/**
 * Cancel the plugin command currently running.
 * @returns true when there was one to cancel.
 */
export function cancelActive() {
    if (activeDesktopOperation !== null) {
        activeDesktopOperation.userCancelled = true;
        progress.cancelling = true;
        activeDesktopOperation.cancel();
        return true;
    }
    if (activeChild === null)
        return false;
    cancelRequested = true;
    progress.cancelling = true;
    killTree(activeChild);
    return true;
}
/** Whether `pnpm` resolves on PATH; success is cached, absence is re-probed. */
let pnpmReady = false;
/** Probe `pnpm --version` on PATH. */
export function probePnpm() {
    if (pnpmReady)
        return Promise.resolve(true);
    return new Promise((resolvePromise) => {
        const child = spawnShim('pnpm', ['--version'], { stdio: 'ignore', viaShell: winCmdShim, env: spawnEnv() });
        child.on('error', () => resolvePromise(false));
        child.on('close', (code) => {
            pnpmReady = code === 0;
            resolvePromise(pnpmReady);
        });
    });
}
function runQuiet(file, args, timeoutMs) {
    return new Promise((resolvePromise) => {
        const child = spawnShim(file, args, {
            env: spawnEnv(),
            stdio: ['ignore', 'pipe', 'pipe'],
            viaShell: winCmdShim,
        });
        let output = '';
        const timer = setTimeout(() => killChild(child), timeoutMs);
        const collect = (chunk) => { output = (output + chunk.toString()).slice(-8 * 1024); };
        child.stdout?.on('data', collect);
        child.stderr?.on('data', collect);
        child.on('error', (error) => { clearTimeout(timer); resolvePromise({ code: 127, output: error.message }); });
        child.on('close', (code) => { clearTimeout(timer); resolvePromise({ code, output }); });
    });
}
/**
 * Provision pnpm without user involvement: corepack (ships with Node) first,
 * a global npm install as fallback.
 * @returns true when `pnpm --version` succeeds afterwards.
 */
export async function provisionPnpm() {
    const corepack = await runQuiet('corepack', ['enable', 'pnpm'], 60 * 1000);
    logEvent(corepack.code === 0 ? 'info' : 'warn', 'setup-pnpm', `corepack enable: exit=${String(corepack.code)} ${corepack.output.slice(-200)}`);
    if (await probePnpm())
        return { ok: true };
    const npm = await runQuiet('npm', ['install', '-g', 'pnpm'], 3 * 60 * 1000);
    logEvent(npm.code === 0 ? 'info' : 'error', 'setup-pnpm', `npm -g: exit=${String(npm.code)} ${npm.output.slice(-200)}`);
    if (await probePnpm())
        return { ok: true };
    // Both provisioning tools missing = Node itself is not on this process's
    // PATH (typical for Finder/Dock launches with nvm/fnm). Pointing the user
    // back at this same button would be a dead end (#32) — say what actually
    // helps.
    const pathIssue = /ENOENT/.test(corepack.output) && /ENOENT/.test(npm.output);
    return {
        ok: false,
        hint: pathIssue
            ? '这台机器的 dsh 进程找不到 Node（从图形界面启动不继承终端 PATH）。请改从终端启动 dsh，或安装 Homebrew 版 pnpm：brew install pnpm / This dsh process cannot find Node (GUI launches skip your shell PATH). Start dsh from a terminal, or install pnpm via Homebrew: brew install pnpm'
            : undefined,
    };
}
/** Singleton progress state; the status route reads it, runDshPlugin writes it. */
export const progress = {
    active: false,
    target: '',
    startedAt: 0,
    lastLine: '',
    phase: null,
    done: 0,
    total: null,
    currentPackage: null,
    downloaded: null,
    size: null,
    ndjson: false,
    error: null,
    cancelling: false,
};
/** Identifies this host process; the client scopes its pending-restart flags to it. */
export const BOOT_ID = `${String(process.pid)}-${String(Date.now())}`;
/**
 * Central allowlist for every spawn target, regardless of which route built
 * it (defense in depth on top of per-route validation — the win32 bare-dsh
 * fallback runs through a shell). Suggested in #16 by @anupamme.
 */
const TARGET_RE = /^[A-Za-z0-9@:./_#+-]+$/;
/** Mutating pnpm commands get the structured reporter appended. */
const NDJSON_COMMANDS = new Set(['add', 'remove', 'install']);
/** Apply profile-specific pnpm compatibility and the structured reporter. */
function preparePluginArgs(profileDirectory, pluginArgs) {
    let args = pluginArgsFor(profileDirectory, [...pluginArgs]);
    const target = args[args.length - 1] ?? '';
    if (!TARGET_RE.test(target)) {
        return { error: `unsafe plugin target rejected: ${JSON.stringify(target)}` };
    }
    if (NDJSON_COMMANDS.has(args[0]))
        args = [...args, '--reporter=ndjson'];
    return { args, target };
}
/** Reset the singleton status snapshot before one operation starts. */
function beginProgress(target) {
    progress.active = true;
    progress.target = target;
    progress.startedAt = Date.now();
    progress.lastLine = '';
    progress.phase = null;
    progress.done = 0;
    progress.total = null;
    progress.currentPackage = null;
    progress.downloaded = null;
    progress.size = null;
    progress.ndjson = false;
    progress.error = null;
    progress.cancelling = false;
    return createProgressTracker();
}
/**
 * Line-buffered progress feed: pnpm's ndjson reporter emits one JSON object
 * per line on stdout, and chunk boundaries can split a line. Human fallback
 * lines (older pnpm without structured events) still update `lastLine`.
 */
function makeProgressFeeder(tracker) {
    let lineBuffer = '';
    return (chunk) => {
        lineBuffer += chunk;
        let nl;
        while ((nl = lineBuffer.indexOf('\n')) !== -1) {
            const line = lineBuffer.slice(0, nl);
            lineBuffer = lineBuffer.slice(nl + 1);
            const trimmed = line.trim();
            if (trimmed === '')
                continue;
            tracker.feed(trimmed);
            // Human lines never start with '{'; JSON lines are consumed by the tracker.
            if (!trimmed.startsWith('{'))
                progress.lastLine = trimmed.slice(0, 200);
        }
    };
}
/** Run one `dsh plugin --profile <p> …` command with timeout and progress tracking. */
export function runDshPlugin(profile, pluginArgs) {
    const { file, args, cwd, viaShell } = dshArgv();
    const prepared = preparePluginArgs(profileDir(profile), pluginArgs);
    if ('error' in prepared) {
        logEvent('error', 'install', prepared.error);
        return Promise.resolve({ exitCode: 1, timedOut: false, stdout: '', stderr: prepared.error, cancelled: false });
    }
    pluginArgs = prepared.args;
    const tracker = beginProgress(prepared.target);
    const feed = makeProgressFeeder(tracker);
    return new Promise((resolvePromise) => {
        const child = spawnShim(file, [...args, 'plugin', '--profile', profile, ...pluginArgs], {
            cwd,
            // pnpm v10 blocks forever on a silent interactive prompt without a TTY
            // (observed on re-add over a pinned git spec); CI mode forces it to act
            // or fail instead of asking.
            env: spawnEnv(),
            stdio: ['ignore', 'pipe', 'pipe'],
            viaShell,
            // Own process group on POSIX so cancel/timeout can kill the whole
            // tree (dsh wrapper + pnpm grandchild) with one group signal.
            detached: process.platform !== 'win32',
        });
        activeChild = child;
        cancelRequested = false;
        let stdout = '';
        let stderr = '';
        let timedOut = false;
        const timer = setTimeout(() => {
            timedOut = true;
            killTree(child);
        }, INSTALL_TIMEOUT_MS);
        child.stdout?.on('data', (chunk) => {
            const text = chunk.toString();
            stdout = (stdout + text).slice(-256 * 1024);
            feed(text);
            syncProgress(tracker);
        });
        child.stderr?.on('data', (chunk) => {
            const text = chunk.toString();
            stderr = (stderr + text).slice(-64 * 1024);
            feed(text);
            syncProgress(tracker);
        });
        child.on('error', (error) => {
            clearTimeout(timer);
            progress.active = false;
            progress.cancelling = false;
            if (activeChild === child)
                activeChild = null;
            resolvePromise({ exitCode: 127, timedOut: false, stdout, stderr: `${stderr}\n${error.message}`, cancelled: false });
        });
        child.on('close', (code) => {
            clearTimeout(timer);
            progress.active = false;
            progress.cancelling = false;
            if (activeChild === child)
                activeChild = null;
            const failed = code !== 0 || timedOut;
            if (failed)
                progress.error = tracker.snapshot.error;
            const ignoredBuilds = tracker.snapshot.ignoredBuilds;
            resolvePromise({
                exitCode: code,
                timedOut,
                stdout,
                stderr,
                cancelled: cancelRequested,
                ...(ignoredBuilds.length > 0 ? { ignoredBuilds } : {}),
            });
        });
    });
}
/**
 * Adapt DSH Desktop's generation-scoped package manager to the existing
 * market runner. There is no runtime import or dependency on Desktop: the
 * Host supplies this public service only when the package is mounted there.
 */
export function createDesktopPluginRuntime(service, activeProfileDir, invokingDir = process.cwd(), timeoutMs = INSTALL_TIMEOUT_MS) {
    if (!isAbsolute(activeProfileDir) || activeProfileDir.includes('\0')) {
        throw new Error('dsh-market: Desktop profile directory must be an absolute path without NUL');
    }
    if (!isAbsolute(invokingDir) || invokingDir.includes('\0')) {
        throw new Error('dsh-market: Desktop invoking directory must be an absolute path without NUL');
    }
    const owner = Symbol('dsh-market desktop runtime');
    let closed = false;
    const runPlugin = async (_profile, pluginArgs) => {
        if (closed) {
            return {
                exitCode: 127,
                timedOut: false,
                stdout: '',
                stderr: 'dsh-market: Desktop package runtime is disposed',
                cancelled: false,
            };
        }
        const prepared = preparePluginArgs(activeProfileDir, pluginArgs);
        if ('error' in prepared) {
            logEvent('error', 'install', prepared.error);
            return { exitCode: 1, timedOut: false, stdout: '', stderr: prepared.error, cancelled: false };
        }
        const abort = new AbortController();
        let handle;
        try {
            handle = service.runPlugin(prepared.args, invokingDir, abort.signal);
        }
        catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            const busy = /another desktop pnpm operation is already running/i.test(message);
            return {
                exitCode: 127,
                timedOut: false,
                stdout: '',
                stderr: message,
                cancelled: false,
                ...(busy ? { busy: true } : {}),
            };
        }
        const tracker = beginProgress(prepared.target);
        const feed = makeProgressFeeder(tracker);
        let stdout = '';
        let stderr = '';
        let timedOut = false;
        const collectStdout = (chunk) => {
            const text = chunk.toString();
            stdout = (stdout + text).slice(-256 * 1024);
            feed(text);
            syncProgress(tracker);
        };
        const collectStderr = (chunk) => {
            const text = chunk.toString();
            stderr = (stderr + text).slice(-64 * 1024);
            feed(text);
            syncProgress(tracker);
        };
        handle.stdout.on('data', collectStdout);
        handle.stderr.on('data', collectStderr);
        let active;
        let timer;
        const done = (async () => {
            try {
                const outcome = await handle.done;
                const failed = outcome.exitCode !== 0 || outcome.signal !== null || timedOut;
                if (failed)
                    progress.error = tracker.snapshot.error;
                const ignoredBuilds = tracker.snapshot.ignoredBuilds;
                return {
                    exitCode: outcome.exitCode,
                    timedOut,
                    stdout,
                    stderr,
                    cancelled: active.userCancelled,
                    ...(ignoredBuilds.length > 0 ? { ignoredBuilds } : {}),
                };
            }
            catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                progress.error = tracker.snapshot.error;
                return {
                    exitCode: 127,
                    timedOut,
                    stdout,
                    stderr: `${stderr}${stderr === '' ? '' : '\n'}${message}`,
                    cancelled: active.userCancelled,
                };
            }
            finally {
                if (timer !== undefined)
                    clearTimeout(timer);
                progress.active = false;
                progress.cancelling = false;
                handle.stdout.off('data', collectStdout);
                handle.stderr.off('data', collectStderr);
                if (activeDesktopOperation === active)
                    activeDesktopOperation = null;
            }
        })();
        active = { owner, cancel: () => { handle.cancel(); }, done, userCancelled: false };
        activeDesktopOperation = active;
        timer = setTimeout(() => {
            timedOut = true;
            abort.abort(new Error('dsh-market: Desktop package operation timed out'));
            // The public handle owns an explicit process-tree cancellation path.
            // Use it as well as AbortSignal so a structurally compatible provider
            // that does not observe the signal cannot strand the route or teardown.
            handle.cancel();
        }, timeoutMs);
        timer.unref?.();
        return done;
    };
    const cancelOwned = (userCancelled) => {
        const active = activeDesktopOperation;
        if (active?.owner !== owner)
            return false;
        if (userCancelled)
            active.userCancelled = true;
        progress.cancelling = true;
        active.cancel();
        return true;
    };
    return {
        runPlugin,
        // The service is backed by Desktop's packaged pnpm; system discovery and
        // global provisioning are neither needed nor allowed in this mode.
        probePnpm: () => Promise.resolve(true),
        provisionPnpm: () => Promise.resolve({ ok: true }),
        cancelActive: () => cancelOwned(true),
        dispose: async () => {
            closed = true;
            const active = activeDesktopOperation;
            if (active?.owner !== owner)
                return;
            cancelOwned(false);
            await active.done.catch(() => { });
        },
    };
}
/** Copy the tracker's snapshot into the singleton the status route reads. */
function syncProgress(tracker) {
    const snap = tracker.snapshot;
    progress.phase = snap.phase;
    progress.done = snap.done;
    progress.total = snap.total;
    progress.currentPackage = snap.currentPackage;
    progress.downloaded = snap.downloaded;
    progress.size = snap.size;
    progress.ndjson = snap.seen;
    if (snap.error !== null)
        progress.error = snap.error;
}
