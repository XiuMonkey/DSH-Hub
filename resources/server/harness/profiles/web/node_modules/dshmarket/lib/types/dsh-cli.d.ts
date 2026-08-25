/**
 * Process layer: re-invoking the dsh CLI that launched this host, spawning
 * `dsh plugin` commands with timeouts and live progress, and provisioning
 * pnpm. This is the only module that starts child processes.
 *
 * Installs run through node:child_process, not ctx.shell: the shell service is
 * the agent's sandboxed executor and denies writes to the profile directory.
 */
import type { ChildProcess } from 'node:child_process';
import { type ProgressPhase } from './ndjson.ts';
/**
 * Windows npm/corepack/pnpm are `.cmd` shims. Node's `spawn` without a shell
 * cannot start them (ENOENT / EINVAL). Same pattern as dsh's `plugin` forwarder.
 */
export declare const winCmdShim: boolean;
/**
 * Quote one argv token for a cmd.exe `/c` command line. cmd only groups with
 * double quotes, so a token that needs quoting gets wrapped and embedded
 * quotes are doubled.
 */
export declare function quoteCmdArg(arg: string): string;
/**
 * Build a cmd.exe command line from argv. Only the Windows shim path uses
 * this: cmd re-parses the joined string, so every token is quoted before
 * joining.
 */
export declare function cmdCommandLine(argv: readonly string[]): string;
/**
 * Argv re-invoking the CLI that launched this host process, so installs work
 * whether dsh runs from a global bin, a local install, or repo source
 * (`node --import tsx/esm .../bin.ts`). Falls back to a PATH `dsh`.
 */
export declare function dshArgv(): {
    file: string;
    args: string[];
    cwd: string | undefined;
    viaShell: boolean;
};
/** Outcome of one spawned plugin command. */
export interface InstallResult {
    exitCode: number | null;
    timedOut: boolean;
    stdout: string;
    stderr: string;
    /** True when the run ended because the user cancelled it. */
    cancelled: boolean;
    /** Desktop's generation-wide package-operation gate rejected the start. */
    busy?: boolean;
    /** Package names pnpm reported as having ignored build scripts (ndjson). */
    ignoredBuilds?: string[];
}
/** The shape every orchestration function takes to run plugin commands (injectable in tests). */
export type PluginRunner = (profile: string, pluginArgs: string[]) => Promise<InstallResult>;
/** Package-operation boundary consumed by the HTTP route layer. */
export interface PluginCommandRuntime {
    runPlugin: PluginRunner;
    probePnpm(): Promise<boolean>;
    provisionPnpm(): Promise<{
        ok: boolean;
        hint?: string;
    }>;
    cancelActive(): boolean;
}
/** Structural subset of DSH Desktop's public `desktopPnpm` contract. */
export interface DesktopPnpmLike {
    runPlugin(args: readonly string[], invokingDir: string, signal?: AbortSignal): {
        readonly stdout: NodeJS.ReadableStream;
        readonly stderr: NodeJS.ReadableStream;
        readonly done: Promise<{
            readonly exitCode: number | null;
            readonly signal: NodeJS.Signals | null;
        }>;
        cancel(): void;
    };
}
/** Desktop runtime also owns cleanup of any operation started by this fiber. */
export interface DesktopPluginRuntime extends PluginCommandRuntime {
    dispose(): Promise<void>;
}
/**
 * Kill a spawned child and, on Windows, its whole process tree — `kill()`
 * there only terminates the wrapper, leaving pnpm children running.
 * (Contributed in #7 by @mraing.)
 */
export declare function killChild(child: ChildProcess): void;
/**
 * Cancel the plugin command currently running.
 * @returns true when there was one to cancel.
 */
export declare function cancelActive(): boolean;
/** Probe `pnpm --version` on PATH. */
export declare function probePnpm(): Promise<boolean>;
/**
 * Provision pnpm without user involvement: corepack (ships with Node) first,
 * a global npm install as fallback.
 * @returns true when `pnpm --version` succeeds afterwards.
 */
export declare function provisionPnpm(): Promise<{
    ok: boolean;
    hint?: string;
}>;
/** Live progress of the running plugin command, for the status route. */
export interface InstallProgress {
    active: boolean;
    target: string;
    startedAt: number;
    lastLine: string;
    /** Parsed from pnpm's ndjson stage events; null when none arrived. */
    phase: ProgressPhase;
    /** Distinct packages resolved/fetched so far. */
    done: number;
    total: number | null;
    currentPackage: string | null;
    downloaded: number | null;
    size: number | null;
    /** True when structured ndjson progress has been observed. */
    ndjson: boolean;
    /** Last fatal error from the stream (only meaningful after a failure). */
    error: string | null;
    /** True from the moment the user asks to cancel until the run ends. */
    cancelling: boolean;
}
/** Singleton progress state; the status route reads it, runDshPlugin writes it. */
export declare const progress: InstallProgress;
/** Identifies this host process; the client scopes its pending-restart flags to it. */
export declare const BOOT_ID: string;
/** Run one `dsh plugin --profile <p> …` command with timeout and progress tracking. */
export declare function runDshPlugin(profile: string, pluginArgs: string[]): Promise<InstallResult>;
/**
 * Adapt DSH Desktop's generation-scoped package manager to the existing
 * market runner. There is no runtime import or dependency on Desktop: the
 * Host supplies this public service only when the package is mounted there.
 */
export declare function createDesktopPluginRuntime(service: DesktopPnpmLike, activeProfileDir: string, invokingDir?: string, timeoutMs?: number): DesktopPluginRuntime;
