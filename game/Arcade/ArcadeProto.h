/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/
#ifndef __ARCADE_PROTO_H__
#define __ARCADE_PROTO_H__

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>

/**
 * Minimal proto3 wire-format codec for the arcade SDK integration.
 *
 * The arcade SDK exchanges serialized protobuf messages through a C ABI.
 * Rather than pulling the whole protobuf library into the engine, we hand-encode
 * the dozen small messages we need. Field numbers come straight from
 * ThirdParty/arcade_sdk/proto/*.proto and game/Arcade/vendor.proto.
 *
 * Writer: append fields in any order; nested messages are built with a separate
 * writer and embedded with Message(). Zero/empty values are omitted, matching
 * proto3 default semantics.
 *
 * Reader: iterate fields with Next(), pull the value with the accessor matching
 * the declared type, or Skip() unknown fields.
 */
namespace ArcadeProto {

enum WireType {
	WIRE_VARINT = 0,
	WIRE_FIXED64 = 1,
	WIRE_LENGTH = 2,
	WIRE_FIXED32 = 5,
};

class Writer {
public:
	void Varint( uint64_t v );
	void Tag( int field, WireType wt );

	// proto3 scalars (omitted when zero, like protoc-generated code)
	void PutBool( int field, bool v );
	void Int32( int field, int32_t v );
	void Int64( int field, int64_t v );
	void UInt32( int field, uint32_t v );
	void UInt64( int field, uint64_t v );
	void Enum( int field, int v ) { Int32( field, v ); }
	void Float( int field, float v );
	void Double( int field, double v );
	void String( int field, const char *s );
	void String( int field, const std::string &s ) { String( field, s.c_str() ); }
	void Bytes( int field, const uint8_t *data, size_t len );
	void PutMessage( int field, const Writer &sub );

	// Force-emit variants for oneof members whose zero value must still be present
	// (a `bool_value = false` inside a oneof, an empty response message, ...).
	void PutBoolAlways( int field, bool v );
	void PutMessageAlways( int field, const Writer &sub );

	const uint8_t *Data() const { return buf.data(); }
	size_t Size() const { return buf.size(); }
	const std::vector<uint8_t> &Buffer() const { return buf; }
	void Clear() { buf.clear(); }

private:
	std::vector<uint8_t> buf;
};

class Reader {
public:
	Reader() : p( nullptr ), end( nullptr ), error( false ) {}
	Reader( const uint8_t *data, size_t len ) : p( data ), end( data + len ), error( false ) {}
	Reader( const std::vector<uint8_t> &v ) : Reader( v.data(), v.size() ) {}

	// Advance to the next field. Returns false at end of buffer or on malformed data.
	bool Next( int &field, WireType &wt );

	uint64_t Varint();
	int64_t Int64() { return (int64_t)Varint(); }
	int32_t Int32() { return (int32_t)Varint(); }
	bool ReadBool() { return Varint() != 0; }
	float Float();
	double Double();
	// Returns a view into the source buffer (not NUL-terminated) for a length-delimited field.
	bool Bytes( const uint8_t *&data, size_t &len );
	std::string String();
	// Sub-message view for a length-delimited field.
	Reader ReadMessage();
	void Skip( WireType wt );

	bool AtEnd() const { return p >= end; }
	bool HadError() const { return error; }

private:
	const uint8_t *p;
	const uint8_t *end;
	bool error;
};

} // namespace ArcadeProto

#endif // __ARCADE_PROTO_H__
