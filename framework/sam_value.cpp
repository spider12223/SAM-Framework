/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_value.cpp
	Desc: see sam_value.hpp.

-------------------------------------------------------------------------------*/

#include "sam_value.hpp"
#include "sam_net.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
	bool sameKey(const std::string& a, const std::string& b)
	{
		if ( a.size() != b.size() ) { return false; }
		for ( std::size_t k = 0; k < a.size(); ++k )
		{
			if ( std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]) ) { return false; }
		}
		return true;
	}

	void write(const SamValue& v, SAMNet::Writer& w)
	{
		switch ( v.type )
		{
			case SamValue::Type::Nil:  w.u8(SAMNet::Tag::Nil); return;
			case SamValue::Type::Bool: w.u8(v.b ? SAMNet::Tag::True : SAMNet::Tag::False); return;
			case SamValue::Type::Int:  w.u8(SAMNet::Tag::Int); w.i64(v.i); return;
			case SamValue::Type::Num:  w.u8(SAMNet::Tag::Num); w.f64(v.n); return;
			case SamValue::Type::Str:  w.u8(SAMNet::Tag::Str); w.str16(v.s); return;
			case SamValue::Type::Arr:
				w.u8(SAMNet::Tag::Arr);
				w.u16((std::uint16_t)std::min<std::size_t>(v.arr.size(), 65535));
				for ( std::size_t k = 0; k < v.arr.size() && k < 65535; ++k ) { write(v.arr[k], w); }
				return;
			case SamValue::Type::Map:
				w.u8(SAMNet::Tag::Map);
				w.u16((std::uint16_t)std::min<std::size_t>(v.map.size(), 65535));
				for ( std::size_t k = 0; k < v.map.size() && k < 65535; ++k )
				{
					w.u8(SAMNet::Tag::Str);
					w.str16(v.map[k].first);
					write(v.map[k].second, w);
				}
				return;
		}
	}

	// The same depth and size rules samLuaDecode / samJsDecode apply, so a value this
	// accepts is one either runtime could have produced.
	bool read(SAMNet::Reader& r, SamValue& out, int depth)
	{
		const std::uint8_t t = r.u8();
		if ( !r.ok ) { return false; }
		switch ( t )
		{
			case SAMNet::Tag::Nil:   out = SamValue::nil(); return true;
			case SAMNet::Tag::False: out = SamValue::boolean(false); return true;
			case SAMNet::Tag::True:  out = SamValue::boolean(true); return true;
			case SAMNet::Tag::Int:   out = SamValue::integer(r.i64()); return r.ok;
			case SAMNet::Tag::Num:   out = SamValue::number(r.f64()); return r.ok;
			case SAMNet::Tag::Str:   out = SamValue::string(r.str16()); return r.ok;
			case SAMNet::Tag::Arr:
			{
				if ( depth >= SAMNet::MAX_DEPTH ) { return false; }
				const int count = r.u16();
				if ( !r.ok || count > 4096 ) { return false; }
				out = SamValue::array();
				out.arr.reserve((std::size_t)count);
				for ( int k = 0; k < count; ++k )
				{
					SamValue e;
					if ( !read(r, e, depth + 1) ) { return false; }
					out.arr.push_back(std::move(e));
				}
				return true;
			}
			case SAMNet::Tag::Map:
			{
				if ( depth >= SAMNet::MAX_DEPTH ) { return false; }
				const int count = r.u16();
				if ( !r.ok || count > 4096 ) { return false; }
				out = SamValue::object();
				out.map.reserve((std::size_t)count);
				for ( int k = 0; k < count; ++k )
				{
					const std::uint8_t kt = r.u8();
					std::string key;
					if ( kt == SAMNet::Tag::Str ) { key = r.str16(); }
					else if ( kt == SAMNet::Tag::Int ) { key = std::to_string(r.i64()); }
					else { return false; }
					if ( !r.ok ) { return false; }
					SamValue e;
					if ( !read(r, e, depth + 1) ) { return false; }
					out.set(key, e);
				}
				return true;
			}
			default:
				return false;
		}
	}
}

SamValue SamValue::nil() { return SamValue(); }
SamValue SamValue::boolean(bool v) { SamValue x; x.type = Type::Bool; x.b = v; return x; }
SamValue SamValue::integer(long long v) { SamValue x; x.type = Type::Int; x.i = v; return x; }
SamValue SamValue::number(double v) { SamValue x; x.type = Type::Num; x.n = v; return x; }
SamValue SamValue::string(const std::string& v) { SamValue x; x.type = Type::Str; x.s = v; return x; }
SamValue SamValue::array() { SamValue x; x.type = Type::Arr; return x; }
SamValue SamValue::object() { SamValue x; x.type = Type::Map; return x; }

SamValue& SamValue::set(const std::string& key, const SamValue& value)
{
	if ( type != Type::Map ) { *this = object(); }
	for ( auto& kv : map )
	{
		if ( sameKey(kv.first, key) ) { kv.second = value; return *this; }
	}
	map.emplace_back(key, value);
	return *this;
}

SamValue& SamValue::push(const SamValue& value)
{
	if ( type != Type::Arr ) { *this = array(); }
	arr.push_back(value);
	return *this;
}

std::string SamValue::encode() const
{
	SAMNet::Writer w;
	write(*this, w);
	return w.buf;
}

bool SamValue::decode(const std::string& bytes, SamValue& out)
{
	out = nil();
	SAMNet::Reader r(bytes);
	SamValue v;
	if ( !read(r, v, 0) || !r.ok || !r.atEnd() ) { return false; }
	out = std::move(v);
	return true;
}

const SamValue* SamValue::get(const std::string& key) const
{
	if ( type != Type::Map ) { return nullptr; }
	for ( const auto& kv : map )
	{
		if ( sameKey(kv.first, key) ) { return &kv.second; }
	}
	return nullptr;
}

std::string SamValue::str(const std::string& key, const std::string& fallback) const
{
	const SamValue* v = get(key);
	return (v && v->type == Type::Str) ? v->s : fallback;
}

long long SamValue::whole(const std::string& key, long long fallback) const
{
	const SamValue* v = get(key);
	if ( !v ) { return fallback; }
	if ( v->type == Type::Int ) { return v->i; }
	if ( v->type == Type::Num && std::isfinite(v->n) && std::fabs(v->n) < 9.2e18 ) { return (long long)v->n; }
	return fallback;
}

double SamValue::real(const std::string& key, double fallback) const
{
	const SamValue* v = get(key);
	if ( !v ) { return fallback; }
	if ( v->type == Type::Num && std::isfinite(v->n) ) { return v->n; }
	if ( v->type == Type::Int ) { return (double)v->i; }
	return fallback;
}

bool SamValue::flag(const std::string& key, bool fallback) const
{
	const SamValue* v = get(key);
	if ( !v ) { return fallback; }
	if ( v->type == Type::Bool ) { return v->b; }
	// 0 and 1 are how an event field says it, so a mod that copies one into a spec is not wrong.
	if ( v->type == Type::Int ) { return v->i != 0; }
	return fallback;
}

bool SamValue::wrongType(const std::string& key, Type want) const
{
	const SamValue* v = get(key);
	if ( !v || v->isNil() ) { return false; }
	if ( want == Type::Int || want == Type::Num ) { return !v->isNumber(); }
	if ( want == Type::Map ) { return !v->isMap(); }
	if ( want == Type::Bool ) { return v->type != Type::Bool && v->type != Type::Int; }
	return v->type != want;
}

const char* SamValue::typeName(Type t)
{
	switch ( t )
	{
		case Type::Nil:  return "nil";
		case Type::Bool: return "boolean";
		case Type::Int:
		case Type::Num:  return "number";
		case Type::Str:  return "string";
		case Type::Arr:
		case Type::Map:  return "table";
	}
	return "value";
}
