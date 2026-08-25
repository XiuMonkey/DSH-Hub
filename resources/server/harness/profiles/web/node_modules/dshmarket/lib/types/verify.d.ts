/**
 * Post-install activation verification (P0-2): what "installed" actually
 * means for a package in a dsh profile.
 *
 * The ground truth is the same manifest the dsh CLI reconciles:
 * `<profile>/package.json` → `dsh.profile.bundles`. A package is a
 * profile-layer plugin only when its name is in that list; a package
 * without `dsh.bundle` in its own manifest is never reconciled there and
 * therefore never activates through the normal boot path (client-only
 * plugins get a market-owned shim mount instead).
 *
 * State taxonomy (IMPROVEMENT-PLAN P0-2):
 *   live    – mounted into the running composition (hot mount present)
 *   restart – installed and will activate on the next boot, but not live now
 *   inert   – installed but never a profile-layer plugin (no dsh.bundle)
 *   broken  – installed but validation failed (no dsh surface / no entry
 *             artifact) — the next boot could fail
 *   missing – not present in node_modules
 */
export type ActivationState = 'live' | 'restart' | 'inert' | 'broken' | 'missing';
export interface ActivationResult {
    state: ActivationState;
    /** Bilingual, user-facing explanations (zh / en joined with " / "). */
    reasons: string[];
    /** True when the package is in the profile's `dsh.profile.bundles`. */
    bundle: boolean;
    /** True when the package is live in the running composition. */
    hot: boolean;
}
/**
 * Verify the activation state of one installed package.
 * @param live - names live in the current composition; defaults to the
 * market's hot-mount table (injectable for tests).
 */
export declare function verifyActivation(profile: string, name: string, live?: ReadonlySet<string>, explicitDir?: string): ActivationResult;
