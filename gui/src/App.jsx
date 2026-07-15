import { HashRouter, Routes, Route, NavLink, Navigate } from 'react-router-dom';
import { ModProvider } from '@/state/ModContext.jsx';
import Dashboard from '@/pages/Dashboard.jsx';
import ClassEditor from '@/pages/ClassEditor.jsx';
import ItemEditor from '@/pages/ItemEditor.jsx';
import MonsterEditor from '@/pages/MonsterEditor.jsx';
import SpellEditor from '@/pages/SpellEditor.jsx';
import PatchEditor from '@/pages/PatchEditor.jsx';
import ModBuilder from '@/pages/ModBuilder.jsx';
import Validator from '@/pages/Validator.jsx';
import ApiReference from '@/pages/ApiReference.jsx';

const NAV = [
  { to: '/dashboard', icon: '🏰', label: 'Dashboard' },
  { to: '/class-editor', icon: '🛡', label: 'Class Editor' },
  { to: '/item-editor', icon: '⚔', label: 'Item Editor' },
  { to: '/monster-editor', icon: '👹', label: 'Monster Editor' },
  { to: '/spell-editor', icon: '✨', label: 'Spell Editor' },
  { to: '/patch-editor', icon: '🧩', label: 'Patch Editor' },
  { to: '/mod-builder', icon: '📦', label: 'Mod Builder' },
  { to: '/validator', icon: '📜', label: 'Validator' },
  { to: '/api-reference', icon: '📖', label: 'API Reference' },
];

function Banner() {
  return (
    <header
      className="sam-panel mx-3 mt-3 mb-2 py-3 text-center"
      style={{ borderRadius: 6 }}
    >
      <h1 className="sam-title text-4xl m-0" style={{ fontWeight: 600 }}>
        S.A.M Framework
      </h1>
      <div className="sam-subtitle text-sm mt-1">— Support All Mods —</div>
    </header>
  );
}

function Sidebar() {
  return (
    <nav className="w-52 shrink-0 px-3 py-4 space-y-1">
      {NAV.map((n) => (
        <NavLink
          key={n.to}
          to={n.to}
          className={({ isActive }) => `sam-nav-item ${isActive ? 'active' : ''}`}
        >
          <span aria-hidden>{n.icon}</span>
          <span>{n.label}</span>
        </NavLink>
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
                <Route path="/" element={<Navigate to="/dashboard" replace />} />
                <Route path="/dashboard" element={<Dashboard />} />
                <Route path="/class-editor" element={<ClassEditor />} />
                <Route path="/item-editor" element={<ItemEditor />} />
                <Route path="/monster-editor" element={<MonsterEditor />} />
                <Route path="/spell-editor" element={<SpellEditor />} />
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
