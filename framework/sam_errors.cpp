/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_errors.cpp
	Desc: implementation of the modder-friendly diagnostics helpers.

-------------------------------------------------------------------------------*/

#include "sam_errors.hpp"
#include "sam_logger.hpp"

#include <algorithm>
#include <cctype>

namespace
{
	std::string toLower(std::string s)
	{
		for ( char& c : s ) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
		return s;
	}
}

namespace SAMErrors
{

int levenshtein(const std::string& a, const std::string& b)
{
	const size_t m = a.size();
	const size_t n = b.size();
	if ( m == 0 ) { return static_cast<int>(n); }
	if ( n == 0 ) { return static_cast<int>(m); }

	std::vector<int> prev(n + 1), cur(n + 1);
	for ( size_t j = 0; j <= n; ++j ) { prev[j] = static_cast<int>(j); }

	for ( size_t i = 1; i <= m; ++i )
	{
		cur[0] = static_cast<int>(i);
		for ( size_t j = 1; j <= n; ++j )
		{
			const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
		}
		std::swap(prev, cur);
	}
	return prev[n];
}

std::string suggest(const std::string& input, const std::vector<std::string>& candidates)
{
	if ( input.empty() || candidates.empty() ) { return ""; }
	const std::string lin = toLower(input);
	// Allow more slack for longer names; a typo is usually 1-2 edits.
	const int threshold = std::max(2, static_cast<int>(input.size()) / 3);

	int best = threshold + 1;
	std::string bestName;
	for ( const std::string& cand : candidates )
	{
		const int d = levenshtein(lin, toLower(cand));
		if ( d < best )
		{
			best = d;
			bestName = cand;
			if ( d == 0 ) { break; }
		}
	}
	return (best <= threshold) ? bestName : std::string();
}

std::string displayFile(const std::string& nsOrLabel, const std::string& relPath)
{
	if ( nsOrLabel.empty() ) { return relPath; }
	if ( relPath.empty() ) { return nsOrLabel; }
	return nsOrLabel + "/" + relPath;
}

void reportSemantic(const char* module, const std::string& file, const std::string& field,
	const std::string& value, const std::string& valueNote, const std::string& expected,
	const std::string& fix, const std::string& consequence, bool warn)
{
	auto emit = [&](const std::string& line) {
		if ( warn ) { SAMLogger::warn(module, line); }
		else { SAMLogger::error(module, line); }
	};

	emit("Invalid value in " + file);
	if ( !field.empty() )
	{
		emit("  field:    " + field);
	}
	std::string v = "  value:    \"" + value + "\"";
	if ( !valueNote.empty() ) { v += "  (" + valueNote + ")"; }
	emit(v);
	if ( !expected.empty() ) { emit("  expected: " + expected); }
	if ( !fix.empty() )      { emit("  fix:      " + fix); }
	if ( !consequence.empty() ) { emit("  -> " + consequence); }
}

void reportSyntax(const char* module, const std::string& file, const std::string& text,
	const std::string& parseError, std::size_t byteOffset, const std::string& consequence)
{
	// Compute 1-based line/column from the byte offset.
	std::size_t line = 1, col = 1;
	const std::size_t limit = std::min(byteOffset, text.size());
	for ( std::size_t i = 0; i < limit; ++i )
	{
		if ( text[i] == '\n' ) { ++line; col = 1; }
		else { ++col; }
	}

	SAMLogger::error(module, "JSON syntax error in " + file);
	SAMLogger::error(module, "  at:       line " + std::to_string(line) + ", column " + std::to_string(col)
		+ " (byte " + std::to_string(byteOffset) + ")");
	if ( !parseError.empty() ) { SAMLogger::error(module, "  detail:   " + parseError); }
	SAMLogger::error(module, "  fix:      check for a missing comma, quote, bracket or trailing comma near there.");
	if ( !consequence.empty() ) { SAMLogger::error(module, "  -> " + consequence); }
}

} // namespace SAMErrors
