/*
 * Lightweight, advisory linter for behavior scripts — cross-checks the script
 * text against the machine-readable API manifest (data/samApi.js). It catches the
 * high-signal mistakes (unknown sam_ call, bad event name, bad stat/effect name)
 * without a real parser, so it works for both Lua and JS. Never blocks — it's a hint.
 */
import { SAM_FUNCTIONS, SAM_EVENTS, PLAYER_STATS, EFFECTS } from '@/data/samApi.js';

const FN_NAMES = new Set(SAM_FUNCTIONS.map((f) => f.name));
const EVENT_NAMES = new Set(SAM_EVENTS.map((e) => e.name));
const STATS = new Set(PLAYER_STATS.map((s) => s.toUpperCase()));
const EFFECT_NAMES = new Set(EFFECTS.map((e) => e.toUpperCase()));

// Only match sam_x immediately followed by "(" — i.e. an actual call.
const SAM_CALL = /\bsam_[a-z0-9_]+(?=\s*\()/g;
const EVENT_CMP = /event\.name\s*===?\s*["']([^"']+)["']/g;
const STAT_CALL = /\bsam_(?:get|set)_stat\s*\(\s*[^,]+,\s*["']([^"']+)["']/g;
const EFFECT_CALL = /\bsam_(?:apply|remove|has)_effect\s*\(\s*[^,]+,\s*["']([^"']+)["']/g;

function effectOk(s) {
  const u = s.toUpperCase();
  if (EFFECT_NAMES.has(u)) return true;
  if (u.startsWith('CUSTOM:')) return true;           // custom slot alias
  if (/^\d+$/.test(s)) { const n = Number(s); return n >= 135 && n < 160; } // raw custom slot
  return false;
}

/** Returns [{ line, severity: 'warn', message }] — empty when the script looks clean. */
export function lintScript(code) {
  if (!code || !code.trim()) return [];
  const out = [];
  code.split('\n').forEach((raw, i) => {
    const line = i + 1;
    // Drop line comments so `-- sam_foo` / `// sam_foo` don't false-positive.
    const src = raw.replace(/--.*$/, '').replace(/\/\/.*$/, '');
    let m;
    SAM_CALL.lastIndex = 0;
    while ((m = SAM_CALL.exec(src))) {
      if (!FN_NAMES.has(m[0])) out.push({ line, severity: 'warn', message: `unknown function \`${m[0]}()\` — not in the SAM API` });
    }
    EVENT_CMP.lastIndex = 0;
    while ((m = EVENT_CMP.exec(src))) {
      if (!EVENT_NAMES.has(m[1])) out.push({ line, severity: 'warn', message: `unknown event "${m[1]}" — check the API Reference` });
    }
    STAT_CALL.lastIndex = 0;
    while ((m = STAT_CALL.exec(src))) {
      if (!STATS.has(m[1].toUpperCase())) out.push({ line, severity: 'warn', message: `"${m[1]}" isn't a known stat` });
    }
    EFFECT_CALL.lastIndex = 0;
    while ((m = EFFECT_CALL.exec(src))) {
      if (!effectOk(m[1])) out.push({ line, severity: 'warn', message: `"${m[1]}" isn't a known effect (or a custom slot 135–159)` });
    }
  });
  return out;
}
