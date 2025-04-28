#pragma once

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __GNUC__
// requires statement expression extension
#define MIN(A,B) ({__typeof(A) _A=(A); __typeof(B) _B=(B); _A<_B?_A:_B;})
#define MAX(A,B) ({__typeof(A) _A=(A); __typeof(B) _B=(B); _A>_B?_A:_B;})
#else
#define MIN(A,B) ((A)<(B)?(A):(B))
#define MAX(A,B) ((A)>(B)?(A):(B))
#endif

#define countof(x) ((intptr_t)(sizeof(x)/sizeof(x[0])))

void *memcpy(void * restrict dst, const void *restrict src, size_t);
void *memset(void *, int, size_t);
int memcmp(const void*, const void *, size_t);
size_t strlen(const char*);

void *malloc(size_t);
void *calloc(size_t count, size_t size);
void *realloc(void*, size_t);
void *reallocarray(void *, size_t n, size_t size);
void free(void*);
void *aligned_alloc(size_t alignment, size_t size);

#ifndef static_assert
#define static_assert _Static_assert
#endif

#define forp(B, N, P) for(typeof(B) P = B; P < (B + N); ++P)
#define incptr(P, N) P = (typeof(P))(((const char*)P) + N);

#define FOR(I,N) for(u32 I = 0; I < N; ++I)

#define foreach(I,X,ARR,N) \
	for(u32 I = 0, iter##I = 1; I < N ; ++I, iter##I=1)for(typeof(ARR) X = ARR + I; iter##I; iter##I=0)

#define foreachv(I,X,ARR,N) \
	for(u32 I = 0, iter##I = 1; I < N ; ++I, iter##I=1)for(typeof((ARR)[0]) X = ARR[I]; iter##I; iter##I=0)

#define KB(X) ((X) << 10)
#define MB(X) ((X) << 20)
#define GB(X) ((X) << 30)

#define unused(x) (void)x;

typedef unsigned int uint;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;

#define alignpower2(x, align) ((x+(align-1)) & (~(align-1)))

#if !defined(HAVE___BUILTIN_EXPECT)
#  define __builtin_expect(x, y) (x)
#endif
#ifndef likely
#  ifdef HAVE___BUILTIN_EXPECT
#    define likely(x)   __builtin_expect(!!(x), 1)
#    define unlikely(x) __builtin_expect(!!(x), 0)
#  else
#    define likely(x)   x
#    define unlikely(x) x
#  endif
#endif

//# generic memory based operations

#define zerostruct(X) memset(X, 0, sizeof(*(X)))
#define zeromem(M,N) memset(M, 0, N)
#define memeq(a,b,n) (0==memcmp(a,b,n))
void *memset(void*, int, unsigned long);

//# strings and slices

typedef struct {
	const char *str;
	int32_t len;
} Slice;

typedef struct {
	u8 len;
	char str[255];
} ShortStr;

void shortStrSet(ShortStr *, const char *, size_t n);

#define SLICE(X,Y) (Slice){.str=X,.len=Y}
#define S(X) (Slice){.str=X,.len=sizeof(X)-1}

static inline _Bool sliceEq(Slice a, Slice b){
	return a.len == b.len && memeq(a.str, b.str, a.len);
}
static inline int sliceCmp(Slice a, Slice b) {
	uint32_t m = MIN(a.len, b.len);
	int d = memcmp(a.str, b.str, m);
	if(d)
		return d;
	if(a.len < b.len)
		return -1;
	if(a.len > b.len)
		return 1;
	return 0;
}

char *readFile(const char *path, size_t *len);
const char *parseFloat(const char *s, float *restrict out);
const char *parseI32(const char* s, int32_t* restrict out);
const char *parseU32(const char* s, uint32_t* restrict out);

//# printing


typedef struct Iobuf {
	char *buf;
	int count;
	int cap;
	FILE *fp;
} Iobuf;

typedef struct Dummy {} Dummy;

#define print_item(iobuf,x) _Generic(x,\
	i32: printI32,\
	i64: printI64,\
	u32: printU32,\
	u64: printU64,\
	char*: printCstr,\
	const char*: printCstr,\
	float: printFloat,\
	Slice: printSlice,\
	Dummy*: printDummy\
	)(&iobuf,x)


#define pItem2(iobuf,x) __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(x), Dummy*), (void)0, print_item(iobuf,x))

#define STUB(X) do{eprintln("STUBBED:", X);}while(0)
#define notimpl assertm(0, "not implemented");

#define _print(iobuf,a,b,c,d,e,f,g,h,i,j,k,l,m,...) pItem2(iobuf,a);pItem2(iobuf,b);pItem2(iobuf,c);pItem2(iobuf,d);pItem2(iobuf,e);pItem2(iobuf,f);pItem2(iobuf,g);pItem2(iobuf,h);pItem2(iobuf,i);pItem2(iobuf,j);pItem2(iobuf,k);pItem2(iobuf,l);pItem2(iobuf,m)
#define sprint(iobuf,a...) _print(iobuf, a, (Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0,(Dummy*)0)

static inline void iobufFlush(Iobuf *buf) { 
	fwrite(buf->buf, 1, (size_t)buf->count, buf->fp);
	buf->count = 0;
}

#define print(a...)\
	do{\
		char tmpbuf[2048];\
		Iobuf iobuf={tmpbuf,0,sizeof(tmpbuf), stdout};\
		sprint(iobuf, a);\
		iobufFlush(&iobuf);\
	}while(0)

#define println(a...) print(a, "\n")

#define eprint(a...)\
	do{\
		char tmpbuf[2048];\
		Iobuf iobuf={tmpbuf,0,sizeof(tmpbuf), stderr};\
		sprint(iobuf, a);\
		iobufFlush(&iobuf);\
	}while(0)

#define eprintln(a...) eprint(a, "\n")

#ifndef NDEBUG
#define assert(x) if(!(x)){onFailedAssert(#x, __FILE__, __LINE__);}
#define assertm(x,m...) if(!(x)){eprintln(m);onFailedAssert(#x, __FILE__, __LINE__);}
#else
#define assert(x)
#define assertm(x,m...)
#endif

static inline void printDummy(Iobuf *buf, Dummy *d) { unused(buf); unused(d); }

void onFailedAssert(const char *cond, const char *file, long line);
void onFailedBoundsCheck(u32 idx, u32 cap, const char *file, long line);

void printU32(Iobuf *buf, u32 x);
void printU64(Iobuf *buf, u64 x);
void printI32(Iobuf *buf, i32 x);
void printI64(Iobuf *buf, i64 x);
void printFloat(Iobuf *buf, float x);
void printCstr(Iobuf *buf, const char *);
void printSlice(Iobuf *buf, Slice s);
