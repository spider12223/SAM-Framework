/*
 * Dashboard — the Great Hall of the S.A.M Framework.
 * Welcomes the modder, shows live session status from the mod being built,
 * and offers quick travel to the editors. Every count shown here is derived
 * from the schemas at runtime (see data/schemas.js) — never hardcoded.
 */
import { useNavigate } from 'react-router-dom';
import { ITEM_TYPES, SKILLS, CATEGORIES, SLOTS } from '@/data/schemas.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, GoldButton } from '@/components/ui.jsx';

/** The framework's own version (mods declare their own framework_min_version). */
const SAM_FRAMEWORK_VERSION = '2.1.0';

/** Where players get S.A.M itself (the framework is a dependency, not a mod). */
const WORKSHOP_URL = 'https://steamcommunity.com/sharedfiles/filedetails/?id=3763844472';
const INSTALLER_URL = 'https://github.com/spider12223/SAM-Framework/releases/latest';

const TRAVELS = [
  { to: '/class-editor', icon: '🛡', label: 'Class Editor', desc: 'Forge a playable class — attributes, skills, starting gear.' },
  { to: '/item-editor', icon: '⚔', label: 'Item Editor', desc: 'Define a custom item — category, slot, stats and effects.' },
  { to: '/monster-editor', icon: '👹', label: 'Monster Editor', desc: 'Craft a monster variant — stats, gear, followers, world spawns.' },
  { to: '/mod-builder', icon: '📦', label: 'Mod Builder', desc: 'Set the manifest, import/export zips, test in Barony, see changes.' },
  { to: '/validator', icon: '📜', label: 'Validator', desc: 'Check any class, item, monster or mod against the draft-07 schemas.' },
];

/* What landed in this release. Kept short and in plain language: this panel is the first
 * thing a returning modder sees, and the point is 'here is what you can do now that you
 * could not before', not a changelog. */
const WHATS_NEW = [
  ['Your creatures can think for themselves',
   'Barony runs every thing in the world through a function once per frame, and until now that function was always ours. sam_register_behavior lets your script BE it. A turret that tracks and leads a moving target, a boss that changes the rules at half health, a companion that actually pathfinds -- none of that is picked from a list of things we thought of. You get the frame and decide what to spend it on.'],
  ['Mods can change what happens, not just hear about it',
   'Sixty-one of sixty-five events used to be announcements: your mod learned a player was hit, after they were hit. Now assigning to the event changes it. event.damage = event.damage / 2 really halves the blow, and returning false refuses it outright. Two mods that both reduce damage BOTH apply -- each sees what the one before it left, instead of overwriting from the original.'],
  ['A place to come back to',
   'Barony floors are strictly one way; a ladder only ever counts upward. sam_travel_to_level goes anywhere, including back UP, and sam_set_chest_stash turns a chest into storage that survives descending, dying later, quitting and loading.'],
  ['Progress that belongs to one character',
   'sam_world_save stores values inside one savegame, so a brand new character never inherits the last one\'s unlocks. Saves now also record which mods made them and say so plainly if that set changes -- because content ids used to shift when you reordered your mod list, quietly turning saved custom items into different ones.'],
  ['Windows, projectiles, and the game\'s own data',
   'Real panels with buttons, lists and text boxes. Projectiles with their own speed, model and damage. And read access to every item, monster and spell the game knows about, so a shop or a bestiary can be built from real data instead of a hand-written table.'],
];

/** sam-well stat box: big gold number over a small-caps label. */
function StatTile({ value, label }) {
  return (
    <div className="sam-well px-4 py-3 text-center">
      <div style={{ color: 'var(--color-gold-bright)', fontSize: '2rem', lineHeight: 1.1 }}>
        {value}
      </div>
      <div className="sam-label mt-1">{label}</div>
    </div>
  );
}

export default function Dashboard() {
  const navigate = useNavigate();
  const { meta, classes, items, monsters } = useMod();

  const hasMod = Boolean(meta.name.trim() || meta.namespace.trim());

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* ------------------------------------------------- get S.A.M banner */}
      <Panel title="Get S.A.M Framework">
        <p className="m-0" style={{ color: 'var(--color-parchment)' }}>
          <strong style={{ color: 'var(--color-gold)' }}>S.A.M is the engine your mods run on.</strong>{' '}
          Players install it once — then every mod built with S.A.M just works. It's a dependency, not a
          playable mod on its own.
        </p>
        <div className="mt-3 flex flex-wrap gap-3">
          <a className="sam-btn" href={INSTALLER_URL} target="_blank" rel="noreferrer">⬇ Download Installer</a>
          <a className="sam-btn" href={WORKSHOP_URL} target="_blank" rel="noreferrer">🎮 Steam Workshop</a>
        </div>
        <div className="mt-3 sam-well px-4 py-3">
          <div className="sam-label">Building a mod? Tell your players where to get S.A.M:</div>
          <div className="mt-1 text-sm" style={{ color: 'var(--color-parchment)' }}>
            Point them to the{' '}
            <a href={INSTALLER_URL} target="_blank" rel="noreferrer" style={{ color: 'var(--color-gold)' }}>installer from GitHub Releases</a>{' '}
            (it finds Barony and sets S.A.M up), then have them enable your mod in Barony's{' '}
            <span className="sam-label">Mods</span> menu. It's on the{' '}
            <a href={WORKSHOP_URL} target="_blank" rel="noreferrer" style={{ color: 'var(--color-gold)' }}>Steam Workshop</a>{' '}
            too, but Steam can't replace the game's program by itself, so they still run the installer.
          </div>
        </div>
      </Panel>

      {/* --------------------------------------------------- what is new */}
      <Panel title={`New in ${SAM_FRAMEWORK_VERSION}`}>
        <p className="m-0" style={{ color: 'var(--color-parchment)' }}>
          This one is about reach.{' '}
          <strong style={{ color: 'var(--color-gold)' }}>
            Mods can now ship whole dungeons and drive monsters themselves.
          </strong>{' '}
          Some of it is new, and some of it turned out to be sitting in the engine already,
          working and undocumented.
        </p>
        <ul className="mt-3 mb-0 space-y-2" style={{ listStyle: 'none', padding: 0 }}>
          {WHATS_NEW.map(([title, blurb]) => (
            <li key={title} className="sam-well px-4 py-3">
              <div style={{ color: 'var(--color-gold)' }}>{title}</div>
              <div className="mt-1 text-sm" style={{ color: 'var(--color-parchment)' }}>{blurb}</div>
            </li>
          ))}
        </ul>
        <div className="mt-3 text-sm" style={{ color: '#8a7749' }}>
          Two complete example mods ship with the framework: one uses every capability once, the
          other is a full custom boss fight. They are the fastest way to learn.{' '}
          <a href="https://github.com/spider12223/SAM-Framework/tree/main/examples" target="_blank"
             rel="noreferrer" style={{ color: 'var(--color-gold)' }}>Browse the examples</a>
          {' '}or read the{' '}
          <a href="https://github.com/spider12223/SAM-Framework/blob/main/docs/scripting-reference.md"
             target="_blank" rel="noreferrer" style={{ color: 'var(--color-gold)' }}>scripting reference</a>.
        </div>
      </Panel>

      {/* ------------------------------------------------------- welcome */}
      <Panel title="Welcome, Modder">
        <p className="m-0" style={{ color: 'var(--color-parchment)' }}>
          <strong style={{ color: 'var(--color-gold)' }}>S.A.M</strong> — Support All Mods —
          lets you build Barony classes and items as plain JSON, with no C++ and no compiler
          in sight.
        </p>
        <p className="mt-2 mb-0" style={{ color: 'var(--color-parchment)' }}>
          The workflow: craft your classes and items in the editors, gather them in the
          {' '}<span className="sam-label">Mod Builder</span> to set the manifest and bundle a
          zip, then drop that zip into <span className="sam-mono">Barony/mods/</span> to play.
        </p>
      </Panel>

      {/* --------------------------------------------------- forge status */}
      <Panel title="Forge Status">
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
          <StatTile value={classes.length} label="Saved Classes" />
          <StatTile value={items.length} label="Saved Items" />
          <StatTile value={monsters.length} label="Saved Monsters" />
          <div className="sam-well px-4 py-3 text-center flex flex-col justify-center">
            {hasMod ? (
              <>
                <div className="truncate" style={{ color: 'var(--color-parchment)', fontSize: '1.15rem' }}>
                  {meta.name.trim() || 'Untitled Mod'}
                </div>
                <div className="sam-mono mt-1 truncate" style={{ color: '#6b5a35' }}>
                  {meta.namespace.trim() || 'no namespace'}
                </div>
              </>
            ) : (
              <div className="text-sm" style={{ color: '#6b5a35' }}>
                no mod configured yet
              </div>
            )}
            <div className="sam-label mt-1">Current Mod</div>
          </div>
        </div>
        {!hasMod && (
          <div className="mt-3 text-sm" style={{ color: '#6b5a35' }}>
            Name your mod and set a namespace in the <span className="sam-label">Mod Builder</span> to
            stamp every class and item with a shared id.
          </div>
        )}
      </Panel>

      {/* ---------------------------------------------------- quick travel */}
      <Panel title="Quick Travel">
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
          {TRAVELS.map((t) => (
            <div key={t.to} className="space-y-2">
              <GoldButton className="w-full justify-center" onClick={() => navigate(t.to)}>
                <span aria-hidden>{t.icon}</span> {t.label}
              </GoldButton>
              <div className="text-sm text-center" style={{ color: '#6b5a35' }}>{t.desc}</div>
            </div>
          ))}
        </div>
      </Panel>

      {/* ------------------------------------------------------- framework */}
      <Panel title="Framework">
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
          <div className="sam-well px-4 py-3">
            <div className="sam-label">Version</div>
            <div className="sam-mono mt-1" style={{ color: 'var(--color-parchment)' }}>
              S.A.M {SAM_FRAMEWORK_VERSION} · JSON Schema draft-07
            </div>
          </div>
          <div className="sam-well px-4 py-3">
            <div className="sam-label">Enums (read from schemas/ at runtime)</div>
            <div className="sam-mono mt-1" style={{ color: 'var(--color-parchment)' }}>
              {ITEM_TYPES.length} item types · {SKILLS.length} skills ·{' '}
              {CATEGORIES.length} categories · {SLOTS.length} slots
            </div>
          </div>
        </div>
        <div className="mt-3 text-sm" style={{ color: '#6b5a35' }}>
          Every list the GUI offers is derived from <span className="sam-mono">SAM-Framework/schemas/</span>{' '}
          — update a schema on the C++ side and the editors follow automatically.
        </div>
        <div className="mt-3 flex flex-wrap gap-3">
          <a
            className="sam-btn"
            href={`${import.meta.env.BASE_URL}docs/schema-reference.html`}
            target="_blank"
            rel="noreferrer"
          >
            📜 Schema Reference
          </a>
          <a
            className="sam-btn"
            href="https://github.com/spider12223/SAM-Framework"
            target="_blank"
            rel="noreferrer"
          >
            ⚔ GitHub
          </a>
        </div>
      </Panel>
    </div>
  );
}
