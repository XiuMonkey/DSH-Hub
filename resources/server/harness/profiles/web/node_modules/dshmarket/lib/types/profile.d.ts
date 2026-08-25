/**
 * Profile filesystem reads — everything the market learns from a dsh
 * profile directory (manifest, lockfile, installed package trees). Pure
 * functions of the directory contents; no processes, no network.
 */
/**
 * Resolve a profile name to its directory under DSH_HOME (default ~/.dsh).
 * An explicit directory is used by hosts, such as DSH Desktop, that own the
 * active profile location rather than deriving it from process environment.
 */
export declare function profileDir(profile: string, explicitDir?: string): string;
/** Community dependencies of the profile (in-box bundles filtered out). */
export declare function readInstalled(profile: string, explicitDir?: string): Record<string, string>;
/**
 * RAW dependency map of the profile manifest — including the in-box bundles
 * readInstalled() filters out. This is the rollback snapshot (#65): restoring
 * a filtered view would delete @deepseek-ai/dsh-base and friends.
 */
export declare function readManifestDeps(profile: string, explicitDir?: string): Record<string, string>;
/**
 * Restore the profile manifest's dependency map to a pre-operation snapshot,
 * leaving every other manifest field untouched. pnpm writes package.json
 * BEFORE it finishes installing (#65, #69: a 404/blocked-build failure lands
 * after the write), so a failed add leaves ghost dependencies that break
 * every later pnpm run — and pnpm itself can no longer remove them (the same
 * failure re-fires on any mutation). Direct manifest surgery is the only
 * reliable rollback; the lockfile is left as-is (pnpm reconciles it from the
 * manifest on the next run).
 * @returns names whose entries were dropped or reverted, empty when nothing changed.
 */
export declare function restoreManifestDeps(profile: string, snapshot: Record<string, string>, explicitDir?: string): string[];
/** The version actually present in the profile's node_modules, or null. */
export declare function readInstalledVersion(profile: string, name: string, explicitDir?: string): string | null;
/** Pinned commit per `owner/repo` from the profile lockfile's codeload tarball URLs. */
export declare function readLockCommits(profile: string, explicitDir?: string): Map<string, string>;
/** True when the installed package's manifest declares a dsh plugin surface. */
export declare function hasDshManifest(dir: string): boolean;
/**
 * True when the package's declared entry artifact actually exists — github
 * source checkouts of build-required plugins ship no lib/, and promoting one
 * into the bundle layer bricks the next boot (ERR_MODULE_NOT_FOUND kills the
 * whole profile, #18).
 */
export declare function entryArtifactExists(dir: string): boolean;
/** Plugin subdirectories (depth 2) of a collection checkout, as relative paths. */
export declare function pluginSubdirs(root: string): string[];
/**
 * Allow the given packages' build scripts in the profile's
 * pnpm-workspace.yaml `allowBuilds` block (the key dsh profiles use),
 * merging with existing entries and leaving the rest of the yaml intact.
 * (#6 by @qichuang321.)
 * @returns every package now allowed.
 */
export declare function setAllowBuilds(profile: string, packages: string[], explicitDir?: string): string[];
