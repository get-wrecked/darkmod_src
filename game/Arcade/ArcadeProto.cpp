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
#include "precompiled.h"
#include "ArcadeProto.h"
#include "../../tests/testing.h"

#include <string.h>

namespace ArcadeProto {

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void Writer::Varint( uint64_t v ) {
	while ( v >= 0x80 ) {
		buf.push_back( (uint8_t)( ( v & 0x7F ) | 0x80 ) );
		v >>= 7;
	}
	buf.push_back( (uint8_t)v );
}

void Writer::Tag( int field, WireType wt ) {
	Varint( ( (uint64_t)field << 3 ) | (uint64_t)wt );
}

void Writer::PutBool( int field, bool v ) {
	if ( v ) {
		PutBoolAlways( field, v );
	}
}

void Writer::PutBoolAlways( int field, bool v ) {
	Tag( field, WIRE_VARINT );
	Varint( v ? 1 : 0 );
}

void Writer::Int32( int field, int32_t v ) {
	if ( v != 0 ) {
		Tag( field, WIRE_VARINT );
		// negative int32 is sign-extended to 10 bytes on the wire, same as int64
		Varint( (uint64_t)(int64_t)v );
	}
}

void Writer::Int64( int field, int64_t v ) {
	if ( v != 0 ) {
		Tag( field, WIRE_VARINT );
		Varint( (uint64_t)v );
	}
}

void Writer::UInt32( int field, uint32_t v ) {
	if ( v != 0 ) {
		Tag( field, WIRE_VARINT );
		Varint( v );
	}
}

void Writer::UInt64( int field, uint64_t v ) {
	if ( v != 0 ) {
		Tag( field, WIRE_VARINT );
		Varint( v );
	}
}

void Writer::Float( int field, float v ) {
	if ( v != 0.0f ) {
		Tag( field, WIRE_FIXED32 );
		uint32_t bits;
		memcpy( &bits, &v, 4 );
		for ( int i = 0; i < 4; i++ ) {
			buf.push_back( (uint8_t)( bits >> ( 8 * i ) ) );
		}
	}
}

void Writer::Double( int field, double v ) {
	if ( v != 0.0 ) {
		Tag( field, WIRE_FIXED64 );
		uint64_t bits;
		memcpy( &bits, &v, 8 );
		for ( int i = 0; i < 8; i++ ) {
			buf.push_back( (uint8_t)( bits >> ( 8 * i ) ) );
		}
	}
}

void Writer::String( int field, const char *s ) {
	if ( s && s[0] ) {
		Bytes( field, (const uint8_t *)s, strlen( s ) );
	}
}

void Writer::Bytes( int field, const uint8_t *data, size_t len ) {
	if ( len == 0 ) {
		return;
	}
	Tag( field, WIRE_LENGTH );
	Varint( len );
	buf.insert( buf.end(), data, data + len );
}

void Writer::PutMessage( int field, const Writer &sub ) {
	if ( sub.Size() == 0 ) {
		return;
	}
	PutMessageAlways( field, sub );
}

void Writer::PutMessageAlways( int field, const Writer &sub ) {
	Tag( field, WIRE_LENGTH );
	Varint( sub.Size() );
	buf.insert( buf.end(), sub.buf.begin(), sub.buf.end() );
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

uint64_t Reader::Varint() {
	uint64_t result = 0;
	int shift = 0;
	while ( p < end ) {
		uint8_t b = *p++;
		if ( shift < 64 ) {
			result |= (uint64_t)( b & 0x7F ) << shift;
		}
		if ( !( b & 0x80 ) ) {
			return result;
		}
		shift += 7;
		if ( shift > 63 ) {
			// more than 10 bytes: malformed
			error = true;
			p = end;
			return 0;
		}
	}
	error = true;
	return 0;
}

bool Reader::Next( int &field, WireType &wt ) {
	if ( error || p >= end ) {
		return false;
	}
	uint64_t key = Varint();
	if ( error ) {
		return false;
	}
	field = (int)( key >> 3 );
	wt = (WireType)( key & 7 );
	if ( field == 0 || ( wt != WIRE_VARINT && wt != WIRE_FIXED64 && wt != WIRE_LENGTH && wt != WIRE_FIXED32 ) ) {
		error = true;
		return false;
	}
	return true;
}

float Reader::Float() {
	if ( end - p < 4 ) {
		error = true;
		p = end;
		return 0.0f;
	}
	uint32_t bits = 0;
	for ( int i = 0; i < 4; i++ ) {
		bits |= (uint32_t)p[i] << ( 8 * i );
	}
	p += 4;
	float v;
	memcpy( &v, &bits, 4 );
	return v;
}

double Reader::Double() {
	if ( end - p < 8 ) {
		error = true;
		p = end;
		return 0.0;
	}
	uint64_t bits = 0;
	for ( int i = 0; i < 8; i++ ) {
		bits |= (uint64_t)p[i] << ( 8 * i );
	}
	p += 8;
	double v;
	memcpy( &v, &bits, 8 );
	return v;
}

bool Reader::Bytes( const uint8_t *&data, size_t &len ) {
	uint64_t n = Varint();
	if ( error || n > (uint64_t)( end - p ) ) {
		error = true;
		p = end;
		data = nullptr;
		len = 0;
		return false;
	}
	data = p;
	len = (size_t)n;
	p += n;
	return true;
}

std::string Reader::String() {
	const uint8_t *data;
	size_t len;
	if ( !Bytes( data, len ) ) {
		return std::string();
	}
	return std::string( (const char *)data, len );
}

Reader Reader::ReadMessage() {
	const uint8_t *data;
	size_t len;
	if ( !Bytes( data, len ) ) {
		return Reader();
	}
	return Reader( data, len );
}

void Reader::Skip( WireType wt ) {
	switch ( wt ) {
	case WIRE_VARINT:
		Varint();
		break;
	case WIRE_FIXED64:
		if ( end - p < 8 ) { error = true; p = end; } else { p += 8; }
		break;
	case WIRE_FIXED32:
		if ( end - p < 4 ) { error = true; p = end; } else { p += 4; }
		break;
	case WIRE_LENGTH: {
		const uint8_t *data;
		size_t len;
		Bytes( data, len );
		break;
	}
	default:
		error = true;
		p = end;
	}
}

} // namespace ArcadeProto

// ---------------------------------------------------------------------------
// Tests (run in-game with "runTests")
// ---------------------------------------------------------------------------

TEST_CASE( "ArcadeProto:Roundtrip" ) {
	using namespace ArcadeProto;

	Writer sub;
	sub.String( 1, "inner" );
	sub.Double( 2, 2.5 );

	Writer w;
	w.UInt64( 1, 0x1234567890ULL );
	w.String( 2, "hello" );
	w.Int64( 3, -42 );
	w.Float( 4, 1.5f );
	w.PutBool( 5, true );
	w.PutBool( 6, false );		// omitted
	w.PutMessage( 7, sub );
	w.Int32( 8, -7 );
	w.UInt32( 9, 300 );

	Reader r( w.Data(), w.Size() );
	int field;
	WireType wt;
	int seen = 0;
	while ( r.Next( field, wt ) ) {
		seen++;
		switch ( field ) {
		case 1: CHECK( wt == WIRE_VARINT ); CHECK( r.Varint() == 0x1234567890ULL ); break;
		case 2: CHECK( wt == WIRE_LENGTH ); CHECK( r.String() == "hello" ); break;
		case 3: CHECK( wt == WIRE_VARINT ); CHECK( r.Int64() == -42 ); break;
		case 4: CHECK( wt == WIRE_FIXED32 ); CHECK( r.Float() == 1.5f ); break;
		case 5: CHECK( wt == WIRE_VARINT ); CHECK( r.ReadBool() == true ); break;
		case 7: {
			CHECK( wt == WIRE_LENGTH );
			Reader s = r.ReadMessage();
			int f2; WireType w2;
			CHECK( s.Next( f2, w2 ) ); CHECK( f2 == 1 ); CHECK( s.String() == "inner" );
			CHECK( s.Next( f2, w2 ) ); CHECK( f2 == 2 ); CHECK( s.Double() == 2.5 );
			CHECK( !s.Next( f2, w2 ) );
			CHECK( !s.HadError() );
			break;
		}
		case 8: CHECK( wt == WIRE_VARINT ); CHECK( r.Int32() == -7 ); break;
		case 9: CHECK( wt == WIRE_VARINT ); CHECK( r.Varint() == 300 ); break;
		default: CHECK( false ); r.Skip( wt );
		}
	}
	CHECK( seen == 8 );
	CHECK( !r.HadError() );
}

TEST_CASE( "ArcadeProto:KnownEncodings" ) {
	using namespace ArcadeProto;
	// Reference bytes from the protobuf encoding documentation.
	{
		Writer w;
		w.Int32( 1, 150 );		// 08 96 01
		CHECK( w.Size() == 3 );
		CHECK( w.Data()[0] == 0x08 ); CHECK( w.Data()[1] == 0x96 ); CHECK( w.Data()[2] == 0x01 );
	}
	{
		Writer w;
		w.String( 2, "testing" );	// 12 07 74 65 73 74 69 6e 67
		CHECK( w.Size() == 9 );
		CHECK( w.Data()[0] == 0x12 ); CHECK( w.Data()[1] == 0x07 ); CHECK( w.Data()[2] == 0x74 );
	}
	{
		Writer w;
		w.Int64( 1, -1 );		// 08 ff ff ff ff ff ff ff ff ff 01
		CHECK( w.Size() == 11 );
	}
}

TEST_CASE( "ArcadeProto:Malformed" ) {
	using namespace ArcadeProto;
	// truncated length-delimited field
	const uint8_t bad[] = { 0x12, 0x10, 0x41 };
	Reader r( bad, sizeof( bad ) );
	int field; WireType wt;
	CHECK( r.Next( field, wt ) );
	CHECK( r.String().empty() );
	CHECK( r.HadError() );
	CHECK( !r.Next( field, wt ) );

	// unterminated varint
	const uint8_t bad2[] = { 0x08, 0x80, 0x80 };
	Reader r2( bad2, sizeof( bad2 ) );
	CHECK( r2.Next( field, wt ) );
	r2.Varint();
	CHECK( r2.HadError() );
}
