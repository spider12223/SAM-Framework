/*
 * Start Here: the plain-English walkthrough for someone who has never used
 * S.A.M. Takes them from a fresh install all the way to playing their own mod.
 * Written to be read by a person, not skimmed for keywords: short sentences,
 * one idea per line, no jargon. This is the app's default landing page.
 */
import { useNavigate } from 'react-router-dom';
import { Panel, GoldButton } from '@/components/ui.jsx';

const WORKSHOP_URL = 'https://steamcommunity.com/sharedfiles/filedetails/?id=3763844472';
const INSTALLER_URL = 'https://github.com/spider12223/SAM-Framework/releases/latest';

/** A numbered step card: gold number badge on the left, title and text on the right. */
function Step({ n, title, children }) {
  return (
    <div className="sam-well p-4 flex gap-4">
      <span
        className="shrink-0"
        aria-hidden
        style={{
          display: 'inline-flex',
          alignItems: 'center',
          justifyContent: 'center',
          width: '2.2rem',
          height: '2.2rem',
          borderRadius: '50%',
          background: 'rgba(212,168,75,0.14)',
          border: '1px solid var(--color-gold-dim)',
          color: 'var(--color-gold-bright)',
          fontWeight: 600,
          fontSize: '1.1rem',
        }}
      >
        {n}
      </span>
      <div className="min-w-0">
        <div className="mb-1.5" style={{ color: 'var(--color-gold)', fontWeight: 600, fontSize: '1.05rem' }}>
          {title}
        </div>
        <div className="space-y-2 text-sm leading-relaxed" style={{ color: 'var(--color-parchment)' }}>
          {children}
        </div>
      </div>
    </div>
  );
}

export default function StartHere() {
  const navigate = useNavigate();

  return (
    <div className="space-y-4 max-w-4xl mx-auto">
      {/* ---------------------------------------------------------- intro */}
      <Panel title="Start Here">
        <p className="m-0" style={{ color: 'var(--color-parchment)' }}>
          <strong style={{ color: 'var(--color-gold)' }}>S.A.M lets you add your own classes, items,
          monsters, and more to Barony without writing any code.</strong>{' '}
          You build what you want in this tool, it hands you a mod file, and you drop that file into
          the game. That is the whole loop.
        </p>
        <p className="mt-2 mb-0" style={{ color: 'var(--color-parchment)' }}>
          Below is the full path, from a fresh install to playing your own mod. The first time through
          takes about ten minutes.
        </p>
      </Panel>

      {/* --------------------------------------------------- the four steps */}
      <Panel title="From Install to Playing">
        <div className="space-y-3">
          <Step n={1} title="Install S.A.M (you only do this once)">
            <p>
              S.A.M is the engine your mods run on. You install it one time. Anyone who wants to play
              what you make installs it too.
            </p>
            <p>
              You need Barony on Steam, and you need to have opened the game at least once so its files
              are fully there.
            </p>
            <p>
              Download the installer below and run it. It finds your Barony and puts everything in the
              right place. That is the whole install.
            </p>
            <p>
              One thing to know: subscribing on the Steam Workshop does not set it up by itself. Steam
              can share mod content, but it cannot replace the game's own program, so you still have to
              run the installer to finish (or copy the patched <span className="sam-mono">barony.exe</span>{' '}
              into your Barony folder by hand). The installer is the simplest way to do it.
            </p>
            <div className="mt-1 flex flex-wrap gap-3">
              <a className="sam-btn" href={INSTALLER_URL} target="_blank" rel="noreferrer">⬇ Download Installer</a>
              <a className="sam-btn" href={WORKSHOP_URL} target="_blank" rel="noreferrer">🎮 S.A.M on Steam Workshop</a>
            </div>
          </Step>

          <Step n={2} title="Build your content in the editors">
            <p>
              Pick an editor from the sidebar on the left. The Class Editor makes a playable class. The
              Item Editor makes a weapon or item. There are editors for monsters, spells, effects,
              races, and sounds too.
            </p>
            <p>
              Fill in the fields. Give it a name, set the numbers, choose the starting gear. Everything
              updates as you type, and there is nothing to code.
            </p>
            <p>
              Save each thing you make. It gets added to the mod you are building.
            </p>
            <div className="mt-1 flex flex-wrap gap-3">
              <GoldButton onClick={() => navigate('/class-editor')}>🛡 Open Class Editor</GoldButton>
              <GoldButton onClick={() => navigate('/item-editor')}>⚔ Open Item Editor</GoldButton>
            </div>
          </Step>

          <Step n={3} title="Name it and bundle it">
            <p>
              Open the Mod Builder. Give your mod a name and a short id called a namespace. Keep the
              namespace lowercase with no spaces, like a folder name (for example{' '}
              <span className="sam-mono">darkblade</span>).
            </p>
            <p>
              The Mod Builder gathers everything you saved into one mod, then lets you download it as a
              zip file.
            </p>
            <div className="mt-1 flex flex-wrap gap-3">
              <GoldButton onClick={() => navigate('/mod-builder')}>📦 Open Mod Builder</GoldButton>
            </div>
          </Step>

          <Step n={4} title="Put it in the game and play">
            <p>
              Unzip the mod folder into Barony's <span className="sam-mono">mods</span> folder. To find
              that folder, right click Barony in Steam, choose Manage, then Browse local files, and open
              the <span className="sam-mono">mods</span> folder.
            </p>
            <p>
              Launch Barony, open the <span className="sam-label">Mods</span> menu, and turn your mod on.
              Start a game and your class or item is right there.
            </p>
          </Step>
        </div>
      </Panel>

      {/* ------------------------------------------------- optional next steps */}
      <Panel title="Want to Do More? (optional)">
        <div className="space-y-3">
          <div className="sam-well p-4">
            <div className="sam-label mb-1">Make your mod actually do things</div>
            <p className="m-0 text-sm leading-relaxed" style={{ color: 'var(--color-parchment)' }}>
              Want a class that fires a barrage, an item with a special power, or a monster that reacts
              when you hit it? You can add a small behavior script in Lua, JavaScript, or TypeScript. The
              API Reference lists every command you can call and what each one does, with examples.
            </p>
            <div className="mt-3">
              <GoldButton onClick={() => navigate('/api-reference')}>📖 Open API Reference</GoldButton>
            </div>
          </div>

          <div className="sam-well p-4">
            <div className="sam-label mb-1">Check it before you share it</div>
            <p className="m-0 text-sm leading-relaxed" style={{ color: 'var(--color-parchment)' }}>
              Not sure your mod is right? The Validator checks it against the rules and points out
              anything that is off, so you catch mistakes before players do.
            </p>
            <div className="mt-3">
              <GoldButton onClick={() => navigate('/validator')}>📜 Open Validator</GoldButton>
            </div>
          </div>

          <p className="m-0 text-sm" style={{ color: '#6b5a35' }}>
            When you are ready, the Dashboard shows what you have built so far and links to every editor.{' '}
            <button
              type="button"
              className="underline"
              style={{ color: 'var(--color-gold)' }}
              onClick={() => navigate('/dashboard')}
            >
              Go to the Dashboard
            </button>
            .
          </p>
        </div>
      </Panel>
    </div>
  );
}
