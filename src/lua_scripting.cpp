/*
 * lua_scripting.cpp
 *
 *  Created on: 2013-8-6
 *      Author: yinqiwen@gmail.com
 */
#include "ardb_server.hpp"
#include "lua_scripting.hpp"
#include "logger.hpp"
#include "util/rand.h"
#include <string.h>
#include <limits>

#define MAX_LUA_STR_SIZE 1024

namespace ardb
{
    extern "C"
    {
        int (luaopen_cjson)(lua_State *L);
        int (luaopen_struct)(lua_State *L);
        int (luaopen_cmsgpack)(lua_State *L);
    }

    /* Take a Redis reply in the Redis protocol format and convert it into a
     * Lua type. Thanks to this function, and the introduction of not connected
     * clients, it is trivial to implement the redis() lua function.
     *
     * Basically we take the arguments, execute the Redis command in the context
     * of a non connected client, then take the generated reply and convert it
     * into a suitable Lua type. With this trick the scripting feature does not
     * need the introduction of a full Redis internals API. Basically the script
     * is like a normal client that bypasses all the slow I/O paths.
     *
     * Note: in this function we do not do any sanity check as the reply is
     * generated by Redis directly. This allows us to go faster.
     * The reply string can be altered during the parsing as it is discarded
     * after the conversion is completed.
     *
     * Errors are returned as a table with a single 'err' field set to the
     * error string.
     */

    static void redisProtocolToLuaType(lua_State *lua, RedisReply& reply)
    {
        switch (reply.type)
        {
            case REDIS_REPLY_INTEGER:
            {
                lua_pushnumber(lua, (lua_Number) reply.integer);
                break;
            }
            case REDIS_REPLY_NIL:
            {
                lua_pushboolean(lua, 0);
                break;
            }
            case REDIS_REPLY_STRING:
            {
                lua_pushlstring(lua, reply.str.data(), reply.str.size());
                break;
            }
            case REDIS_REPLY_STATUS:
            {
                lua_newtable(lua);
                lua_pushstring(lua, "ok");
                lua_pushlstring(lua, reply.str.data(), reply.str.size());
                lua_settable(lua, -3);
                break;
            }
            case REDIS_REPLY_ERROR:
            {
                lua_newtable(lua);
                lua_pushstring(lua, "err");
                lua_pushlstring(lua, reply.str.data(), reply.str.size());
                lua_settable(lua, -3);
                break;
            }
            case REDIS_REPLY_ARRAY:
            {
//				if (reply.elements.empty())
//				{
//					lua_pushboolean(lua, 0);
//					return;
//				}
                lua_newtable(lua);
                for (uint32 j = 0; j < reply.elements.size(); j++)
                {
                    lua_pushnumber(lua, j + 1);
                    redisProtocolToLuaType(lua, reply.elements[j]);
                    lua_settable(lua, -3);
                }
                break;
            }
            default:
            {
                break;
            }
        }
    }

    /* Set an array of Redis String Objects as a Lua array (table) stored into a
     * global variable. */
    static void luaSetGlobalArray(lua_State *lua, const std::string& var, SliceArray& elev)
    {
        uint32 j;

        lua_newtable(lua);
        for (j = 0; j < elev.size(); j++)
        {
            lua_pushlstring(lua, elev[j].data(), elev[j].size());
            lua_rawseti(lua, -2, j + 1);
        }
        lua_setglobal(lua, var.c_str());
    }

    /* This function installs metamethods in the global table _G that prevent
     * the creation of globals accidentally.
     *
     * It should be the last to be called in the scripting engine initialization
     * sequence, because it may interact with creation of globals. */
    static void scriptingEnableGlobalsProtection(lua_State *lua)
    {
        const char *s[32];
        std::string code;
        int j = 0;

        /* strict.lua from: http://metalua.luaforge.net/src/lib/strict.lua.html.
         * Modified to be adapted to Redis. */
        s[j++] = "local mt = {}\n";
        s[j++] = "setmetatable(_G, mt)\n";
        s[j++] = "mt.__newindex = function (t, n, v)\n";
        s[j++] = "  if debug.getinfo(2) then\n";
        s[j++] = "    local w = debug.getinfo(2, \"S\").what\n";
        s[j++] = "    if w ~= \"main\" and w ~= \"C\" then\n";
        s[j++] = "      error(\"Script attempted to create global variable '\"..tostring(n)..\"'\", 2)\n";
        s[j++] = "    end\n";
        s[j++] = "  end\n";
        s[j++] = "  rawset(t, n, v)\n";
        s[j++] = "end\n";
        s[j++] = "mt.__index = function (t, n)\n";
        s[j++] = "  if debug.getinfo(2) and debug.getinfo(2, \"S\").what ~= \"C\" then\n";
        s[j++] = "    error(\"Script attempted to access unexisting global variable '\"..tostring(n)..\"'\", 2)\n";
        s[j++] = "  end\n";
        s[j++] = "  return rawget(t, n)\n";
        s[j++] = "end\n";
        s[j++] = NULL;

        for (j = 0; s[j] != NULL; j++)
        {
            code.append(s[j]);
        }
        luaL_loadbuffer(lua, code.c_str(), code.size(), "@enable_strict_lua");
        lua_pcall(lua, 0, 0, 0);
    }
    static void luaLoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc)
    {
        lua_pushcfunction(lua, luafunc);
        lua_pushstring(lua, libname);
        lua_call(lua, 1, 0);
    }

    static void luaPushError(lua_State *lua, const char *error)
    {
        lua_Debug dbg;

        lua_newtable(lua);
        lua_pushstring(lua, "err");

        /* Attempt to figure out where this function was called, if possible */
        if (lua_getstack(lua, 1, &dbg) && lua_getinfo(lua, "nSl", &dbg))
        {
            char tmp[MAX_LUA_STR_SIZE];
            snprintf(tmp, MAX_LUA_STR_SIZE - 1, "%s: %d: %s", dbg.source, dbg.currentline, error);
            lua_pushstring(lua, tmp);
        }
        else
        {
            lua_pushstring(lua, error);
        }
        lua_settable(lua, -3);
    }

    static int luaReplyToRedisReply(lua_State *lua, RedisReply& reply)
    {
        int t = lua_type(lua, -1);
        switch (t)
        {
            case LUA_TSTRING:
            {
                reply.type = REDIS_REPLY_STRING;
                reply.str.append((char*) lua_tostring(lua, -1), lua_strlen(lua, -1));
                break;
            }
            case LUA_TBOOLEAN:
                if (lua_toboolean(lua, -1))
                {
                    reply.type = REDIS_REPLY_INTEGER;
                    reply.integer = 1;
                }
                else
                {
                    reply.type = REDIS_REPLY_NIL;
                }
                break;
            case LUA_TNUMBER:
                reply.type = REDIS_REPLY_INTEGER;
                reply.integer = (long long) lua_tonumber(lua, -1);
                break;
            case LUA_TTABLE:
                /* We need to check if it is an array, an error, or a status reply.
                 * Error are returned as a single element table with 'err' field.
                 * Status replies are returned as single element table with 'ok' field */
                lua_pushstring(lua, "err");
                lua_gettable(lua, -2);
                t = lua_type(lua, -1);
                if (t == LUA_TSTRING)
                {
                    std::string err = lua_tostring(lua, -1);
                    string_replace(err, "\r\n", " ");
                    reply.type = REDIS_REPLY_ERROR;
                    reply.str = err;
                    lua_pop(lua, 2);
                    return 0;
                }

                lua_pop(lua, 1);
                lua_pushstring(lua, "ok");
                lua_gettable(lua, -2);
                t = lua_type(lua, -1);
                if (t == LUA_TSTRING)
                {
                    std::string ok = lua_tostring(lua, -1);
                    string_replace(ok, "\r\n", " ");
                    reply.str = ok;
                    reply.type = REDIS_REPLY_STATUS;
                    lua_pop(lua, 1);
                }
                else
                {
                    //void *replylen = addDeferredMultiBulkLength(c);
                    int j = 1, mbulklen = 0;

                    lua_pop(lua, 1);
                    /* Discard the 'ok' field value we popped */
                    reply.type = REDIS_REPLY_ARRAY;
                    while (1)
                    {
                        lua_pushnumber(lua, j++);
                        lua_gettable(lua, -2);
                        t = lua_type(lua, -1);
                        if (t == LUA_TNIL)
                        {
                            lua_pop(lua, 1);
                            break;
                        }
                        RedisReply r;
                        luaReplyToRedisReply(lua, r);
                        reply.elements.push_back(r);
                        mbulklen++;
                    }
                }
                break;
            default:
            {
                reply.type = REDIS_REPLY_NIL;
                break;
            }

        }
        lua_pop(lua, 1);
        return 0;
    }
    ArdbServer* LUAInterpreter::m_server = NULL;
    std::string LUAInterpreter::m_killing_func;
    LUAInterpreter::LUAInterpreter(ArdbServer* server) :
            m_lua(NULL)
    {
        m_server = server;
        Init();
    }

    /* Define a lua function with the specified function name and body.
     * The function name musts be a 2 characters long string, since all the
     * functions we defined in the Lua context are in the form:
     *
     *   f_<hex sha1 sum>
     *
     * On success REDIS_OK is returned, and nothing is left on the Lua stack.
     * On error REDIS_ERR is returned and an appropriate error is set in the
     * client context. */
    int LUAInterpreter::CreateLuaFunction(const std::string& funcname, const std::string& body, std::string& err)
    {
        std::string funcdef = "function ";
        funcdef.append(funcname);
        funcdef.append("() ");
        funcdef.append(body);
        funcdef.append(" end");

        if (luaL_loadbuffer(m_lua, funcdef.c_str(), funcdef.size(), "@user_script"))
        {
            err.append("Error compiling script (new function): ").append(lua_tostring(m_lua, -1)).append("\n");
            lua_pop(m_lua, 1);
            return -1;
        }
        if (lua_pcall(m_lua, 0, 0, 0))
        {
            err.append("Error running script (new function): ").append(lua_tostring(m_lua, -1)).append("\n");
            lua_pop(m_lua, 1);
            return -1;
        }

        /* We also save a SHA1 -> Original script map in a dictionary
         * so that we can replicate / write in the AOF all the
         * EVALSHA commands as EVAL using the original script. */
        m_server->m_db->SaveScript(funcname, body);
        return 0;
    }

    int LUAInterpreter::LoadLibs()
    {
        luaLoadLib(m_lua, "", luaopen_base);
        luaLoadLib(m_lua, LUA_TABLIBNAME, luaopen_table);
        luaLoadLib(m_lua, LUA_STRLIBNAME, luaopen_string);
        luaLoadLib(m_lua, LUA_MATHLIBNAME, luaopen_math);
        luaLoadLib(m_lua, LUA_DBLIBNAME, luaopen_debug);

        luaLoadLib(m_lua, "cjson", luaopen_cjson);
        luaLoadLib(m_lua, "struct", luaopen_struct);
        luaLoadLib(m_lua, "cmsgpack", luaopen_cmsgpack);
        return 0;
    }

    int LUAInterpreter::RemoveUnsupportedFunctions()
    {
        lua_pushnil(m_lua);
        lua_setglobal(m_lua, "loadfile");
        return 0;
    }

    int LUAInterpreter::CallArdb(lua_State *lua, bool raise_error)
    {
        int j, argc = lua_gettop(lua);
        ArgumentArray cmdargs;

        /* Require at least one argument */
        if (argc == 0)
        {
            luaPushError(lua, "Please specify at least one argument for redis.call()");
            return 1;
        }

        /* Build the arguments vector */
        for (j = 0; j < argc; j++)
        {
            if (!lua_isstring(lua, j + 1))
                break;
            std::string arg;
            arg.append(lua_tostring(lua, j + 1), lua_strlen(lua, j + 1));
            cmdargs.push_back(arg);
        }

        /* Check if one of the arguments passed by the Lua script
         * is not a string or an integer (lua_isstring() return true for
         * integers as well). */
        if (j != argc)
        {
            luaPushError(lua, "Lua redis() command arguments must be strings or integers");
            return 1;
        }

        /* Setup our fake client for command execution */

        RedisCommandFrame cmd(cmdargs);
        lower_string(cmd.GetMutableCommand());
        ArdbServer::RedisCommandHandlerSetting* setting = m_server->FindRedisCommandHandlerSetting(cmd.GetCommand());
        /* Command lookup */
        if (NULL == setting)
        {
            luaPushError(lua, "Unknown Redis command called from Lua script");
            return -1;
        }

        /* There are commands that are not allowed inside scripts. */
        if (setting->flags & ARDB_CMD_NOSCRIPT)
        {
            luaPushError(lua, "This Redis command is not allowed from scripts");
            return -1;
        }

        //TODO consider forbid readonly slave to exec write cmd

        ArdbConnContext* ctx = m_server->m_ctx_local.GetValue();
        LUAInterpreter& interpreter = m_server->m_ctx_lua.GetValue(NULL, NULL);
        RedisReply& reply = ctx->reply;
        reply.Clear();
        m_server->ProcessRedisCommand(*ctx, cmd,
        ARDB_PROCESS_WITHOUT_REPLICATION);
        if (raise_error && reply.type != REDIS_REPLY_ERROR)
        {
            raise_error = 0;
        }
        redisProtocolToLuaType(interpreter.m_lua, reply);

        if (raise_error)
        {
            /* If we are here we should have an error in the stack, in the
             * form of a table with an "err" field. Extract the string to
             * return the plain error. */
            lua_pushstring(lua, "err");
            lua_gettable(lua, -2);
            return lua_error(lua);
        }
        return 1;
    }

    int LUAInterpreter::PCall(lua_State *lua)
    {
        return CallArdb(lua, true);
    }

    int LUAInterpreter::Call(lua_State *lua)
    {
        return CallArdb(lua, false);
    }

    int LUAInterpreter::Log(lua_State *lua)
    {
        int j, argc = lua_gettop(lua);
        int level;
        std::string log;
        if (argc < 2)
        {
            luaPushError(lua, "redis.log() requires two arguments or more.");
            return 1;
        }
        else if (!lua_isnumber(lua, -argc))
        {
            luaPushError(lua, "First argument must be a number (log level).");
            return 1;
        }
        level = (int) lua_tonumber(lua, -argc);
        if (level < FATAL_LOG_LEVEL || level > TRACE_LOG_LEVEL)
        {
            luaPushError(lua, "Invalid debug level.");
            return 1;
        }

        /* Glue together all the arguments */
        for (j = 1; j < argc; j++)
        {
            size_t len;
            char *s;

            s = (char*) lua_tolstring(lua, (-argc) + j, &len);
            if (s)
            {
                if (j != 1)
                {
                    log.append(" ");
                }
                log.append(s, len);
            }
        }
        LOG_WITH_LEVEL((LogLevel )level, "%s", log.c_str());
        return 0;
    }

    int LUAInterpreter::SHA1Hex(lua_State *lua)
    {
        int argc = lua_gettop(lua);
        size_t len;
        char *s;

        if (argc != 1)
        {
            luaPushError(lua, "wrong number of arguments");
            return 1;
        }

        s = (char*) lua_tolstring(lua, 1, &len);
        std::string digest = sha1_sum_data(s, len);
        lua_pushstring(lua, digest.c_str());
        return 1;
    }

    int LUAInterpreter::ReturnSingleFieldTable(lua_State *lua, const std::string& field)
    {
        if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING)
        {
            luaPushError(lua, "wrong number or type of arguments");
            return 1;
        }

        lua_newtable(lua);
        lua_pushstring(lua, field.c_str());
        lua_pushvalue(lua, -3);
        lua_settable(lua, -3);
        return 1;
    }

    int LUAInterpreter::ErrorReplyCommand(lua_State *lua)
    {
        return ReturnSingleFieldTable(lua, "err");
    }
    int LUAInterpreter::StatusReplyCommand(lua_State *lua)
    {
        return ReturnSingleFieldTable(lua, "ok");
    }
    int LUAInterpreter::MathRandom(lua_State *L)
    {
        /* the `%' avoids the (rare) case of r==1, and is needed also because on
         some systems (SunOS!) `rand()' may return a value larger than RAND_MAX */
        lua_Number r = (lua_Number) (redisLrand48() % REDIS_LRAND48_MAX) / (lua_Number) REDIS_LRAND48_MAX;
        switch (lua_gettop(L))
        { /* check number of arguments */
            case 0:
            { /* no arguments */
                lua_pushnumber(L, r); /* Number between 0 and 1 */
                break;
            }
            case 1:
            { /* only upper limit */
                int u = luaL_checkint(L, 1);
                luaL_argcheck(L, 1 <= u, 1, "interval is empty");
                lua_pushnumber(L, floor(r * u) + 1); /* int between 1 and `u' */
                break;
            }
            case 2:
            { /* lower and upper limits */
                int l = luaL_checkint(L, 1);
                int u = luaL_checkint(L, 2);
                luaL_argcheck(L, l <= u, 2, "interval is empty");
                lua_pushnumber(L, floor(r * (u - l + 1)) + l); /* int between `l' and `u' */
                break;
            }
            default:
                return luaL_error(L, "wrong number of arguments");
        }
        return 1;
    }
    int LUAInterpreter::MathRandomSeed(lua_State *lua)
    {
        redisSrand48(luaL_checkint(lua, 1));
        return 0;
    }

    void LUAInterpreter::MaskCountHook(lua_State *lua, lua_Debug *ar)
    {
        ARDB_NOTUSED(ar);
        ARDB_NOTUSED(lua);
        ArdbConnContext* ctx = m_server->m_ctx_local.GetValue();
        uint64 elapsed = get_current_epoch_millis() - ctx->GetLua().lua_time_start;
        if (elapsed >= (uint64) m_server->m_cfg.lua_time_limit && !ctx->GetLua().lua_timeout)
        {
            WARN_LOG(
                    "Lua slow script detected: %s still in execution after %llu milliseconds. You can try killing the script using the SCRIPT KILL command.",
                    ctx->GetLua().lua_executing_func, elapsed);
            ctx->GetLua().lua_timeout = true;
        }
        if (ctx->GetLua().lua_timeout)
        {
            ctx->conn->GetService().Continue();
        }
        if (ctx->GetLua().lua_kill)
        {
            WARN_LOG("Lua script killed by user with SCRIPT KILL.");
            lua_pushstring(lua, "Script killed by user with SCRIPT KILL...");
            lua_error(lua);
        }
    }

    int LUAInterpreter::Init()
    {
        m_lua = lua_open();

        LoadLibs();
        RemoveUnsupportedFunctions();

        /* Register the redis commands table and fields */
        lua_newtable(m_lua);

        /* redis.call */
        lua_pushstring(m_lua, "call");
        lua_pushcfunction(m_lua, LUAInterpreter::Call);
        lua_settable(m_lua, -3);

        /* redis.pcall */
        lua_pushstring(m_lua, "pcall");
        lua_pushcfunction(m_lua, LUAInterpreter::PCall);
        lua_settable(m_lua, -3);

        /* redis.log and log levels. */
        lua_pushstring(m_lua, "log");
        lua_pushcfunction(m_lua, LUAInterpreter::Log);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_DEBUG");
        lua_pushnumber(m_lua, DEBUG_LOG_LEVEL);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_VERBOSE");
        lua_pushnumber(m_lua, TRACE_LOG_LEVEL);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_NOTICE");
        lua_pushnumber(m_lua, INFO_LOG_LEVEL);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "LOG_WARNING");
        lua_pushnumber(m_lua, WARN_LOG_LEVEL);
        lua_settable(m_lua, -3);

        /* redis.sha1hex */
        lua_pushstring(m_lua, "sha1hex");
        lua_pushcfunction(m_lua, LUAInterpreter::SHA1Hex);
        lua_settable(m_lua, -3);

        /* redis.error_reply and redis.status_reply */
        lua_pushstring(m_lua, "error_reply");
        lua_pushcfunction(m_lua, LUAInterpreter::ErrorReplyCommand);
        lua_settable(m_lua, -3);
        lua_pushstring(m_lua, "status_reply");
        lua_pushcfunction(m_lua, LUAInterpreter::StatusReplyCommand);
        lua_settable(m_lua, -3);

        /* Finally set the table as 'redis' global var. */
        lua_setglobal(m_lua, "redis");
        lua_getglobal(m_lua, "redis");
        lua_setglobal(m_lua, "ardb");

        /* Replace math.random and math.randomseed with our implementations. */
        lua_getglobal(m_lua, LUA_MATHLIBNAME);
        if (lua_isnil(m_lua, -1))
        {
            ERROR_LOG("Failed to load lib math");
        }
        lua_pushstring(m_lua, "random");
        lua_pushcfunction(m_lua, LUAInterpreter::MathRandom);
        lua_settable(m_lua, -3);

        lua_pushstring(m_lua, "randomseed");
        lua_pushcfunction(m_lua, LUAInterpreter::MathRandomSeed);
        lua_settable(m_lua, -3);

        lua_setglobal(m_lua, "math");

        /* Add a helper function we use for pcall error reporting.
         * Note that when the error is in the C function we want to report the
         * information about the caller, that's what makes sense from the point
         * of view of the user debugging a script. */
        {
            const char *errh_func = "function __redis__err__handler(err)\n"
                    "  local i = debug.getinfo(2,'nSl')\n"
                    "  if i and i.what == 'C' then\n"
                    "    i = debug.getinfo(3,'nSl')\n"
                    "  end\n"
                    "  if i then\n"
                    "    return i.source .. ':' .. i.currentline .. ': ' .. err\n"
                    "  else\n"
                    "    return err\n"
                    "  end\n"
                    "end\n";
            luaL_loadbuffer(m_lua, errh_func, strlen(errh_func), "@err_handler_def");
            lua_pcall(m_lua, 0, 0, 0);
        }

        scriptingEnableGlobalsProtection(m_lua);
        return 0;
    }

    int LUAInterpreter::Eval(const std::string& func, SliceArray& keys, SliceArray& args, bool isSHA1Func,
            RedisReply& reply)
    {
        DEBUG_LOG("Exec script:%s", func.c_str());
        redisSrand48(0);
        std::string err;
        std::string funcname = "f_";
        const std::string* funptr = &func;
        if (isSHA1Func)
        {
            if (func.size() != 40)
            {
                reply.type = REDIS_REPLY_ERROR;
                reply.str = "-NOSCRIPT No matching script. Please use EVAL.";
                return -1;
            }
            funcname.append(func);
        }
        else
        {
            funcname.append(sha1_sum(func));
        }
        /* Push the pcall error handler function on the stack. */
        lua_getglobal(m_lua, "__redis__err__handler");

        lua_getglobal(m_lua, funcname.c_str());
        if (lua_isnil(m_lua, -1))
        {
            lua_pop(m_lua, 1);
            /* remove the nil from the stack */
            /* Function not defined... let's define it if we have the
             * body of the function. If this is an EVALSHA call we can just
             * return an error. */
            std::string cachedfunc;
            if (isSHA1Func)
            {
                if (!m_server->m_db->GetScript(funcname, cachedfunc))
                {
                    lua_pop(m_lua, 1);
                    /* remove the error handler from the stack. */
                    reply.type = REDIS_REPLY_ERROR;
                    reply.str = "-NOSCRIPT No matching script. Please use EVAL.";
                    return -1;
                }
                funptr = &cachedfunc;
            }
            if (CreateLuaFunction(funcname, *funptr, err))
            {
                reply.type = REDIS_REPLY_ERROR;
                reply.str = err;
                lua_pop(m_lua, 1);
                return -1;
            }
            lua_getglobal(m_lua, funcname.c_str());
        }

        /* Populate the argv and keys table accordingly to the arguments that
         * EVAL received. */
        luaSetGlobalArray(m_lua, "KEYS", keys);
        luaSetGlobalArray(m_lua, "ARGV", args);

        bool delhook = false;
        ArdbConnContext* ctx = m_server->m_ctx_local.GetValue();
        if (NULL != ctx && m_server->m_cfg.lua_time_limit > 0)
        {
            lua_sethook(m_lua, MaskCountHook, LUA_MASKCOUNT, 100000);
            delhook = true;
        }
        ctx->GetLua().lua_time_start = get_current_epoch_millis();
        ctx->GetLua().lua_executing_func = funcname.c_str() + 2;
        ctx->GetLua().lua_kill = false;
        int errid = lua_pcall(m_lua, 0, 1, -2);
        ctx->GetLua().lua_executing_func = NULL;
        if (delhook)
        {
            lua_sethook(m_lua, MaskCountHook, 0, 0); /* Disable hook */
        }
        if (ctx->GetLua().lua_timeout)
        {
            ctx->GetLua().lua_timeout = false;
        }
        lua_gc(m_lua, LUA_GCSTEP, 1);

        if (errid)
        {
            reply.type = REDIS_REPLY_ERROR;
            char tmp[1024];
            snprintf(tmp, 1023, "Error running script (call to %s): %s\n", funcname.c_str(), lua_tostring(m_lua, -1));
            reply.str = tmp;
            lua_pop(m_lua, 2);
            /*  Consume the Lua reply and remove error handler. */
        }
        else
        {
            /* On success convert the Lua return value into Redis reply */
            reply.Clear();
            luaReplyToRedisReply(m_lua, reply);
            lua_pop(m_lua, 1); /* Remove the error handler. */
        }

        return 0;
    }

    bool LUAInterpreter::Exists(const std::string& sha)
    {
        std::string funcname = "f_";
        funcname.append(sha);
        std::string funcbody;
        return m_server->m_db->GetScript(funcname, funcbody) == 0;
    }

    int LUAInterpreter::Load(const std::string& func, std::string& ret)
    {
        std::string funcname = "f_";
        ret.clear();
        ret = sha1_sum(func);
        funcname.append(ret);
        return CreateLuaFunction(funcname, func, ret) == 0;
    }

    void LUAInterpreter::Reset()
    {
        lua_close(m_lua);
        Init();
    }

    int LUAInterpreter::Flush()
    {
        m_server->m_service->FireUserEvent(SCRIPT_FLUSH_EVENT);
        return 0;
    }

    int LUAInterpreter::Kill(const std::string& funcname)
    {
        m_killing_func = funcname;
        m_server->m_service->FireUserEvent(SCRIPT_KILL_EVENT);
        return 0;
    }

    void LUAInterpreter::ScriptEventCallback(ChannelService* serv, uint32 ev, void* data)
    {
        ArdbServer* server = (ArdbServer*) data;
        ArdbConnContext* ctx = server->m_ctx_local.GetValue();
        if (NULL == ctx)
        {
            //no connection
            return;
        }
        switch (ev)
        {
            case SCRIPT_FLUSH_EVENT:
            {
                LUAInterpreter& lua = server->m_ctx_lua.GetValue(ArdbServer::LUAInterpreterCreator, server);
                lua.Reset();
                server->m_db->FlushScripts();
                break;
            }
            case SCRIPT_KILL_EVENT:
            {
                if (ctx->GetLua().lua_executing_func != NULL)
                {
                    if (!strcasecmp(m_killing_func.c_str(), "all")
                            || !strcasecmp(m_killing_func.c_str(), ctx->GetLua().lua_executing_func))
                    {
                        ctx->GetLua().lua_kill = true;
                    }
                }
                break;
            }
            default:
            {
                break;
            }
        }
    }

    LUAInterpreter::~LUAInterpreter()
    {
        lua_close(m_lua);
    }
}

