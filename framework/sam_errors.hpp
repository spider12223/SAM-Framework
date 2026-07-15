/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_errors.hpp
	Desc: modder-friendly diagnostics — structured multi-line errors that name the
	      file, the JSON field path, the offending value vs. what was expected, and
	      a "did you mean?" suggestion (Levenshtein) when a modder typos an enum
	      value. The goal: a modder should be able to fix their JSON from the log
	      line alone, without reading any C++.

	Decoupled from Barony (std only), so it compiles into both the game and the
	editor and can be used by every loader.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

namespace SAMErrors
{
	// Edit distance between two strings (case-sensitive).
	int levenshtein(const std::string& a, const std::string& b);

	// The closest candidate to `input` within a sensible edit-distance threshold
	// (case-insensitive), or "" if nothing is close enough. Used for "did you
	// mean X?" suggestions against enum name lists.
	std::string suggest(const std::string& input, const std::vector<std::string>& candidates);

	// A short, modder-readable file label: "namespace/relative/path.json".
	std::string displayFile(const std::string& nsOrLabel, const std::string& relPath);

	// True if a mod-supplied RELATIVE path tries to escape its mod folder — it is
	// absolute (leading '/' or '\\', or an "X:" drive prefix) or contains a ".."
	// path segment. Loaders must reject such a path BEFORE joining it onto the mod
	// directory and opening the result, so a crafted mod.json entry like
	// "../../other/file" cannot reach outside the mod's own folder. Header-only so
	// every loader (game + editor) can call it without a link dependency.
	inline bool relPathEscapes(const std::string& rel)
	{
		if ( !rel.empty() && (rel[0] == '/' || rel[0] == '\\') ) { return true; } // absolute
		if ( rel.size() >= 2 && rel[1] == ':' ) { return true; }                   // "X:" drive
		std::size_t i = 0;
		for ( ;; )
		{
			const std::size_t j = rel.find_first_of("/\\", i);
			const std::size_t end = (j == std::string::npos) ? rel.size() : j;
			if ( end - i == 2 && rel[i] == '.' && rel[i + 1] == '.' ) { return true; } // ".." segment
			if ( j == std::string::npos ) { break; }
			i = j + 1;
		}
		return false;
	}

	// A structured SEMANTIC error (valid JSON, but a value is wrong). Emits a
	// header + indented detail lines through SAMLogger. `valueNote`, `fix`, and
	// `consequence` may be empty (their lines are then omitted). `warn=true`
	// downgrades it to WARN (e.g. a bad optional value that's merely skipped).
	//   Invalid value in <file>
	//     field:    <field>
	//     value:    "<value>"  (<valueNote>)
	//     expected: <expected>
	//     fix:      <fix>
	//     -> <consequence>
	void reportSemantic(const char* module, const std::string& file, const std::string& field,
		const std::string& value, const std::string& valueNote, const std::string& expected,
		const std::string& fix, const std::string& consequence, bool warn = false);

	// A structured SYNTAX error (malformed JSON). Computes the line/column from a
	// byte offset (e.g. nlohmann parse_error::byte) into the original text.
	void reportSyntax(const char* module, const std::string& file, const std::string& text,
		const std::string& parseError, std::size_t byteOffset, const std::string& consequence);
}
