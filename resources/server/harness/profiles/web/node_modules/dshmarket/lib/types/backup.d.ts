/**
 * Portable profile backups: configuration only, never installed packages.
 *
 * The profile directory is plain user data — aside from package.json it can
 * hold API keys (config.toml), tokens, or the WebDAV password when stored
 * server-side. Backups therefore behave like `dsh export` and carry the same
 * credential-warning disclaimer in the UI (review #63).
 */
export declare const BACKUP_FORMAT = "dsh-profile-backup";
export declare const MAX_BACKUP_BYTES: number;
/** File names that routinely contain credentials (backup exports).
 *  Values are never masked in place — the export is one-to-one for
 *  faithful restores — but presence is surfaced by the UI warning. */
export declare const SECRET_FILE_HINTS: RegExp;
/** Count of exported files whose names look like they carry credentials. */
export declare function secretFileCount(profile: string, explicitDir?: string): number;
export type BackupFile = {
    path: 'package.json';
    json: Record<string, unknown>;
} | {
    path: string;
    lines: string[];
};
export interface ProfileBackup {
    format: typeof BACKUP_FORMAT;
    version: 0.2;
    createdAt: string;
    profile: string;
    files: BackupFile[];
}
/** Serialize every profile file except dependencies, lock state, and market cache. */
export declare function createProfileBackup(profile: string, explicitDir?: string): ProfileBackup;
/** Atomically overwrite backed-up files and return a rollback for install failure. */
export declare function restoreProfileBackup(profile: string, value: unknown, explicitDir?: string): {
    files: number;
    rollback(): void;
};
interface PublicAddress {
    address: string;
    family: 4 | 6;
}
export declare function uploadWebdav(url: string, username: string, password: string, backup: ProfileBackup): Promise<void>;
/** Refuse non-global IPv4 targets, including metadata and carrier NAT ranges. */
export declare function isPublicIpv4(ip: string): boolean;
/** Only public internet target hostnames are reachable for WebDAV. */
export declare function isPublicHostname(hostname: string): boolean;
/**
 * Whether a WebDAV hostname may be fetched: public https targets only.
 * Exported for tests.
 */
export declare function isPublicTarget(hostname: string): boolean;
/** Only global-unicast IPv6 is usable for a server-side WebDAV connection. */
export declare function isPublicIpv6(ip: string): boolean;
/** Resolve once, reject every unsafe answer, and return the address to pin. */
export declare function resolvePublicAddress(hostname: string): Promise<PublicAddress>;
export declare function downloadWebdav(url: string, username: string, password: string): Promise<unknown>;
export {};
