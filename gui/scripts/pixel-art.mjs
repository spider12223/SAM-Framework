// Draws the Mod Builder's pixel art (window frames, plaques, the selector brackets, the fleur-de-lis,
// gems, the stone floor) from the ASCII maps below and writes them to src/pixel-art.css as CSS custom
// properties holding SVG data URIs.
//
//   node scripts/pixel-art.mjs
//
// The look is Barony's in-game menus, redrawn here: no image or font is taken from the game. Each map
// is one art pixel per character; the CSS scales them by whole numbers (2px a pixel) with
// border-image or background-size, so every edge stays crisp. Run it again after editing a map and
// commit the regenerated CSS with it.
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.resolve(here, '../src/pixel-art.css');

// One palette for every map. '.' is transparent.
const C = {
  K: '#07080b', // outline
  k: '#000000', // inset shadow
  H: '#7d5434', // oak, lit edge
  O: '#4f301b', // oak
  o: '#2b180c', // oak, shaded edge
  Y: '#f7d677', // gold, lit
  G: '#d9a23f', // gold
  g: '#8c5a15', // gold, shaded
  P: '#c2463f', // crimson, lit
  R: '#86202a', // crimson
  r: '#45101a', // crimson, shaded
  E: '#63c178', // emerald, lit
  M: '#2f8043', // emerald
  m: '#174624', // emerald, shaded
  S: '#3c4454', // field edge
  T: '#7f8b9f', // field edge, lit (slots)
  B: '#050608', // field body: Barony's option rows are black
  D: '#2a070b', // fleur on the band
  W: '#f2ead7', // bone
  I: '#2f343e', // iron
  i: '#596070', // iron, lit
  F: '#7b808c', // grip dots
  A: '#4f93d8', // sapphire edge (advice)
};

function svg(rows, extraPalette = {}, ground = null) {
  const pal = { ...C, ...extraPalette };
  const h = rows.length;
  const w = rows[0].length;
  for (const r of rows) {
    if (r.length !== w) throw new Error(`ragged map row "${r}" (want ${w})`);
  }
  let rects = ground ? `<rect width="${w}" height="${h}" fill="${ground}"/>` : '';
  for (let y = 0; y < h; y++) {
    let x = 0;
    while (x < w) {
      const ch = rows[y][x];
      if (ch === '.') { x++; continue; }
      let run = 1;
      while (x + run < w && rows[y][x + run] === ch) run++;
      const fill = pal[ch];
      if (!fill) throw new Error(`no colour for "${ch}"`);
      rects += `<rect x="${x}" y="${y}" width="${run}" height="1" fill="${fill}"/>`;
      x += run;
    }
  }
  const body = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" shape-rendering="crispEdges">${rects}</svg>`;
  // Only what a data URI inside url("...") needs escaped: about half the size of encodeURIComponent.
  const uri = body.replace(/"/g, "'").replace(/#/g, '%23').replace(/</g, '%3C').replace(/>/g, '%3E');
  return `url("data:image/svg+xml,${uri}")`;
}

const recolor = (rows, map) => rows.map((r) => [...r].map((c) => map[c] ?? c).join(''));

// ---------------------------------------------------------------- window frame (slice 5)
// Oak band lit on its outer top-left and inner bottom-right, a gold rivet in each corner, black
// outline inside and out. The middle two rows and columns are what border-image stretches.
const WINDOW = [
  '.KKKKKKKKKK.',
  'KHHHHHHHHHHK',
  'KHYGOOOOYGoK',
  'KHGgooooGgoK',
  'KHOoKKKKHOoK',
  'KHOoK..KHOoK',
  'KHOoK..KHOoK',
  'KHOoKKKKHOoK',
  'KHYGHHHHYGoK',
  'KHGgOOOOGgoK',
  'KooooooooooK',
  '.KKKKKKKKKK.',
];

// ---------------------------------------------------------------- plaques (buttons, slice 3)
const PLAQUE = [
  '.KKKKK.',
  'KYYYYYK',
  'KYGGGgK',
  'KYGGGgK',
  'KYGGGgK',
  'KgggggK',
  '.KKKKK.',
];
const PLAQUE_DOWN = [
  '.KKKKK.',
  'KgggggK',
  'KgGGGYK',
  'KgGGGYK',
  'KgGGGYK',
  'KGYYYYK',
  '.KKKKK.',
];
const OAK = { Y: 'H', G: 'O', g: 'o' };
const CRIMSON = { Y: 'P', G: 'R', g: 'r' };
const EMERALD = { Y: 'E', G: 'M', g: 'm' };

// ---------------------------------------------------------------- fields (inputs, wells, slots)
const FIELD = [
  '.SSSSS.',
  'SkkkkkS',
  'SkBBBBS',
  'SkBBBBS',
  'SkBBBBS',
  'SBBBBBS',
  '.SSSSS.',
];

// ---------------------------------------------------------------- selector brackets (slice 5, no fill)
const BRACKET = [
  'KKKKK..KKKKK',
  'KYYYK..KYYYK',
  'KYKKK..KKKYK',
  'KYK......KYK',
  'KKK......KKK',
  '............',
  '............',
  'KKK......KKK',
  'KYK......KYK',
  'KYKKK..KKKYK',
  'KYYYK..KYYYK',
  'KKKKK..KKKKK',
];

// ---------------------------------------------------------------- band ornaments
const FLEUR = [
  '....D....',
  '...DDD...',
  '...DDD...',
  'DD.DDD.DD',
  'D.DDDDD.D',
  '.DDDDDDD.',
  'DDDDDDDDD',
  '..D.D.D..',
  '.DD.D.DD.',
];
const CROSSES = [
  '.D...D...D.',
  'DDD.DDD.DDD',
  '.D...D...D.',
];

// ---------------------------------------------------------------- title bar
const GEM = [
  '..KKK..',
  '.KWLLK.',
  'KWLMMDK',
  'KLMMMDK',
  'KLMMDDK',
  '.KDDDK.',
  '..KKK..',
];
const GEMS = {
  ruby: { L: '#ec5a4f', M: '#b8231f', D: '#6a0d0c' },
  sapphire: { L: '#69b0f3', M: '#2362b4', D: '#0f2f66' },
  emerald: { L: '#6fdc86', M: '#22903f', D: '#0d4b1e' },
};

// ---------------------------------------------------------------- small glyphs
// The row tools: remove, edit, duplicate, drag. Drawn here so no button leans on a font glyph.
const XMARK = [
  'W...W',
  'WW.WW',
  '.WWW.',
  'WW.WW',
  'W...W',
];
const PENCIL = [
  '....WW',
  '...WWW',
  '..WWW.',
  '.WWW..',
  'YWW...',
  'YY....',
];
const COPY = [
  '..WWWWW',
  '..W...W',
  'WWWWW.W',
  'W...W.W',
  'W...WWW',
  'W...W..',
  'WWWWW..',
];
const GRIP = [
  'F.F',
  '...',
  'F.F',
  '...',
  'F.F',
  '...',
  'F.F',
];
// The knot where the title bar's gold rail meets a gem.
const KNOT = [
  '...K...',
  '..KYK..',
  '.KYGgK.',
  'KYGGGgK',
  '.KgGgK.',
  '..KgK..',
  '...K...',
];
const CHECK = [
  '......B',
  '.....BB',
  'B...BB.',
  'BB.BB..',
  '.BBB...',
  '..B....',
];
const CHEVRON = [
  'WWWWWWW',
  '.WWWWW.',
  '..WWW..',
  '...W...',
];
const THUMB = [
  '.KKKKKK.',
  'KHHHHHHK',
  'KHOOOOoK',
  'KiiiiiIK',
  'KHOOOOoK',
  'KHOOOOoK',
  'KHOOOOoK',
  'KHOOOOoK',
  'KiiiiiIK',
  'KHOOOOoK',
  'KooooooK',
  '.KKKKKK.',
];

// ---------------------------------------------------------------- icons
// The attributes, the skills and a few marks the editors use. Each is drawn as fills only on a 10x10
// grid; outlined() rings every shape in black, as Barony outlines its own icons, so a map stays easy to
// read and edit. Shown at 20px (2px a pixel) by <PixelIcon>.
const ICON_PAL = {
  W: '#f2ead7', // bone
  L: '#dfe4ec', // steel, lit
  N: '#8d96a6', // steel
  G: '#d9a23f', // gold
  Y: '#f7d677', // gold, lit
  g: '#8c5a15', // gold, shaded
  H: '#8a5a36', // wood
  o: '#4f301b', // wood, shaded
  R: '#d0473d', // red
  r: '#86202a', // red, shaded
  Q: '#e7b083', // skin
  q: '#a86a45', // skin, shaded
  C: '#ece0bf', // parchment, feather
  c: '#b9a679', // parchment, shaded
  A: '#4f93d8', // sapphire
  a: '#a9d3f7', // sapphire, lit
  V: '#a46be0', // violet
  v: '#5e2f9a', // violet, shaded
  E: '#6fdc86', // emerald, lit
  M: '#2f8043', // emerald
  K: '#07080b', // outline (also a pupil or a stroke inside a shape)
};

function outlined(rows) {
  const h = rows.length;
  const w = rows[0].length;
  const filled = (x, y) => x >= 0 && y >= 0 && x < w && y < h && rows[y][x] !== '.';
  return rows.map((r, y) => [...r].map((ch, x) => {
    if (ch !== '.') return ch;
    return filled(x - 1, y) || filled(x + 1, y) || filled(x, y - 1) || filled(x, y + 1) ? 'K' : '.';
  }).join(''));
}

const ICONS = {
  // ---- attributes
  str: [ // a fist
    '..........',
    '..QQ.QQ...',
    '.QQqQQqQ..',
    '.QQqQQqQQ.',
    '.qqqqqqQQ.',
    '.QQQQQQqQ.',
    '..QQQQQQ..',
    '..QQQQQq..',
    '...qqqq...',
    '..........',
  ],
  dex: [ // a feather: a broad vane, a pale shaft down its middle, a split near the base
    '..........',
    '......CCc.',
    '.....CCWc.',
    '....CCWCc.',
    '...CCWCc..',
    '..CCWCc.c.',
    '..CWCc....',
    '..Wc.c....',
    '.H........',
    '..........',
  ],
  con: [ // a heart
    '..........',
    '..RR..RR..',
    '.RWRRRRRr.',
    '.RRRRRRRr.',
    '.RRRRRRRr.',
    '..RRRRRr..',
    '...RRRr...',
    '....Rr....',
    '..........',
    '..........',
  ],
  int: [ // a tome with a gold clasp
    '..........',
    '..AAAAAAa.',
    '.HAaAAAAa.',
    '.HAAAYAAa.',
    '.HAAYGYAa.',
    '.HAAAYAAa.',
    '.HAAAAAAa.',
    '.HCCCCCCC.',
    '..........',
    '..........',
  ],
  per: [ // an eye
    '..........',
    '..........',
    '...WWWW...',
    '..WAAaAW..',
    '.WAAKKAAW.',
    '..WAAAAW..',
    '...WWWW...',
    '..........',
    '..........',
    '..........',
  ],
  chr: [ // a smiling face
    '..........',
    '...QQQQ...',
    '..QQQQQQ..',
    '.QQKQQKQQ.',
    '.QQQQQQQQ.',
    '.QKQQQQKQ.',
    '..QKKKKQ..',
    '...QQQQ...',
    '..........',
    '..........',
  ],
  // ---- skills
  sword: [
    '..........',
    '.......LL.',
    '......LLN.',
    '.....LLN..',
    '....LLN...',
    '.G.LLN....',
    '..GLN.....',
    '..HG......',
    '.H..G.....',
    '..........',
  ],
  axe: [
    '..........',
    '...H.NN...',
    '...HNLLN..',
    '...HLLLLN.',
    '...HNLLN..',
    '...H.NN...',
    '...H......',
    '...H......',
    '...o......',
    '..........',
  ],
  mace: [
    '..........',
    '....N.....',
    '...NLN....',
    '..NLLLN...',
    '...NLN....',
    '....H.....',
    '....H.....',
    '....H.....',
    '....o.....',
    '..........',
  ],
  polearm: [
    '..........',
    '.L..L..L..',
    '.L..L..L..',
    '.LLLLLLL..',
    '....L.....',
    '....H.....',
    '....H.....',
    '....H.....',
    '....o.....',
    '..........',
  ],
  bow: [
    '..........',
    '..WHH.....',
    '..W..H....',
    '..W...H...',
    '..W...H...',
    '..W...H...',
    '..W...H...',
    '..W..H....',
    '..WHH.....',
    '..........',
  ],
  shield: [
    '..........',
    '.GGGGGGGG.',
    '.GRRRRRRG.',
    '.GRRRYRRG.',
    '.GRRYYYRG.',
    '.GRRRYRRG.',
    '..GRRRRG..',
    '...GRRG...',
    '....GG....',
    '..........',
  ],
  stealth: [ // a crescent moon
    '..........',
    '....WWW...',
    '..WWW.....',
    '.WWW......',
    '.WW.......',
    '.WW.......',
    '.WWW......',
    '..WWWW....',
    '....WWW...',
    '..........',
  ],
  key: [
    '..........',
    '..........',
    '.GGG......',
    'G...G.....',
    'G...GGGGG.',
    'G...G.G.G.',
    '.GGG..G.G.',
    '..........',
    '..........',
    '..........',
  ],
  lens: [ // a magnifying glass
    '..........',
    '..NNN.....',
    '.N...N....',
    'N.aAA.N...',
    'N.AAA.N...',
    '.N...N....',
    '..NNNHH...',
    '......HH..',
    '.......HH.',
    '..........',
  ],
  coin: [
    '..........',
    '..GGGG....',
    '.GYYYYg...',
    '.GYGGYg...',
    '.GYGGYg...',
    '.GYYYYg...',
    '..gggg....',
    '..........',
    '..........',
    '..........',
  ],
  crown: [
    '..........',
    '..........',
    '.G..GG..G.',
    '.GG.GG.GG.',
    '.GGGGGGGG.',
    '.GYGRRGYG.',
    '.GGGGGGGG.',
    '.gggggggg.',
    '..........',
    '..........',
  ],
  orb: [ // a crystal orb on a gold stand
    '..........',
    '...VVVV...',
    '..VWVVVv..',
    '.VWVVVVVv.',
    '.VVVVVVVv.',
    '..VVVVvv..',
    '...vvvv...',
    '..GGGGGG..',
    '..........',
    '..........',
  ],
  sparkle: [
    '....Y.....',
    '....Y.....',
    '...YWY....',
    'YYYWWWYYY.',
    '...YWY....',
    '....Y.....',
    '....Y..Y..',
    '......YWY.',
    '.......Y..',
    '..........',
  ],
  amulet: [
    '.G......G.',
    '..G....G..',
    '...G..G...',
    '....GG....',
    '...GAAG...',
    '..GAaAAG..',
    '..GAAAAG..',
    '...GAAG...',
    '....GG....',
    '..........',
  ],
  flask: [
    '..........',
    '....HH....',
    '....LN....',
    '....LN....',
    '...LEEN...',
    '..LEEEEN..',
    '.LEWEEEEN.',
    '.LEEEEEEN.',
    '..NNNNNN..',
    '..........',
  ],
  // ---- marks
  hand: [ // "also starts in hand"
    '..........',
    '...Q.Q....',
    '..QQ.Q.Q..',
    '..QQ.Q.Q..',
    'Q.QQQQQQ..',
    'QQQQQQQq..',
    '.QQQQQQq..',
    '..QQQQq...',
    '...QQq....',
    '..........',
  ],
  mana: [ // a drop of mana
    '..........',
    '....A.....',
    '...AA.....',
    '...AAA....',
    '..AaAAA...',
    '..aAAAA...',
    '..AAAAA...',
    '...AAA....',
    '..........',
    '..........',
  ],
};

// ---------------------------------------------------------------- the stone floor
// Barony's menus sit on a dark slate speckled with light. A fixed-seed scatter, so a tile is the same on
// every build.
function stoneTile(ground, specks, seed = 0x5a3d, size = 32) {
  const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const rows = [];
  for (let y = 0; y < size; y++) {
    let r = '';
    for (let x = 0; x < size; x++) {
      const v = rnd();
      r += v < 0.035 ? 'b' : v < 0.075 ? 'c' : v < 0.085 ? 'd' : '.';
    }
    rows.push(r);
  }
  return svg(rows, specks, ground);
}

const vars = {
  '--px-window': svg(WINDOW),
  '--px-plaque-gold': svg(PLAQUE),
  '--px-plaque-gold-down': svg(PLAQUE_DOWN),
  '--px-plaque-oak': svg(recolor(PLAQUE, OAK)),
  '--px-plaque-oak-down': svg(recolor(PLAQUE_DOWN, OAK)),
  '--px-plaque-crimson': svg(recolor(PLAQUE, CRIMSON)),
  '--px-plaque-crimson-down': svg(recolor(PLAQUE_DOWN, CRIMSON)),
  '--px-plaque-emerald': svg(recolor(PLAQUE, EMERALD)),
  '--px-plaque-emerald-down': svg(recolor(PLAQUE_DOWN, EMERALD)),
  '--px-field': svg(FIELD),
  '--px-field-focus': svg(recolor(FIELD, { S: 'G' })),
  '--px-field-error': svg(recolor(FIELD, { S: 'P' })),
  '--px-field-info': svg(recolor(FIELD, { S: 'A' })),
  '--px-slot': svg(recolor(FIELD, { S: 'T' })),
  '--px-bracket': svg(BRACKET),
  '--px-fleur': svg(FLEUR),
  '--px-crosses': svg(CROSSES),
  '--px-gem-ruby': svg(GEM, GEMS.ruby),
  '--px-gem-sapphire': svg(GEM, GEMS.sapphire),
  '--px-gem-emerald': svg(GEM, GEMS.emerald),
  '--px-check': svg(CHECK),
  '--px-chevron': svg(CHEVRON),
  '--px-thumb': svg(THUMB),
  '--px-xmark': svg(XMARK),
  '--px-pencil': svg(PENCIL),
  '--px-copy': svg(COPY),
  '--px-grip': svg(GRIP),
  '--px-knot': svg(KNOT),
  // the floor, and the lighter slate inside every window
  '--px-stone': stoneTile('#141925', { b: '#1c2232', c: '#191e2c', d: '#26304a' }),
  '--px-stone-window': stoneTile('#1c2333', { b: '#242c3f', c: '#20283a', d: '#303b56' }, 0x1f7b),
  ...Object.fromEntries(Object.entries(ICONS).map(([name, rows]) => [`--icon-${name}`, svg(outlined(rows), ICON_PAL)])),
};

const css = `/* GENERATED by scripts/pixel-art.mjs -- edit the maps there, not this file. */
:root {
${Object.entries(vars).map(([k, v]) => `  ${k}: ${v};`).join('\n')}
}
`;
writeFileSync(OUT, css);
console.log(`wrote ${path.relative(process.cwd(), OUT)} (${Object.keys(vars).length} images)`);
