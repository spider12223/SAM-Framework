/*
 * The character heads a class can force, derived from Barony's real engine formula.
 *
 * These indices are NOT guesses — they mirror playerHeadSprite() (actplayer.cpp:5742).
 * That function is four different formulas depending on the appearance value, because
 * Head6 was added out of sequence and the Med/Dark blocks carry one extra model each
 * (their per-sex stride is 13, not 12). Do NOT "simplify" this into one expression —
 * you would silently produce wrong heads:
 *
 *   appearance 0-4   : 113 + 12*sex + appearance
 *   appearance 5     : 332 + sex
 *   appearance 6-11  : 341 + 13*sex + (appearance - 6)
 *   appearance 12-17 : 367 + 13*sex + (appearance - 12)
 *
 * Non-human races have exactly ONE head per sex — there is no "goblin face 3". Their
 * indices are hardcoded in the same function.
 */

const SEX = { male: 0, female: 1 };

/** Barony's playerHeadSprite arithmetic for HUMAN, kept branch-for-branch. */
function humanHeadSprite(sex, appearance) {
  const s = SEX[sex];
  if (appearance <= 4) return 113 + 12 * s + appearance;
  if (appearance === 5) return 332 + s;
  if (appearance <= 11) return 341 + 13 * s + (appearance - 6);
  return 367 + 13 * s + (appearance - 12);
}

/** The 18 human appearance names, in engine order (lang 5405+). */
const APPEARANCE_NAMES = [
  'Landguard', 'Northborn', 'Firebrand', 'Hardbred', 'Highwatch', 'Gloomforge',
  'Pyrebloom', 'Snakestone', 'Windclan', 'Warblood', 'Millbound', 'Sunstalk',
  'Claymount', 'Stormward', 'Tradefell', 'Nightscape', 'Baytower', 'Whetsong',
];

/** Skin tone is appearance/6; the face digit is appearance%6. */
function toneOf(appearance) {
  return ['Light', 'Medium', 'Dark'][Math.floor(appearance / 6)];
}

/** All 36 human heads (3 tones x 6 faces x 2 sexes), each with its real model index. */
export const HUMAN_HEADS = (() => {
  const out = [];
  for (const sex of ['male', 'female']) {
    for (let a = 0; a < 18; a += 1) {
      out.push({
        id: String(humanHeadSprite(sex, a)),
        index: humanHeadSprite(sex, a),
        label: `${sex === 'male' ? '♂' : '♀'} ${toneOf(a)} — ${APPEARANCE_NAMES[a]}`,
        race: 'HUMAN',
        sex,
        tone: toneOf(a),
        appearance: a,
      });
    }
  }
  return out;
})();

/**
 * One head per sex per non-human race, straight from playerHeadSprite's switch.
 * Where a race has a single entry it has no sex split in the engine either.
 */
export const RACE_HEADS = [
  { race: 'SKELETON',  male: 686,  female: 1049 },
  { race: 'GOBLIN',    male: 694,  female: 752 },
  { race: 'INCUBUS',   male: 702,  female: 702 },
  { race: 'SUCCUBUS',  male: 710,  female: 710 },
  { race: 'VAMPIRE',   male: 718,  female: 756 },
  { race: 'INSECTOID', male: 726,  female: 760 },
  { race: 'GOATMAN',   male: 734,  female: 768 },
  { race: 'AUTOMATON', male: 742,  female: 770 },
  { race: 'TROLL',     male: 817,  female: 817 },
  { race: 'IMP',       male: 827,  female: 827 },
  { race: 'DRYAD',     male: 1963, female: 1992 },
  { race: 'MYCONID',   male: 1998, female: 1997 },
  { race: 'GREMLIN',   male: 2047, female: 2048 },
  { race: 'GNOME',     male: 2213, female: 2214 },
].flatMap((r) => ([
  { id: String(r.male), index: r.male, label: `♂ ${r.race}`, race: r.race, sex: 'male' },
  ...(r.female !== r.male
    ? [{ id: String(r.female), index: r.female, label: `♀ ${r.race}`, race: r.race, sex: 'female' }]
    : []),
]));

/** Every vanilla head the editor can offer. */
export const ALL_HEADS = [...HUMAN_HEADS, ...RACE_HEADS];

/**
 * Races a class look can target. "default" covers anything without its own entry —
 * and a race with neither is deliberately left alone, so a class never breaks a race
 * the author didn't write for.
 */
export const APPEARANCE_RACES = [
  'default', 'HUMAN', 'SKELETON', 'VAMPIRE', 'SUCCUBUS', 'GOATMAN', 'AUTOMATON',
  'INCUBUS', 'GOBLIN', 'INSECTOID', 'RAT', 'TROLL', 'SPIDER', 'IMP', 'GNOME',
  'GREMLIN', 'DRYAD', 'MYCONID', 'SALAMANDER',
];

/** Human-readable label for a head id, falling back to the raw value for custom models. */
export function headLabel(id) {
  const h = ALL_HEADS.find((x) => x.id === String(id));
  return h ? h.label : String(id);
}
