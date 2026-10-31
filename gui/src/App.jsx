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
import MusicEditor from '@/pages/MusicEditor.jsx';
import ModelEditor from '@/pages/ModelEditor.jsx';
import RecipeEditor from '@/pages/RecipeEditor.jsx';
import PatchEditor from '@/pages/PatchEditor.jsx';
import ModBuilder from '@/pages/ModBuilder.jsx';
import Validator from '@/pages/Validator.jsx';
import ApiReference from '@/pages/ApiReference.jsx';
import { NAV_ICONS } from '@/data/navIcons.js';

// Fifteen destinations in one flat list is a scanning problem, not a styling one: every
// row looks equally likely, so finding the Race Editor means reading all fifteen labels.
// Grouped by what you are DOING, the list is read as six short lists, and the heading
// usually settles it before you read a single row.
const NAV_GROUPS = [
  ['Start', [
    { to: '/start', label: 'Start Here' },
    { to: '/dashboard', label: 'Dashboard' },
  ]],
  ['Content', [
    { to: '/class-editor', label: 'Class Editor' },
    { to: '/item-editor', label: 'Item Editor' },
    { to: '/monster-editor', label: 'Monster Editor' },
    { to: '/race-editor', label: 'Race Editor' },
  ]],
  ['Magic', [
    { to: '/spell-editor', label: 'Spell Editor' },
    { to: '/effect-editor', label: 'Effect Editor' },
  ]],
  ['Assets', [
    { to: '/sound-editor', label: 'Sound Editor' },
    { to: '/music-editor', label: 'Music Editor' },
    { to: '/model-editor', label: 'Model Editor' },
  ]],
  ['Tweaks', [
    { to: '/recipe-editor', label: 'Recipe Editor' },
    { to: '/patch-editor', label: 'Patch Editor' },
  ]],
  ['Ship', [
    { to: '/mod-builder', label: 'Mod Builder' },
    { to: '/validator', label: 'Validator' },
    { to: '/api-reference', label: 'API Reference' },
  ]],
];

// The top of Barony's Settings window: the name in a black plaque with a gem either side and the
// sapphire on its top edge (drawn by CSS), and a gold rail running out to the frame.
function TopBar() {
  return (
    <header className="sam-topbar">
      <span className="sam-rail" aria-hidden />
      <span className="sam-knot" aria-hidden />
      <span className="sam-gem sam-gem-ruby" aria-hidden />
      <div className="sam-titleplate">
        <h1 className="sam-title">S.A.M Framework</h1>
        <div className="sam-subtitle">Support All Mods</div>
      </div>
      <span className="sam-gem sam-gem-emerald" aria-hidden />
      <span className="sam-knot" aria-hidden />
      <span className="sam-rail" aria-hidden />
    </header>
  );
}

function Sidebar() {
  return (
    <nav className="sam-nav" aria-label="Editors">
      {NAV_GROUPS.map(([group, items]) => (
        <div key={group} className="sam-nav-group">
          <div className="sam-nav-group-label">{group}</div>
          {items.map((n) => (
            <NavLink
              key={n.to}
              to={n.to}
              className={({ isActive }) => `sam-nav-item ${isActive ? 'active' : ''}`}
            >
              <img className="sam-nav-icon" src={NAV_ICONS[n.to]} alt="" draggable={false} />
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
        <div className="sam-shell">
          <TopBar />
          <div className="sam-body">
            <Sidebar />
            <main className="sam-main">
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
                <Route path="/music-editor" element={<MusicEditor />} />
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
