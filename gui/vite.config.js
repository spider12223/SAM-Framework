import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));

// The three JSON schemas live one level up (SAM-Framework/schemas/) and are the
// single source of truth: the app derives every enum list (item types, skills,
// categories, slots...) from them at runtime — nothing is hardcoded here.
//
// base: the site is served from a GitHub Pages *project* path
// (https://<user>.github.io/SAM-Framework/), so production asset URLs must be
// prefixed with '/SAM-Framework/'. Dev server stays at '/'. Routing is
// hash-based (HashRouter), which is Pages-safe on its own.
export default defineConfig(({ command }) => ({
  base: command === 'build' ? '/SAM-Framework/' : '/',
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      '@schemas': path.resolve(here, '../schemas'),
      '@': path.resolve(here, 'src'),
    },
  },
  server: {
    fs: {
      // allow importing ../schemas/*.json from outside the gui/ root
      allow: [path.resolve(here, '..')],
    },
  },
}));
