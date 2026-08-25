import { HashRouter, Routes, Route, NavLink, Navigate } from 'react-router-dom';
import { ModProvider } from '@/state/ModContext.jsx';
import StartHere from '@/pages/StartHere.jsx';
import Dashboard from '@/pages/Dashboard.jsx';
import ClassEditor from '@/pages/ClassEditor.jsx';
import ItemEditor from '@/pages/ItemEditor.jsx';
import MonsterEditor from '@/pages/MonsterEditor.jsx';
import SpellEditor from '@/pages/SpellEditor.jsx';
import EffectEditor from '@/pages/EffectEditor.jsx';
import RaceEditor from '@/pages/RaceEditor.jsx';
import SoundEditor from '@/pages/SoundEditor.jsx';
import ModelEditor from '@/pages/ModelEditor.jsx';
import RecipeEditor from '@/pages/RecipeEditor.jsx';
import PatchEditor from '@/pages/PatchEditor.jsx';
import ModBuilder from '@/pages/ModBuilder.jsx';
import Validator from '@/pages/Validator.jsx';
import ApiReference from '@/pages/ApiReference.jsx';

// Fifteen destinations in one flat list is a scanning problem, not a styling one: every
// row looks equally likely, so finding the Race Editor means reading all fifteen labels.
// Grouped by what you are DOING, the list is read as six short lists, and the heading
// usually settles it before you read a single row.
const NAV_GROUPS = [
  ['Start', [
    { to: '/start', icon: '🧭', label: 'Start Here' },
    { to: '/dashboard', icon: '🏰', label: 'Dashboard' },
  ]],
  ['Content', [
    { to: '/class-editor', icon: '🛡', label: 'Class Editor' },
    { to: '/item-editor', icon: '⚔', label: 'Item Editor' },
    { to: '/monster-editor', icon: '👹', label: 'Monster Editor' },
    { to: '/race-editor', icon: '🧬', label: 'Race Editor' },
  ]],
  ['Magic', [
    { to: '/spell-editor', icon: '✨', label: 'Spell Editor' },
    { to: '/effect-editor', icon: '🌀', label: 'Effect Editor' },
  ]],
  ['Assets', [
    { to: '/sound-editor', icon: '🔊', label: 'Sound Editor' },
    { to: '/model-editor', icon: '🧊', label: 'Model Editor' },
  ]],
  ['Tweaks', [
    { to: '/recipe-editor', icon: '🔧', label: 'Recipe Editor' },
    { to: '/patch-editor', icon: '🧩', label: 'Patch Editor' },
  ]],
  ['Ship', [
    { to: '/mod-builder', icon: '📦', label: 'Mod Builder' },
    { to: '/validator', icon: '📜', label: 'Validator' },
    { to: '/api-reference', icon: '📖', label: 'API Reference' },
  ]],
];

function Banner() {
  return (
    <header
      className="sam-panel sam-banner mx-3 mt-3 mb-2 py-3 text-center"
      style={{ borderRadius: 6 }}
    >
      <h1 className="sam-title text-4xl m-0" style={{ fontWeight: 600 }}>
        S.A.M Framework
      </h1>
      <div className="sam-subtitle text-sm mt-1.5">Support All Mods</div>
    </header>
  );
}

function Sidebar() {
  return (
    <nav className="w-52 shrink-0 px-3 py-4">
      {NAV_GROUPS.map(([group, items]) => (
        <div key={group} className="sam-nav-group">
          <div className="sam-nav-group-label">{group}</div>
          {items.map((n) => (
            <NavLink
              key={n.to}
              to={n.to}
              className={({ isActive }) => `sam-nav-item ${isActive ? 'active' : ''}`}
            >
              <span aria-hidden>{n.icon}</span>
              <span>{n.label}</span>
            </NavLink>
          ))}
        </div>
      ))}
    </nav>
  );
}

export default function App() {
  return (
    <ModProvider>
      <HashRouter>
        <div className="min-h-full flex flex-col">
          <Banner />
          <div className="flex flex-1 min-h-0">
            <Sidebar />
            <main className="flex-1 min-w-0 px-3 pb-6 overflow-y-auto">
              <Routes>
                <Route path="/" element={<Navigate to="/start" replace />} />
                <Route path="/start" element={<StartHere />} />
                <Route path="/dashboard" element={<Dashboard />} />
                <Route path="/class-editor" element={<ClassEditor />} />
                <Route path="/item-editor" element={<ItemEditor />} />
                <Route path="/monster-editor" element={<MonsterEditor />} />
                <Route path="/spell-editor" element={<SpellEditor />} />
                <Route path="/effect-editor" element={<EffectEditor />} />
                <Route path="/race-editor" element={<RaceEditor />} />
                <Route path="/sound-editor" element={<SoundEditor />} />
                <Route path="/model-editor" element={<ModelEditor />} />
                <Route path="/recipe-editor" element={<RecipeEditor />} />
                <Route path="/patch-editor" element={<PatchEditor />} />
                <Route path="/mod-builder" element={<ModBuilder />} />
                <Route path="/validator" element={<Validator />} />
                <Route path="/api-reference" element={<ApiReference />} />
              </Routes>
            </main>
          </div>
        </div>
      </HashRouter>
    </ModProvider>
  );
}
