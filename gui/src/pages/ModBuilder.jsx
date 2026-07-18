/*
 * Mod Builder — the final assembly bench.
 * Edits the mod.json manifest (identity, dependencies, version gating), lists
 * everything saved from the editors (classes/items/monsters/spells/patches +
 * behavior scripts), validates it all, then:
 *   • Export .zip / Import .zip       — round-trippable mod archive
 *   • Test in Barony                  — write straight into Barony/mods/ (Chromium)
 *   • Launch Barony + Read sam_log    — close the test loop
 *   • Edit / Clone / Remove           — manage any saved entry
 *   • Changes                         — JSON diff since the last export/import
 */
import { useMemo, useRef, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { NAMESPACE_PATTERN, VERSION_PATTERN, BARONY_VERSION_PATTERN, DEP_PATTERN } from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { buildModZip, downloadBlob, buildModFiles, contentPaths, buildManifest } from '@/lib/exportZip.js';
import { parseModZip } from '@/lib/importZip.js';
import { isFsSupported, isFilePickSupported, getModsDirHandle, writeModToDir, readSamLog } from '@/lib/fsAccess.js';
import { canonicalize, diffLines, collapseHunks, diffSummary } from '@/lib/jsonDiff.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, GoldButton, ItemRow, ErrorList, SavedNote } from '@/components/ui.jsx';

const BARONY_APPID = '371970';

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
  const { meta, classes, items, monsters, spells, effects, patches, scripts, assets, baseline, dispatch } = useMod();
  const navigate = useNavigate();
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const [notice, setNotice] = useState('');
  const [importReport, setImportReport] = useState([]);
  const [fallbackMsg, setFallbackMsg] = useState(false);
  const [showDiff, setShowDiff] = useState(false);
  const [logLines, setLogLines] = useState(null);
  const [depDraft, setDepDraft] = useState({ ns: '', kind: 'required', version: '' });
  const zipRef = useRef(null);

  const setMeta = (patch) => dispatch({ type: 'setMeta', patch });
  const mod = { meta, classes, items, monsters, spells, effects, patches, scripts, assets };

  const namespaceBad = meta.namespace !== '' && !NAMESPACE_PATTERN.test(meta.namespace);
  const versionBad = meta.version !== '' && !VERSION_PATTERN.test(meta.version);
  const fwBad = meta.framework_min_version !== '' && !VERSION_PATTERN.test(meta.framework_min_version);

  const paths = useMemo(
    () => contentPaths(classes, items, monsters, spells, patches, effects),
    [classes, items, monsters, spells, patches, effects]
  );

  const collectErrors = () => {
    const all = [];
    const push = (source, res) => {
      for (const e of res.errors) all.push({ path: `${source} ${e.path}`, message: e.message });
    };
    push('mod.json', validate('mod', buildManifest(meta, paths)));
    classes.forEach((c, i) => push(paths.classPaths[i], validate('class', c)));
    items.forEach((it, i) => push(paths.itemPaths[i], validate('item', it)));
    monsters.forEach((m, i) => push(paths.monsterPaths[i], validate('monster', m)));
    spells.forEach((s, i) => push(paths.spellPaths[i], validate('spell', s)));
    effects.forEach((e, i) => push(paths.effectPaths[i], validate('effect', e)));
    patches.forEach((p, i) => push(paths.patchPaths[i], validate('patch', p)));
    return all;
  };

  const canExport = meta.namespace.trim() !== '' && meta.name.trim() !== '';

  const edit = (kind, id, route) => { dispatch({ type: 'setEditing', kind, id }); navigate(route); };

  const exportMod = async () => {
    setSavedAs(''); setNotice(''); setFallbackMsg(false);
    const all = collectErrors();
    if (all.length) { setErrors(all); return; }
    setErrors([]);
    downloadBlob(await buildModZip(mod), `${meta.namespace}.zip`);
    setSavedAs(meta.namespace);
    dispatch({ type: 'setBaseline' });
  };

  const onImportFile = async (e) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    const hasContent = classes.length || items.length || monsters.length || spells.length || effects.length || patches.length || meta.name.trim() || meta.namespace.trim();
    if (hasContent && !window.confirm('Importing replaces the current mod in this session. Continue?')) return;
    setErrors([]); setNotice(''); setImportReport([]); setSavedAs('');
    try {
      const r = await parseModZip(file);
      dispatch({ type: 'loadMod', ...r });
      dispatch({ type: 'setBaseline' });
      setNotice(`Imported ${r.meta.name || r.meta.namespace || 'mod'}: ${r.classes.length} class(es), ${r.items.length} item(s), ${r.monsters.length} monster(s), ${r.spells.length} spell(s), ${r.effects.length} effect(s), ${r.patches.length} patch(es), ${Object.keys(r.scripts).length} script(s).`);
      setImportReport(r.report);
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
      downloadBlob(await buildModZip(mod), `${meta.namespace}.zip`);
      setFallbackMsg(true);
      dispatch({ type: 'setBaseline' });
      return;
    }
    try {
      const handle = await getModsDirHandle();
      const n = await writeModToDir(handle, meta.namespace, buildModFiles(mod));
      setNotice(`Wrote ${n} file(s) to mods/${meta.namespace}/ — launch Barony and enable the mod in the mods menu.`);
      dispatch({ type: 'setBaseline' });
    } catch (err) {
      if (err?.name === 'AbortError') return;
      setErrors([{ path: 'Test in Barony', message: err.message }]);
    }
  };

  const changeFolder = async () => {
    try { await getModsDirHandle({ forcePick: true }); setNotice('Barony/mods folder updated — Test in Barony will write there.'); }
    catch (err) { if (err?.name !== 'AbortError') setErrors([{ path: 'change folder', message: err.message }]); }
  };

  const viewLog = async () => {
    setErrors([]);
    try {
      const text = await readSamLog();
      const ns = meta.namespace.trim();
      const lines = text.split(/\r?\n/);
      const relevant = lines.filter((l) => /error|warn/i.test(l) || (ns && l.includes(ns)));
      setLogLines({ total: lines.length, shown: (relevant.length ? relevant : lines).slice(-80) });
    } catch (err) {
      if (err?.name === 'AbortError') return;
      setErrors([{ path: 'sam_log.txt', message: err.message }]);
    }
  };

  const clone = (def, saveType, collection) => {
    dispatch({ type: saveType, def: cloneWithNewId(def, new Set(collection.map((x) => x.id))) });
  };

  const addDep = () => {
    const ns = depDraft.ns.trim();
    if (!ns) return;
    const prefix = depDraft.kind === 'optional' ? '?' : depDraft.kind === 'incompatible' ? '!' : '';
    const str = `${prefix}${ns}${depDraft.version.trim() ? `@${depDraft.version.trim()}` : ''}`;
    if (!DEP_PATTERN.test(str)) { setErrors([{ path: 'dependency', message: `"${str}" is not a valid dependency (lowercase namespace, optional @x.y.z).` }]); return; }
    setErrors([]);
    if (!meta.dependencies.includes(str)) setMeta({ dependencies: [...meta.dependencies, str] });
    setDepDraft({ ns: '', kind: 'required', version: '' });
  };
  const removeDep = (d) => setMeta({ dependencies: meta.dependencies.filter((x) => x !== d) });

  // --- diff since baseline ---
  const diffRows = useMemo(() => {
    if (!baseline) return null;
    const a = canonicalize(baseline);
    const b = canonicalize({ meta, classes, items, monsters, spells, effects, patches });
    return collapseHunks(diffLines(a, b));
  }, [baseline, meta, classes, items, monsters, spells, effects, patches]);
  const summary = diffRows ? diffSummary(diffRows) : null;

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* ------------------------------------------------ manifest */}
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
          <textarea className="sam-input" rows={2} value={meta.description} onChange={(e) => setMeta({ description: e.target.value })} placeholder="Adds the Assassin class and a set of shadow-themed weapons." />
        </Field>
      </Panel>

      {/* ------------------------------------------ dependencies + compat */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Dependencies">
          <div className="flex flex-wrap gap-2 mb-3 min-h-8">
            {meta.dependencies.length === 0 && <span className="text-sm" style={{ color: '#6b5a35' }}>No dependencies. Add other S.A.M mods this one needs, integrates with, or conflicts with.</span>}
            {meta.dependencies.map((d) => (
              <span key={d} className="sam-well px-2 py-1 text-sm inline-flex items-center gap-2" style={{ color: 'var(--color-parchment)' }}>
                {d.startsWith('!') ? '⛔' : d.startsWith('?') ? '◇' : '◆'} <span className="sam-mono">{d}</span>
                <button type="button" className="sam-step sam-remove" style={{ width: 18, height: 18, fontSize: '0.7rem' }} onClick={() => removeDep(d)} aria-label={`remove ${d}`}>✕</button>
              </span>
            ))}
          </div>
          <div className="grid grid-cols-[1fr_9rem_7rem_auto] gap-2 items-center">
            <TextInput value={depDraft.ns} onChange={(v) => setDepDraft((p) => ({ ...p, ns: v }))} placeholder="other_mod" />
            <select className="sam-input" value={depDraft.kind} onChange={(e) => setDepDraft((p) => ({ ...p, kind: e.target.value }))}>
              <option value="required">required</option>
              <option value="optional">optional</option>
              <option value="incompatible">incompatible</option>
            </select>
            <TextInput value={depDraft.version} onChange={(v) => setDepDraft((p) => ({ ...p, version: v }))} placeholder="min ver" />
            <GoldButton onClick={addDep}>Add</GoldButton>
          </div>
        </Panel>

        <Panel title="Compatibility (optional)">
          <div className="grid grid-cols-2 gap-3">
            <Field label="Framework max version"><TextInput value={meta.framework_max_version} onChange={(v) => setMeta({ framework_max_version: v })} placeholder="e.g. 0.9.2" /></Field>
            <Field label="Barony min version"><TextInput value={meta.barony_min_version} onChange={(v) => setMeta({ barony_min_version: v })} placeholder="e.g. 5.0.0" /></Field>
            <Field label="Barony max version"><TextInput value={meta.barony_max_version} onChange={(v) => setMeta({ barony_max_version: v })} placeholder="e.g. 5.9.9" /></Field>
            <Field label="Incompatible Barony ver" hint="Only field that hard-blocks."><TextInput value={meta.incompatible_with_barony_version} onChange={(v) => setMeta({ incompatible_with_barony_version: v })} placeholder="rare" /></Field>
          </div>
          {[meta.framework_max_version, meta.barony_min_version, meta.barony_max_version, meta.incompatible_with_barony_version]
            .some((v) => v && !(v === meta.framework_max_version ? VERSION_PATTERN : BARONY_VERSION_PATTERN).test(v)) && (
            <div className="sam-error text-sm mt-2">version fields must be MAJOR.MINOR.PATCH (Barony fields may start with "v").</div>
          )}
        </Panel>
      </div>

      {/* -------------------------------------------- bundled content */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4 items-start">
        <Panel title="Bundled Classes">
          <div className="space-y-2">
            {classes.length === 0 && <EmptyHint where="/class-editor" what="classes" />}
            {classes.map((def) => (
              <ItemRow key={def.id} icon={scripts[def.id] ? '🛡📜' : '🛡'} name={def.name} sub={`${def.id}${scripts[def.id] ? ` · ${scripts[def.id].lang} script` : ''}`}
                onEdit={() => edit('class', def.id, '/class-editor')}
                onClone={() => clone(def, 'saveClass', classes)}
                onRemove={() => dispatch({ type: 'removeClass', id: def.id })} />
            ))}
          </div>
        </Panel>
        <Panel title="Bundled Items">
          <div className="space-y-2">
            {items.length === 0 && <EmptyHint where="/item-editor" what="items" />}
            {items.map((def) => (
              <ItemRow key={def.id} icon="⚔" name={def.name_identified} sub={def.id}
                onEdit={() => edit('item', def.id, '/item-editor')}
                onClone={() => clone(def, 'saveItem', items)}
                onRemove={() => dispatch({ type: 'removeItem', id: def.id })} />
            ))}
          </div>
        </Panel>
        <Panel title="Bundled Monsters">
          <div className="space-y-2">
            {monsters.length === 0 && <EmptyHint where="/monster-editor" what="monsters" />}
            {monsters.map((def) => (
              <ItemRow key={def.id} icon="👹" name={def.name} sub={def.id}
                onEdit={() => edit('monster', def.id, '/monster-editor')}
                onClone={() => clone(def, 'saveMonster', monsters)}
                onRemove={() => dispatch({ type: 'removeMonster', id: def.id })} />
            ))}
          </div>
        </Panel>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4 items-start">
        <Panel title="Bundled Spells">
          <div className="space-y-2">
            {spells.length === 0 && <EmptyHint where="/spell-editor" what="spells" />}
            {spells.map((def) => (
              <ItemRow key={def.id} icon="✨" name={def.name} sub={`${def.id} · ${def.payload}`}
                onEdit={() => edit('spell', def.id, '/spell-editor')}
                onClone={() => clone(def, 'saveSpell', spells)}
                onRemove={() => dispatch({ type: 'removeSpell', id: def.id })} />
            ))}
          </div>
        </Panel>
        <Panel title="Bundled Effects">
          <div className="space-y-2">
            {effects.length === 0 && <EmptyHint where="/effect-editor" what="effects" />}
            {effects.map((def) => (
              <ItemRow key={def.id} icon="🌀" name={def.name} sub={effectSub(def)}
                onEdit={() => edit('effect', def.id, '/effect-editor')}
                onClone={() => clone(def, 'saveEffect', effects)}
                onRemove={() => dispatch({ type: 'removeEffect', id: def.id })} />
            ))}
          </div>
        </Panel>
        <Panel title="Bundled Patches">
          <div className="space-y-2">
            {patches.length === 0 && <EmptyHint where="/patch-editor" what="patches" />}
            {patches.map((def) => (
              <ItemRow key={def.target} icon="🧩" name={def.target} sub={`${def.operations.length} operation(s)`}
                onEdit={() => edit('patch', def.target, '/patch-editor')}
                onRemove={() => dispatch({ type: 'removePatch', target: def.target })} />
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
      {savedAs && <SavedNote>Exported <span className="sam-mono">{savedAs}.zip</span> — it holds a <span className="sam-mono">{savedAs}/</span> folder; unzip it straight into <span className="sam-mono">Barony/mods/</span></SavedNote>}
      {fallbackMsg && (
        <div className="sam-well p-3 text-sm" style={{ color: 'var(--color-parchment)' }}>
          Your browser can't write files directly, so the mod was downloaded as a zip. It already holds a{' '}
          <span className="sam-mono">{meta.namespace}/</span> folder — unzip it into <span className="sam-mono">Barony/mods/</span> so you get{' '}
          <span className="sam-mono">Barony/mods/{meta.namespace}/mod.json</span>, then launch Barony. (Direct "Test in Barony" needs Chromium — Chrome, Edge.)
        </div>
      )}

      <input ref={zipRef} type="file" accept=".zip,application/zip" className="hidden" onChange={onImportFile} />
      <div className="flex flex-wrap items-center justify-end gap-3">
        <a className="sam-btn" href={`steam://run/${BARONY_APPID}`} title="Launch Barony via Steam">▶ Launch Barony</a>
        {isFilePickSupported && <GoldButton onClick={viewLog}>📄 Read sam_log.txt</GoldButton>}
        <GoldButton onClick={() => zipRef.current?.click()}>📥 Import Mod (.zip)</GoldButton>
        <GoldButton onClick={testInBarony} disabled={!canExport}>{isFsSupported ? '⚔ Test in Barony' : '⚔ Test in Barony (download)'}</GoldButton>
        <GoldButton tone="green" onClick={exportMod} disabled={!canExport}>📦 Export Mod (.zip)</GoldButton>
      </div>
      {isFsSupported && (
        <div className="text-right text-xs" style={{ color: '#6b5a35' }}>
          "Test in Barony" writes <span className="sam-mono">mods/{meta.namespace || '<namespace>'}/</span> into a folder you pick once.
          {' '}<button type="button" className="underline" style={{ color: '#8a6d2e' }} onClick={changeFolder}>change folder</button>
        </div>
      )}

      {logLines && (
        <Panel title={`sam_log.txt — ${logLines.shown.length} of ${logLines.total} lines (errors/warnings + this mod)`}>
          <pre className="sam-mono m-0 p-3 overflow-x-auto text-xs sam-well" style={{ maxHeight: 320 }}>
            {logLines.shown.map((l, i) => (
              <div key={i} style={{ color: /error/i.test(l) ? '#e07a6a' : /warn/i.test(l) ? '#d4a84b' : '#9b8a5a' }}>{l || ' '}</div>
            ))}
          </pre>
          <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>
            Tip: sam_log.txt lives next to your Barony save data. <button type="button" className="underline" style={{ color: '#8a6d2e' }} onClick={() => readSamLog({ forcePick: true }).then(viewLog).catch(() => {})}>pick a different file</button>
          </div>
        </Panel>
      )}

      {/* ------------------------------------------------------- changes */}
      <Panel title="Changes Since Last Export / Import">
        {!baseline ? (
          <div className="text-sm" style={{ color: '#6b5a35' }}>Export or import once to set a baseline — then this panel shows exactly what you've changed.</div>
        ) : (
          <>
            <div className="flex items-center gap-3 mb-2">
              <GoldButton onClick={() => setShowDiff((s) => !s)}>{showDiff ? '▾ Hide changes' : '▸ Show changes'}</GoldButton>
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
├─ classes/*.json   classes/*.lua|js|ts   (behavior scripts)
├─ items/*.json
├─ monsters/*.json
├─ spells/*.json
├─ effects/*.json
├─ patches/*.json
└─ portraits/*.png  (uploaded art)`}
        </pre>
        <p className="mt-2 text-sm" style={{ color: '#6b5a35' }}>
          Unzip into <span className="sam-mono">Barony/mods/{meta.namespace || '<namespace>'}/</span> and S.A.M loads it from the mods menu.
        </p>
      </Panel>
    </div>
  );
}

/** One-line summary of what an effect does, for the Mod Builder row. */
function effectSub(def) {
  const bits = [];
  const sm = def.stat_modifiers ?? {};
  for (const k of ['STR', 'DEX', 'CON', 'INT', 'PER', 'CHR']) {
    if (sm[k]) bits.push(`${sm[k] > 0 ? '+' : ''}${sm[k]} ${k}`);
  }
  if (def.move_speed_mult && def.move_speed_mult !== 1) bits.push(`×${def.move_speed_mult} spd`);
  if (def.hp_per_second) bits.push(`${def.hp_per_second > 0 ? '+' : ''}${def.hp_per_second} HP/s`);
  if (def.mp_per_second) bits.push(`${def.mp_per_second > 0 ? '+' : ''}${def.mp_per_second} MP/s`);
  return bits.length ? `${def.id} · ${bits.join(', ')}` : def.id;
}

function EmptyHint({ where, what }) {
  return (
    <div className="text-sm py-2 text-center" style={{ color: '#6b5a35' }}>
      No {what} yet — forge one in the{' '}
      <span className="sam-mono" style={{ color: 'var(--color-parchment)' }}>{where}</span>.
    </div>
  );
}
