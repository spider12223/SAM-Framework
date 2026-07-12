/*
 * Validator — a JSON validation console for modders.
 * Paste or upload a mod/class/item JSON, pick (or auto-detect) its schema,
 * and validate it against the real S.A.M draft-07 schemas via lib/validate.
 * Nothing here touches session mod state — it's a pure check bench.
 */
import { useRef, useState } from 'react';
import { SCHEMA_KINDS, validate } from '@/lib/validate.js';
import {
  Panel, Field, Select, GoldButton, ErrorList, SavedNote,
} from '@/components/ui.jsx';

/** Friendly labels for the three schema kinds. */
const SCHEMA_LABELS = {
  mod: 'Mod manifest (mod.json)',
  class: 'Class definition',
  item: 'Item definition',
};

/** Which schema file each kind validates against (for the success note). */
const SCHEMA_FILE = {
  mod: 'schemas/mod.schema.json',
  class: 'schemas/class.schema.json',
  item: 'schemas/item.schema.json',
};

/** Guess the schema kind from an object's shape (see task heuristics). */
function detectKind(data) {
  if (!data || typeof data !== 'object' || Array.isArray(data)) return null;
  if ('namespace' in data && 'framework_min_version' in data) return 'mod';
  if ('stats' in data) return 'class';
  if ('name_identified' in data) return 'item';
  return null;
}

/** Last token of a JSON pointer: "/stats/STR" -> "STR". */
function lastToken(path) {
  if (!path) return '';
  const tail = path.split('/').pop();
  return tail && tail !== path ? tail : ''; // ignore non-pointer paths like "(parse)"
}

/** Split raw text on `token`, wrapping every match in a <mark>. */
function highlight(raw, token) {
  if (!token) return raw;
  const parts = raw.split(token);
  if (parts.length === 1) return raw; // token not present — skip highlight
  const markStyle = { background: 'rgba(160,51,39,0.35)', color: '#e07a6a' };
  const out = [];
  parts.forEach((part, i) => {
    if (i > 0) out.push(<mark key={`m${i}`} style={markStyle}>{token}</mark>);
    out.push(<span key={`s${i}`}>{part}</span>);
  });
  return out;
}

export default function Validator() {
  const [text, setText] = useState('');
  const [schemaKind, setSchemaKind] = useState('auto'); // 'auto' | 'mod' | 'class' | 'item'
  const [report, setReport] = useState(null); // { errors, success } | null
  const fileRef = useRef(null);

  const schemaOptions = [
    { value: 'auto', label: 'Auto-detect (by shape)' },
    ...SCHEMA_KINDS.map((k) => ({ value: k, label: SCHEMA_LABELS[k] })),
  ];

  const onFile = (e) => {
    const file = e.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => setText(String(reader.result ?? ''));
    reader.readAsText(file);
    e.target.value = ''; // allow re-picking the same file
  };

  const runValidate = () => {
    setReport(null);

    // 1. parse
    let data;
    try {
      data = JSON.parse(text);
    } catch (err) {
      setReport({ errors: [{ path: '(parse)', message: err.message }], success: null });
      return;
    }

    // 2. resolve the schema kind (explicit or detected)
    let kind = schemaKind;
    if (kind === 'auto') {
      kind = detectKind(data);
      if (!kind) {
        setReport({
          errors: [{
            path: '(auto-detect)',
            message: 'Could not tell mod / class / item from the shape — pick a schema above and validate again.',
          }],
          success: null,
        });
        return;
      }
    }

    // 3. validate
    const result = validate(kind, data);
    if (result.valid) {
      const count = data && typeof data === 'object' && !Array.isArray(data)
        ? Object.keys(data).length
        : 0;
      setReport({ errors: [], success: { kind, count, file: SCHEMA_FILE[kind] } });
    } else {
      setReport({ errors: result.errors, success: null });
    }
  };

  const errors = report?.errors ?? [];
  const success = report?.success ?? null;
  const showSource = errors.length > 0 && text.trim().length > 0;
  const token = showSource ? lastToken(errors[0]?.path) : '';

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* ------------------------------------------------------ load JSON */}
      <Panel title="Load JSON">
        <input
          ref={fileRef}
          type="file"
          accept=".json,application/json"
          className="hidden"
          onChange={onFile}
        />
        <div className="flex items-center gap-3 mb-3">
          <GoldButton onClick={() => fileRef.current?.click()}>📂 Upload .json</GoldButton>
          <span className="text-xs" style={{ color: '#6b5a35' }}>
            …or paste the JSON below. Uploading fills the box.
          </span>
        </div>
        <textarea
          className="sam-input sam-mono"
          rows={14}
          spellCheck={false}
          value={text}
          onChange={(e) => setText(e.target.value)}
          placeholder={'{\n  "id": "mymod:warden",\n  "name": "Warden",\n  "stats": { "STR": 12 }\n}'}
          aria-label="JSON to validate"
        />
      </Panel>

      {/* -------------------------------------------------------- schema */}
      <Panel title="Schema">
        <div className="grid grid-cols-1 sm:grid-cols-[1fr_auto] gap-3 items-end">
          <Field
            label="Validate against"
            hint="Auto-detect reads the shape: namespace + framework_min_version → mod, stats → class, name_identified → item."
          >
            <Select value={schemaKind} onChange={setSchemaKind} options={schemaOptions} />
          </Field>
          <GoldButton onClick={runValidate} disabled={!text.trim()}>
            ✓ Validate
          </GoldButton>
        </div>
      </Panel>

      {/* -------------------------------------------------------- results */}
      {success && (
        <SavedNote>
          ✓ Valid {success.kind} JSON — {success.count} field{success.count === 1 ? '' : 's'} checked.
          {' '}Validated against <span className="sam-mono">{success.file}</span>.
        </SavedNote>
      )}
      <ErrorList errors={errors} />

      {showSource && (
        <Panel title="Source" bodyClassName="p-0">
          <div className="px-4 pt-3 text-xs" style={{ color: '#6b5a35' }}>
            {token
              ? <>Highlighting <span className="sam-mono sam-error">{token}</span> — the field at the first error.</>
              : 'The JSON that failed to validate.'}
          </div>
          <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>
            {highlight(text, token)}
          </pre>
        </Panel>
      )}

      {/* ---------------------------------------------------------- hint */}
      <Panel title="Schemas It Checks">
        <div className="text-sm space-y-1" style={{ color: 'var(--color-parchment)' }}>
          <div>Validation runs against the three source-of-truth schemas that live in <span className="sam-mono">SAM-Framework/schemas/</span>:</div>
          <ul className="space-y-1 mt-2">
            <li><span className="sam-mono">mod.schema.json</span> <span style={{ color: '#6b5a35' }}>— the {SCHEMA_LABELS.mod.toLowerCase()}</span></li>
            <li><span className="sam-mono">class.schema.json</span> <span style={{ color: '#6b5a35' }}>— a {SCHEMA_LABELS.class.toLowerCase()}</span></li>
            <li><span className="sam-mono">item.schema.json</span> <span style={{ color: '#6b5a35' }}>— an {SCHEMA_LABELS.item.toLowerCase()}</span></li>
          </ul>
        </div>
      </Panel>
    </div>
  );
}
