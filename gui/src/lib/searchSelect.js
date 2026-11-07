/*
 * The choices behind <SearchSelect> (components/ui.jsx), kept here so they can be tested.
 *
 * The bug these fix: Enter always took the FIRST option containing the typed text, so typing an
 * option exactly ("mymod:fire") picked an earlier one that merely contains it ("mymod:fireball"),
 * and with free values allowed there was no way to enter a value that is part of an option's
 * name: the "use what I typed" row only appeared when nothing matched at all.
 */

/** Case-insensitive substring matches for a query, capped at maxShown (all options when empty). */
export function searchMatches(options, query, maxShown) {
  const q = query.trim().toUpperCase();
  const list = q ? options.filter((o) => o.toUpperCase().includes(q)) : options;
  return list.slice(0, maxShown);
}

/** The option equal to the query, ignoring case, or undefined. */
export function exactOption(options, query) {
  const q = query.trim().toUpperCase();
  return q ? options.find((o) => o.toUpperCase() === q) : undefined;
}

/** What Enter picks: the exact option, else the first match, else (when free values are allowed)
 *  the typed text. null = nothing to pick; an empty query picks nothing. */
export function enterPick(options, matches, query, allowCustom) {
  const q = query.trim();
  if (!q) return null;
  const exact = exactOption(options, q);
  if (exact !== undefined) return exact;
  if (matches.length) return matches[0];
  return allowCustom ? q : null;
}

/** Whether to offer "use what I typed": free values allowed, something typed, and it is not an option. */
export function offerTyped(options, query, allowCustom) {
  return Boolean(allowCustom && query.trim() && exactOption(options, query) === undefined);
}
