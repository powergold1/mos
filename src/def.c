#include "def.h"

static bool iobufAppend(Iobuf *buf, const char *s, int n)
{
	while(n > 0){
		int to_copy = buf->cap - buf->count;
		if( to_copy > n )
			to_copy = n;
		memcpy(buf->buf + buf->count, s, to_copy);
		buf->count += to_copy;
		assert(buf->count <= buf->cap);
		s += to_copy;
		n -= to_copy;
		if( buf->count == buf->cap && buf->fp){
			iobufFlush(buf);
		} else {
			return 0;
		}
	}
	return 1;
}

void printU32(Iobuf *buf, u32 x) { 
	char tmp[32];
	char *p = tmp + sizeof(tmp);
	do {
		u32 digit = x % 10;
		*(--p) = (char)(digit + '0');
		x /= 10;
	} while(x);
	size_t count = (tmp + sizeof(tmp)) - p;
	iobufAppend(buf, p, count);
}

void printU64(Iobuf *buf, u64 x) { 
	char tmp[32];
	char *p = tmp + sizeof(tmp);
	do {
		u64 digit = x % 10;
		*(--p) = (char)(digit + '0');
		x /= 10;
	} while(x);
	size_t count = (tmp + sizeof(tmp)) - p;
	iobufAppend(buf, p, count);
}

void printI64(Iobuf *buf, i64 x) { 
	char tmp[32];
	char *p = tmp + sizeof(tmp);
	if( x == (i64)0x8000000000000000LL ){
		Slice s = S("-9223372036854775808");
		iobufAppend(buf, s.str, s.len);
		return;
	}
	bool neg = x < 0;
	if( neg )
		x = -x;
	do {
		i64 digit = x % 10;
		*(--p) = (char)(digit + '0');
		x /= 10;
	} while(x);
	if( neg )
		*(--p) = '-';
	size_t count = (tmp + sizeof(tmp)) - p;
	iobufAppend(buf, p, count);
}

void printI32(Iobuf *buf, i32 x) { 
	char tmp[32];
	char *p = tmp + sizeof(tmp);
	if( x == (i32)0x80000000 ){
		Slice s = S("-2147483648");
		iobufAppend(buf, s.str, s.len);
		return;
	}
	bool neg = x < 0;
	if( neg )
		x = -x;
	do {
		i32 digit = x % 10;
		*(--p) = (char)(digit + '0');
		x /= 10;
	} while(x);
	if( neg )
		*(--p) = '-';
	size_t count = (tmp + sizeof(tmp)) - p;
	iobufAppend(buf, p, count);
}

char *gcvt(double, int, char*);
void printFloat(Iobuf *buf, float x)
{
	char tmp[32];
	gcvt((double)x, 6, tmp);
	//fmtFloat(6, (double)x, tmp);
	int count = (int)strlen(tmp);
	iobufAppend(buf, tmp, count);
}


void printCstr(Iobuf *buf, const char *s)
{
	size_t len = strlen(s);
	iobufAppend(buf, s, len);
}


void printSlice(Iobuf *buf, Slice s)
{
	iobufAppend(buf, s.str, s.len);
}

[[noreturn]] void abort(void);

void onFailedBoundsCheck(u32 idx, u32 cap, const char *file, long line)
{
	char tmpbuf[2048];
	Iobuf o={tmpbuf,0,sizeof(tmpbuf),stderr};
	printCstr(&o, file);
	printSlice(&o, S(":"));
	printI64(&o, line);
	printSlice(&o, S(": Array index "));
	printU32(&o, idx);
	printSlice(&o, S(" out of bounds. Array has "));
	printU32(&o, cap);
	printSlice(&o, S(" elements\n"));
	iobufFlush(&o);
	//__builtin_trap();
	abort();
}


void onFailedAssert(const char *cond, const char *file, long line)
{
	char tmpbuf[2048];
	Iobuf o={tmpbuf,0,sizeof(tmpbuf),stderr};
	printCstr(&o, file);
	printSlice(&o, S(":"));
	printI64(&o, line);
	printSlice(&o, S(": Assertion `"));
	printCstr(&o, cond);
	printSlice(&o, S("` failed\n"));
	iobufFlush(&o);
	//__builtin_trap();
	abort();
}


char *readFile(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if(!f){
		eprintln("failed to open file ", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long l = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(l < 0) {
		fclose(f);
		return NULL;
	}
	size_t s = (size_t)l;
	char *res = malloc(s);
	size_t got = fread(res, 1, s, f);
	fclose(f);
	if(got != s){
		eprintln("file size mismatch, expected ", (i64)l, ", but got ", (u64)s);
		free(res);
		return NULL;
	}
	if(len)
		*len = s;
	return res;
}


const char* parseI32(const char* s, int32_t* restrict out)
{
	return NULL;
}

const char* parseU32(const char* s, uint32_t* restrict out)
{
	u8 d = (u8)*s - 0x30u;
	u32 res = 0;
	if(d > 9)
		return NULL;
	res = d;
	while(1){
		s++;
		d = (u8)*s - 0x30u;
		if(d > 9)
			break;
		res = res * 10 + d;
	}
	*out = res;
	return s;
}

const char* parseFloat(const char* s, float* restrict out)
{
	enum {Maxpower=20};
	static const f64 pospower[Maxpower] = {
		1.0e0,  1.0e1,  1.0e2,  1.0e3,  1.0e4,  1.0e5,  1.0e6,  1.0e7,  1.0e8,  1.0e9,
		1.0e10, 1.0e11, 1.0e12, 1.0e13, 1.0e14, 1.0e15, 1.0e16, 1.0e17, 1.0e18, 1.0e19,
	};
	static const f64 negpower[Maxpower] = {
		1.0e0,   1.0e-1,  1.0e-2,  1.0e-3,  1.0e-4,  1.0e-5,  1.0e-6,  1.0e-7,  1.0e-8,  1.0e-9,
		1.0e-10, 1.0e-11, 1.0e-12, 1.0e-13, 1.0e-14, 1.0e-15, 1.0e-16, 1.0e-17, 1.0e-18, 1.0e-19,
	};
	union {
		f64 f;
		u64 i;
	} u;
	u64 signmask;
	f64 num;
	f64 fra;
	f64 div;
	u32 eval;
	const f64 *powers;

	char c = *s;
	if(c=='+'){
		signmask = 0;
		s++;
	}else if(c=='-'){
		signmask = 0x8000000000000000ULL;
		s++;
	}else{
		signmask = 0;
	}

	num = 0.0;
	while(1) {
		unsigned v = ((unsigned)(*s)-0x30);
		if(v>=10)
			break;
		num = 10.0 * num + (f64)v;
		s++;
	}

	if(*s == '.') {
		s++;
		fra = 0.0;
		div = 1.0;
		while(1) {
			unsigned v = ((unsigned)(*s)-0x30);
			if(v>=10)
				break;
			fra = 10.0 * fra + (f64)v;
			s++;
			div *= 10.0;
		}
		num += fra / div;
	}

	c = *s;
	if(c=='e'||c=='E'){
		s++;
		c=*s;
		if(c=='+') {
			powers=pospower;
			s++;
		}else if(c=='-'){
			powers=negpower;
			s++;
		} else {
			powers=pospower;
		}

		eval = 0;
		while (1) {
			c=*s;
			unsigned v = ((unsigned)c-0x30);
			if(v>=10)
				break;
			eval = 10 * eval + v;
			s++;
		}

		num *= (eval >= Maxpower) ? 0.0 : powers[eval];
	}

	u.f = num;
	u.i |= signmask;
	*out = (float)(u.f);

	return s;
}

void shortStrSet(ShortStr *s, const char *p, size_t n)
{
	assert(n<=255);
	memcpy(s->str, p, n);
	s->len = (u8)n;
}
