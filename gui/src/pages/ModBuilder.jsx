/*
 * Mod Builder — the final assembly bench.
 * Edits the mod.json manifest (bound to session meta), lists the classes and
 * items saved from the editors, then validates EVERYTHING against the schemas
 * and bundles it into the drop-in .zip (mod.json + classes/*.json + items/*.json).
 * Manifest shape mirrors lib/exportZip.js exactly so what we validate is what we ship.
 */
import { useMemo, useState } from 'react';
import { NAMESPACE_PATTERN, VERSION_PATTERN } from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { buildModZip, downloadBlob } from '@/lib/exportZip.js';
import { useMod } from '@/state/ModContext.jsx';
import {
  Panel, Field, TextInput, GoldButton, ItemRow, ErrorList, SavedNote,
} from '@/components/ui.jsx';

/** Same file-stem rule lib/exportZip.js uses, so manifest paths match the zip. */
function fileStem(id, fallback) {
  const tail = (id ?? '').split(':')[1];
  return (tail && tail.trim()) || fallback;
}

export default function ModBuilder() {
  const { meta, classes, items, dispatch } = useMod();
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  const setMeta = (patch) => dispatch({ type: 'setMeta', patch });

  const namespaceBad = meta.namespace !== '' && !NAMESPACE_PATTERN.test(meta.namespace);
  const versionBad = meta.version !== '' && !VERSION_PATTERN.test(meta.version);
  const fwBad = meta.framework_min_version !== '' && !VERSION_PATTERN.test(meta.framework_min_version);

  // Paths exactly as buildModZip lays them out.
  const classPaths = useMemo(
    () => classes.map((c, i) => `classes/${fileStem(c.id, `class_${i + 1}`)}.json`),
    [classes]
  );
  const itemPaths = useMemo(
    () => items.map((it, i) => `items/${fileStem(it.id, `item_${i + 1}`)}.json`),
    [items]
  );

  const buildManifest = () => ({
    namespace: meta.namespace,
    name: meta.name,
    author: meta.author ?? '',
    version: meta.version,
    framework_min_version: meta.framework_min_version,
    dependencies: [],
    classes: classPaths,
    items: itemPaths,
    plugins: [],
    description: meta.description ?? '',
  });

  const collectErrors = () => {
    const all = [];
    const push = (source, res) => {
      for (const e of res.errors) all.push({ path: `${source} ${e.path}`, message: e.message });
    };
    push('mod.json', validate('mod', buildManifest()));
    classes.forEach((c, i) => push(classPaths[i], validate('class', c)));
    items.forEach((it, i) => push(itemPaths[i], validate('item', it)));
    return all;
  };

  const exportMod = async () => {
    setSavedAs('');
    const all = collectErrors();
    if (all.length) {
      setErrors(all);
      return;
    }
    setErrors([]);
    const blob = await buildModZip(meta, classes, items);
    downloadBlob(blob, `${meta.namespace}.zip`);
    setSavedAs(meta.namespace);
  };

  const canExport = meta.namespace.trim() !== '' && meta.name.trim() !== '';

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* ------------------------------------------------ manifest fields */}
      <Panel title="Mod Manifest">
        <div className="grid grid-cols-1 md:grid-cols-2 gap-3">
          <Field
            label="Namespace"
            hint="Prefix for every class & item id — e.g. darkblade → darkblade:assassin"
          >
            <TextInput
              value={meta.namespace}
              onChange={(v) => setMeta({ namespace: v })}
              placeholder="darkblade"
            />
            {namespaceBad && (
              <div className="sam-error text-sm mt-1">
                lowercase letters, digits, _ — must start with a letter
              </div>
            )}
          </Field>

          <Field label="Mod Name" hint="Shown to players in the mods menu.">
            <TextInput
              value={meta.name}
              onChange={(v) => setMeta({ name: v })}
              placeholder="Darkblade Pack"
            />
          </Field>

          <Field label="Author">
            <TextInput
              value={meta.author}
              onChange={(v) => setMeta({ author: v })}
              placeholder="coolmodder"
            />
          </Field>

          <Field label="Version" hint="MAJOR.MINOR.PATCH">
            <TextInput
              value={meta.version}
              onChange={(v) => setMeta({ version: v })}
              placeholder="1.0.0"
            />
            {versionBad && (
              <div className="sam-error text-sm mt-1">must be MAJOR.MINOR.PATCH — e.g. 1.0.0</div>
            )}
          </Field>

          <Field label="Framework Min Version" hint="Lowest S.A.M version this mod needs.">
            <TextInput
              value={meta.framework_min_version}
              onChange={(v) => setMeta({ framework_min_version: v })}
              placeholder="0.1.0"
            />
            {fwBad && (
              <div className="sam-error text-sm mt-1">must be MAJOR.MINOR.PATCH — e.g. 0.1.0</div>
            )}
          </Field>
        </div>

        <div className="sam-divider" />
        <Field label="Description" hint="A short blurb shown to players.">
          <textarea
            className="sam-input"
            rows={3}
            value={meta.description}
            onChange={(e) => setMeta({ description: e.target.value })}
            placeholder="Adds the Assassin class and a set of shadow-themed weapons."
          />
        </Field>
      </Panel>

      {/* -------------------------------------------- bundled classes/items */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Bundled Classes">
          <div className="space-y-2">
            {classes.length === 0 && (
              <div className="text-sm py-2 text-center" style={{ color: '#6b5a35' }}>
                No classes yet — forge one in the{' '}
                <span className="sam-mono" style={{ color: 'var(--color-parchment)' }}>/class-editor</span>.
              </div>
            )}
            {classes.map((def) => (
              <ItemRow
                key={def.id}
                icon="🛡"
                name={def.name}
                sub={def.id}
                onRemove={() => dispatch({ type: 'removeClass', id: def.id })}
              />
            ))}
          </div>
        </Panel>

        <Panel title="Bundled Items">
          <div className="space-y-2">
            {items.length === 0 && (
              <div className="text-sm py-2 text-center" style={{ color: '#6b5a35' }}>
                No items yet — forge one in the{' '}
                <span className="sam-mono" style={{ color: 'var(--color-parchment)' }}>/item-editor</span>.
              </div>
            )}
            {items.map((def) => (
              <ItemRow
                key={def.id}
                icon="⚔"
                name={def.name_identified}
                sub={def.id}
                onRemove={() => dispatch({ type: 'removeItem', id: def.id })}
              />
            ))}
          </div>
        </Panel>
      </div>

      {/* ---------------------------------------------------- export flow */}
      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && (
          <SavedNote>
            Exported <span className="sam-mono">{savedAs}.zip</span> — unzip into{' '}
            <span className="sam-mono">Barony/mods/{savedAs}/</span>
          </SavedNote>
        )}
        <GoldButton tone="green" onClick={exportMod} disabled={!canExport}>
          📦 Export Mod (.zip)
        </GoldButton>
      </div>

      {/* --------------------------------------------------- layout hint */}
      <Panel title="Zip Layout">
        <div className="text-sm space-y-2" style={{ color: 'var(--color-parchment)' }}>
          <p style={{ color: '#6b5a35' }}>
            Export bundles the manifest and every saved definition into one archive:
          </p>
          <pre className="sam-mono m-0 text-xs" style={{ color: '#9b8a5a' }}>
{`${meta.namespace || 'mynamespace'}.zip
├─ mod.json
├─ classes/*.json
└─ items/*.json`}
          </pre>
          <p style={{ color: '#6b5a35' }}>
            Unzip into <span className="sam-mono">Barony/mods/{meta.namespace || '<namespace>'}/</span>{' '}
            and S.A.M loads it from the game's mods menu.
          </p>
        </div>
      </Panel>
    </div>
  );
}
