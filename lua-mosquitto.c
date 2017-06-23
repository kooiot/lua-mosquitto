/*

  lua-mosquitto.c - Lua bindings to libmosquitto

  Copyright (c) 2014 Bart Van Der Meerssche <bart@flukso.net>
                     Natanael Copa <ncopa@alpinelinux.org>
                     Karl Palsson <karlp@remake.is>

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

/***
 * Lua bindings to libmosquitto
 *
 * This documentation is partial, and doesn't cover all functionality yet.
 * @module mosquitto
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <mosquitto.h>

#include "compat.h"

/* re-using mqtt3 message types as callback types */
#define CONNECT		0x10
#define PUBLISH		0x30
#define SUBSCRIBE	0x80
#define UNSUBSCRIBE	0xA0
#define DISCONNECT	0xE0
/* add two extra callback types */
#define MESSAGE		0x01
#define LOG			0x02

enum connect_return_codes {
	CONN_ACCEPT,
	CONN_REF_BAD_PROTOCOL,
	CONN_REF_BAD_ID,
	CONN_REF_SERVER_NOAVAIL,
	CONN_REF_BAD_LOGIN,
	CONN_REF_NO_AUTH,
	CONN_REF_BAD_TLS
};

/* unique naming for userdata metatables */
#define MOSQ_META_CTX	"mosquitto.ctx"

typedef struct {
	lua_State *L;
	struct mosquitto *mosq;
	int on_connect;
	int on_disconnect;
	int on_publish;
	int on_message;
	int on_subscribe;
	int on_unsubscribe;
	int on_log;
} ctx_t;

static int mosq_initialized = 0;

/* handle mosquitto lib return codes */
static int mosq__pstatus(lua_State *L, int mosq_errno) {
	switch (mosq_errno) {
		case MOSQ_ERR_SUCCESS:
			lua_pushboolean(L, true);
			return 1;
			break;

		case MOSQ_ERR_INVAL:
		case MOSQ_ERR_NOMEM:
		case MOSQ_ERR_PROTOCOL:
		case MOSQ_ERR_NOT_SUPPORTED:
			return luaL_error(L, mosquitto_strerror(mosq_errno));
			break;

		case MOSQ_ERR_NO_CONN:
		case MOSQ_ERR_CONN_LOST:
		case MOSQ_ERR_PAYLOAD_SIZE:
			lua_pushnil(L);
			lua_pushinteger(L, mosq_errno);
			lua_pushstring(L, mosquitto_strerror(mosq_errno));
			return 3;
			break;

		case MOSQ_ERR_ERRNO:
			lua_pushnil(L);
			lua_pushinteger(L, errno);
			lua_pushstring(L, strerror(errno));
			return 3;
			break;
	}

	return 0;
}

/***
 * Library functions
 * @section lib_functions
 */

/***
 * Return mosquitto library version.
 * @function version
 * @treturn string version string "major.minor.revision"
 * @see mosquitto_lib_version
 */
static int mosq_version(lua_State *L)
{
	int major, minor, rev;
	char version[16];

	mosquitto_lib_version(&major, &minor, &rev);
	sprintf(version, "%i.%i.%i", major, minor, rev);
	lua_pushstring(L, version);
	return 1;
}

/***
 * Does a topic match a subscription string?
 * @function topic_matches_sub
 * @tparam string subscription eg, blah/+/wop/#
 * @tparam string topic to test
 * @return[1] boolean result
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 * @see mosquitto_topic_matches_sub
 */
static int mosq_topic_matches_sub(lua_State *L)
{
	const char *sub = luaL_checkstring(L, 1);
	const char *topic = luaL_checkstring(L, 2);

	bool result;
	int rc = mosquitto_topic_matches_sub(sub, topic, &result);
	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq__pstatus(L, rc);
	} else {
		lua_pushboolean(L, result);
		return 1;
	}
}

/***
 * @function init
 * @see mosquitto_lib_init
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
*/
static int mosq_init(lua_State *L)
{
	if (!mosq_initialized)
		mosquitto_lib_init();

	return mosq__pstatus(L, MOSQ_ERR_SUCCESS);
}

/***
 * Cleanup mosquitto library.
 * This is called automatically by garbage collection, you shouldn't normally
 * have to call this.
 * @function cleanup
 * @see mosquitto_lib_cleanup
 * @return[1] true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int mosq_cleanup(lua_State *L)
{
	mosquitto_lib_cleanup();
	mosq_initialized = 0;
	return mosq__pstatus(L, MOSQ_ERR_SUCCESS);
}

static void ctx__on_init(ctx_t *ctx)
{
	ctx->on_connect = LUA_REFNIL;
	ctx->on_disconnect = LUA_REFNIL;
	ctx->on_publish = LUA_REFNIL;
	ctx->on_message = LUA_REFNIL;
	ctx->on_subscribe = LUA_REFNIL;
	ctx->on_unsubscribe = LUA_REFNIL;
	ctx->on_log = LUA_REFNIL;
}

static void ctx__on_clear(ctx_t *ctx)
{
	luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->on_connect);
	luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->on_disconnect);
	luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->on_publish);
	luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->on_message);
	luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->on_subscribe);
	luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->on_unsubscribe);
	luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->on_log);
}

/***
 * Create a new mosquitto instance
 * @function new
 * @tparam[opt=nil] string client id, optional. nil to allow library to generate
 * @tparam[opt=true] boolean clean_session
 * @return[1] a mosquitto instance
 * @raise For some out of memory or illegal states, or both nil id and clean session true
 * @see mosquitto_new
 */
static int mosq_new(lua_State *L)
{
	const char *id = luaL_optstring(L, 1, NULL);
	bool clean_session = (lua_isboolean(L, 2) ? lua_toboolean(L, 2) : true);

	if (id == NULL && !clean_session) {
		return luaL_argerror(L, 2, "if 'id' is nil then 'clean session' must be true");
	}

	ctx_t *ctx = (ctx_t *) lua_newuserdata(L, sizeof(ctx_t));

	/* ctx will be passed as void *obj arg in the callback functions */
	ctx->mosq = mosquitto_new(id, clean_session, ctx);

	if (ctx->mosq == NULL) {
		return luaL_error(L, strerror(errno));
	}

	ctx->L = L;
	ctx__on_init(ctx);

	luaL_getmetatable(L, MOSQ_META_CTX);
	lua_setmetatable(L, -2);

	return 1;
}

static ctx_t * ctx_check(lua_State *L, int i)
{
	return (ctx_t *) luaL_checkudata(L, i, MOSQ_META_CTX);
}

/***
 * Instance functions
 * @section instance_functions
 */

/***
 * Destroy context
 * This is called automatically by garbage collection, you shouldn't normally
 * have to call this.
 * @function destroy
 * @see mosquitto_destroy
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_destroy(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	mosquitto_destroy(ctx->mosq);

	/* clean up Lua callback functions in the registry */
	ctx__on_clear(ctx);

	/* remove all methods operating on ctx */
	lua_newtable(L);
	lua_setmetatable(L, -2);

	return mosq__pstatus(L, MOSQ_ERR_SUCCESS);
}

/***
 * Reinitialise
 * @function reinitialise
 * @tparam[opt=nil] string client id, nil to allow the library to determine
 * @tparam[opt=true] boolean clean session.
 * @see mosquitto_reinitialise
 * @see new
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
*/
static int ctx_reinitialise(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const char *id = luaL_optstring(L, 1, NULL);
	bool clean_session = (lua_isboolean(L, 2) ? lua_toboolean(L, 2) : true);

	if (id == NULL && !clean_session) {
		return luaL_argerror(L, 3, "if 'id' is nil then 'clean session' must be true");
	}

	int rc = mosquitto_reinitialise(ctx->mosq, id, clean_session, ctx);

	/* clean up Lua callback functions in the registry */
	ctx__on_clear(ctx);
	ctx__on_init(ctx);

	return mosq__pstatus(L, rc);
}

/***
 * Set a Will
 * @function will_set
 * @tparam string topic as per mosquitto_will_set
 * @tparam string payload as per mosquitto_will_set (but a proper lua string)
 * @tparam[opt=0] number qos 0, 1 or 2
 * @tparam[opt=false] boolean retain
 * @see mosquitto_will_set
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_will_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const char *topic = luaL_checkstring(L, 2);
	size_t payloadlen = 0;
	const void *payload = NULL;

	if (!lua_isnil(L, 3)) {
		payload = lua_tolstring(L, 3, &payloadlen);
	};

	int qos = luaL_optinteger(L, 4, 0);
	bool retain = lua_toboolean(L, 5);

	int rc = mosquitto_will_set(ctx->mosq, topic, payloadlen, payload, qos, retain);
	return mosq__pstatus(L, rc);
}

/***
 * Clear a will
 * @function will_clear
 * @see mosquitto_will_clear
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_will_clear(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);

	int rc = mosquitto_will_clear(ctx->mosq);
	return mosq__pstatus(L, rc);
}

/***
 * Set login details
 * @function login_set
 * @tparam[opt=nil] string username may be nil
 * @tparam[opt=nil] string password may be nil
 * @see mosquitto_username_pw_set
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_login_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const char *username = (lua_isnil(L, 2) ? NULL : luaL_checkstring(L, 2));
	const char *password = (lua_isnil(L, 3) ? NULL : luaL_checkstring(L, 3));

	int rc = mosquitto_username_pw_set(ctx->mosq, username, password);
	return mosq__pstatus(L, rc);
}

/***
 * Set TLS details
 * This doesn't currently support callbacks for passphrase prompting!
 * @function tls_set
 * @tparam[opt=nil] string cafile may be nil
 * @tparam[opt=nil] string capath may be nil
 * @tparam[opt=nil] string certfile may be nil
 * @tparam[opt=nil] string keyfile may be nil
 * @see mosquitto_tls_set
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_tls_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const char *cafile = luaL_optstring(L, 2, NULL);
	const char *capath = luaL_optstring(L, 3, NULL);
	const char *certfile = luaL_optstring(L, 4, NULL);
	const char *keyfile = luaL_optstring(L, 5, NULL);

	// the last param is a callback to a function that asks for a passphrase for a keyfile
	// our keyfiles should NOT have a passphrase
	int rc = mosquitto_tls_set(ctx->mosq, cafile, capath, certfile, keyfile, 0);
	return mosq__pstatus(L, rc);
}

/***
 * Set TLS insecure flags
 * @function tls_insecure_set
 * @tparam boolean value true or false
 * @see mosquitto_tls_insecure_set
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_tls_insecure_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	bool value = lua_toboolean(L, 2);

	int rc = mosquitto_tls_insecure_set(ctx->mosq, value);
	return mosq__pstatus(L, rc);
}

/***
 * Set TLS PSK options
 * @function tls_psk_set
 * @tparam string psk
 * @tparam string identity
 * @tparam[opt] string ciphers
 * @see mosquitto_tls_psk_set
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_tls_psk_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const char *psk = luaL_checkstring(L, 2);
	const char *identity = luaL_checkstring(L, 3);
	const char *ciphers = luaL_optstring(L, 4, NULL);

	int rc = mosquitto_tls_psk_set(ctx->mosq, psk, identity, ciphers);
	return mosq__pstatus(L, rc);
}

/***
 * Set TLS options, must be called before connect.
 * @function tls_opts_set
 * @tparam bool cert true for SSL_VERIFY_PEER, false for SSL_VERIFY_NONE
 * @tparam[opt=nil] string tls_version nil to use default
 * @tparam[opt=nil] string ciphers nil to use the default set
 * @see mosquitto_tls_opts_set
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_tls_opts_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const bool cert_required = lua_toboolean(L, 2);
	const char *tls_version = luaL_optstring(L, 3, NULL);
	const char *ciphers = luaL_optstring(L, 4, NULL);

	int rc = mosquitto_tls_opts_set(ctx->mosq, cert_required ? 1 : 0, tls_version, ciphers);
	return mosq__pstatus(L, rc);
}

/***
 * Set/clear threaded flag
 * @function threaded_set
 * @tparam boolean value true or false
 * @see mosquitto_threaded_set
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_threaded_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	bool value = lua_toboolean(L, 2);

	int rc = mosquitto_threaded_set(ctx->mosq, value);
	return mosq__pstatus(L, rc);
}

/***
 * Set client options, normally required before connect.
 * This will call _string_option or _int_option automatically based on the
 * parameters provided.
 * @function option
 * @tparam number option code, eg M.OPT_TLS_ALPN or M.OPT_PROTOCOL_VERSION
 * @tparam string_or_number value the value of the option to set
 * @see mosquitto_string_option
 * @see mosquitto_int_option
 * @see option_types
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For illegal arguments
 */
static int ctx_option(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	enum mosq_opt_t option = luaL_checkint(L, 2);
	int type = lua_type(L, 3);
	int rc;

	if (type == LUA_TNUMBER) {
		int val = lua_tonumber(L, 3);
		rc = mosquitto_int_option(ctx->mosq, option, val);
	} else if (type == LUA_TSTRING) {
		const char *val = lua_tolstring(L, 3, NULL);
		rc = mosquitto_string_option(ctx->mosq, option, val);
	} else {
		return luaL_argerror(L, 3, "values must be numeric or string");
	}
	return mosq__pstatus(L, rc);
}

/***
 * Connect to a broker
 * @function connect
 * @tparam[opt=localhost] string host
 * @tparam[opt=1883] number port
 * @tparam[opt=60] number keepalive in seconds
 * @see mosquitto_connect
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_connect(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const char *host = luaL_optstring(L, 2, "localhost");
	int port = luaL_optinteger(L, 3, 1883);
	int keepalive = luaL_optinteger(L, 4, 60);

	int rc =  mosquitto_connect(ctx->mosq, host, port, keepalive);
	return mosq__pstatus(L, rc);
}

/***
 * connect (async)
 * @function connect_async
 * @tparam[opt=localhost] string host
 * @tparam[opt=1883] number port
 * @tparam[opt=60] number keepalive in seconds
 * @see mosquitto_connect_async
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_connect_async(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	const char *host = luaL_optstring(L, 2, "localhost");
	int port = luaL_optinteger(L, 3, 1883);
	int keepalive = luaL_optinteger(L, 4, 60);

	int rc =  mosquitto_connect_async(ctx->mosq, host, port, keepalive);
	return mosq__pstatus(L, rc);
}

/***
 * @function reconnect
 * @see mosquitto_reconnect
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_reconnect(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);

	int rc = mosquitto_reconnect(ctx->mosq);
	return mosq__pstatus(L, rc);
}

/***
 * @function disconnect
 * @see mosquitto_disconnect
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_disconnect(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);

	int rc = mosquitto_disconnect(ctx->mosq);
	return mosq__pstatus(L, rc);
}

/***
 * Publish a message
 * @function publish
 * @tparam string topic
 * @tparam string payload (may be nil)
 * @tparam[opt=0] number qos 0, 1 or 2
 * @tparam[opt=nil] boolean retain flag
 * @return 
 * @see mosquitto_publish
 * @treturn[1] number MID can be used for correlation with callbacks
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_publish(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	int mid;	/* message id is referenced in the publish callback */
	const char *topic = luaL_checkstring(L, 2);
	size_t payloadlen = 0;
	const void *payload = NULL;

	if (!lua_isnil(L, 3)) {
		payload = lua_tolstring(L, 3, &payloadlen);
	};

	int qos = luaL_optinteger(L, 4, 0);
	bool retain = lua_toboolean(L, 5);

	int rc = mosquitto_publish(ctx->mosq, &mid, topic, payloadlen, payload, qos, retain);

	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq__pstatus(L, rc);
	} else {
		lua_pushinteger(L, mid);
		return 1;
	}
}

/***
 * Subscribe to a topic
 * @function subscribe
 * @tparam string topic eg "blah/+/json/#"
 * @tparam[opt=0] number qos 0, 1 or 2
 * @treturn[1] number MID can be used for correlation with callbacks
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 * @see mosquitto_subscribe
 */
static int ctx_subscribe(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	int mid;
	const char *sub = luaL_checkstring(L, 2);
	int qos = luaL_optinteger(L, 3, 0);

	int rc = mosquitto_subscribe(ctx->mosq, &mid, sub, qos);

	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq__pstatus(L, rc);
	} else {
		lua_pushinteger(L, mid);
		return 1;
	}
}

/***
 * Unsubscribe from a topic
 * @function unsubscribe
 * @tparam string topic to unsubscribe from
 * @see mosquitto_unsubscribe
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_unsubscribe(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	int mid;
	const char *sub = luaL_checkstring(L, 2);

	int rc = mosquitto_unsubscribe(ctx->mosq, &mid, sub);

	if (rc != MOSQ_ERR_SUCCESS) {
		return mosq__pstatus(L, rc);
	} else {
		lua_pushinteger(L, mid);
		return 1;
	}
}

static int mosq_loop(lua_State *L, bool forever)
{
	ctx_t *ctx = ctx_check(L, 1);
	int timeout = luaL_optinteger(L, 2, -1);
	int max_packets = luaL_optinteger(L, 3, 1);
	int rc;
	if (forever) {
		rc = mosquitto_loop_forever(ctx->mosq, timeout, max_packets);
	} else {
		rc = mosquitto_loop(ctx->mosq, timeout, max_packets);
	}
	return mosq__pstatus(L, rc);
}

/***
 * run the loop manually
 * @function loop
 * @tparam[opt=-1] number timeout how long in ms to wait for traffic (-1 for library default)
 * @tparam[opt=1] number max_packets
 * @see mosquitto_loop
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_loop(lua_State *L)
{
	return mosq_loop(L, false);
}

/***
 * run the loop forever, blocking
 * @function loop_forever
 * @tparam[opt=-1] number timeout how long in ms to wait for traffic (-1 for library default)
 * @tparam[opt=1] number max_packets
 * @see mosquitto_loop_forever
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_loop_forever(lua_State *L)
{
	return mosq_loop(L, true);
}

/***
 * Start a loop thread
 * @function loop_start
 * @see mosquitto_loop_start
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_loop_start(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);

	int rc = mosquitto_loop_start(ctx->mosq);
	return mosq__pstatus(L, rc);
}

/***
 * Stop an existing loop thread
 * @function loop_stop
 * @see mosquitto_loop_stop
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_loop_stop(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	bool force = lua_toboolean(L, 2);

	int rc = mosquitto_loop_stop(ctx->mosq, force);
	return mosq__pstatus(L, rc);
}

/***
 * Get the underlying socket
 * @function socket
 * @treturn[1] number the socket number
 * @treturn[2] boolean false if the socket was uninitialized
 * @see mosquitto_socket
 */
static int ctx_socket(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);

	int fd = mosquitto_socket(ctx->mosq);
	switch (fd) {
		case -1:
			lua_pushboolean(L, false);
			break;
		default:
			lua_pushinteger(L, fd);
			break;
	}

	return 1;
}

/***
 * Handle loop read events manually
 * @function loop_read
 * @tparam[opt=1] number max_packets
 * @see mosquitto_loop_read
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_loop_read(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	int max_packets = luaL_optinteger(L, 2, 1);

	int rc = mosquitto_loop_read(ctx->mosq, max_packets);
	return mosq__pstatus(L, rc);
}

/***
 * Handle loop write events manually
 * @function loop_write
 * @tparam[opt=1] number max_packets
 * @see mosquitto_loop_write
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_loop_write(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	int max_packets = luaL_optinteger(L, 2, 1);

	int rc = mosquitto_loop_write(ctx->mosq, max_packets);
	return mosq__pstatus(L, rc);
}

/***
 * Handle loop misc events manually
 * @function loop_misc
 * @see mosquitto_loop_misc
 * @return[1] boolean true
 * @return[2] nil
 * @treturn[2] number error code
 * @treturn[2] string error description.
 * @raise For some out of memory or illegal states
 */
static int ctx_loop_misc(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);

	int rc = mosquitto_loop_misc(ctx->mosq);
	return mosq__pstatus(L, rc);
}

/***
 * Does the library want to write?
 * @function want_write
 * @see mosquitto_want_write
 * @treturn boolean result
 */
static int ctx_want_write(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);

	lua_pushboolean(L, mosquitto_want_write(ctx->mosq));
	return 1;
}

static void ctx_on_connect(
	struct mosquitto *mosq,
	void *obj,
	int rc)
{
	ctx_t *ctx = obj;
	bool success = false;
	char *str = "reserved for future use";

	switch(rc) {
		case CONN_ACCEPT:
			success = true;
			str = "connection accepted";
			break;

		case CONN_REF_BAD_PROTOCOL:
			str = "connection refused - incorrect protocol version";
			break;

		case CONN_REF_BAD_ID:
			str = "connection refused - invalid client identifier";
			break;

		case CONN_REF_SERVER_NOAVAIL:
			str = "connection refused - server unavailable";
			break;

		case CONN_REF_BAD_LOGIN:
			str = "connection refused - bad username or password";
			break;

		case CONN_REF_NO_AUTH:
			str = "connection refused - not authorised";
			break;

		case CONN_REF_BAD_TLS:
			str = "connection refused - TLS error";
			break;
	}

	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->on_connect);

	lua_pushboolean(ctx->L, success);
	lua_pushinteger(ctx->L, rc);
	lua_pushstring(ctx->L, str);

	lua_call(ctx->L, 3, 0);
}


static void ctx_on_disconnect(
	struct mosquitto *mosq,
	void *obj,
	int rc)
{
	ctx_t *ctx = obj;
	bool success = true;
	char *str = "client-initiated disconnect";

	if (rc) {
		success = false;
		str = "unexpected disconnect";
	}

	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->on_disconnect);

	lua_pushboolean(ctx->L, success);
	lua_pushinteger(ctx->L, rc);
	lua_pushstring(ctx->L, str);

	lua_call(ctx->L, 3, 0);
}

static void ctx_on_publish(
	struct mosquitto *mosq,
	void *obj,
	int mid)
{
	ctx_t *ctx = obj;

	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->on_publish);
	lua_pushinteger(ctx->L, mid);
	lua_call(ctx->L, 1, 0);
}

static void ctx_on_message(
	struct mosquitto *mosq,
	void *obj,
	const struct mosquitto_message *msg)
{
	ctx_t *ctx = obj;

	/* push registered Lua callback function onto the stack */
	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->on_message);
	/* push function args */
	lua_pushinteger(ctx->L, msg->mid);
	lua_pushstring(ctx->L, msg->topic);
	lua_pushlstring(ctx->L, msg->payload, msg->payloadlen);
	lua_pushinteger(ctx->L, msg->qos);
	lua_pushboolean(ctx->L, msg->retain);

	lua_call(ctx->L, 5, 0); /* args: mid, topic, payload, qos, retain */
}

static void ctx_on_subscribe(
	struct mosquitto *mosq,
	void *obj,
	int mid,
	int qos_count,
	const int *granted_qos)
{
	ctx_t *ctx = obj;
	int i;

	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->on_subscribe);
	lua_pushinteger(ctx->L, mid);

	for (i = 0; i < qos_count; i++) {
		lua_pushinteger(ctx->L, granted_qos[i]);
	}

	lua_call(ctx->L, qos_count + 1, 0);
}

static void ctx_on_unsubscribe(
	struct mosquitto *mosq,
	void *obj,
	int mid)
{
	ctx_t *ctx = obj;

	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->on_unsubscribe);
	lua_pushinteger(ctx->L, mid);
	lua_call(ctx->L, 1, 0);
}

static void ctx_on_log(
	struct mosquitto *mosq,
	void *obj,
	int level,
	const char *str)
{
	ctx_t *ctx = obj;

	lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->on_log);

	lua_pushinteger(ctx->L, level);
	lua_pushstring(ctx->L, str);

	lua_call(ctx->L, 2, 0);
}

static int callback_type_from_string(const char *);

static int ctx_callback_set(lua_State *L)
{
	ctx_t *ctx = ctx_check(L, 1);
	int callback_type;

	if (lua_isstring(L, 2)) {
		callback_type = callback_type_from_string(lua_tostring(L, 2));
	} else {
		callback_type = luaL_checkinteger(L, 2);
	}

	if (!lua_isfunction(L, 3)) {
		return luaL_argerror(L, 3, "expecting a callback function");
	}

	/* pop the function from the stack and store it in the registry */
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);

	switch (callback_type) {
		case CONNECT:
			ctx->on_connect = ref;
			mosquitto_connect_callback_set(ctx->mosq, ctx_on_connect);
			break;

		case DISCONNECT:
			ctx->on_disconnect = ref;
			mosquitto_disconnect_callback_set(ctx->mosq, ctx_on_disconnect);
			break;

		case PUBLISH:
			ctx->on_publish = ref;
			mosquitto_publish_callback_set(ctx->mosq, ctx_on_publish);
			break;

		case MESSAGE:
			/* store the reference into the context, to be retrieved by ctx_on_message */
			ctx->on_message = ref;
			/* register C callback in mosquitto */
			mosquitto_message_callback_set(ctx->mosq, ctx_on_message);
			break;

		case SUBSCRIBE:
			ctx->on_subscribe = ref;
			mosquitto_subscribe_callback_set(ctx->mosq, ctx_on_subscribe);
			break;

		case UNSUBSCRIBE:
			ctx->on_unsubscribe = ref;
			mosquitto_unsubscribe_callback_set(ctx->mosq, ctx_on_unsubscribe);
			break;

		case LOG:
			ctx->on_log = ref;
			mosquitto_log_callback_set(ctx->mosq, ctx_on_log);
			break;

		default:
			luaL_unref(L, LUA_REGISTRYINDEX, ref);
			luaL_argerror(L, 2, "not a proper callback type");
			break;
	}

	return mosq__pstatus(L, MOSQ_ERR_SUCCESS);
}

struct define {
	const char* name;
	int value;
};

/***
 * Module Constants
 * @section mod_constants
 */

/*** Callback ids
 * @table callback_ids
 * @field ON_CONNECT
 * @field ON_DISCONNECT
 * @field ON_PUBLISH
 * @field ON_MESSAGE
 * @field ON_SUBSCRIBE
 * @field ON_UNSUBSCRIBE
 * @field ON_LOG
 */

/*** Log types
 * @table log_types
 * @field LOG_NONE
 * @field LOG_INFO
 * @field LOG_NOTICE
 * @field LOG_WARNING
 * @field LOG_ERROR
 * @field LOG_DEBUG
 * @field LOG_ALL
 */

/*** Option types
 * @see option
 * @table option_types
 * @field OPT_PROTOCOL_VERSION
 * @field OPT_SSL_CTX
 * @field OPT_SSL_CTX_WITH_DEFAULTS
 * @field OPT_RECEIVE_MAXIMUM
 * @field OPT_SEND_MAXIMUM
 * @field OPT_TLS_KEYFORM
 * @field OPT_TLS_ENGINE
 * @field OPT_TLS_ENGINE_KPASS_SHA1
 * @field OPT_TLS_OCSP_REQUIRED
 * @field OPT_TLS_ALPN
 */

/*** Option Values
 * @see option
 * @table option_values
 * @field MQTT_PROTOCOL_V31
 * @field MQTT_PROTOCOL_V311
 * @field MQTT_PROTOCOL_V5
 */
static const struct define D[] = {
	{"ON_CONNECT",		CONNECT},
	{"ON_DISCONNECT",	DISCONNECT},
	{"ON_PUBLISH",		PUBLISH},
	{"ON_MESSAGE",		MESSAGE},
	{"ON_SUBSCRIBE",	SUBSCRIBE},
	{"ON_UNSUBSCRIBE",	UNSUBSCRIBE},
	{"ON_LOG",			LOG},

	{"LOG_NONE",	MOSQ_LOG_NONE},
	{"LOG_INFO",	MOSQ_LOG_INFO},
	{"LOG_NOTICE",	MOSQ_LOG_NOTICE},
	{"LOG_WARNING",	MOSQ_LOG_WARNING},
	{"LOG_ERROR",	MOSQ_LOG_ERR},
	{"LOG_DEBUG",	MOSQ_LOG_DEBUG},
	{"LOG_SUBSCRIBE", MOSQ_LOG_SUBSCRIBE},
	{"LOG_UNSUBSCRIBE", MOSQ_LOG_UNSUBSCRIBE},
	{"LOG_WEBSOCKETS", MOSQ_LOG_WEBSOCKETS},
	{"LOG_ALL",		MOSQ_LOG_ALL},

	{"OPT_PROTOCOL_VERSION",MOSQ_OPT_PROTOCOL_VERSION},
	{"OPT_SSL_CTX",		MOSQ_OPT_SSL_CTX},
	{"OPT_SSL_CTX_WITH_DEFAULTS", MOSQ_OPT_SSL_CTX_WITH_DEFAULTS},
	{"OPT_RECEIVE_MAXIMUM",	MOSQ_OPT_RECEIVE_MAXIMUM},
	{"OPT_SEND_MAXIMUM",	MOSQ_OPT_SEND_MAXIMUM},
	{"OPT_TLS_KEYFORM",	MOSQ_OPT_TLS_KEYFORM},
	{"OPT_TLS_ENGINE",	MOSQ_OPT_TLS_ENGINE},
	{"OPT_TLS_ENGINE_KPASS_SHA1", MOSQ_OPT_TLS_ENGINE_KPASS_SHA1},
	{"OPT_TLS_OCSP_REQUIRED", MOSQ_OPT_TLS_OCSP_REQUIRED},
	{"OPT_TLS_ALPN",	MOSQ_OPT_TLS_ALPN},

	{"MQTT_PROTOCOL_V31",	MQTT_PROTOCOL_V31},
	{"MQTT_PROTOCOL_V311",	MQTT_PROTOCOL_V311},
	{"MQTT_PROTOCOL_V5",	MQTT_PROTOCOL_V5},

	{NULL,			0}
};

static int callback_type_from_string(const char *typestr)
{
	const struct define *def = D;
	/* filter out LOG_ strings */
	if (strstr(typestr, "ON_") != typestr)
		return -1;
	while (def->name != NULL) {
		if (strcmp(def->name, typestr) == 0)
			return def->value;
		def++;
	}
	return -1;
}

static void mosq_register_defs(lua_State *L, const struct define *D)
{
	while (D->name != NULL) {
		lua_pushinteger(L, D->value);
		lua_setfield(L, -2, D->name);
		D++;
	}
}

static const struct luaL_Reg R[] = {
	{"version",	mosq_version},
	{"init",	mosq_init},
	{"cleanup",	mosq_cleanup},
	{"__gc",	mosq_cleanup},
	{"new",		mosq_new},
	{"topic_matches_sub",mosq_topic_matches_sub},
	{NULL,		NULL}
};

static const struct luaL_Reg ctx_M[] = {
	{"destroy",			ctx_destroy},
	{"__gc",			ctx_destroy},
	{"reinitialise",	ctx_reinitialise},
	{"will_set",		ctx_will_set},
	{"will_clear",		ctx_will_clear},
	{"login_set",		ctx_login_set},
	{"tls_insecure_set",	ctx_tls_insecure_set},
	{"tls_set",		ctx_tls_set},
	{"tls_psk_set",		ctx_tls_psk_set},
	{"tls_opts_set",	ctx_tls_opts_set},
	{"threaded_set",	ctx_threaded_set},
	{"option",		ctx_option},
	{"connect",			ctx_connect},
	{"connect_async",	ctx_connect_async},
	{"reconnect",		ctx_reconnect},
	{"disconnect",		ctx_disconnect},
	{"publish",			ctx_publish},
	{"subscribe",		ctx_subscribe},
	{"unsubscribe",		ctx_unsubscribe},
	{"loop",			ctx_loop},
	{"loop_forever",	ctx_loop_forever},
	{"loop_start",		ctx_loop_start},
	{"loop_stop",		ctx_loop_stop},
	{"socket",			ctx_socket},
	{"loop_read",			ctx_loop_read},
	{"loop_write",			ctx_loop_write},
	{"loop_misc",			ctx_loop_misc},
	{"want_write",		ctx_want_write},
	{"callback_set",	ctx_callback_set},
	{"__newindex",		ctx_callback_set},

#ifdef LUA_MOSQUITTO_COMPAT
	/* those are kept for compat */
	{"set_will",		ctx_will_set},
	{"clear_will",		ctx_will_clear},
	{"set_login",		ctx_login_set},
	{"start_loop",		ctx_loop_start},
	{"stop_loop",		ctx_loop_stop},
	{"set_callback",	ctx_callback_set},
	{"read",		ctx_loop_read},
	{"write",		ctx_loop_write},
	{"misc",		ctx_loop_misc},
#endif

	{NULL,		NULL}
};

int luaopen_mosquitto(lua_State *L)
{
	mosquitto_lib_init();
	mosq_initialized = 1;

#ifdef LUA_ENVIRONINDEX
	/* set private environment for this module */
	lua_newtable(L);
	lua_replace(L, LUA_ENVIRONINDEX);
#endif

	/* metatable.__index = metatable */
	luaL_newmetatable(L, MOSQ_META_CTX);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, ctx_M, 0);

	luaL_newlib(L, R);

	/* register callback defs into mosquitto table */
	mosq_register_defs(L, D);

	return 1;
}
