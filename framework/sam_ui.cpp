/*-------------------------------------------------------------------------------
	S.A.M Framework - interactive mod panels. See sam_ui.hpp for the design.
-------------------------------------------------------------------------------*/

#include "sam_ui.hpp"
#include <set>
#include "sam_logger.hpp"

#include <map>
#include <string>
#include <vector>

#ifndef EDITOR
#	include "main.hpp"
#	include "game.hpp"       // intro
#	include "ui/Frame.hpp"
#	include "ui/Field.hpp"
#	include "ui/Button.hpp"
#	include "ui/Font.hpp"     // sizeText: measure before you place
#	include "player.hpp"    // players[]->shootmode: frees the cursor for a GUI
#	include "sam_lua_runtime.hpp"   // dispatchUiEvent
#	define SAM_UI_HAVE_ENGINE 1
#endif

namespace
{
	const char* MOD = "UI";

	// The retained description of every panel, which IS the truth. The Frames are a rendering
	// of it and the engine may delete them at any floor change (Frame::guiDestroy), exactly as
	// it does to the script HUD. Rebuilding from this is what makes a panel survive a ladder.
	struct ListRow
	{
		std::string id;
		std::string text;
		unsigned int color = 0;
	};

	struct UiWidget
	{
		enum Kind { LABEL, BUTTON, IMAGE, LIST, INPUT } kind = LABEL;
		std::string id;
		int x = 0, y = 0, w = 0, h = 0;
		std::string text;      // LABEL / BUTTON / INPUT (current text)
		std::string path;      // IMAGE (already resolved)
		unsigned int color = 0;
		std::string font;            // empty = inherit the panel's
		int rowHeight = 0;           // LIST; 0 = derive from the font
		std::vector<ListRow> rows;   // LIST
	};

	struct Panel
	{
		std::string ns, id, title;
		int x = 0, y = 0, w = 0, h = 0;
		bool modal = false;
		// Appearance is the mod's, not ours. These are the defaults, nothing more.
		unsigned int bg = 0, border = 0;
		int borderWidth = -1;
		std::string font;
		// Insertion-ordered so a script's draw order is the order it sees on screen. A map
		// would silently re-sort a list the modder built in a deliberate order.
		std::vector<UiWidget> widgets;
	};

	// Keyed "ns\0id" so two mods may both own a panel called "main".
	std::map<std::string, Panel> s_panels;

	// Whether WE are the reason the cursor is free.
	//
	// A modal panel is unclickable without this. In first-person play the mouse is captured
	// for looking (players[n]->shootmode == true) and clicks never reach the widget tree; the
	// engine sets shootmode = false whenever it opens a GUI of its own (inventory, chest,
	// shop). A modal panel has to do the same.
	//
	// Tracked as "did we clear it" rather than blindly restoring, so closing a panel while the
	// player also has their inventory open cannot yank the cursor back and lock them out of it.
	bool s_heldCursor = false;

	std::string keyOf(const std::string& ns, const std::string& id)
	{
		return ns + std::string(1, '\0') + id;
	}

#ifdef SAM_UI_HAVE_ENGINE
	const char* const kRoot = "sam_ui";

	// The engine default (pixel_maz.ttf#32#2) is a 32px face built for menus, and a list of it
	// fits about four rows before it clips. Panels start on the 16px multiline face instead;
	// sam_ui_font overrides it per panel or per widget.
	const char* const kDefaultFont = "fonts/pixel_maz_multiline.ttf#16#2";

	int fontLineHeight(const std::string& f)
	{
		int w = 0, h = 0;
		if ( Font* fo = Font::get(f.empty() ? kDefaultFont : f.c_str()) )
		{
			fo->sizeText("Ag", &w, &h);
		}
		return h > 0 ? h : 16;
	}

	// Button::setCallback takes a bare function pointer with no user data, so the only thing
	// the handler gets is the Button itself. We therefore encode the route in the widget's
	// NAME and look it up here. Frame names cannot contain a NUL, so this uses a separator
	// that cannot occur in a namespace or an id.
	const char kSep = '~';

	std::string widgetName(const std::string& ns, const std::string& panel, const std::string& id)
	{
		return std::string("w") + kSep + ns + kSep + panel + kSep + id;
	}

	bool parseWidgetName(const std::string& name, std::string& ns, std::string& panel, std::string& id)
	{
		if ( name.size() < 2 || name[0] != 'w' || name[1] != kSep ) { return false; }
		const size_t a = name.find(kSep, 2);
		if ( a == std::string::npos ) { return false; }
		const size_t b = name.find(kSep, a + 1);
		if ( b == std::string::npos ) { return false; }
		ns    = name.substr(2, a - 2);
		panel = name.substr(a + 1, b - a - 1);
		id    = name.substr(b + 1);
		return true;
	}

	Frame* root(bool createIfMissing)
	{
		if ( !gui ) { return nullptr; }
		if ( Frame* f = gui->findFrame(kRoot) ) { return f; }
		if ( !createIfMissing ) { return nullptr; }
		Frame* f = gui->addFrame(kRoot);
		if ( !f ) { return nullptr; }
		f->setSize(SDL_Rect{ 0, 0, Frame::virtualScreenX, Frame::virtualScreenY });
		f->setActualSize(SDL_Rect{ 0, 0, Frame::virtualScreenX, Frame::virtualScreenY });
		f->setColor(0);
		f->setBorder(0);
		// The container itself stays hollow so it never eats a click meant for the game. The
		// individual PANELS turn that off; a click only counts inside a panel's own rectangle.
		f->setHollow(true);
		return f;
	}

	std::string panelFrameName(const std::string& ns, const std::string& id)
	{
		return std::string("p") + kSep + ns + kSep + id;
	}

	// Rows encode their route the same way widgets do, with the row id after a second marker.
	// entry_t::click is also a bare function pointer, so the name is again the only channel.
	const char kRowSep = '|';

	// True only while we are inside the engine's own widget processing, dispatching a click
	// into a mod script. A handler is free to rebuild the very list it was called from -- the
	// module documents that as the way to drive a search box -- but the engine is still
	// holding a pointer to the clicked entry at that moment, so anything we free here is
	// freed under its feet. See the LIST branch of paintWidget.
	bool s_inDispatch = false;

	struct DispatchGuard
	{
		// Saved/restored rather than blindly cleared, so a nested dispatch cannot disarm the
		// outer guard on its way out.
		bool prev;
		DispatchGuard()  : prev(s_inDispatch) { s_inDispatch = true; }
		~DispatchGuard() { s_inDispatch = prev; }
	};

	// Panels whose widgets changed while we were inside a script dispatch, and which must
	// therefore be repainted on the next frame instead of right now.
	std::set<std::string> s_dirty;

	// Call before any repaint. While a script handler is running, the engine is part-way
	// through its own widget processing and still holds pointers into the very list or
	// button that was clicked -- rebuilding underneath it is what caused a use-after-free.
	// Deferring also avoids the subtler bug: entries are appended AFTER any we retire, so a
	// list rebuilt mid-dispatch would draw its stale rows first.
	bool deferRepaint(const std::string& ns, const std::string& id)
	{
		if ( !s_inDispatch ) { return false; }
		s_dirty.insert(keyOf(ns, id));
		return true;
	}

	// THE ONE C CALLBACK every S.A.M button shares. It recovers the route from the button's
	// name and hands it to the script runtimes, which dispatch ui.on_click to Lua and JS alike.
	void onSamButton(Button& b)
	{
		std::string ns, panel, id;
		if ( !parseWidgetName(b.getName() ? b.getName() : "", ns, panel, id) ) { return; }
		DispatchGuard g;
		SAMLua::dispatchUiEvent("ui.on_click", ns, panel, id, "");
	}

	// A row in a mod list was clicked.
	void onSamEntry(Frame::entry_t& e)
	{
		const std::string full = e.name;
		const size_t bar = full.find(kRowSep);
		if ( bar == std::string::npos ) { return; }
		std::string ns, panel, id;
		if ( !parseWidgetName(full.substr(0, bar), ns, panel, id) ) { return; }
		DispatchGuard g;
		SAMLua::dispatchUiEvent("ui.on_select", ns, panel, id, full.substr(bar + 1));
	}

	// Enter was pressed in a mod text box. Field::callback fires once the text is committed,
	// which is the moment a search should run.
	void onSamField(Field& f)
	{
		std::string ns, panel, id;
		if ( !parseWidgetName(f.getName() ? f.getName() : "", ns, panel, id) ) { return; }
		DispatchGuard g;
		SAMLua::dispatchUiEvent("ui.on_submit", ns, panel, id, f.getText() ? f.getText() : "");
	}

	void paintWidget(Frame* panelFrame, const Panel& p, const UiWidget& v)
	{
		const std::string wn = widgetName(p.ns, p.id, v.id);

		if ( v.kind == UiWidget::LABEL )
		{
			Field* f = panelFrame->findField(wn.c_str());
			if ( !f )
			{
				f = panelFrame->addField(wn.c_str(), 512);
				if ( !f ) { return; }
				f->setHJustify(Field::justify_t::LEFT);
				f->setVJustify(Field::justify_t::TOP);
			}
			const std::string lf = v.font.empty() ? (p.font.empty() ? kDefaultFont : p.font) : v.font;
			f->setFont(lf.c_str());
			// Height from the font, not a magic 24: a label in a 32px face was being clipped
			// to a 24px box.
			const int lh = fontLineHeight(lf);
			f->setSize(SDL_Rect{ v.x, v.y, v.w > 0 ? v.w : p.w, lh * 2 });
			f->setText(v.text.c_str());
			f->setColor(v.color);
			return;
		}

		if ( v.kind == UiWidget::BUTTON )
		{
			Button* b = panelFrame->findButton(wn.c_str());
			if ( !b )
			{
				b = panelFrame->addButton(wn.c_str());
				if ( !b ) { return; }
				b->setCallback(&onSamButton);
			}
			b->setSize(SDL_Rect{ v.x, v.y, v.w, v.h });
			b->setFont((v.font.empty() ? (p.font.empty() ? std::string(kDefaultFont) : p.font) : v.font).c_str());
			b->setText(v.text.c_str());
			b->setTextColor(v.color);
			b->setTextHighlightColor(makeColor(255, 255, 255, 255));
			return;
		}

		if ( v.kind == UiWidget::LIST )
		{
			// A sub-frame holding entries. setActualSize taller than the visible size is what
			// turns scrolling on (Frame.hpp:385 sets allowScrolling from it).
			Frame* lf = panelFrame->findFrame(wn.c_str());
			if ( !lf )
			{
				lf = panelFrame->addFrame(wn.c_str());
				if ( !lf ) { return; }
			}
			lf->setSize(SDL_Rect{ v.x, v.y, v.w, v.h });
			lf->setColor(makeColor(10, 9, 8, 200));
			lf->setBorderColor(makeColor(96, 80, 48, 255));
			lf->setBorder(1);
			lf->setHollow(false);
			lf->setClickable(true);
			lf->setScrollBarsEnabled(true);

			// Rebuild the rows wholesale. A list is redrawn on every keystroke of a search
			// box, so this path has to be simple more than it has to be clever.
			//
			// Safe to clear outright: every caller that could be inside a script dispatch
			// has already returned via deferRepaint(), so the engine is never part-way
			// through processing these entries when we get here.
			lf->clearEntries();
			// 'lf' above is the list Frame; name the font distinctly.
			const std::string rowFont = v.font.empty() ? (p.font.empty() ? kDefaultFont : p.font) : v.font;
			const int rowH = v.rowHeight > 0 ? v.rowHeight : (fontLineHeight(rowFont) + 4);
			// Tell the engine the same two numbers we are about to size the content with.
			// Without these the sub-frame keeps Frame's default 24px face and lays rows out
			// at its own ~36px pitch, while the scroll extent below is computed from OUR
			// 24px -- so the extent came to two thirds of the real content and the last
			// third of every list could not be scrolled to. It also made sam_ui_font and
			// sam_ui_list_row_height silently inert on lists, which the API reference
			// promises they are not.
			lf->setFont(rowFont.c_str());
			lf->setEntrySize(rowH);
			for ( const ListRow& r : v.rows )
			{
				const std::string en = wn + std::string(1, kRowSep) + r.id;
				Frame::entry_t* e = lf->addEntry(en.c_str(), false);
				if ( !e ) { continue; }
				e->text = r.text;
				e->color = r.color;
				e->clickable = true;
				e->click = &onSamEntry;
			}
			const int contentH = (int)v.rows.size() * rowH;
			lf->setActualSize(SDL_Rect{ 0, 0, v.w, contentH > v.h ? contentH : v.h });
			return;
		}

		if ( v.kind == UiWidget::INPUT )
		{
			Field* f = panelFrame->findField(wn.c_str());
			if ( !f )
			{
				f = panelFrame->addField(wn.c_str(), 128);
				if ( !f ) { return; }
				f->setHJustify(Field::justify_t::LEFT);
				f->setVJustify(Field::justify_t::CENTER);
				f->setEditable(true);
				f->setCallback(&onSamField);
			}
			f->setFont((v.font.empty() ? (p.font.empty() ? std::string(kDefaultFont) : p.font) : v.font).c_str());
			f->setSize(SDL_Rect{ v.x, v.y, v.w, v.h });
			f->setColor(v.color);
			f->setBackgroundColor(makeColor(10, 9, 8, 220));
			// Only seed the text when the script sets it; otherwise a repaint would wipe what
			// the player is halfway through typing.
			if ( !v.text.empty() && (!f->getText() || f->getText()[0] == '\0') )
			{
				f->setText(v.text.c_str());
			}
			return;
		}

		// IMAGE
		Frame::image_t* img = panelFrame->findImage(wn.c_str());
		if ( !img )
		{
			panelFrame->addImage(SDL_Rect{ v.x, v.y, v.w, v.h }, v.color, v.path.c_str(), wn.c_str());
			return;
		}
		img->pos = SDL_Rect{ v.x, v.y, v.w, v.h };
		img->color = v.color;
		img->path = v.path;
	}

	// Build (or rebuild) one panel's Frame from its retained description.
	Frame* paintPanel(Frame* r, const Panel& p)
	{
		const std::string fn = panelFrameName(p.ns, p.id);
		Frame* f = r->findFrame(fn.c_str());
		if ( !f )
		{
			f = r->addFrame(fn.c_str());
			if ( !f ) { return nullptr; }
		}
		f->setSize(SDL_Rect{ p.x, p.y, p.w, p.h });
		f->setActualSize(SDL_Rect{ 0, 0, p.w, p.h });
		f->setColor(p.bg ? p.bg : makeColor(18, 16, 14, 236));
		f->setBorderColor(p.border ? p.border : makeColor(160, 132, 74, 255));
		f->setBorder(p.borderWidth >= 0 ? p.borderWidth : 2);
		// This is the whole point of the module: a panel is NOT hollow, so it receives input.
		f->setHollow(false);
		f->setClickable(true);

		if ( !p.title.empty() )
		{
			Field* t = f->findField("~title");
			if ( !t )
			{
				t = f->addField("~title", 128);
				if ( t )
				{
					t->setHJustify(Field::justify_t::CENTER);
					t->setVJustify(Field::justify_t::TOP);
				}
			}
			if ( t )
			{
				t->setSize(SDL_Rect{ 0, 6, p.w, 24 });
				t->setText(p.title.c_str());
				t->setColor(makeColor(214, 184, 116, 255));
			}
		}
		for ( const UiWidget& v : p.widgets ) { paintWidget(f, p, v); }
		return f;
	}

	Panel* find(const std::string& ns, const std::string& id)
	{
		auto it = s_panels.find(keyOf(ns, id));
		return ( it != s_panels.end() ) ? &it->second : nullptr;
	}

	// Add or update a widget in the retained description, then draw it if the tree is up.
	bool setWidget(const std::string& ns, const std::string& panelId, const UiWidget& v)
	{
		if ( v.id.empty() ) { return false; }
		Panel* p = find(ns, panelId);
		if ( !p )
		{
			SAM_WARN(MOD, "[" + ns + "] tried to add widget '" + v.id + "' to panel '" + panelId
				+ "', which is not open. Call sam_ui_open first.");
			return false;
		}
		bool replaced = false;
		bool kindChanged = false;
		for ( UiWidget& existing : p->widgets )
		{
			if ( existing.id == v.id )
			{
				kindChanged = ( existing.kind != v.kind );
				existing = v; replaced = true; break;
			}
		}
		if ( !replaced ) { p->widgets.push_back(v); }

		// A widget id reused with a different KIND has to rebuild the panel: paintWidget
		// finds engine widgets by name, so a label turned into a text box found the old
		// non-editable Field and kept it -- a box nobody could type into -- and the reverse
		// left a 'label' that still ate the keyboard. A full repaint recreates by kind.
		if ( kindChanged ) { s_dirty.insert(keyOf(ns, panelId)); return true; }
		if ( deferRepaint(ns, panelId) ) { return true; }   // repainted next frame
		Frame* r = root(true);
		if ( !r ) { return true; }   // recorded; ensure() paints it when the UI exists
		Frame* pf = paintPanel(r, *p);
		if ( pf ) { paintWidget(pf, *p, v); }
		return true;
	}
#endif
}

// ---- panels ----------------------------------------------------------------------------

bool SAMUi::open(const std::string& ns, const std::string& id,
	int x, int y, int w, int h, const std::string& title, bool modal)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)id; (void)x; (void)y; (void)w; (void)h; (void)title; (void)modal;
	return false;
#else
	if ( id.empty() ) { return false; }
	if ( w < 16 ) { w = 16; }
	if ( h < 16 ) { h = 16; }

	Panel& p = s_panels[keyOf(ns, id)];
	const bool isNew = p.id.empty();
	p.ns = ns; p.id = id; p.title = title;
	p.x = x; p.y = y; p.w = w; p.h = h; p.modal = modal;
	if ( isNew ) { p.widgets.clear(); }

	// Panel coordinates are VIRTUAL screen units, not the player's device resolution:
	// 1280x720 on a 16:9 display at the default UI scale, regardless of monitor size. A
	// panel placed past that edge is not merely clipped -- Frame::draw computes a negative
	// width and returns early (Frame.cpp:513-515), so nothing is drawn at all: no panel, no
	// widgets, no input. Since this call still succeeds, and the framework exposes no way to
	// ask how big the screen is, a modder who assumed device pixels sees their mod do
	// nothing with no explanation. Say so.
	if ( x >= Frame::virtualScreenX || y >= Frame::virtualScreenY || x + w <= 0 || y + h <= 0 )
	{
		SAM_WARN(MOD, "[" + ns + "] panel '" + id + "' is placed at ("
			+ std::to_string(x) + "," + std::to_string(y) + ") which is outside the "
			+ std::to_string(Frame::virtualScreenX) + "x" + std::to_string(Frame::virtualScreenY)
			+ " virtual screen, so it will not be drawn. These are virtual units, not your"
			" monitor's pixels -- they do not change with resolution.");
	}

	if ( deferRepaint(ns, id) ) { return true; }   // repainted next frame by ensure()
	if ( Frame* r = root(true) ) { paintPanel(r, p); }
	SAM_INFO(MOD, "[" + ns + "] opened panel '" + id + "' " + std::to_string(w) + "x"
		+ std::to_string(h) + " at (" + std::to_string(x) + "," + std::to_string(y) + ")"
		+ (modal ? " (modal)" : ""));
	return true;
#endif
}

bool SAMUi::close(const std::string& ns, const std::string& id)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)id;
	return false;
#else
	auto it = s_panels.find(keyOf(ns, id));
	if ( it == s_panels.end() ) { return false; }
	if ( Frame* r = root(false) )
	{
		const std::string fn = panelFrameName(ns, id);
		if ( Frame* f = r->findFrame(fn.c_str()) ) { f->removeSelf(); }
	}
	s_panels.erase(it);
	return true;
#endif
}

void SAMUi::closeNamespace(const std::string& ns)
{
#ifdef SAM_UI_HAVE_ENGINE
	const std::string prefix = ns + std::string(1, '\0');
	for ( auto it = s_panels.begin(); it != s_panels.end(); )
	{
		if ( it->first.compare(0, prefix.size(), prefix) != 0 ) { ++it; continue; }
		if ( Frame* r = root(false) )
		{
			const std::string fn = panelFrameName(it->second.ns, it->second.id);
			if ( Frame* f = r->findFrame(fn.c_str()) ) { f->removeSelf(); }
		}
		it = s_panels.erase(it);
	}
#else
	(void)ns;
#endif
}

void SAMUi::closeAll()
{
	const bool had = !s_panels.empty();
	s_panels.clear();
#ifdef SAM_UI_HAVE_ENGINE
	if ( Frame* r = root(false) ) { r->removeSelf(); }
#endif
	if ( had ) { SAM_INFO(MOD, "Closed every mod panel."); }
}

bool SAMUi::isOpen(const std::string& ns, const std::string& id)
{
	return s_panels.find(keyOf(ns, id)) != s_panels.end();
}

bool SAMUi::clearWidgets(const std::string& ns, const std::string& id)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)id;
	return false;
#else
	Panel* p = find(ns, id);
	if ( !p ) { return false; }
	p->widgets.clear();
	// Drop and rebuild the Frame: removing individual children one kind at a time is fiddly
	// and this is the path a search box takes on every keystroke, so it must be simple.
	if ( deferRepaint(ns, id) ) { return true; }   // repainted next frame by ensure()
	if ( Frame* r = root(false) )
	{
		const std::string fn = panelFrameName(ns, id);
		if ( Frame* f = r->findFrame(fn.c_str()) ) { f->removeSelf(); }
		paintPanel(r, *p);
	}
	return true;
#endif
}

// ---- widgets ---------------------------------------------------------------------------

bool SAMUi::label(const std::string& ns, const std::string& panel, const std::string& id,
	int x, int y, int w, const std::string& text, unsigned int rgba)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)x; (void)y; (void)w; (void)text; (void)rgba;
	return false;
#else
	UiWidget v; v.kind = UiWidget::LABEL; v.id = id;
	v.x = x; v.y = y; v.w = w; v.text = text; v.color = rgba;
	return setWidget(ns, panel, v);
#endif
}

bool SAMUi::button(const std::string& ns, const std::string& panel, const std::string& id,
	int x, int y, int w, int h, const std::string& text)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)x; (void)y; (void)w; (void)h; (void)text;
	return false;
#else
	UiWidget v; v.kind = UiWidget::BUTTON; v.id = id;
	v.x = x; v.y = y; v.w = w; v.h = h; v.text = text;
	v.color = makeColor(214, 184, 116, 255);
	return setWidget(ns, panel, v);
#endif
}

bool SAMUi::image(const std::string& ns, const std::string& panel, const std::string& id,
	int x, int y, int w, int h, const std::string& path, unsigned int rgba)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)x; (void)y; (void)w; (void)h; (void)path; (void)rgba;
	return false;
#else
	if ( path.empty() ) { return false; }
	UiWidget v; v.kind = UiWidget::IMAGE; v.id = id;
	v.x = x; v.y = y; v.w = w; v.h = h; v.path = path; v.color = rgba;
	return setWidget(ns, panel, v);
#endif
}

bool SAMUi::list(const std::string& ns, const std::string& panel, const std::string& id,
	int x, int y, int w, int h)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)x; (void)y; (void)w; (void)h;
	return false;
#else
	UiWidget v; v.kind = UiWidget::LIST; v.id = id;
	v.x = x; v.y = y; v.w = w; v.h = h;
	// Keep any rows already added, so re-declaring a list to move it does not empty it.
	if ( Panel* p = find(ns, panel) )
	{
		for ( const UiWidget& e : p->widgets )
		{
			if ( e.id == id && e.kind == UiWidget::LIST ) { v.rows = e.rows; break; }
		}
	}
	return setWidget(ns, panel, v);
#endif
}

bool SAMUi::listAdd(const std::string& ns, const std::string& panel, const std::string& id,
	const std::string& rowId, const std::string& text, unsigned int rgba)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)rowId; (void)text; (void)rgba;
	return false;
#else
	Panel* p = find(ns, panel);
	if ( !p ) { return false; }
	for ( UiWidget& v : p->widgets )
	{
		if ( v.id != id || v.kind != UiWidget::LIST ) { continue; }
		ListRow r; r.id = rowId; r.text = text; r.color = rgba;
		bool replaced = false;
		for ( ListRow& existing : v.rows )
		{
			if ( existing.id == rowId ) { existing = r; replaced = true; break; }
		}
		if ( !replaced ) { v.rows.push_back(r); }
		// Same rule as setWidget: while a script handler is running, the engine is part-way
		// through its own widget processing and still holds a pointer into the very entry
		// that was clicked. Rebuilding the list here freed it under the engine's feet -- a
		// ui.on_select handler that refills its own list is the ordinary case. Defer.
		if ( deferRepaint(ns, panel) ) { return true; }
		if ( Frame* rt = root(false) )
		{
			if ( Frame* pf = rt->findFrame(panelFrameName(ns, panel).c_str()) )
			{
				paintWidget(pf, *p, v);
			}
		}
		return true;
	}
	SAM_WARN(MOD, "[" + ns + "] sam_ui_list_add: no list '" + id + "' on panel '" + panel + "'.");
	return false;
#endif
}

bool SAMUi::listClear(const std::string& ns, const std::string& panel, const std::string& id)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id;
	return false;
#else
	Panel* p = find(ns, panel);
	if ( !p ) { return false; }
	for ( UiWidget& v : p->widgets )
	{
		if ( v.id != id || v.kind != UiWidget::LIST ) { continue; }
		v.rows.clear();
		// Same rule as setWidget: while a script handler is running, the engine is part-way
		// through its own widget processing and still holds a pointer into the very entry
		// that was clicked. Rebuilding the list here freed it under the engine's feet -- a
		// ui.on_select handler that refills its own list is the ordinary case. Defer.
		if ( deferRepaint(ns, panel) ) { return true; }
		if ( Frame* rt = root(false) )
		{
			if ( Frame* pf = rt->findFrame(panelFrameName(ns, panel).c_str()) )
			{
				paintWidget(pf, *p, v);
			}
		}
		return true;
	}
	return false;
#endif
}

bool SAMUi::input(const std::string& ns, const std::string& panel, const std::string& id,
	int x, int y, int w, int h, const std::string& text)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)x; (void)y; (void)w; (void)h; (void)text;
	return false;
#else
	UiWidget v; v.kind = UiWidget::INPUT; v.id = id;
	v.x = x; v.y = y; v.w = w; v.h = h; v.text = text;
	v.color = makeColor(230, 222, 205, 255);
	return setWidget(ns, panel, v);
#endif
}

std::string SAMUi::inputText(const std::string& ns, const std::string& panel, const std::string& id)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id;
	return "";
#else
	// Read the live Field rather than the retained copy: the player has been typing into the
	// widget, and the description only holds what the SCRIPT last set.
	if ( Frame* rt = root(false) )
	{
		if ( Frame* pf = rt->findFrame(panelFrameName(ns, panel).c_str()) )
		{
			if ( Field* f = pf->findField(widgetName(ns, panel, id).c_str()) )
			{
				return f->getText() ? f->getText() : "";
			}
		}
	}
	return "";
#endif
}

bool SAMUi::panelStyle(const std::string& ns, const std::string& panel,
	unsigned int bg, unsigned int border, int borderWidth)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)bg; (void)border; (void)borderWidth;
	return false;
#else
	Panel* p = find(ns, panel);
	if ( !p ) { return false; }
	if ( bg ) { p->bg = bg; }
	if ( border ) { p->border = border; }
	if ( borderWidth >= 0 ) { p->borderWidth = borderWidth; }
	if ( deferRepaint(ns, panel) ) { return true; }   // repainted next frame by ensure()
	if ( Frame* r = root(false) ) { paintPanel(r, *p); }
	return true;
#endif
}

bool SAMUi::font(const std::string& ns, const std::string& panel, const std::string& id,
	const std::string& fontName)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)fontName;
	return false;
#else
	Panel* p = find(ns, panel);
	if ( !p ) { return false; }
	if ( id.empty() )
	{
		// Panel-wide: applies to what is there and to anything added later.
		p->font = fontName;
		for ( UiWidget& v : p->widgets ) { v.font.clear(); }
	}
	else
	{
		bool found = false;
		for ( UiWidget& v : p->widgets ) { if ( v.id == id ) { v.font = fontName; found = true; break; } }
		if ( !found ) { return false; }
	}
	// Drop and rebuild: a font change moves every metric on the panel.
	if ( deferRepaint(ns, panel) ) { return true; }   // repainted next frame by ensure()
	if ( Frame* r = root(false) )
	{
		if ( Frame* f = r->findFrame(panelFrameName(ns, panel).c_str()) ) { f->removeSelf(); }
		paintPanel(r, *p);
	}
	return true;
#endif
}

bool SAMUi::listRowHeight(const std::string& ns, const std::string& panel,
	const std::string& id, int px)
{
#ifndef SAM_UI_HAVE_ENGINE
	(void)ns; (void)panel; (void)id; (void)px;
	return false;
#else
	Panel* p = find(ns, panel);
	if ( !p ) { return false; }
	for ( UiWidget& v : p->widgets )
	{
		if ( v.id != id || v.kind != UiWidget::LIST ) { continue; }
		v.rowHeight = px > 0 ? px : 0;
		if ( Frame* r = root(false) )
		{
			if ( Frame* pf = r->findFrame(panelFrameName(ns, panel).c_str()) ) { paintWidget(pf, *p, v); }
		}
		return true;
	}
	return false;
#endif
}

bool SAMUi::textSize(const std::string& text, const std::string& fontName, int& w, int& h)
{
	w = 0; h = 0;
#ifndef SAM_UI_HAVE_ENGINE
	(void)text; (void)fontName;
	return false;
#else
	Font* f = Font::get(fontName.empty() ? kDefaultFont : fontName.c_str());
	if ( !f ) { return false; }
	f->sizeText(text.c_str(), &w, &h);
	return true;
#endif
}

// ---- engine hooks -----------------------------------------------------------------------

bool SAMUi::keyboardCaptured()
{
#ifndef SAM_UI_HAVE_ENGINE
	return false;
#else
	if ( s_panels.empty() ) { return false; }   // free in vanilla and for most mods
	Frame* rt = root(false);
	if ( !rt ) { return false; }
	for ( const auto& kv : s_panels )
	{
		const Panel& p = kv.second;
		Frame* pf = rt->findFrame(panelFrameName(p.ns, p.id).c_str());
		if ( !pf ) { continue; }
		for ( const UiWidget& v : p.widgets )
		{
			if ( v.kind != UiWidget::INPUT ) { continue; }
			Field* f = pf->findField(widgetName(p.ns, p.id, v.id).c_str());
			if ( f && f->isActivated() ) { return true; }
		}
	}
	return false;
#endif
}

void SAMUi::ensure()
{
#ifdef SAM_UI_HAVE_ENGINE
	// Cursor ownership, before the early-out: a panel may have been closed since last frame,
	// and if we cleared shootmode for it we still owe the player their mouse back.
	{
		const bool wantCursor = modalActive();
		if ( wantCursor )
		{
			// Re-assert EVERY frame rather than only on the rising edge. The engine sets
			// shootmode back to true in a dozen places of its own -- closing an inventory,
			// finishing a level load, doNewGame -- so a flag set once and never rechecked
			// left a modal panel still drawn but permanently unclickable, with the player
			// unable to recover it from inside the game. The first floor change was enough
			// to trigger it.
			//
			// Still yield to a vanilla GUI the player opened themselves: while gui_mode is
			// anything other than NONE the engine owns the cursor, and fighting it every
			// frame would break their inventory instead.
			// Scoped to the player who actually owns the mouse. A panel has no owner, so
			// taking the cursor from every local player meant one splitscreen player opening
			// a mod window disabled the OTHER player's aiming as well.
			const int kbOwner = inputs.getPlayerIDAllowedKeyboard();
			for ( int c = 0; c < MAXPLAYERS; ++c )
			{
				if ( players[c] && players[c]->isLocalPlayer()
					&& ( kbOwner < 0 || c == kbOwner )
					&& players[c]->gui_mode == GUI_MODE_NONE && players[c]->shootmode )
				{
					players[c]->shootmode = false;
				}
			}
			s_heldCursor = true;
		}
		else if ( !wantCursor && s_heldCursor )
		{
			// Only restore what we took, and only to the player who actually has the mouse.
			// A panel has no owner, so an unscoped restore handed the cursor back to every
			// local player in splitscreen -- and handed it back at all in situations where
			// the engine had deliberately taken it, such as the death and gameover screens.
			// gui_mode covers an open inventory; isLocalPlayerAlive covers the rest.
			const int owner = inputs.getPlayerIDAllowedKeyboard();
			for ( int c = 0; c < MAXPLAYERS; ++c )
			{
				if ( players[c] && players[c]->isLocalPlayer()
					&& ( owner < 0 || c == owner )
					&& players[c]->gui_mode == GUI_MODE_NONE
					&& players[c]->isLocalPlayerAlive() )
				{
					players[c]->shootmode = true;
				}
			}
			s_heldCursor = false;
		}
	}

	// Repaint anything a script changed while we were inside its own event handler. Doing it
	// here rather than there is what keeps the engine's widget processing off our backs.
	if ( !s_dirty.empty() )
	{
		if ( Frame* r = root(false) )
		{
			for ( const std::string& k : s_dirty )
			{
				auto it = s_panels.find(k);
				if ( it == s_panels.end() ) { continue; }   // closed since
				const Panel& dp = it->second;
				// The rebuild below destroys every widget on the panel, including a text box
				// the player is halfway through typing into. Carry the live text and the
				// keyboard focus across, or a script that updates one label wipes the
				// player's input and hands their keystrokes back to the game's bindings.
				struct LiveInput { std::string name; std::string text; bool active; };
				std::vector<LiveInput> live;
				if ( Frame* f = r->findFrame(panelFrameName(dp.ns, dp.id).c_str()) )
				{
					for ( const UiWidget& w : dp.widgets )
					{
						if ( w.kind != UiWidget::INPUT ) { continue; }
						const std::string wn = widgetName(dp.ns, dp.id, w.id);
						if ( Field* fld = f->findField(wn.c_str()) )
						{
							live.push_back({ wn, fld->getText() ? fld->getText() : "", fld->isActivated() });
						}
					}
					f->removeSelf();
				}
				if ( Frame* nf = paintPanel(r, dp) )
				{
					for ( const LiveInput& li : live )
					{
						if ( Field* fld = nf->findField(li.name.c_str()) )
						{
							fld->setText(li.text.c_str());
							if ( li.active ) { fld->activate(); }
						}
					}
				}
			}
		}
		s_dirty.clear();
	}

	// The vanilla path and the common modded one: one empty() test, no engine state touched.
	if ( s_panels.empty() ) { return; }
	if ( !gui ) { return; }
	if ( gui->findFrame(kRoot) ) { return; }
	// The engine rebuilt the widget tree underneath us. Put every panel back exactly as the
	// script left it -- same reasoning as SAMHud::ensure.
	Frame* r = root(true);
	if ( !r ) { return; }
	for ( const auto& kv : s_panels ) { paintPanel(r, kv.second); }
	SAM_DEBUG(MOD, "Rebuilt " + std::to_string(s_panels.size()) + " mod panel(s) after a UI reset.");
#endif
}

bool SAMUi::modalActive()
{
#ifdef SAM_UI_HAVE_ENGINE
	if ( s_panels.empty() ) { return false; }
	if ( intro ) { return false; }   // never hold the cursor over a menu
	for ( const auto& kv : s_panels ) { if ( kv.second.modal ) { return true; } }
#endif
	return false;
}

int SAMUi::count() { return (int)s_panels.size(); }
