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

register(
  `data:text/javascript,
   const SRC = ${JSON.stringify(SRC)};
   export function resolve(spec, ctx, next) {
     if (spec.startsWith('@/')) return next(SRC + spec.slice(2), ctx);
     return next(spec, ctx);
   }`,
  pathToFileURL('./'),
);
