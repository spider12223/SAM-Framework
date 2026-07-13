/*
 * Mod Builder — the final assembly bench.
 * Edits the mod.json manifest, lists everything saved from the editors,
 * validates it all against the schemas, then:
 *   • Export .zip           — drop-in mod archive
 *   • Import .zip           — load an existing mod back in to edit
 *   • Test in Barony        — write straight into Barony/mods/ (Chromium)
 *   • Clone                 — duplicate any saved class/item/monster
 *   • Changes               — JSON diff since the last export/import
 * Manifest shape comes from lib/exportZip so what we validate is what we ship.
 */
import { useMemo, useRef, useState } from 'react';
import { NAMESPACE_PATTERN, VERSION_PATTERN } from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import {
  buildModZip, downloadBlob, buildModFiles, contentPaths, buildManifest,
} from '@/lib/exportZip.js';
import { parseModZip } from '@/lib/importZip.js';
import { isFsSupported, getModsDirHandle, writeModToDir } from '@/lib/fsAccess.js';
import { canonicalize, diffLines, collapseHunks, diffSummary } from '@/lib/jsonDiff.js';
import { useMod } from '@/state/ModContext.jsx';
import {
  Panel, Field, TextInput, GoldButton, ItemRow, ErrorList, SavedNote,
} from '@/components/ui.jsx';

/** structuredClone with a fresh, de-duped id + "(copy)" name. */
function cloneWithNewId(def, existingIds) {
  const copy = structuredClone(def);
  const [ns, tail = 'copy'] = String(def.id ?? '').split(':');
  let n = 1;
  let id;
  do { id = `${ns}:${tail}_copy${n === 1 ? '' : n}`; n++; } while (existingIds.has(id));
  copy.id = id;
  if (copy.name) copy.name = `${copy.name} (copy)`;
  if (copy.name_identified) copy.name_identified = `${copy.name_identified} (copy)`;
  return copy;
}

export default function ModBuilder() {
  const { meta, classes, items, monsters, assets, baseline, dispatch } = useMod();
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const [notice, setNotice] = useState('');       // success line (export/import/test)
  const [importReport, setImportReport] = useState([]);
  const [fallbackMsg, setFallbackMsg] = useState(false);
  const [showDiff, setShowDiff] = useState(false);
  const zipRef = useRef(null);

  const setMeta = (patch) => dispatch({ type: 'setMeta', patch });

  const namespaceBad = meta.namespace !== '' && !NAMESPACE_PATTERN.test(meta.namespace);
  const versionBad = meta.version !== '' && !VERSION_PATTERN.test(meta.version);
  const fwBad = meta.framework_min_version !== '' && !VERSION_PATTERN.test(meta.framework_min_version);

  const { classPaths, itemPaths, monsterPaths } = useMemo(
    () => contentPaths(classes, items, monsters),
    [classes, items, monsters]
  );

  const collectErrors = () => {
    const all = [];
    const push = (source, res) => {
      for (const e of res.errors) all.push({ path: `${source} ${e.path}`, message: e.message });
    };
    push('mod.json', validate('mod', buildManifest(meta, classPaths, itemPaths, monsterPaths)));
    classes.forEach((c, i) => push(classPaths[i], validate('class', c)));
    items.forEach((it, i) => push(itemPaths[i], validate('item', it)));
    monsters.forEach((m, i) => push(monsterPaths[i], validate('monster', m)));
    return all;
  };

  const canExport = meta.namespace.trim() !== '' && meta.name.trim() !== '';

  const exportMod = async () => {
    setSavedAs(''); setNotice(''); setFallbackMsg(false);
    const all = collectErrors();
    if (all.length) { setErrors(all); return; }
    setErrors([]);
    const blob = await buildModZip(meta, classes, items, monsters, assets);
    downloadBlob(blob, `${meta.namespace}.zip`);
    setSavedAs(meta.namespace);
    dispatch({ type: 'setBaseline' });
  };

  const onImportFile = async (e) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    const hasContent = classes.length || items.length || monsters.length || meta.name.trim() || meta.namespace.trim();
    if (hasContent && !window.confirm('Importing replaces the current mod in this session. Continue?')) return;
    setErrors([]); setNotice(''); setImportReport([]); setSavedAs('');
    try {
      const { meta: m, classes: c, items: it, monsters: mo, assets: a, report } = await parseModZip(file);
      dispatch({ type: 'loadMod', meta: m, classes: c, items: it, monsters: mo, assets: a });
      dispatch({ type: 'setBaseline' });
      setNotice(`Imported ${m.name || m.namespace || 'mod'}: ${c.length} class(es), ${it.length} item(s), ${mo.length} monster(s), ${Object.keys(a).length} asset(s).`);
      setImportReport(report);
    } catch (err) {
      setErrors([{ path: 'import', message: err.message }]);
    }
  };

  const testInBarony = async () => {
    setSavedAs(''); setNotice(''); setFallbackMsg(false);
    const all = collectErrors();
    if (all.length) { setErrors(all); return; }
    setErrors([]);
    if (!isFsSupported) {
      const blob = await buildModZip(meta, classes, items, monsters, assets);
      downloadBlob(blob, `${meta.namespace}.zip`);
      setFallbackMsg(true);
      dispatch({ type: 'setBaseline' });
      return;
    }
    try {
      const handle = await getModsDirHandle();
      const files = buildModFiles(meta, classes, items, monsters, assets);
      const n = await writeModToDir(handle, meta.namespace, files);
      setNotice(`Wrote ${n} file(s) to mods/${meta.namespace}/ — launch Barony and enable the mod in the mods menu.`);
      dispatch({ type: 'setBaseline' });
    } catch (err) {
      if (err?.name === 'AbortError') return; // user cancelled the folder picker
      setErrors([{ path: 'Test in Barony', message: err.message }]);
    }
  };

  const changeFolder = async () => {
    try { await getModsDirHandle({ forcePick: true }); setNotice('Barony/mods folder updated — Test in Barony will write there.'); }
    catch (err) { if (err?.name !== 'AbortError') setErrors([{ path: 'change folder', message: err.message }]); }
  };

  const clone = (def, saveType, collection) => {
    const ids = new Set(collection.map((x) => x.id));
    dispatch({ type: saveType, def: cloneWithNewId(def, ids) });
  };

  // --- diff since baseline ---
  const diffRows = useMemo(() => {
    if (!baseline) return null;
    const a = canonicalize(baseline);
    const b = canonicalize({ meta, classes, items, monsters });
    return collapseHunks(diffLines(a, b));
  }, [baseline, meta, classes, items, monsters]);
  const summary = diffRows ? diffSummary(diffRows) : null;

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* ------------------------------------------------ manifest fields */}
      <Panel title="Mod Manifest">
        <div className="grid grid-cols-1 md:grid-cols-2 gap-3">
          <Field label="Namespace" hint="Prefix for every id — e.g. darkblade → darkblade:assassin">
            <TextInput value={meta.namespace} onChange={(v) => setMeta({ namespace: v })} placeholder="darkblade" />
            {namespaceBad && <div className="sam-error text-sm mt-1">lowercase letters, digits, _ — must start with a letter</div>}
          </Field>
          <Field label="Mod Name" hint="Shown to players in the mods menu.">
            <TextInput value={meta.name} onChange={(v) => setMeta({ name: v })} placeholder="Darkblade Pack" />
          </Field>
          <Field label="Author">
            <TextInput value={meta.author} onChange={(v) => setMeta({ author: v })} placeholder="coolmodder" />
          </Field>
          <Field label="Version" hint="MAJOR.MINOR.PATCH">
            <TextInput value={meta.version} onChange={(v) => setMeta({ version: v })} placeholder="1.0.0" />
            {versionBad && <div className="sam-error text-sm mt-1">must be MAJOR.MINOR.PATCH — e.g. 1.0.0</div>}
          </Field>
          <Field label="Framework Min Version" hint="Lowest S.A.M version this mod needs.">
            <TextInput value={meta.framework_min_version} onChange={(v) => setMeta({ framework_min_version: v })} placeholder="0.1.0" />
            {fwBad && <div className="sam-error text-sm mt-1">must be MAJOR.MINOR.PATCH — e.g. 0.1.0</div>}
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

      {/* -------------------------------------------- bundled content */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4 items-start">
        <Panel title="Bundled Classes">
          <div className="space-y-2">
            {classes.length === 0 && <EmptyHint where="/class-editor" what="classes" />}
            {classes.map((def) => (
              <ItemRow
                key={def.id} icon="🛡" name={def.name} sub={def.id}
                onClone={() => clone(def, 'saveClass', classes)}
                onRemove={() => dispatch({ type: 'removeClass', id: def.id })}
              />
            ))}
          </div>
        </Panel>
        <Panel title="Bundled Items">
          <div className="space-y-2">
            {items.length === 0 && <EmptyHint where="/item-editor" what="items" />}
            {items.map((def) => (
              <ItemRow
                key={def.id} icon="⚔" name={def.name_identified} sub={def.id}
                onClone={() => clone(def, 'saveItem', items)}
                onRemove={() => dispatch({ type: 'removeItem', id: def.id })}
              />
            ))}
          </div>
        </Panel>
        <Panel title="Bundled Monsters">
          <div className="space-y-2">
            {monsters.length === 0 && <EmptyHint where="/monster-editor" what="monsters" />}
            {monsters.map((def) => (
              <ItemRow
                key={def.id} icon="👹" name={def.name} sub={def.id}
                onClone={() => clone(def, 'saveMonster', monsters)}
                onRemove={() => dispatch({ type: 'removeMonster', id: def.id })}
              />
            ))}
          </div>
        </Panel>
      </div>

      {/* ---------------------------------------------------- action bar */}
      <ErrorList errors={errors} />
      {importReport.length > 0 && (
        <div className="sam-well p-3">
          <div className="sam-label mb-2" style={{ color: '#d4a84b' }}>Import notes ({importReport.length} skipped/flagged)</div>
          <ul className="space-y-1">
            {importReport.map((r, i) => (
              <li key={i} className="text-sm"><span className="sam-mono">{r.path}</span> <span style={{ color: 'var(--color-parchment)' }}>— {r.message}</span></li>
            ))}
          </ul>
        </div>
      )}
      {notice && <SavedNote>{notice}</SavedNote>}
      {savedAs && (
        <SavedNote>Exported <span className="sam-mono">{savedAs}.zip</span> — unzip into <span className="sam-mono">Barony/mods/{savedAs}/</span></SavedNote>
      )}
      {fallbackMsg && (
        <div className="sam-well p-3 text-sm" style={{ color: 'var(--color-parchment)' }}>
          Your browser can't write files directly, so the mod was downloaded as a zip.
          Unzip it so you get <span className="sam-mono">Barony/mods/{meta.namespace}/mod.json</span>, then launch Barony.
          {' '}(Direct "Test in Barony" needs a Chromium browser — Chrome, Edge.)
        </div>
      )}

      <input ref={zipRef} type="file" accept=".zip,application/zip" className="hidden" onChange={onImportFile} />
      <div className="flex flex-wrap items-center justify-end gap-3">
        <GoldButton onClick={() => zipRef.current?.click()}>📥 Import Mod (.zip)</GoldButton>
        <GoldButton onClick={testInBarony} disabled={!canExport}>
          {isFsSupported ? '⚔ Test in Barony' : '⚔ Test in Barony (download)'}
        </GoldButton>
        <GoldButton tone="green" onClick={exportMod} disabled={!canExport}>📦 Export Mod (.zip)</GoldButton>
      </div>
      {isFsSupported && (
        <div className="text-right text-xs" style={{ color: '#6b5a35' }}>
          "Test in Barony" writes <span className="sam-mono">mods/{meta.namespace || '<namespace>'}/</span> into a folder you pick once.
          {' '}<button type="button" className="underline" style={{ color: '#8a6d2e' }} onClick={changeFolder}>change folder</button>
        </div>
      )}

      {/* ------------------------------------------------------- changes */}
      <Panel title="Changes Since Last Export / Import">
        {!baseline ? (
          <div className="text-sm" style={{ color: '#6b5a35' }}>
            Export or import once to set a baseline — then this panel shows exactly what you've changed.
          </div>
        ) : (
          <>
            <div className="flex items-center gap-3 mb-2">
              <GoldButton onClick={() => setShowDiff((s) => !s)}>
                {showDiff ? '▾ Hide changes' : '▸ Show changes'}
              </GoldButton>
              {summary && (
                <span className="sam-mono text-sm">
                  {summary.added === 0 && summary.removed === 0
                    ? <span style={{ color: '#6b5a35' }}>no changes since baseline</span>
                    : <><span className="sam-diff-add-text">+{summary.added}</span>{' '}<span className="sam-diff-del-text">−{summary.removed}</span> lines</>}
                </span>
              )}
            </div>
            {showDiff && diffRows && (
              <pre className="sam-mono m-0 p-3 overflow-x-auto text-xs sam-well" style={{ maxHeight: 420 }}>
                {diffRows.map((r, i) => {
                  if (r.kind === 'gap') return <div key={i} className="sam-diff-ctx" style={{ textAlign: 'center' }}>⋯ {r.count} unchanged line{r.count === 1 ? '' : 's'} ⋯</div>;
                  const cls = r.kind === 'add' ? 'sam-diff-add' : r.kind === 'del' ? 'sam-diff-del' : 'sam-diff-ctx';
                  const sign = r.kind === 'add' ? '+' : r.kind === 'del' ? '−' : ' ';
                  return <div key={i} className={cls}>{sign} {r.text}</div>;
                })}
              </pre>
            )}
          </>
        )}
      </Panel>

      {/* --------------------------------------------------- layout hint */}
      <Panel title="Zip Layout">
        <pre className="sam-mono m-0 text-xs" style={{ color: '#9b8a5a' }}>
{`${meta.namespace || 'mynamespace'}.zip
├─ mod.json
├─ classes/*.json
├─ items/*.json
├─ monsters/*.json
└─ portraits/*.png  (uploaded art)`}
        </pre>
        <p className="mt-2 text-sm" style={{ color: '#6b5a35' }}>
          Unzip into <span className="sam-mono">Barony/mods/{meta.namespace || '<namespace>'}/</span> and S.A.M loads it from the mods menu.
        </p>
      </Panel>
    </div>
  );
}

function EmptyHint({ where, what }) {
  return (
    <div className="text-sm py-2 text-center" style={{ color: '#6b5a35' }}>
      No {what} yet — forge one in the{' '}
      <span className="sam-mono" style={{ color: 'var(--color-parchment)' }}>{where}</span>.
    </div>
  );
}
