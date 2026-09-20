/*
 * Teach plain Node the `@/` alias that vite.config.js gives the app, so the test can import
 * the REAL codegen.js rather than a copy. A copy would drift, and a drifting test of a
 * drift-prevention tool is worse than no test.
 *
 *   node --import ./test/alias-loader.mjs test/codegen.test.mjs
 */
import { register } from 'node:module';
import { pathToFileURL } from 'node:url';

const SRC = new URL('../src/', import.meta.url).href;
const SCHEMAS = new URL('../../schemas/', import.meta.url).href; // matches vite's @schemas alias

//
// It also loads a bare `import x from './a.json'` the way Vite does. Plain Node insists on
// `with { type: 'json' }`, which Vite code never writes, so without this nothing that reaches
// the schemas (validate.js, and through it importZip.js) could be tested -- including the
// check that an exported mod.json passes the builder's own validator.
register(
  `data:text/javascript,
   import { readFile } from 'node:fs/promises';
   const SRC = ${JSON.stringify(SRC)};
   const SCHEMAS = ${JSON.stringify(SCHEMAS)};
   export function resolve(spec, ctx, next) {
     if (spec.startsWith('@schemas/')) return next(SCHEMAS + spec.slice('@schemas/'.length), ctx);
     if (spec.startsWith('@/')) return next(SRC + spec.slice(2), ctx);
     return next(spec, ctx);
   }
   export async function load(url, ctx, next) {
     if (url.startsWith('file:') && url.endsWith('.json') && !(ctx.importAttributes && ctx.importAttributes.type)) {
       const text = await readFile(new URL(url), 'utf8');
       return { format: 'module', source: 'export default ' + text + ';', shortCircuit: true };
     }
     return next(url, ctx);
   }`,
  pathToFileURL('./'),
);
