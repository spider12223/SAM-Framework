/*-------------------------------------------------------------------------------
	S.A.M Framework — script-driven HUD. See sam_hud.hpp for the design.
-------------------------------------------------------------------------------*/

#include "sam_hud.hpp"
#include "sam_logger.hpp"

#include <functional>   // std::hash -- child frame names
#include <map>
#include <string>

#ifndef EDITOR
#	include "main.hpp"
#	include "ui/Frame.hpp"   // gui (public root Frame), Frame::virtualScreenX/Y
#	include "ui/Field.hpp"
#	define SAM_HUD_HAVE_UI 1
#endif

namespace
{
	const char* MOD = "HUD";

	// WHAT A WIDGET IS, AND WHY WE KEEP IT.
	//
	// The widget tree is NOT the source of truth, because we do not own its lifetime. Every
	// floor change calls createLevelLoadScreen, which is baseCreateLoadingScreen with a null
	// background, which runs Frame::guiDestroy() + Frame::guiInit() (ui/LoadingScreen.cpp:27)
	// and deletes the entire gui tree out from under us -- our container with it.
	//
	// The first version of this file kept only a set of ids and let the Frames be the truth.
	// After one ladder that set described widgets which no longer existed: clear() returned
	// true for a widget that was already gone, count() reported widgets nobody could see, and
	// a HUD the docs promised would "stay until you clear it" silently vanished with no event
	// and no log line.
	//
	// So the map below IS the HUD. The Frames are a rendering of it, rebuilt by ensure()
	// whenever the engine throws them away. That makes clear()/count() honest and makes the
	// documented lifetime actually true.
	struct HudWidget
	{
		enum Kind { TEXT, BAR, IMAGE } kind = TEXT;
		int x = 0, y = 0, w = 0, h = 0;
		std::string text;      // TEXT
		std::string path;      // IMAGE (already resolved by SAMImages)
		double frac = 0.0;     // BAR
		unsigned int color = 0;
	};

	// Empty in vanilla, always. Everything in this file keys off that.
	// Keyed by "namespace\0id" so ids are scoped to the mod that set them: two mods may both
	// call their bar "hp", and clearing one mod's HUD cannot touch another's.
	std::map<std::string, HudWidget> s_widgets;

	// A NUL separator, so a namespace or id containing the separator cannot forge a key for
	// another mod -- neither can contain a NUL, because both arrive as C strings.
	std::string keyOf(const std::string& ns, const std::string& id)
	{
		return ns + std::string(1, '\0') + id;
	}

#ifdef SAM_HUD_HAVE_UI
	const char* const kContainer = "sam_hud";

	// The container is created lazily, so a game with no mod drawing a HUD never builds it.
	// Returns nullptr when the UI is not up yet (early load, or a headless path), which is a
	// normal condition rather than an error: a script may fire before the root exists.
	Frame* container(bool createIfMissing)
	{
		if ( !gui ) { return nullptr; }
		if ( Frame* f = gui->findFrame(kContainer) ) { return f; }
		if ( !createIfMissing ) { return nullptr; }
		Frame* f = gui->addFrame(kContainer);
		if ( !f ) { return nullptr; }
		// Full-screen, fully transparent: it is a coordinate space, not a panel. Children
		// position themselves in virtual screen pixels so a mod's HUD lines up with vanilla's
		// at any resolution.
		f->setSize(SDL_Rect{ 0, 0, Frame::virtualScreenX, Frame::virtualScreenY });
		f->setActualSize(SDL_Rect{ 0, 0, Frame::virtualScreenX, Frame::virtualScreenY });
		f->setColor(0);
		f->setBorder(0);
		f->setHollow(true);   // never eats clicks: a HUD must not steal input from the game
		return f;
	}

	// One child frame per element, named by the mod's id, so an update is a lookup rather
	// than a rebuild and two mods cannot collide unless they pick the same id.
	// The key contains a NUL, which cannot go into a Frame name, so name the child by a hash
	// of the key. Collisions between two mods would need a full 64-bit hash collision.
	std::string childName(const std::string& key)
	{
		return "e_" + std::to_string((unsigned long long)std::hash<std::string>{}(key));
	}

	Frame* cellFor(Frame* root, const std::string& id)
	{
		const std::string cn = childName(id);
		if ( Frame* c = root->findFrame(cn.c_str()) ) { return c; }
		Frame* c = root->addFrame(cn.c_str());
		if ( !c ) { return nullptr; }
		c->setColor(0);
		c->setBorder(0);
		c->setHollow(true);
		return c;
	}

	// Draw one retained widget into the tree. Used both by the public setters and by the
	// rebuild after the engine wipes the gui, so exactly one place knows how a kind is laid out.
	bool paint(Frame* root, const std::string& id, const HudWidget& v)
	{
		Frame* cell = cellFor(root, id);
		if ( !cell ) { return false; }

		if ( v.kind == HudWidget::TEXT )
		{
			// Generous width so long text is never clipped; the field draws left-aligned inside it.
			cell->setSize(SDL_Rect{ v.x, v.y, Frame::virtualScreenX, 24 });
			cell->setActualSize(SDL_Rect{ 0, 0, Frame::virtualScreenX, 24 });
			cell->setColor(0);
			Field* f = cell->findField("t");
			if ( !f )
			{
				f = cell->addField("t", 256);
				if ( !f ) { return false; }
				f->setHJustify(Field::justify_t::LEFT);
				f->setVJustify(Field::justify_t::TOP);
			}
			f->setSize(SDL_Rect{ 0, 0, Frame::virtualScreenX, 24 });
			f->setText(v.text.c_str());
			f->setColor(v.color);
			return true;
		}

		if ( v.kind == HudWidget::BAR )
		{
			// The outer cell is the empty track, the inner frame is the filled portion. Two frames
			// with colours is all a bar needs; no images, so nothing to ship and nothing to load.
			cell->setSize(SDL_Rect{ v.x, v.y, v.w, v.h });
			cell->setActualSize(SDL_Rect{ 0, 0, v.w, v.h });
			cell->setColor(makeColor(0, 0, 0, 160));
			cell->setBorder(0);
			Frame* fill = cell->findFrame("f");
			if ( !fill )
			{
				fill = cell->addFrame("f");
				if ( !fill ) { return false; }
				fill->setBorder(0);
				fill->setHollow(true);
			}
			const int fw = (int)(v.w * v.frac);
			fill->setSize(SDL_Rect{ 0, 0, fw > 0 ? fw : 1, v.h });
			fill->setActualSize(SDL_Rect{ 0, 0, fw > 0 ? fw : 1, v.h });
			fill->setColor(v.color);
			// A zero-length bar should read as empty, not as a 1px sliver of colour.
			fill->setInvisible(v.frac <= 0.0);
			return true;
		}

		// IMAGE. A frame CLIPS its children to its own size, so the cell has to be at least as
		// big as the picture or the picture is cropped. Zero w/h means "natural size", which
		// Frame resolves at draw time from the loaded image -- but the cell needs a number now,
		// so fall back to the full HUD in that case and let the image sit inside it.
		const int cw = ( v.w > 0 ) ? v.w : Frame::virtualScreenX;
		const int ch = ( v.h > 0 ) ? v.h : Frame::virtualScreenY;
		cell->setSize(SDL_Rect{ v.x, v.y, cw, ch });
		cell->setActualSize(SDL_Rect{ 0, 0, cw, ch });
		cell->setColor(0);
		Frame::image_t* img = cell->findImage("i");
		if ( !img )
		{
			img = cell->addImage(SDL_Rect{ 0, 0, v.w, v.h }, v.color, v.path.c_str(), "i");
			return img != nullptr;
		}
		img->pos = SDL_Rect{ 0, 0, v.w, v.h };
		img->color = v.color;
		img->path = v.path;
		return true;
	}

	// Record the widget, then draw it. A false return means the UI was not up to draw into;
	// the widget is recorded either way and ensure() paints it once the root exists.
	bool set(const std::string& ns, const std::string& id, const HudWidget& v)
	{
		if ( id.empty() ) { return false; }
		const std::string k = keyOf(ns, id);
		s_widgets[k] = v;
		Frame* root = container(true);
		if ( !root ) { return false; }
		return paint(root, k, v);
	}
#endif
}

bool SAMHud::text(const std::string& ns, const std::string& id, int x, int y, const std::string& value, unsigned int rgba)
{
#ifndef SAM_HUD_HAVE_UI
	(void)ns; (void)id; (void)x; (void)y; (void)value; (void)rgba;
	return false;
#else
	HudWidget v;
	v.kind = HudWidget::TEXT;
	v.x = x; v.y = y;
	v.text = value;
	v.color = rgba;
	return set(ns, id, v);
#endif
}

bool SAMHud::bar(const std::string& ns, const std::string& id, int x, int y, int w, int h, double frac, unsigned int rgba)
{
#ifndef SAM_HUD_HAVE_UI
	(void)ns; (void)id; (void)x; (void)y; (void)w; (void)h; (void)frac; (void)rgba;
	return false;
#else
	if ( w < 1 ) { w = 1; }
	if ( h < 1 ) { h = 1; }
	if ( !(frac >= 0.0) ) { frac = 0.0; }   // also catches NaN
	if ( frac > 1.0 ) { frac = 1.0; }
	HudWidget v;
	v.kind = HudWidget::BAR;
	v.x = x; v.y = y; v.w = w; v.h = h;
	v.frac = frac;
	v.color = rgba;
	return set(ns, id, v);
#endif
}

bool SAMHud::image(const std::string& ns, const std::string& id, int x, int y, int w, int h,
	const std::string& path, unsigned int rgba)
{
#ifndef SAM_HUD_HAVE_UI
	(void)ns; (void)id; (void)x; (void)y; (void)w; (void)h; (void)path; (void)rgba;
	return false;
#else
	if ( path.empty() ) { return false; }
	if ( w < 0 ) { w = 0; }
	if ( h < 0 ) { h = 0; }
	HudWidget v;
	v.kind = HudWidget::IMAGE;
	v.x = x; v.y = y; v.w = w; v.h = h;
	v.path = path;
	v.color = rgba;
	return set(ns, id, v);
#endif
}

bool SAMHud::clear(const std::string& ns, const std::string& id)
{
	// Answer from the map, not the tree: the tree may have been thrown away by a floor change
	// since this widget was set, and "was it showing" must not depend on that.
#ifdef SAM_HUD_HAVE_UI
	const std::string k = keyOf(ns, id);
	if ( s_widgets.erase(k) == 0 ) { return false; }
	if ( Frame* root = container(false) )
	{
		const std::string cn = childName(k);
		if ( Frame* cell = root->findFrame(cn.c_str()) ) { cell->removeSelf(); }
	}
	return true;
#else
	(void)ns; (void)id;
	return false;
#endif
}

void SAMHud::clearNamespace(const std::string& ns)
{
#ifdef SAM_HUD_HAVE_UI
	const std::string prefix = ns + std::string(1, '\0');
	Frame* root = container(false);
	for ( auto it = s_widgets.begin(); it != s_widgets.end(); )
	{
		if ( it->first.compare(0, prefix.size(), prefix) != 0 ) { ++it; continue; }
		if ( root )
		{
			const std::string cn = childName(it->first);
			if ( Frame* cell = root->findFrame(cn.c_str()) ) { cell->removeSelf(); }
		}
		it = s_widgets.erase(it);
	}
#else
	(void)ns;
#endif
}

void SAMHud::clearAll()
{
	const bool had = !s_widgets.empty();
	s_widgets.clear();
#ifdef SAM_HUD_HAVE_UI
	// Drop the whole container, so nothing a mod drew can survive its unload.
	if ( Frame* root = container(false) ) { root->removeSelf(); }
#endif
	if ( had ) { SAM_INFO(MOD, "Cleared the script HUD."); }
}

void SAMHud::ensure()
{
#ifdef SAM_HUD_HAVE_UI
	// The vanilla path, and the common modded one: nothing to keep alive, so this costs a
	// single empty() test per frame and touches no engine state.
	if ( s_widgets.empty() ) { return; }
	if ( !gui ) { return; }
	// Still there? Nothing to do -- one lookup on a frame that already exists.
	if ( gui->findFrame(kContainer) ) { return; }
	// The engine rebuilt the gui tree underneath us (a floor change, or anything else calling
	// Frame::guiDestroy). Put the HUD back exactly as the script left it.
	Frame* root = container(true);
	if ( !root ) { return; }
	for ( const auto& kv : s_widgets ) { paint(root, kv.first, kv.second); }
	SAM_DEBUG(MOD, "Rebuilt the script HUD after the engine reset the UI ("
		+ std::to_string(s_widgets.size()) + " widget(s)).");
#endif
}

int SAMHud::count() { return (int)s_widgets.size(); }
