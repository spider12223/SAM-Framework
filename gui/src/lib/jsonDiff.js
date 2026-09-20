/*
 * Zero-dependency JSON diff for the Mod Builder "Changes" panel.
 *  canonicalize(state) — stable serialization of every collection the baseline snapshot
 *                        holds (sorted object keys, collections sorted by key) -> line array
 *  diffLines(a, b)     — LCS line diff -> [{ kind: 'same'|'add'|'del', text }]
 *  collapseHunks(rows) — folds long unchanged runs into { kind:'gap', count } markers
 *
 * COLLECTIONS is the whole list on purpose. This used to destructure four of them, so the
 * panel that says it "shows exactly what you've changed" reported "no changes since
 * baseline" after adding a sound, a track, a race, a spell, a recipe or a patch -- the
 * review step before an export could not see this release's headline feature. It is derived
 * from the baseline snapshot in state/modReducer.js ('setBaseline'); the two lists must name
 * the same collections, or a diff is either blind again or compares a collection against
 * nothing.
 */
export const COLLECTIONS = [
  'classes', 'items', 'monsters', 'spells', 'effects', 'races', 'sounds', 'music',
  'recipes', 'patches',
];

/** A stable sort key. Content ids use `id`; a sound or track that REPLACES something is
 *  keyed by what it replaces, and a patch by the file it targets. */
function entryKey(x) {
  return String(x?.id ?? x?.replace ?? x?.target ?? '');
}

/** Recursively sort object keys so serialization is order-independent. */
function sortKeys(value) {
  if (Array.isArray(value)) return value.map(sortKeys);
  if (value && typeof value === 'object') {
    const out = {};
    for (const k of Object.keys(value).sort()) out[k] = sortKeys(value[k]);
    return out;
  }
  return value;
}

/** Stable stringify of the diffable slice of mod state -> array of lines. */
export function canonicalize(state) {
  const byKey = (a, b) => entryKey(a).localeCompare(entryKey(b));
  const doc = { meta: state?.meta };
  for (const name of COLLECTIONS) doc[name] = [...(state?.[name] ?? [])].sort(byKey);
  return JSON.stringify(sortKeys(doc), null, 2).split('\n');
}

/** Classic LCS line diff — fine at mod scale (a few hundred lines). */
export function diffLines(aLines, bLines) {
  const n = aLines.length;
  const m = bLines.length;
  // DP table of LCS lengths.
  const dp = Array.from({ length: n + 1 }, () => new Uint16Array(m + 1));
  for (let i = n - 1; i >= 0; i--) {
    for (let j = m - 1; j >= 0; j--) {
      dp[i][j] = aLines[i] === bLines[j]
        ? dp[i + 1][j + 1] + 1
        : Math.max(dp[i + 1][j], dp[i][j + 1]);
    }
  }
  const rows = [];
  let i = 0;
  let j = 0;
  while (i < n && j < m) {
    if (aLines[i] === bLines[j]) {
      rows.push({ kind: 'same', text: aLines[i] });
      i++; j++;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      rows.push({ kind: 'del', text: aLines[i] });
      i++;
    } else {
      rows.push({ kind: 'add', text: bLines[j] });
      j++;
    }
  }
  while (i < n) rows.push({ kind: 'del', text: aLines[i++] });
  while (j < m) rows.push({ kind: 'add', text: bLines[j++] });
  return rows;
}

/** Fold runs of unchanged lines, keeping `context` lines around each change. */
export function collapseHunks(rows, context = 2) {
  const out = [];
  let run = [];
  const flushRun = (isLast) => {
    if (run.length <= context * 2 + 1) {
      out.push(...run);
    } else {
      const head = out.length === 0 ? 0 : context; // no leading context at file start
      const tail = isLast ? 0 : context;
      out.push(...run.slice(0, head));
      out.push({ kind: 'gap', count: run.length - head - tail });
      if (tail) out.push(...run.slice(-tail));
    }
    run = [];
  };
  for (const row of rows) {
    if (row.kind === 'same') run.push(row);
    else {
      flushRun(false);
      out.push(row);
    }
  }
  flushRun(true);
  return out;
}

/** One-line summary: { added, removed }. */
export function diffSummary(rows) {
  let added = 0;
  let removed = 0;
  for (const r of rows) {
    if (r.kind === 'add') added++;
    else if (r.kind === 'del') removed++;
  }
  return { added, removed };
}
