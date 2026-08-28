/**
 * Update detection: per-plugin comparison of what the profile has against
 * the source of truth — git HEAD for github installs, the npm latest
 * dist-tag for registry installs — with a TTL cache.
 */
export interface UpdateStatus {
    kind: 'github' | 'npm' | 'linked';
    version: string | null;
    current: string | null;
    latest: string | null;
    updateAvailable: boolean;
}
/**
 * Semver precedence: negative / 0 / positive like a comparator, or null when
 * either side isn't a plain semver version. Build metadata is ignored, a
 * release outranks any prerelease of the same core, and prerelease
 * identifiers compare numerically when both are numeric (so `rc.10` > `rc.9`).
 */
export declare function compareVersions(a: string, b: string): number | null;
/**
 * True only when the registry's `latest` is semantically HIGHER than what the
 * profile has (#64 by @ZeroOrigin64). A plain `!==` also fires when a
 * package's `latest` dist-tag is left pointing at an OLDER release than the
 * pinned install — clicking "update" then rewrote the exact pin to `@latest`
 * and downgraded the profile until it no longer booted.
 *
 * Undecidable inputs (missing or non-semver versions) report no update:
 * without a direction we cannot promise the "update" isn't a downgrade.
 */
export declare function isUpgrade(installed: string | null, latest: string | null): boolean;
/** Drop the cached listing (after a successful install/update/uninstall). */
export declare function invalidateUpdates(): void;
/**
 * Evidence check behind the "wait a day" stale diagnosis (#45): whether the
 * package's CURRENT latest release was published recently enough to sit
 * inside pnpm's default fresh-release window. pnpm's silent hold leaves no
 * trace in its output, so the publish time is the only verifiable signal.
 * @returns true/false when the npm time metadata answers, null when it
 *   can't be determined (offline, unpublished, non-npm) — callers must NOT
 *   claim the safety wait on null.
 */
export declare function latestPublishedRecently(name: string, windowMs?: number): Promise<boolean | null>;
/** The registry's current `latest` version for a package, or null when it can't be read. */
export declare function fetchNpmLatest(name: string): Promise<string | null>;
/** Per-plugin update checks; a failed check reports no update rather than failing the listing. */
export declare function checkUpdates(profile: string, force?: boolean, explicitDir?: string): Promise<Record<string, UpdateStatus>>;
