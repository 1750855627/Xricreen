/*
   LZ4 - Fast LZ compression algorithm
   Copyright (C) 2011-2012, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
  
       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.
  
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//**************************************
// Compilation Directives
//**************************************
#if __STDC_VERSION__ >= 199901L
  /* "restrict" is a known keyword */
#else
#define restrict  // Disable restrict
#endif

#ifdef _MSC_VER
#define inline __forceinline
#endif

#ifdef __GNUC__
#define _PACKED __attribute__ ((packed))
#else
#define _PACKED
#endif

#define ARCH64 (__x86_64__ || __ppc64__ || _WIN64 || __LP64__)   // Detect 64 bits mode


//**************************************
// Includes
//**************************************
#include <stdlib.h>   // for malloc
#include <string.h>   // for memset
#include "lz4.h"


//**************************************
// Performance parameter               
//**************************************
// Increasing this value improves compression ratio
// Lowering this value reduces memory usage
// Lowering may also improve speed, typically on reaching cache size limits (L1 32KB for Intel, 64KB for AMD)
// Memory usage formula for 32 bits systems : N->2^(N+2) Bytes (examples : 17 -> 512KB ; 12 -> 16KB)
#define HASH_LOG 12

//#define _FORCE_SW_BITCOUNT   // Uncomment for better performance if target platform has no hardware support for LowBitCount


//**************************************
// Basic Types
//**************************************
#if defined(_MSC_VER)    // Visual Studio does not support 'stdint' natively
#define BYTE	unsigned __int8
#define U16		unsigned __int16
#define U32		unsigned __int32
#define S32		__int32
#define U64		unsigned __int64
#else
#include <stdint.h>
#define BYTE	uint8_t
#define U16		uint16_t
#define U32		uint32_t
#define S32		int32_t
#define U64		uint64_t
#endif


//**************************************
// Constants
//**************************************
#define MINMATCH 4
#define SKIPSTRENGTH 6
#define STACKLIMIT 13
#define HEAPMODE (HASH_LOG>STACKLIMIT)  // Defines if memory is allocated into the stack (local variable), or into the heap (malloc()).
#define COPYLENGTH 8
#define LASTLITERALS 5
#define MFLIMIT (COPYLENGTH+MINMATCH)
#define MINLENGTH (MFLIMIT+1)

#define MAXD_LOG 16
#define MAX_DISTANCE ((1 << MAXD_LOG) - 1)

#define HASHTABLESIZE (1 << HASH_LOG)
#define HASH_MASK (HASHTABLESIZE - 1)

#define ML_BITS 4
#define ML_MASK ((1U<<ML_BITS)-1)
#define RUN_BITS (8-ML_BITS)
#define RUN_MASK ((1U<<RUN_BITS)-1)


//**************************************
// Local structures
//**************************************
struct refTables
{
	const BYTE* hashTable[HASHTABLESIZE];
};

typedef struct _U64_S
{
	U64 v;
} _PACKED U64_S;

typedef struct _U32_S
{
	U32 v;
} _PACKED U32_S;

typedef struct _U16_S
{
	U16 v;
} _PACKED U16_S;

#define A64(x) (((U64_S *)(x))->v)
#define A32(x) (((U32_S *)(x))->v)
#define A16(x) (((U16_S *)(x))->v)


//**************************************
// Architecture-specific macros
//**************************************
#if ARCH64	// 64-bit
#define COPYSTEP 8
#define LZ4_COPYSTEP(s,d)		A64(d) = A64(s); d+=8; s+=8;
#define LZ4_COPYPACKET(s,d)		LZ4_COPYSTEP(s,d)
#else		// 32-bit
#define COPYSTEP 4
#define LZ4_COPYSTEP(s,d)		A32(d) = A32(s); d+=4; s+=4;
#define LZ4_COPYPACKET(s,d)		LZ4_COPYSTEP(s,d); LZ4_COPYSTEP(s,d);
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LZ4_READ_LITTLEENDIAN_16(d,s,p) { d = s - A16(p); }
#define LZ4_WRITE_LITTLEENDIAN_16(p,v) { A16(p) = v; p+=2; }
#define LZ4_NbCommonBytes LZ4_NbCommonBytes_LittleEndian
#else		// Big Endian
#define LZ4_READ_LITTLEENDIAN_16(d,s,p) { int delta = p[0]; delta += p[1] << 8; d = s - delta; }
#define LZ4_WRITE_LITTLEENDIAN_16(p,v) { int delta = v; *p++ = delta; *op++ = delta>>8; }
#define LZ4_NbCommonBytes LZ4_NbCommonBytes_BigEndian
#endif


//**************************************
// Macros
//**************************************
#define LZ4_HASH_FUNCTION(i)	(((i) * 2654435761U) >> ((MINMATCH*8)-HASH_LOG))
#define LZ4_HASH_VALUE(p)		LZ4_HASH_FUNCTION(A32(p))
#define LZ4_WILDCOPY(s,d,e)		do { LZ4_COPYPACKET(s,d) } while (d<e);
#define LZ4_BLINDCOPY(s,d,l)	{ BYTE* e=d+l; LZ4_WILDCOPY(s,d,e); d=e; }


//****************************
// Private functions
//****************************
inline static int LZ4_NbCommonBytes_LittleEndian (register U32 val)
{
    #if defined(_MSC_VER) && !defined(_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanForward( &r, val );
    return (int)(r>>3);
    #elif defined(__GNUC__)  && !defined(_FORCE_SW_BITCOUNT)
    return (__builtin_ctz(val) >> 3); 
    #else
	static const int DeBruijnBytePos[32] = { 0, 0, 3, 0, 3, 1, 3, 0, 3, 2, 2, 1, 3, 2, 0, 1, 3, 3, 1, 2, 2, 2, 2, 0, 3, 1, 2, 0, 1, 0, 1, 1 };
	return DeBruijnBytePos[((U32)((val & -val) * 0x077CB531U)) >> 27];
    #endif
}

inline static int LZ4_NbCommonBytes_BigEndian (register U32 val)
{
    #if defined(_MSC_VER) && !defined(_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanReverse( &r, val );
    return (int)(r>>3);
    #elif defined(__GNUC__)  && !defined(_FORCE_SW_BITCOUNT)
    return (__builtin_clz(val) >> 3); 
    #else
	int r;
	if (!(val>>16)) { r=2; val>>=8; } else { r=0; val>>=24; }
	r += (!val);
	return r;
    #endif
}


//******************************
// Public Compression functions
//******************************
int LZ4_compressCtx(void** ctx,
				 char* source, 
				 char* dest,
				 int isize)
{	
#if HEAPMODE
	struct refTables *srt = (struct refTables *) (*ctx);
	const BYTE** HashTable;
#else
	const BYTE* HashTable[HASHTABLESIZE] = {0};
#endif

	const BYTE* ip = (BYTE*) source;       
	const BYTE* anchor = ip;
	const BYTE* const iend = ip + isize;
	const BYTE* const mflimit = iend - MFLIMIT;
#define matchlimit (iend - LASTLITERALS)

	BYTE* op = (BYTE*) dest;
	
	int len, length;
	const int skipStrength = SKIPSTRENGTH;
	U32 forwardH;


	// Init 
	if (isize<MINLENGTH) goto _last_literals;
#if HEAPMODE
	if (*ctx == NULL) 
	{
		srt = (struct refTables *) malloc ( sizeof(struct refTables) );
		*ctx = (void*) srt;
	}
	HashTable = srt->hashTable;
	memset((void*)HashTable, 0, sizeof(srt->hashTable));
#else
	(void) ctx;
#endif


	// First Byte
	HashTable[LZ4_HASH_VALUE(ip)] = ip;
	ip++; forwardH = LZ4_HASH_VALUE(ip);
	
	// Main Loop
    for ( ; ; ) 
	{
		int findMatchAttempts = (1U << skipStrength) + 3;
		const BYTE* forwardIp = ip;
		const BYTE* ref;
		BYTE* token;

		// Find a match
		do {
			U32 h = forwardH;
			int step = findMatchAttempts++ >> skipStrength;
			ip = forwardIp;
			forwardIp = ip + step;

			if (forwardIp > mflimit) { goto _last_literals; }

			forwardH = LZ4_HASH_VALUE(forwardIp);
			ref = HashTable[h];
			HashTable[h] = ip;

		} while ((ref < ip - MAX_DISTANCE) || (A32(ref) != A32(ip)));

		// Catch up
		while ((ip>anchor) && (ref>(BYTE*)source) && (ip[-1]==ref[-1])) { ip--; ref--; }  

		// Encode Literal length
		length = ip - anchor;
		token = op++;
		if (length>=(int)RUN_MASK) { *token=(RUN_MASK<<ML_BITS); len = length-RUN_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE)len; } 
		else *token = (length<<ML_BITS);

		// Copy Literals
		LZ4_BLINDCOPY(anchor, op, length);


_next_match:
		// Encode Offset
		LZ4_WRITE_LITTLEENDIAN_16(op,ip-ref);

		// Start Counting
		ip+=MINMATCH; ref+=MINMATCH;   // MinMatch verified
		anchor = ip;
		while (ip<matchlimit-3)
		{
			U32 diff = A32(ref) ^ A32(ip);
			if (!diff) { ip+=4; ref+=4; continue; }
			ip += LZ4_NbCommonBytes(diff);
			goto _endCount;
		}
		if ((ip<(matchlimit-1)) && (A16(ref) == A16(ip))) { ip+=2; ref+=2; }
		if ((ip<matchlimit) && (*ref == *ip)) ip++;
_endCount:

		// Encode MatchLength
		len = (ip - anchor);
		if (len>=(int)ML_MASK) { *token+=ML_MASK; len-=ML_MASK; for(; len > 509 ; len-=510) { *op++ = 255; *op++ = 255; } if (len > 254) { len-=255; *op++ = 255; } *op++ = (BYTE)len; } 
		else *token += len;	

		// Test end of chunk
		if (ip > mflimit) { anchor = ip;  break; }

		// Fill table
		HashTable[LZ4_HASH_VALUE(ip-2)] = ip-2;

		// Test next position
		ref = HashTable[LZ4_HASH_VALUE(ip)];
		HashTable[LZ4_HASH_VALUE(ip)] = ip;
		if ((ref > ip - (MAX_DISTANCE + 1)) && (A32(ref) == A32(ip))) { token = op++; *token=0; goto _next_match; }

		// Prepare next loop
		anchor = ip++; 
		forwardH = LZ4_HASH_VALUE(ip);
	}

_last_literals:
	// Encode Last Literals
	{
		int lastRun = iend - anchor;
		if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; } 
		else *op++ = (lastRun<<ML_BITS);
		memcpy(op, anchor, iend - anchor);
		op += iend-anchor;
	} 

	// End
	return (int) (((char*)op)-dest);
}



// Note : this function is valid only if isize < LZ4_64KLIMIT
#define LZ4_64KLIMIT ((1U<<16) + (MFLIMIT-1))
#define HASHLOG64K (HASH_LOG+1)
#define LZ4_HASH64K_FUNCTION(i)	(((i) * 2654435761U) >> ((MINMATCH*8)-HASHLOG64K))
#define LZ4_HASH64K_VALUE(p)	LZ4_HASH64K_FUNCTION(A32(p))
int LZ4_compress64kCtx(void** ctx,
				 char* source, 
				 char* dest,
				 int isize)
{	
#if HEAPMODE
	struct refTables *srt = (struct refTables *) (*ctx);
	U16* HashTable;
#else
	U16 HashTable[HASHTABLESIZE<<1] = {0};
#endif

	const BYTE* ip = (BYTE*) source;       
	const BYTE* anchor = ip;
	const BYTE* const base = ip;
	const BYTE* const iend = ip + isize;
	const BYTE* const mflimit = iend - MFLIMIT;
#define matchlimit (iend - LASTLITERALS)

	BYTE* op = (BYTE*) dest;
	
	int len, length;
	const int skipStrength = SKIPSTRENGTH;
	U32 forwardH;


	// Init 
	if (isize<MINLENGTH) goto _last_literals;
#if HEAPMODE
	if (*ctx == NULL) 
	{
		srt = (struct refTables *) malloc ( sizeof(struct refTables) );
		*ctx = (void*) srt;
	}
	HashTable = (U16*)(srt->hashTable);
	memset((void*)HashTable, 0, sizeof(srt->hashTable));
#else
	(void) ctx;
#endif


	// First Byte
	ip++; forwardH = LZ4_HASH64K_VALUE(ip);
	
	// Main Loop
    for ( ; ; ) 
	{
		int findMatchAttempts = (1U << skipStrength) + 3;
		const BYTE* forwardIp = ip;
		const BYTE* ref;
		BYTE* token;

		// Find a match
		do {
			U32 h = forwardH;
			int step = findMatchAttempts++ >> skipStrength;
			ip = forwardIp;
			forwardIp = ip + step;

			if (forwardIp > mflimit) { goto _last_literals; }

			forwardH = LZ4_HASH64K_VALUE(forwardIp);
			ref = base + HashTable[h];
			HashTable[h] = ip - base;

		} while (A32(ref) != A32(ip));

		// Catch up
		while ((ip>anchor) && (ref>(BYTE*)source) && (ip[-1]==ref[-1])) { ip--; ref--; }  

		// Encode Literal length
		length = ip - anchor;
		token = op++;
		if (length>=(int)RUN_MASK) { *token=(RUN_MASK<<ML_BITS); len = length-RUN_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE)len; } 
		else *token = (length<<ML_BITS);

		// Copy Literals
		LZ4_BLINDCOPY(anchor, op, length);


_next_match:
		// Encode Offset
		LZ4_WRITE_LITTLEENDIAN_16(op,ip-ref);

		// Start Counting
		ip+=MINMATCH; ref+=MINMATCH;   // MinMatch verified
		anchor = ip;
		while (ip<matchlimit-3)
		{
			U32 diff = A32(ref) ^ A32(ip);
			if (!diff) { ip+=4; ref+=4; continue; }
			ip += LZ4_NbCommonBytes(diff);
			goto _endCount;
		}
		if ((ip<(matchlimit-1)) && (A16(ref) == A16(ip))) { ip+=2; ref+=2; }
		if ((ip<matchlimit) && (*ref == *ip)) ip++;
_endCount:
		
		// Encode MatchLength
		len = (ip - anchor);
		if (len>=(int)ML_MASK) { *token+=ML_MASK; len-=ML_MASK; for(; len > 509 ; len-=510) { *op++ = 255; *op++ = 255; } if (len > 254) { len-=255; *op++ = 255; } *op++ = (BYTE)len; } 
		else *token += len;	

		// Test end of chunk
		if (ip > mflimit) { anchor = ip;  break; }

		// Fill table
		HashTable[LZ4_HASH64K_VALUE(ip-2)] = ip - 2 - base;

		// Test next position
		ref = base + HashTable[LZ4_HASH64K_VALUE(ip)];
		HashTable[LZ4_HASH64K_VALUE(ip)] = ip - base;
		if (A32(ref) == A32(ip)) { token = op++; *token=0; goto _next_match; }

		// Prepare next loop
		anchor = ip++; 
		forwardH = LZ4_HASH64K_VALUE(ip);
	}

_last_literals:
	// Encode Last Literals
	{
		int lastRun = iend - anchor;
		if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; } 
		else *op++ = (lastRun<<ML_BITS);
		memcpy(op, anchor, iend - anchor);
		op += iend-anchor;
	} 

	// End
	return (int) (((char*)op)-dest);
}



int LZ4_compress(char* source, 
				 char* dest,
				 int isize)
{
#if HEAPMODE
	void* ctx = malloc(sizeof(struct refTables));
	int result;
	if (isize < LZ4_64KLIMIT)
		result = LZ4_compress64kCtx(&ctx, source, dest, isize);
	else result = LZ4_compressCtx(&ctx, source, dest, isize);
	free(ctx);
	return result;
#else
	if (isize < (int)LZ4_64KLIMIT) return LZ4_compress64kCtx(NULL, source, dest, isize);
	return LZ4_compressCtx(NULL, source, dest, isize);
#endif
}




//****************************
// Decompression functions
//****************************

// Note : The decoding functions LZ4_uncompress() and LZ4_uncompress_unknownOutputSize() 
//		are safe against "buffer overflow" attack type.
//		They will never write nor read outside of the provided input and output buffers.
//		A corrupted input will produce an error result, a negative int, indicating the position of the error within input stream.

int LZ4_uncompress(char* source, 
				 char* dest,
				 int osize)
{	
	// Local Variables
	const BYTE* restrict ip = (const BYTE*) source;
	const BYTE* restrict ref;

	BYTE* restrict op = (BYTE*) dest;
	BYTE* const oend = op + osize;
	BYTE* cpy;

	BYTE token;
	
	int	len, length;
	size_t dec[] ={0, 3, 2, 3, 0, 0, 0, 0};


	// Main Loop
	while (1)
	{
		// get runlength
		token = *ip++;
		if ((length=(token>>ML_BITS)) == RUN_MASK)  { for (;(len=*ip++)==255;length+=255){} length += len; } 

		// copy literals
		cpy = op+length;
		if (cpy>oend-COPYLENGTH) 
		{ 
			if (cpy > oend) goto _output_error;
			memcpy(op, ip, length);
			ip += length;
			break;    // Necessarily EOF
		}
		LZ4_WILDCOPY(ip, op, cpy); ip -= (op-cpy); op = cpy;

		// get offset
		LZ4_READ_LITTLEENDIAN_16(ref,cpy,ip); ip+=2;
		if (ref < (BYTE* const)dest) goto _output_error;		

		// get matchlength
		if ((length=(token&ML_MASK)) == ML_MASK) { for (;*ip==255;length+=255) {ip++;} length += *ip++; } 

		// copy repeated sequence
		if (op-ref<COPYSTEP)
		{
#if ARCH64
			size_t dec2table[]={0, 4, 4, 3, 4, 5, 6, 7};
			size_t dec2 = dec2table[op-ref];
#else
			int dec2 = 0;
#endif
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			ref -= dec[op-ref];
			A32(op)=A32(ref); op += COPYSTEP-4; ref += COPYSTEP-4;
			ref -= dec2;
		} else { LZ4_COPYSTEP(ref,op); }
		cpy = op + length - (COPYSTEP-4);
		if (cpy>oend-COPYLENGTH)
		{
			if (cpy > oend) goto _output_error;	
			LZ4_WILDCOPY(ref, op, (oend-COPYLENGTH));
			while(op<cpy) *op++=*ref++;
			op=cpy;
			if (op == oend) break;    // Check EOF (should never happen, since last 5 bytes are supposed to be literals)
			continue;
		}
		LZ4_WILDCOPY(ref, op, cpy);
		op=cpy;		// correction
	}

	// end of decoding
	return (int) (((char*)ip)-source);

	// write overflow error detected
_output_error:
	return (int) (-(((char*)ip)-source));
}


int LZ4_uncompress_unknownOutputSize(
				char* source, 
				char* dest,
				int isize,
				int maxOutputSize)
{	
	// Local Variables
	const BYTE* restrict ip = (const BYTE*) source;
	const BYTE* const iend = ip + isize;
	const BYTE* restrict ref;

	BYTE* restrict op = (BYTE*) dest;
	BYTE* const oend = op + maxOutputSize;
	BYTE* cpy;

	BYTE token;
	
	int	len, length;
	size_t dec[] ={0, 3, 2, 3, 0, 0, 0, 0};


	// Main Loop
	while (ip<iend)
	{
		// get runlength
		token = *ip++;
		if ((length=(token>>ML_BITS)) == RUN_MASK)  { for (;(len=*ip++)==255;length+=255){} length += len; } 

		// copy literals
		cpy = op+length;
		if (cpy>oend-COPYLENGTH) 
		{ 
			if (cpy > oend) goto _output_error;
			memcpy(op, ip, length);
			op += length;
			break;    // Necessarily EOF
		}
		LZ4_WILDCOPY(ip, op, cpy); ip -= (op-cpy); op = cpy;
		if (ip>=iend) break;    // check EOF

		// get offset
		LZ4_READ_LITTLEENDIAN_16(ref,cpy,ip); ip+=2;
		if (ref < (BYTE* const)dest) goto _output_error;

		// get matchlength
		if ((length=(token&ML_MASK)) == ML_MASK) { for (;(len=*ip++)==255;length+=255){} length += len; }

		// copy repeated sequence
		if (op-ref<COPYSTEP)
		{
#if ARCH64
			size_t dec2table[]={0, 4, 4, 3, 4, 5, 6, 7};
			size_t dec2 = dec2table[op-ref];
#else
			int dec2 = 0;
#endif
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			ref -= dec[op-ref];
			A32(op)=A32(ref); op += COPYSTEP-4; ref += COPYSTEP-4;
			ref -= dec2;
		} else { LZ4_COPYSTEP(ref,op); }
		cpy = op + length - (COPYSTEP-4);
		if (cpy>oend-COPYLENGTH)
		{
			if (cpy > oend) goto _output_error;	
			LZ4_WILDCOPY(ref, op, (oend-COPYLENGTH));
			while(op<cpy) *op++=*ref++;
			op=cpy;
			if (op == oend) break;    // Check EOF (should never happen, since last 5 bytes are supposed to be literals)
			continue;
		}
		LZ4_WILDCOPY(ref, op, cpy);
		op=cpy;		// correction
	}

	// end of decoding
	return (int) (((char*)op)-dest);

	// write overflow error detected
_output_error:
	return (int) (-(((char*)ip)-source));
}

