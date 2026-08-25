/**
 * Self-restart: relaunch the exact DSH invocation that booted this host so
 * pending (non-hot) plugin changes take effect without the user leaving the
 * UI. Contributed in #14 by @ysyyhhh; ported onto the layered architecture.
 *
 * Safety model: the endpoint accepts only direct same-origin loopback
 * requests (no forwarding headers), refuses while a plugin operation runs,
 * and deployments under a supervisor (systemd/launchd/pm2) can disable the
 * whole feature with `allowRestart: false` — the supervisor owns restarts.
 */
import type { IncomingMessage } from 'node:http';
/** Self-restart is enabled by default and disabled only by an explicit false. */
export declare function restartAllowed(config: {
    allowRestart?: boolean;
}): boolean;
/** Whether a process-control request came from this Web host on loopback. */
export declare function trustedRestartRequest(request: Pick<IncomingMessage, 'headers' | 'socket'>): boolean;
/**
 * Whether a download navigation may fetch a sensitive GET export.
 * Browsers do NOT send an Origin header on same-origin GET navigations
 * (`<a href="/..." download>`), so unlike process-control requests a missing
 * Origin is the NORMAL shape of a user-initiated download and must pass.
 * Keep the rest of the posture: loopback peer only, no proxy forwarding
 * headers, and — when an Origin IS present (fetch/CORS attempts) — it must
 * still match Host so a cross-origin page cannot read the export.
 */
export declare function trustedDownloadRequest(request: Pick<IncomingMessage, 'headers' | 'socket'>): boolean;
/** The exact boot invocation the detached restart helper replays. */
export declare function restartLaunch(): {
    file: string;
    args: string[];
    cwd: string;
    viaShell: boolean;
};
/**
 * Platform-correct spawn invocation for the replacement host (#40 by
 * @1123762794): on Windows a `detached` spawn maps to DETACHED_PROCESS — the
 * new host gets NO console, and every console child it later spawns (e.g.
 * DSH sandbox tool runners) pops a visible node window. Wrapping the launch
 * in `powershell -WindowStyle Hidden` gives the host a HIDDEN console that
 * children inherit instead. POSIX keeps the plain detached spawn.
 */
export declare function respawnInvocation(launch: {
    file: string;
    args: string[];
    viaShell: boolean;
}, platform?: NodeJS.Platform): {
    file: string;
    args: string[];
    viaShell: boolean;
    detached: boolean;
};
/** What scheduleRestart reports back to the caller for logging/response. */
export interface RestartResult {
    pid: number;
    helperPid: number | undefined;
    logOut: string;
    logErr: string;
}
/**
 * Relaunch this exact DSH entry after a short detached handoff, then stop
 * this process. The helper outlives us (detached + unref), waits for our
 * port to free up, and logs the replacement's output under tmpdir.
 */
export declare function scheduleRestart(): RestartResult;
