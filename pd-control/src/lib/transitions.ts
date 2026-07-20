/**
 * Transition catalogs mirrored from firmware `pd-transition.c`.
 * Animation (Phase 1) and sprite-block (Phase 2) lists stay separate in the UI.
 */

export const ANIMATION_TRANSITION_CATALOG = [
  "none",
  "wipe-left",
  "wipe-right",
  "wipe-up",
  "wipe-down",
  "wipe-diag-tl",
  "wipe-diag-tr",
  "wipe-diag-bl",
  "wipe-diag-br",
  "slide-left",
  "slide-right",
  "slide-up",
  "slide-down",
  "roll-up",
  "roll-down",
  "split-h",
  "split-v",
  "split-diag",
  "fade",
  "block-build",
  "pixel-build",
  "zoom-in",
  "zoom-out",
  "flip-h",
  "flip-v",
] as const;

export const SPRITE_TRANSITION_CATALOG = [
  "none",
  "sprite-bump-left",
] as const;

export const TRANSITION_CATALOG = [
  ...ANIMATION_TRANSITION_CATALOG,
  ...SPRITE_TRANSITION_CATALOG.filter(
    (id) =>
      !(ANIMATION_TRANSITION_CATALOG as readonly string[]).includes(id)
  ),
] as const;

export type TransitionId = (typeof TRANSITION_CATALOG)[number];
export type ContentPlayMode = "animation" | "sprite";

export type ContentPlayPref = {
  mode?: ContentPlayMode;
  transition: TransitionId;
};

const PLAY_PREFS_PREFIX = "pd.contentPlayPrefs.v1:";

export function transitionLabel(id: string): string {
  if (id === "none") return "No transition";
  return id
    .split("-")
    .map((w) => w.charAt(0).toUpperCase() + w.slice(1))
    .join(" ");
}

export function catalogForMode(mode: ContentPlayMode): readonly TransitionId[] {
  return mode === "sprite"
    ? SPRITE_TRANSITION_CATALOG
    : ANIMATION_TRANSITION_CATALOG;
}

export function defaultTransitionForMode(mode: ContentPlayMode): TransitionId {
  return mode === "sprite" ? "sprite-bump-left" : "none";
}

export function isTransitionInCatalog(
  id: string,
  catalog: readonly string[]
): id is TransitionId {
  return catalog.includes(id);
}

function loadAllPlayPrefs(prefsKey: string): Record<string, ContentPlayPref> {
  try {
    const raw = localStorage.getItem(PLAY_PREFS_PREFIX + prefsKey);
    if (!raw) return {};
    const parsed = JSON.parse(raw) as Record<string, ContentPlayPref>;
    if (!parsed || typeof parsed !== "object") return {};
    return parsed;
  } catch {
    return {};
  }
}

function saveAllPlayPrefs(
  prefsKey: string,
  map: Record<string, ContentPlayPref>
): void {
  try {
    localStorage.setItem(PLAY_PREFS_PREFIX + prefsKey, JSON.stringify(map));
  } catch {
    /* ignore */
  }
}

/** Resolve stored prefs for a content path (stills ignore mode). */
export function resolveContentPlayPref(
  prefsKey: string,
  path: string,
  isSequence: boolean
): { mode: ContentPlayMode; transition: TransitionId } {
  const all = loadAllPlayPrefs(prefsKey);
  const saved = all[path];
  const mode: ContentPlayMode =
    isSequence && saved?.mode === "sprite" ? "sprite" : "animation";
  const catalog = catalogForMode(mode);
  const fallback = defaultTransitionForMode(mode);
  const transition =
    saved?.transition && isTransitionInCatalog(saved.transition, catalog)
      ? saved.transition
      : fallback;
  return { mode, transition };
}

export function setContentPlayMode(
  prefsKey: string,
  path: string,
  mode: ContentPlayMode
): ContentPlayPref {
  const all = loadAllPlayPrefs(prefsKey);
  const prev = all[path];
  const catalog = catalogForMode(mode);
  let transition = prev?.transition ?? defaultTransitionForMode(mode);
  if (!isTransitionInCatalog(transition, catalog)) {
    transition = defaultTransitionForMode(mode);
  }
  const next: ContentPlayPref = { mode, transition };
  all[path] = next;
  saveAllPlayPrefs(prefsKey, all);
  return next;
}

export function setContentPlayTransition(
  prefsKey: string,
  path: string,
  transition: TransitionId,
  isSequence: boolean
): ContentPlayPref {
  const all = loadAllPlayPrefs(prefsKey);
  const prev = all[path];
  const mode: ContentPlayMode =
    isSequence && prev?.mode === "sprite" ? "sprite" : "animation";
  const catalog = catalogForMode(mode);
  const safe = isTransitionInCatalog(transition, catalog)
    ? transition
    : defaultTransitionForMode(mode);
  const next: ContentPlayPref = isSequence
    ? { mode, transition: safe }
    : { transition: safe };
  all[path] = next;
  saveAllPlayPrefs(prefsKey, all);
  return next;
}

export function migrateContentPlayPref(
  prefsKey: string,
  fromPath: string,
  toPath: string
): void {
  if (fromPath === toPath) return;
  const all = loadAllPlayPrefs(prefsKey);
  if (!(fromPath in all)) return;
  all[toPath] = all[fromPath];
  delete all[fromPath];
  saveAllPlayPrefs(prefsKey, all);
}

export function removeContentPlayPref(prefsKey: string, path: string): void {
  const all = loadAllPlayPrefs(prefsKey);
  if (!(path in all)) return;
  delete all[path];
  saveAllPlayPrefs(prefsKey, all);
}

/** Duration used when playing with the selected transition. */
export function durationForTransition(id: string): number {
  if (id === "none") return 0;
  /* Device-side timeline; host duration is informational for classic FB wipes. */
  if (id === "sprite-bump-left") return 2000;
  return 800;
}
