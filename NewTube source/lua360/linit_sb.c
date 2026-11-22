/* linit_sb.c - our version of Lua's linit.c: luaL_openlibs opens only the libraries the 360 build includes
   (base, table, string, math, io). Lua's own linit.c also opens package / os / debug, which we leave out. */
#define linit_c
#define LUA_LIB
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const luaL_Reg lualibs[] = {
    {"", luaopen_base},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_IOLIBNAME, luaopen_io},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {NULL, NULL}
};

LUALIB_API void luaL_openlibs(lua_State *L)
{
    const luaL_Reg *lib = lualibs;
    for (; lib->func; lib++) {
        lua_pushcfunction(L, lib->func);
        lua_pushstring(L, lib->name);
        lua_call(L, 1, 0);
    }
}
