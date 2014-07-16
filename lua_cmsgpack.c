#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "lua.h"
#include "lauxlib.h"

#define LUACMSGPACK_VERSION     "lua-cmsgpack 0.3.1"
#define LUACMSGPACK_COPYRIGHT   "Copyright (C) 2012, Salvatore Sanfilippo"
#define LUACMSGPACK_DESCRIPTION "MessagePack C implementation for Lua"

#define LUACMSGPACK_MAX_NESTING  16 /* Max tables nesting. */

/* ==============================================================================
 * MessagePack implementation and bindings for Lua 5.1/5.2.
 * Copyright(C) 2012 Salvatore Sanfilippo <antirez@gmail.com>
 *
 * http://github.com/antirez/lua-cmsgpack
 *
 * For MessagePack specification check the following web site:
 * http://wiki.msgpack.org/display/MSGPACK/Format+specification
 *
 * See Copyright Notice at the end of this file.
 *
 * CHANGELOG:
 * 19-Feb-2012 (ver 0.1.0): Initial release.
 * 20-Feb-2012 (ver 0.2.0): Tables encoding improved.
 * 20-Feb-2012 (ver 0.2.1): Minor bug fixing.
 * 20-Feb-2012 (ver 0.3.0): Module renamed lua-cmsgpack (was lua-msgpack).
 * 04-Apr-2014 (ver 0.3.1): Lua 5.2 support and minor bug fix.
 * ============================================================================ */

/* --------------------------- Endian conversion --------------------------------
 * We use it only for floats and doubles, all the other conversions are performed
 * in an endian independent fashion. So the only thing we need is a function
 * that swaps a binary string if the arch is little endian (and left it untouched
 * otherwise). */

/* Reverse memory bytes if arch is little endian. Given the conceptual
 * simplicity of the Lua build system we prefer to check for endianess at runtime.
 * The performance difference should be acceptable. */
static void memrevifle(void *ptr, size_t len) {
    unsigned char *p = ptr, *e = p+len-1, aux;
    int test = 1;
    unsigned char *testp = (unsigned char*) &test;

    if (testp[0] == 0) return; /* Big endian, nothign to do. */
    len /= 2;
    while(len--) {
        aux = *p;
        *p = *e;
        *e = aux;
        p++;
        e--;
    }
}

int dump_stack (lua_State *L, const char * msg)
{
   int i;
   int top = lua_gettop(L); /*depth of the stack*/

   printf("\n%s:\n--------\n", msg ? msg : "Dumping stack: ");
   for(i= 1; i <= top; i++) {
      int t = lua_type(L, i);
      printf("%d:\t", i);
      switch(t){
      case LUA_TSTRING:  /* strings */
         printf("'%s'", lua_tostring(L,i));
         break;
      case LUA_TBOOLEAN:  /*boolean values*/
         printf(lua_toboolean(L,i) ? "true" : "false");
         break;
      case LUA_TNUMBER:  /* numbers */
         printf("%g", lua_tonumber(L, i));
         break;
      case LUA_TUSERDATA:
         printf("%s - 0x%08X", lua_typename(L,t), lua_touserdata(L, i));
         break;
      case LUA_TLIGHTUSERDATA:
         printf("lightuserdata - 0x%08X", lua_touserdata(L, i));
         break;
      default:  /*anything else*/
         printf("%s", lua_typename(L,t));
         break;
      }
      printf("\n"); /* put in a separator */
   }
   printf("--------\n"); /* end of listing separator */
   return 0;
}

/* ----------------------------- String buffer ----------------------------------
 * This is a simple implementation of string buffers. The only opereation
 * supported is creating empty buffers and appending bytes to it.
 * The string buffer uses 2x preallocation on every realloc for O(N) append
 * behavior.  */

static void* l_alloc(lua_State* L, size_t _Size)
{
   luaL_getmetafield(L, 1, "alloc");
   lua_pushvalue(L, 1);
   lua_pushinteger(L, _Size);
   lua_call(L, 2, 1);
   // it modified our current ud, so just look it up here
   lua_pop(L, 1);
   return *(void**)lua_touserdata(L, 1);
}

static void* l_realloc(lua_State* L, size_t _NewSize)
{
   luaL_getmetafield(L, 1, "realloc");
   lua_pushvalue(L, 1);
   lua_pushinteger(L, _NewSize);
   lua_call(L, 2, 1);
   // it modified our current ud, so just look it up here
   lua_pop(L, 1);
   return *(void**)lua_touserdata(L, 1);
}

static void l_free(lua_State* L, void * _Memory)
{
   luaL_getmetafield(L, 1, "free");
   lua_pushvalue(L, 1);
   lua_pushlightuserdata(L, _Memory);
   lua_call(L, 2, 1);
   // reset our ud payload
   *(void**)lua_touserdata(L, 1)=NULL;
}

typedef struct mp_buf {
    unsigned char *b;
    size_t len, free;
    lua_State* L;
} mp_buf;

static mp_buf *mp_buf_new(lua_State* L) {
    mp_buf *buf = malloc(sizeof(*buf));
    
    buf->b = NULL;
    buf->len = buf->free = 0;
    buf->L = L;
    return buf;
}

void mp_buf_append(mp_buf *buf, const unsigned char *s, size_t len) {
    if (buf->free < len) {
        size_t newlen = buf->len+len;

      if (buf->L)
         buf->b = l_realloc(buf->L,newlen*2);
      else
         buf->b = realloc(buf->b,newlen*2);
      buf->free = newlen;
    }
    memcpy(buf->b+buf->len,s,len);
    buf->len += len;
    buf->free -= len;
}

void mp_buf_free(mp_buf *buf) {
   // only free the data payload if the allocator is local
   if (buf->L==NULL)
      free(buf->b);
    free(buf);
}

/* ------------------------------ String cursor ----------------------------------
 * This simple data structure is used for parsing. Basically you create a cursor
 * using a string pointer and a length, then it is possible to access the
 * current string position with cursor->p, check the remaining length
 * in cursor->left, and finally consume more string using
 * mp_cur_consume(cursor,len), to advance 'p' and subtract 'left'.
 * An additional field cursor->error is set to zero on initialization and can
 * be used to report errors. */

#define MP_CUR_ERROR_NONE   0
#define MP_CUR_ERROR_EOF    1   /* Not enough data to complete the opereation. */
#define MP_CUR_ERROR_BADFMT 2   /* Bad data format */

typedef struct mp_cur {
    const unsigned char *p;
    size_t left;
    int err;
} mp_cur;

static mp_cur *mp_cur_new(const unsigned char *s, size_t len) {
    mp_cur *cursor = malloc(sizeof(*cursor));

    cursor->p = s;
    cursor->left = len;
    cursor->err = MP_CUR_ERROR_NONE;
    return cursor;
}

static void mp_cur_free(mp_cur *cursor) {
    free(cursor);
}

#define mp_cur_consume(_c,_len) do { _c->p += _len; _c->left -= _len; } while(0)

/* When there is not enough room we set an error in the cursor and return, this
 * is very common across the code so we have a macro to make the code look
 * a bit simpler. */
#define mp_cur_need(_c,_len) do { \
    if (_c->left < _len) { \
        _c->err = MP_CUR_ERROR_EOF; \
        return; \
    } \
} while(0)

/* --------------------------- Low level MP encoding -------------------------- */

static void mp_encode_null(mp_buf* buf) {
	unsigned char b[1];

	b[0] = 0xc0;
	mp_buf_append(buf,b,1);
}

static void mp_encode_binary(mp_buf *buf, const unsigned char *s, size_t len) {
	unsigned char hdr[5];
	int hdrlen = 0;
	
	if (len < 255) {
		// 2^8-1
		hdr[0] = 0xc4;
		hdr[1] = len;
		hdrlen = 2;
	} else if (len < 65535) {
		// 2^16-1
		hdr[0] = 0xc5;
		hdr[1] = (len&0xff00)>>8;
		hdr[2] = len&0xff;
		hdrlen = 3;
	} else if (len < 4294967295) {
		// 2^32-1
		hdr[0] = 0xc6;
		hdr[1] = (len&0xff000000)>>24;
		hdr[2] = (len&0xff0000)>>16;
		hdr[3] = (len&0xff00)>>8;
		hdr[4] = len&0xff;
		hdrlen = 5;
	} else
		mp_encode_null(buf);
	
	if (hdrlen>0) {
		mp_buf_append(buf,hdr,hdrlen);
		mp_buf_append(buf,s,len);
	}
}

static void mp_encode_bytes(mp_buf *buf, const unsigned char *s, size_t len) {
    unsigned char hdr[5];
    int hdrlen;

    if (len < 32) {
        hdr[0] = 0xa0 | (len&0xff); /* fix raw */
        hdrlen = 1;
    } else if (len <= 0xffff) {
        hdr[0] = 0xda;
        hdr[1] = (len&0xff00)>>8;
        hdr[2] = len&0xff;
        hdrlen = 3;
    } else {
        hdr[0] = 0xdb;
        hdr[1] = (len&0xff000000)>>24;
        hdr[2] = (len&0xff0000)>>16;
        hdr[3] = (len&0xff00)>>8;
        hdr[4] = len&0xff;
        hdrlen = 5;
    }
    mp_buf_append(buf,hdr,hdrlen);
    mp_buf_append(buf,s,len);
}

/* we assume IEEE 754 internal format for single and double precision floats. */
static void mp_encode_double(mp_buf *buf, double d) {
    unsigned char b[9];
    float f = d;

    assert(sizeof(f) == 4 && sizeof(d) == 8);
    if (d == (double)f) {
        b[0] = 0xca;    /* float IEEE 754 */
        memcpy(b+1,&f,4);
        memrevifle(b+1,4);
        mp_buf_append(buf,b,5);
    } else if (sizeof(d) == 8) {
        b[0] = 0xcb;    /* double IEEE 754 */
        memcpy(b+1,&d,8);
        memrevifle(b+1,8);
        mp_buf_append(buf,b,9);
    }
}

static void mp_encode_int(mp_buf *buf, int64_t n) {
    unsigned char b[9];
    int enclen;

    if (n >= 0) {
        if (n <= 127) {
            b[0] = n & 0x7f;    /* positive fixnum */
            enclen = 1;
        } else if (n <= 0xff) {
            b[0] = 0xcc;        /* uint 8 */
            b[1] = n & 0xff;
            enclen = 2;
        } else if (n <= 0xffff) {
            b[0] = 0xcd;        /* uint 16 */
            b[1] = (n & 0xff00) >> 8;
            b[2] = n & 0xff;
            enclen = 3;
        } else if (n <= 0xffffffffLL) {
            b[0] = 0xce;        /* uint 32 */
            b[1] = (n & 0xff000000) >> 24;
            b[2] = (n & 0xff0000) >> 16;
            b[3] = (n & 0xff00) >> 8;
            b[4] = n & 0xff;
            enclen = 5;
        } else {
            b[0] = 0xcf;        /* uint 64 */
            b[1] = (n & 0xff00000000000000LL) >> 56;
            b[2] = (n & 0xff000000000000LL) >> 48;
            b[3] = (n & 0xff0000000000LL) >> 40;
            b[4] = (n & 0xff00000000LL) >> 32;
            b[5] = (n & 0xff000000) >> 24;
            b[6] = (n & 0xff0000) >> 16;
            b[7] = (n & 0xff00) >> 8;
            b[8] = n & 0xff;
            enclen = 9;
        }
    } else {
        if (n >= -32) {
            b[0] = ((char)n);   /* negative fixnum */
            enclen = 1;
        } else if (n >= -128) {
            b[0] = 0xd0;        /* int 8 */
            b[1] = n & 0xff;
            enclen = 2;
        } else if (n >= -32768) {
            b[0] = 0xd1;        /* int 16 */
            b[1] = (n & 0xff00) >> 8;
            b[2] = n & 0xff;
            enclen = 3;
        } else if (n >= -2147483648LL) {
            b[0] = 0xd2;        /* int 32 */
            b[1] = (n & 0xff000000) >> 24;
            b[2] = (n & 0xff0000) >> 16;
            b[3] = (n & 0xff00) >> 8;
            b[4] = n & 0xff;
            enclen = 5;
        } else {
            b[0] = 0xd3;        /* int 64 */
            b[1] = (n & 0xff00000000000000LL) >> 56;
            b[2] = (n & 0xff000000000000LL) >> 48;
            b[3] = (n & 0xff0000000000LL) >> 40;
            b[4] = (n & 0xff00000000LL) >> 32;
            b[5] = (n & 0xff000000) >> 24;
            b[6] = (n & 0xff0000) >> 16;
            b[7] = (n & 0xff00) >> 8;
            b[8] = n & 0xff;
            enclen = 9;
        }
    }
    mp_buf_append(buf,b,enclen);
}

static void mp_encode_array(mp_buf *buf, int64_t n) {
    unsigned char b[5];
    int enclen;

    if (n <= 15) {
        b[0] = 0x90 | (n & 0xf);    /* fix array */
        enclen = 1;
    } else if (n <= 65535) {
        b[0] = 0xdc;                /* array 16 */
        b[1] = (n & 0xff00) >> 8;
        b[2] = n & 0xff;
        enclen = 3;
    } else {
        b[0] = 0xdd;                /* array 32 */
        b[1] = (n & 0xff000000) >> 24;
        b[2] = (n & 0xff0000) >> 16;
        b[3] = (n & 0xff00) >> 8;
        b[4] = n & 0xff;
        enclen = 5;
    }
    mp_buf_append(buf,b,enclen);
}

static void mp_encode_map(mp_buf *buf, int64_t n) {
    unsigned char b[5];
    int enclen;

    if (n <= 15) {
        b[0] = 0x80 | (n & 0xf);    /* fix map */
        enclen = 1;
    } else if (n <= 65535) {
        b[0] = 0xde;                /* map 16 */
        b[1] = (n & 0xff00) >> 8;
        b[2] = n & 0xff;
        enclen = 3;
    } else {
        b[0] = 0xdf;                /* map 32 */
        b[1] = (n & 0xff000000) >> 24;
        b[2] = (n & 0xff0000) >> 16;
        b[3] = (n & 0xff00) >> 8;
        b[4] = n & 0xff;
        enclen = 5;
    }
    mp_buf_append(buf,b,enclen);
}

/* ----------------------------- Lua types encoding --------------------------- */

static void mp_encode_lua_string(lua_State *L, mp_buf *buf) {
    size_t len;
    const char *s;

    s = lua_tolstring(L,-1,&len);
    mp_encode_bytes(buf,(const unsigned char*)s,len);
}

static void mp_encode_lua_bool(lua_State *L, mp_buf *buf) {
    unsigned char b = lua_toboolean(L,-1) ? 0xc3 : 0xc2;
    mp_buf_append(buf,&b,1);
}

static void mp_encode_lua_number(lua_State *L, mp_buf *buf) {
    lua_Number n = lua_tonumber(L,-1);

    if (floor(n) != n) {
        mp_encode_double(buf,(double)n);
    } else {
        mp_encode_int(buf,(int64_t)n);
    }
}

static void mp_encode_lua_type(lua_State *L, mp_buf *buf, int level);

static void mp_encode_lua_userdata(lua_State *L, mp_buf *buf, int level) {
	size_t size;
	const unsigned char* pod;
	
	// here I expect the ud to have a metatable with __len defined
	lua_len(L, -1);
	size = lua_tointeger(L, -1) ;
	lua_pop(L, 1);
	// plain old data buffer is at index [1]
	lua_pushinteger(L, 1);
	lua_gettable(L ,-2);
	pod = (const unsigned char*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	// encode null if the size is undefined, meaning the ud won't be packed
	if (size>0)
		mp_encode_binary(buf, pod, size);
	else
		mp_encode_null(buf);
}

static void mp_encode_lua_table_as_lightuserdata(lua_State *L, mp_buf *buf) {

	size_t len;
	const char* pv;
	
	// we're getting {[1]=lud, [2]=size}
	lua_pushinteger(L, 1);
	lua_gettable(L, -2);

	pv = (const char*)lua_touserdata(L, -1);
	
	lua_pushinteger(L, 2);
	lua_gettable(L, -2);
	len = lua_tointeger(L, -1);
	lua_pop(L, 2);

	mp_encode_binary(buf, pv, len);
}

/* Convert a lua table into a message pack list. */
static void mp_encode_lua_table_as_array(lua_State *L, mp_buf *buf, int level) {
#if LUA_VERSION_NUM < 502
    size_t len = lua_objlen(L,-1), j;
#else
    size_t len = lua_rawlen(L,-1), j;
#endif

    mp_encode_array(buf,len);
    for (j = 1; j <= len; j++) {
        lua_pushnumber(L,j);
        lua_gettable(L,-2);
        mp_encode_lua_type(L,buf,level+1);
    }
}

/* Convert a lua table into a message pack key-value map. */
static void mp_encode_lua_table_as_map(lua_State *L, mp_buf *buf, int level) {
	size_t len = 0;

    /* First step: count keys into table. No other way to do it with the
     * Lua API, we need to iterate a first time. Note that an alternative
     * would be to do a single run, and then hack the buffer to insert the
     * map opcodes for message pack. Too hachish for this lib. */
    lua_pushnil(L);
    while(lua_next(L,-2)) {
        lua_pop(L,1); /* remove value, keep key for next iteration. */
        len++;
    }

    /* Step two: actually encoding of the map. */
    mp_encode_map(buf,len);
    lua_pushnil(L);
    while(lua_next(L,-2)) {
        /* Stack: ... key value */
        lua_pushvalue(L,-2); /* Stack: ... key value key */
        mp_encode_lua_type(L,buf,level+1); /* encode key */
        mp_encode_lua_type(L,buf,level+1); /* encode val */
    }
}

/* Returns true if the Lua table on top of the stack is exclusively composed
 * of keys from numerical keys from 1 up to N, with N being the total number
 * of elements, without any hole in the middle. */
static int table_is_an_array(lua_State *L) {
    long count = 0, idx = 0;
    lua_Number n;

    lua_pushnil(L);
    while(lua_next(L,-2)) {
        /* Stack: ... key value */
        lua_pop(L,1); /* Stack: ... key */
        if (lua_type(L,-1) != LUA_TNUMBER) goto not_array;
        n = lua_tonumber(L,-1);
        idx = n;
        if (idx != n || idx < 1) goto not_array;
        count++;
    }
    /* We have the total number of elements in "count". Also we have
     * the max index encountered in "idx". We can't reach this code
     * if there are indexes <= 0. If you also note that there can not be
     * repeated keys into a table, you have that if idx==count you are sure
     * that there are all the keys form 1 to count (both included). */
    return idx == count;

not_array:
    lua_pop(L,1);
    return 0;
}

/* If the length operator returns non-zero, that is, there is at least
 * an object at key '1', we serialize to message pack list. Otherwise
 * we use a map. */
static void mp_encode_lua_table(lua_State *L, mp_buf *buf, int level) {
    if (table_is_an_array(L)) {
		int aiType[2];
		// lightuserdata values are encoded as binaries, provided they are presented inside a table along with their size value
		// ex: {[1]=lud, [2]=size}
		// [1]
		lua_pushinteger(L, 1);
		lua_gettable(L, -2);
		aiType[0] = lua_type(L, -1);
		lua_pop(L, 1);
		// [2]
		lua_pushinteger(L, 2);
		lua_gettable(L, -2);
		aiType[1] = lua_type(L, -1);
		lua_pop(L, 1);

		if ((aiType[0]==LUA_TLIGHTUSERDATA) && (aiType[1]==LUA_TNUMBER))
			mp_encode_lua_table_as_lightuserdata(L, buf);
		else
        mp_encode_lua_table_as_array(L,buf,level);
	 }
    else
        mp_encode_lua_table_as_map(L,buf,level);
}

static void mp_encode_lua_null(lua_State *L, mp_buf *buf) {
	mp_encode_null(buf);
}

static void mp_encode_lua_type(lua_State *L, mp_buf *buf, int level) {
    int t = lua_type(L,-1);

    /* Limit the encoding of nested tables to a specfiied maximum depth, so that
     * we survive when called against circular references in tables. */
    if (t == LUA_TTABLE && level == LUACMSGPACK_MAX_NESTING) t = LUA_TNIL;
    switch(t) {
    case LUA_TSTRING: mp_encode_lua_string(L,buf); break;
    case LUA_TBOOLEAN: mp_encode_lua_bool(L,buf); break;
    case LUA_TNUMBER: mp_encode_lua_number(L,buf); break;
    case LUA_TTABLE: mp_encode_lua_table(L,buf,level); break;
	 case LUA_TUSERDATA: mp_encode_lua_userdata(L, buf, level); break;
    default: mp_encode_lua_null(L,buf); break;
    }
    lua_pop(L,1);
}

static int _mp_pack(lua_State* L, lua_State* bufL) {
   mp_buf *buf;

   // the message should be on top of the stack, with its allocators...
   // if not we'll use the default allocators
   buf = mp_buf_new(bufL);

   mp_encode_lua_type(L,buf,0);
   if (bufL==NULL)
      lua_pushlstring(L,(char*)buf->b,buf->len);
   else {
      // update the size to compensate for O(n)
      luaL_getmetafield(L, 1, "setsize");
      lua_pushvalue(L, 1);
      lua_pushinteger(L, buf->len);
      lua_call(L, 2, 0);
   }
   mp_buf_free(buf);

   return 1;
}

static int mp_pack(lua_State *L) {
   return _mp_pack(L, NULL);
}

static int mp_packmessage(lua_State *L) {
   int res;
   res = _mp_pack(L, L);
   return res;
}

/* --------------------------------- Decoding --------------------------------- */

void mp_decode_to_lua_type(lua_State *L, mp_cur *c);

void mp_decode_to_lua_array(lua_State *L, mp_cur *c, size_t len) {
    int index = 1;

    lua_newtable(L);
    while(len--) {
        lua_pushnumber(L,index++);
        mp_decode_to_lua_type(L,c);
        if (c->err) return;
        lua_settable(L,-3);
    }
}

void mp_decode_to_lua_hash(lua_State *L, mp_cur *c, size_t len) {
    lua_newtable(L);
    while(len--) {
        mp_decode_to_lua_type(L,c); /* key */
        if (c->err) return;
        mp_decode_to_lua_type(L,c); /* value */
        if (c->err) return;
        lua_settable(L,-3);
    }
}

typedef struct {
	char* pod;
	size_t len;
} userdatapod;

static int userdatapod_gc(lua_State* L) {
	userdatapod *pudpod = (userdatapod*)lua_touserdata(L, -1);
	if ((pudpod != NULL) && (pudpod->pod != NULL)) {
		free(pudpod->pod);
		pudpod->len = 0;
		pudpod->pod = NULL;
	}
	return 0;
}

static int userdatapod_len(lua_State* L) {
	userdatapod *pudpod = (userdatapod*)lua_touserdata(L, -1);
	// return the number of bytes
	lua_pushinteger(L, pudpod->len);
	return 1;
}

static void alloc_userdata(lua_State* L, const void* in_pod, const size_t in_size) {
	userdatapod *pudpod;
	// plain old data, allocated in this process space, must be explicitly freed or it will leak
	// the ud represents a place holder for the memory address and its size
	// the ud.__gc will delete the memory pointed to by the address
	// ud.__len is defined, it returns the number of bytes pointed to by the ud address
	// NOTE: I also define the # operator, which will return the pod size in bytes
	pudpod = (userdatapod*)lua_newuserdata(L, sizeof(userdatapod));
	pudpod->len = in_size;
	// alloc the memory
	pudpod->pod = (char*)malloc(pudpod->len);
	// copy the data
	memcpy(pudpod->pod, in_pod, in_size);
	// create a mt
	luaL_newmetatable(L, "userdatapod");
	// garbage collector
	lua_pushstring(L, "__gc");
	lua_pushcfunction(L, userdatapod_gc);
	lua_settable(L, -3);
	// # operator
	lua_pushstring(L, "__len");
	lua_pushcfunction(L, userdatapod_len);
	lua_settable(L, -3);
	// [1]=data
	lua_pushinteger(L, 1);
	lua_pushlightuserdata(L, pudpod->pod);
	lua_settable(L, -3);
	// [2]=len
	lua_pushinteger(L, 2);
	lua_pushinteger(L, pudpod->len);
	lua_settable(L, -3);
	// set the mt
	lua_setmetatable(L, -2);
}

void alloc_lightuserdata_table(lua_State* L, const void* in_pod, const size_t in_size) {
	char* pod;

	// plain old data, allocated in this process space, must be explicitly freed or it will leak
	// format for lightuserdata POD packing is {lud, size}
	// NOTE: I could use userdata and define a __gc... but that would put the ownership in lua's hands
	lua_newtable(L);
	lua_pushinteger(L, 1);
	// allocate the memory
	pod = (char*)malloc(in_size);
	// copy the data
	memcpy(pod, in_pod, in_size);
	// give lua the address
	lua_pushlightuserdata(L, pod);
	lua_settable(L, -3);
	// it needs the size too
	lua_pushinteger(L, 2);
	lua_pushinteger(L, in_size);
	lua_settable(L, -3);
}

/* Decode a Message Pack raw object pointed by the string cursor 'c' to
 * a Lua type, that is left as the only result on the stack. */
void mp_decode_to_lua_type(lua_State *L, mp_cur *c) {
    mp_cur_need(c,1);
    switch(c->p[0]) {
    case 0xcc:  /* uint 8 */
        mp_cur_need(c,2);
        lua_pushnumber(L,c->p[1]);
        mp_cur_consume(c,2);
        break;
    case 0xd0:  /* int 8 */
        mp_cur_need(c,2);
        lua_pushnumber(L,(char)c->p[1]);
        mp_cur_consume(c,2);
        break;
    case 0xcd:  /* uint 16 */
        mp_cur_need(c,3);
        lua_pushnumber(L,
            (c->p[1] << 8) |
             c->p[2]);
        mp_cur_consume(c,3);
        break;
    case 0xd1:  /* int 16 */
        mp_cur_need(c,3);
        lua_pushnumber(L,(int16_t)
            (c->p[1] << 8) |
             c->p[2]);
        mp_cur_consume(c,3);
        break;
    case 0xce:  /* uint 32 */
        mp_cur_need(c,5);
        lua_pushnumber(L,
            ((uint32_t)c->p[1] << 24) |
            ((uint32_t)c->p[2] << 16) |
            ((uint32_t)c->p[3] << 8) |
             (uint32_t)c->p[4]);
        mp_cur_consume(c,5);
        break;
    case 0xd2:  /* int 32 */
        mp_cur_need(c,5);
        lua_pushnumber(L,
            ((int32_t)c->p[1] << 24) |
            ((int32_t)c->p[2] << 16) |
            ((int32_t)c->p[3] << 8) |
             (int32_t)c->p[4]);
        mp_cur_consume(c,5);
        break;
    case 0xcf:  /* uint 64 */
        mp_cur_need(c,9);
        lua_pushnumber(L,
            ((uint64_t)c->p[1] << 56) |
            ((uint64_t)c->p[2] << 48) |
            ((uint64_t)c->p[3] << 40) |
            ((uint64_t)c->p[4] << 32) |
            ((uint64_t)c->p[5] << 24) |
            ((uint64_t)c->p[6] << 16) |
            ((uint64_t)c->p[7] << 8) |
             (uint64_t)c->p[8]);
        mp_cur_consume(c,9);
        break;
    case 0xd3:  /* int 64 */
        mp_cur_need(c,9);
        lua_pushnumber(L,
            ((int64_t)c->p[1] << 56) |
            ((int64_t)c->p[2] << 48) |
            ((int64_t)c->p[3] << 40) |
            ((int64_t)c->p[4] << 32) |
            ((int64_t)c->p[5] << 24) |
            ((int64_t)c->p[6] << 16) |
            ((int64_t)c->p[7] << 8) |
             (int64_t)c->p[8]);
        mp_cur_consume(c,9);
        break;
    case 0xc0:  /* nil */
        lua_pushnil(L);
        mp_cur_consume(c,1);
        break;
    case 0xc3:  /* true */
        lua_pushboolean(L,1);
        mp_cur_consume(c,1);
        break;
    case 0xc2:  /* false */
        lua_pushboolean(L,0);
        mp_cur_consume(c,1);
        break;
    case 0xca:  /* float */
        mp_cur_need(c,5);
        assert(sizeof(float) == 4);
        {
            float f;
            memcpy(&f,c->p+1,4);
            memrevifle(&f,4);
            lua_pushnumber(L,f);
            mp_cur_consume(c,5);
        }
        break;
    case 0xcb:  /* double */
        mp_cur_need(c,9);
        assert(sizeof(double) == 8);
        {
            double d;
            memcpy(&d,c->p+1,8);
            memrevifle(&d,8);
            lua_pushnumber(L,d);
            mp_cur_consume(c,9);
        }
        break;
    case 0xda:  /* raw 16 */
        mp_cur_need(c,3);
        {
            size_t l = (c->p[1] << 8) | c->p[2];
            mp_cur_need(c,3+l);
            lua_pushlstring(L,(char*)c->p+3,l);
            mp_cur_consume(c,3+l);
        }
        break;
    case 0xdb:  /* raw 32 */
        mp_cur_need(c,5);
        {
            size_t l = (c->p[1] << 24) |
                       (c->p[2] << 16) |
                       (c->p[3] << 8) |
                       c->p[4];
            mp_cur_need(c,5+l);
            lua_pushlstring(L,(char*)c->p+5,l);
            mp_cur_consume(c,5+l);
        }
        break;
    case 0xdc:  /* array 16 */
        mp_cur_need(c,3);
        {
            size_t l = (c->p[1] << 8) | c->p[2];
            mp_cur_consume(c,3);
            mp_decode_to_lua_array(L,c,l);
        }
        break;
    case 0xdd:  /* array 32 */
        mp_cur_need(c,5);
        {
            size_t l = (c->p[1] << 24) |
                       (c->p[2] << 16) |
                       (c->p[3] << 8) |
                       c->p[4];
            mp_cur_consume(c,5);
            mp_decode_to_lua_array(L,c,l);
        }
        break;
    case 0xde:  /* map 16 */
        mp_cur_need(c,3);
        {
            size_t l = (c->p[1] << 8) | c->p[2];
            mp_cur_consume(c,3);
            mp_decode_to_lua_hash(L,c,l);
        }
        break;
    case 0xdf:  /* map 32 */
        mp_cur_need(c,5);
        {
            size_t l = (c->p[1] << 24) |
                       (c->p[2] << 16) |
                       (c->p[3] << 8) |
                       c->p[4];
            mp_cur_consume(c,5);
            mp_decode_to_lua_hash(L,c,l);
        }
        break;
	 case 0xc4: /* bin 8 */
		 mp_cur_need(c,2);
		 {
			 const size_t l = c->p[1];
			 mp_cur_need(c,2+l);
			 alloc_userdata(L, c->p+2, l);
			 mp_cur_consume(c,2+l);
		 }
		 break;
	 case 0xc5: /* bin 16 */
		 mp_cur_need(c,3);
		 {
			 const size_t l = (c->p[1]<<8) | c->p[2];
			 mp_cur_need(c,3+l);
			 alloc_userdata(L, c->p+3, l);
			 mp_cur_consume(c,3+l);
		 }
		 break;
	 case 0xc6: /* bin 32 */
		 mp_cur_need(c,5);
		 {
			const size_t l = (c->p[1] << 24) | (c->p[2] << 16) | (c->p[3] << 8) | c->p[4];
			mp_cur_need(c,5+l);
			alloc_userdata(L, c->p+5, l);
			mp_cur_consume(c,5+l);
		 }
		 break;
    default:    /* types that can't be idenitified by first byte value. */
        if ((c->p[0] & 0x80) == 0) {   /* positive fixnum */
            lua_pushnumber(L,c->p[0]);
            mp_cur_consume(c,1);
        } else if ((c->p[0] & 0xe0) == 0xe0) {  /* negative fixnum */
            lua_pushnumber(L,(signed char)c->p[0]);
            mp_cur_consume(c,1);
        } else if ((c->p[0] & 0xe0) == 0xa0) {  /* fix raw */
            size_t l = c->p[0] & 0x1f;
            mp_cur_need(c,1+l);
            lua_pushlstring(L,(char*)c->p+1,l);
            mp_cur_consume(c,1+l);
        } else if ((c->p[0] & 0xf0) == 0x90) {  /* fix map */
            size_t l = c->p[0] & 0xf;
            mp_cur_consume(c,1);
            mp_decode_to_lua_array(L,c,l);
        } else if ((c->p[0] & 0xf0) == 0x80) {  /* fix map */
            size_t l = c->p[0] & 0xf;
            mp_cur_consume(c,1);
            mp_decode_to_lua_hash(L,c,l);
        } else {
            c->err = MP_CUR_ERROR_BADFMT;
        }
    }
}

static int _mp_unpack(lua_State *L, const char* data, size_t len) {
    const unsigned char *s = data;
    mp_cur *c;

    c = mp_cur_new(s,len);
    mp_decode_to_lua_type(L,c);
    
    if (c->err == MP_CUR_ERROR_EOF) {
        mp_cur_free(c);
        lua_pushstring(L,"Missing bytes in input.");
        lua_error(L);
    } else if (c->err == MP_CUR_ERROR_BADFMT) {
        mp_cur_free(c);
        lua_pushstring(L,"Bad data format in input.");
        lua_error(L);
    } else if (c->left != 0) {
        mp_cur_free(c);
        lua_pushstring(L,"Extra bytes in input.");
        lua_error(L);
    } else {
        mp_cur_free(c);
    }
    return 1;
}

static int mp_unpack(lua_State *L)
{
   size_t len;
   const unsigned char *s;

   if (!lua_isstring(L,-1)) {
      lua_pushstring(L,"MessagePack decoding needs a string as input.");
      lua_error(L);
   }

   s = (const unsigned char*) lua_tolstring(L,-1,&len);
   
   return _mp_unpack(L, s, len);
}

static int mp_unpackmessage(lua_State *L) 
{
  size_t len;
  void** ppv = (void**)lua_touserdata(L, -1);
  void* data = *ppv;

   // use the # operator
   lua_len(L, 1);
   len = lua_tointeger(L, -1);
   lua_pop(L, 1);

   return _mp_unpack(L, data, len);
}

/* ---------------------------------------------------------------------------- */

#if LUA_VERSION_NUM < 502
static const struct luaL_reg thislib[] = {
#else
static const struct luaL_Reg thislib[] = {
#endif
    {"pack", mp_pack},
    {"unpack", mp_unpack},
    {"packmessage", mp_packmessage},
    {"unpackmessage", mp_unpackmessage},
    {NULL, NULL}
};

LUALIB_API int luaopen_cmsgpack(lua_State *L) {
#if LUA_VERSION_NUM < 502
    luaL_register(L, "cmsgpack", thislib);
#else
    luaL_newlib(L, thislib);
#endif

    lua_pushliteral(L, LUACMSGPACK_VERSION);
    lua_setfield(L, -2, "_VERSION");
    lua_pushliteral(L, LUACMSGPACK_COPYRIGHT);
    lua_setfield(L, -2, "_COPYRIGHT");
    lua_pushliteral(L, LUACMSGPACK_DESCRIPTION);
    lua_setfield(L, -2, "_DESCRIPTION"); 
    return 1;
}

/******************************************************************************
* Copyright (C) 2012 Salvatore Sanfilippo.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/
