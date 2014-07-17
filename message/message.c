/***
lua message C library used to test the lua-cmsgpack module.
Can also be used as a guideline as to what MessagePack needs from the supplied message userdata object.
@module message
*/

#include <malloc.h>

#include "lua.h"
#include "lauxlib.h"

static const char g_achMessageType[] = "Message_Test";

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

int l_free(lua_State* L);
int l_realloc(lua_State* L);

void setudlen(lua_State* L, int index, size_t len)
{
   // stash the size
   lua_getmetatable(L, index);
   lua_pushinteger(L, len);
   lua_setfield(L, -2, "len");
   lua_setmetatable(L, -2);
}

int l_alloc(lua_State* L)
{
   void** ppv = luaL_checkudata(L, -1, g_achMessageType);

   int iSize = luaL_checkint(L, -2);
   
   if (iSize==0)
      *ppv = NULL;

   else {
      *ppv = malloc(iSize);

      // pop the int, keep the ud
      lua_remove(L, -2);

      if (*ppv==NULL)
         lua_pushnil(L);
   }
   // stash the size
   setudlen(L, 1, iSize);

   return 1;
}

/***
Changes the size of the message's userdata payload.
Acts similarly as a typical realloc call.
@function realloc
@param msg_ud the message userdata object.
@tparam integer size the new size in bytes
@return the message userdata object
*/
int l_realloc(lua_State* L)
{
   void** ppck = (void**)luaL_checkudata(L, 1, g_achMessageType);

   size_t len = (size_t)luaL_checkinteger(L, 2);

   *ppck = realloc(*ppck, len);

   if (*ppck==NULL) {
      len = 0;
      lua_pushnil(L);
   }
   else
      lua_settop(L, 1);

   // stash the size
   setudlen(L, 1, len);
   
   return 1;
}

int l_free(lua_State* L)
{
   int iRet=1;
   void** ppv = (void**)luaL_checkudata(L, 1, g_achMessageType);

   if (*ppv!=NULL)
      free(*ppv);

   *ppv = 0;

   // stash the size
   setudlen(L, 1, 0);

   return 1;
}

/***
Stores the message's userdata size. 
This size can reflect either the total allocated size, or the encoded size inside the a larger allocated data payload.
@function __len
@param msg_ud the message userdata object
@tparam integer size the size in bytes
@return the message userdata object
*/
int l_len(lua_State* L)
{
   luaL_getmetafield(L, 1, "len");
   return 1;
}

/***
Sets the message's final packed size.
The buffer may have been asked to allocate more memory than necessary in its alloc and realloc calls.
SetSize gives the buffer the valid data size inside its allocated memory.
The message should report the size specified in setsize whenever asked by MessagePack.
@function setsize
@param msg_ud the message userdata containing the MessagePack encoded data.
@tparam integer size the final packed size
@return msg_ud the message userdata
*/
int l_setsize(lua_State* L)
{
   // stash the size
   setudlen(L, 1, (size_t)luaL_checkinteger(L, 2));
   lua_settop(L, 1);
   return 1;
}

int l_message(lua_State* L)
{
   void** ppv = (void**)lua_newuserdata(L, sizeof(void*));
   *ppv = NULL;

   // populate the ud metatable
   luaL_newmetatable(L, g_achMessageType);

   lua_pushstring(L, "__gc");
   lua_pushcfunction(L, l_free);
   lua_settable(L, -3);

   lua_pushstring(L, "alloc");
   lua_pushcfunction(L, l_alloc);
   lua_settable(L, -3);
   
   lua_pushstring(L, "realloc");
   lua_pushcfunction(L, l_realloc);
   lua_settable(L, -3);

   lua_pushstring(L, "free");
   lua_pushcfunction(L, l_free);
   lua_settable(L, -3);

   lua_pushstring(L, "setsize");
   lua_pushcfunction(L, l_setsize);
   lua_settable(L, -3);

   lua_pushstring(L, "__len");
   lua_pushcfunction(L, l_len);
   lua_settable(L, -3);

   lua_setmetatable(L, -2);
   return 1;
}

LUALIB_API int luaopen_message(lua_State *L)
{
   lua_newtable(L);
   
   lua_pushstring(L, "alloc");
   lua_pushcfunction(L, l_alloc);
   lua_settable(L, -3);

   lua_pushstring(L, "realloc");
   lua_pushcfunction(L, l_realloc);
   lua_settable(L, -3);

   lua_pushstring(L, "free");
   lua_pushcfunction(L, l_free);
   lua_settable(L, -3);

   lua_pushstring(L, "message");
   lua_pushcfunction(L, l_message);
   lua_settable(L, -3);

   return 1;
}