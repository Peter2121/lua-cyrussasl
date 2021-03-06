#include <lua.h>

#include <sasl/sasl.h>
#include <sasl/saslutil.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "context.h"
#include "luaabstract.h"

#ifndef VERSION
#define VERSION "undefined"
#endif

/* It's unfortunate that Cyrus SASL doesn't export these strings itself; it 
 * only exports constants that refer to them. We could go crazy and 
 * dynamically build this list but that seems like a lot of overhead for 
 * something that they're unlikely to change.
 * Of course, now that I've written this, they'll certainly reorder the 
 * error levels and these strings will no longer reflect reality :P
 */
static const char * const level_strings[] = {
	"none",
	"error",
	"fail",
	"warn",
	"note",
	"debug",
	"trace",
	"pass",
	NULL
};

static int log_cb_ref = LUA_REFNIL;
static int minimum_log_prio = SASL_LOG_NONE;

static int _sasl_log(void *context,
		     int priority,
		     const char *message)
{
  struct _sasl_ctx *ctxp = context;

  if (!message || !context || ctxp->magic != CYRUSSASL_MAGIC)
    return SASL_BADPARAM;

  if (priority < 0 || priority >= sizeof(level_strings) - 1)
    return SASL_BADPARAM;

  set_context_message(context, message);

  if (priority != SASL_LOG_NONE && priority <= minimum_log_prio &&
	log_cb_ref != LUA_REFNIL) {
    /* Function to call */
    lua_rawgeti(ctxp->L, LUA_REGISTRYINDEX, log_cb_ref);

    /* Message */
    lua_pushstring(ctxp->L, message);

    /* Priority */
    lua_pushstring(ctxp->L, level_strings[priority]);

    /* Perform: cb(message, priority) */
    lua_call(ctxp->L, 2, 0);
  }

  return SASL_OK;
}

static int _sasl_canon_user(sasl_conn_t *conn,
			    void *context,
			    const char *user, unsigned ulen,
			    unsigned flags,
			    const char *user_realm,
			    char *out_user, unsigned out_umax,
			    unsigned *out_ulen)
{
  struct _sasl_ctx *ctxp = context;

  if (!conn || !context || !user || ctxp->magic != CYRUSSASL_MAGIC)
    return SASL_BADPARAM;

  if (!(flags & SASL_CU_AUTHID) && !(flags & SASL_CU_AUTHZID))
    return SASL_BADPARAM;

  if (!out_user || !out_ulen || out_umax == 0)
    return SASL_BADPARAM;

  if (ctxp->canon_cb_ref == LUA_REFNIL) {
    if (ulen >= out_umax)
      return SASL_BUFOVER;

    /* out_user may be the same as user, so memmove, not memcpy */
    memmove(out_user, user, ulen);
    out_user[ulen] = '\0';
    *out_ulen = ulen;

    set_context_user(context, user, ulen);
    return SASL_OK;
  }

  /* We have a callback to deal with. */

  int ret = SASL_OK;
  const char *str = NULL;
  size_t len = 0;
  
  /* Function to call */
  lua_rawgeti(ctxp->L, LUA_REGISTRYINDEX, ctxp->canon_cb_ref);
  
  /* Username */
  lua_pushlstring(ctxp->L, user, ulen);
  /* Realm */
  lua_pushstring(ctxp->L, user_realm);
  /* flags (the type of username) */
  if ((flags & SASL_CU_AUTHID) && (flags & SASL_CU_AUTHZID))
    lua_pushliteral(ctxp->L, "both");
  else if (flags & SASL_CU_AUTHID)
    lua_pushliteral(ctxp->L, "authcid");
  else
    lua_pushliteral(ctxp->L, "authzid");
  /* Perform: str = cb(user, user_realm, "both|authcid|authzid") */
  lua_call(ctxp->L, 3, 1);
  
  /* Get the result */
  str = lua_tolstring(ctxp->L, -1, &len);
  if (str == NULL)
    ret = SASL_BADPROT;
  else if (len >= out_umax)
    ret = SASL_BUFOVER;
  else {
    memcpy(out_user, str, len + 1);
    *out_ulen = len;
  }
  
  /* Pop the result of the call off the stack */
  lua_pop(ctxp->L, 1);

  if (ret == SASL_OK)
    set_context_user(context, out_user, *out_ulen);
  else
    set_context_user(context, NULL, 0);

  return ret;
}

/* version = cyrussasl.get_version()
 *
 * Returns a string of the lua_cyrussasl library version.
 */
static int cyrussasl_get_version(lua_State *l)
{
  int numargs = lua_gettop(l);
  if (numargs != 0) {
    lua_pushstring(l, "usage: cyrussasl.get_version()");
    lua_error(l);
    return 0;
  }

  lua_pushstring(l, VERSION);

  return 1;
}

/* 
 * cyrussasl.server_init("appname")
 *
 * appname: the name of this application, from SASL's perspective.
 * 
 * This function does not return any values. On failure, it will throw an 
 * error.
 */
static int cyrussasl_sasl_server_init(lua_State *l)
{
  const char *appname;
  int numargs = lua_gettop(l);
  int err;

  if (numargs != 1) {
    lua_pushstring(l, "usage: cyrussasl.server_init(appname)");
    lua_error(l);
    return 0;
  }

  appname = tostring(l, 1);

  err = sasl_server_init( NULL, /* Global callbacks */
			  appname ); 

  if (err != SASL_OK) {
    lua_pushstring(l, "sasl_server_init failed");
    lua_error(l);
    return 0;
  }

  return 0;
}

static void _init_callbacks(struct _sasl_ctx *ctx)
{
  ctx->callbacks[0].id      = SASL_CB_LOG;         /* Callback for error msg */
  ctx->callbacks[0].proc    = &_sasl_log;
  ctx->callbacks[0].context = ctx;
  ctx->callbacks[1].id      = SASL_CB_CANON_USER;  /* Callback for username */
  ctx->callbacks[1].proc    = &_sasl_canon_user;
  ctx->callbacks[1].context = ctx;
  ctx->callbacks[2].id      = SASL_CB_LIST_END;    /* Terminator */
  ctx->callbacks[2].proc    = NULL;
  ctx->callbacks[2].context = NULL;
}

/* conn = cyrussasl.server_new("serice_name", "host FQDN", "user realm",
 *                             "iplocal", "ipremote")
 *
 * conn: an opaque data structure (from Lua's perspective) related to this 
 *       specific authentication attempt.
 * service_name: the name of the service that's being protected by SASL (e.g.
 *               xmpp, smtp, ...)
 * host FQDN: the fully-qualified domain name of the server that users 
 *            are connecting to. May be nil.
 * user realm: the authentication user realm to use for AuthN purposes. 
 *             May be nil.
 * iplocal: Either nil or a string of the form "a.b.c.d;port" (for IPv4). 
 *          Used to tell the SASL library what the local side of the connection
 *          is using.
 * ipremote: Either nil or a string denoting the remote side of the connection.
 *
 * On error, this throws Lua error exceptions. (It is not the typical
 * case that this method might cause an error, except when attempting
 * to set up SASL initially during development.)
 */
static int cyrussasl_sasl_server_new(lua_State *l)
{
  const char *service_name, *fqdn, *realm, *iplocal, *ipremote;
  int numargs = lua_gettop(l);
  int err;
  sasl_conn_t *conn = NULL;
  struct _sasl_ctx **ctxp = NULL;

  if (numargs != 5) {
    lua_pushstring(l, 
		   "usage: "
		   "conn = "
		   "cyrussasl.server_new(service_name, fqdn, realm, "
		   "iplocal, ipremote)");
    lua_error(l);
    return 0;
  }

  service_name = tostring(l, 1);
  fqdn = tostring(l, 2);
  realm = tostring(l, 3);
  iplocal = tostring(l, 4);
  ipremote = tostring(l, 5);

  ctxp = new_context(l);
  if (!ctxp) {
    lua_pushstring(l, "Unable to allocate a new context");
    lua_error(l);
    return 0;
  }

  _init_callbacks(*ctxp);

  err = sasl_server_new( service_name,       /* service name (passed in) */
			 fqdn,               /* serverFQDN               */
			 realm,              /* user_realm               */
			 iplocal,            /* iplocalport              */
			 ipremote,           /* ipremoteport             */
			 (*ctxp)->callbacks, /* per-connection callbacks */
			 0,                  /* flags                    */
			 &conn );            /* returned connection ptr  */

  (*ctxp)->conn = conn;

  if ( err != SASL_OK ) {
    lua_pushstring(l, "sasl_server_new failed");
    lua_error(l);
    return 0;
  }

  /* Return 1 item on the stack (a userdata pointer to our state struct).
   * It was already pushed on the stack by new_context(). */

  return 1; /* return # of arguments returned on stack */
}

/* (err, data) = cyrussasl.server_start(conn, mech, data)
 *
 * Arguments:
 *   conn: the return result from a previous call to server_new
 *   mech: the mechanism to use during this attempt (e.g. "PLAIN" or "GSSAPI")
 *   data: any data that the client might have sent with its 
 *         mech choice. Data may be an empty string or nil. Note that 
 *         the non-nil case is specifically a Lua string object
 *         (which, by definition, is allowed to contain '\0' bytes).
 * Return values:
 *   err: the (integer) SASL error code reflecting the state of the attempt 
 *        (e.g. SASL_OK, SASL_CONTINUE, SASL_BADMECH, ...)
 *   data: data that the server wants to send to the client in order 
 *         to continue the authN attempt. Returned as a Lua string object.
 */

static int cyrussasl_sasl_server_start(lua_State *l)
{
  int numargs = lua_gettop(l);
  int err;
  struct _sasl_ctx *ctx = NULL;
  const char *mech = NULL;
  const char *data = NULL;
  size_t len;
  unsigned outlen;

  if (numargs != 3) {
    lua_pushstring(l, 
		   "usage: "
		   "(err, data) = cyrussasl.server_start(conn, mech, data)");
    lua_error(l);
    return 0;
  }

  /* Pull arguments off of the stack... */

  ctx = get_context(l, 1);
  mech = tostring(l, 2);
  /* Allow the 'data' param to be nil, or an empty string. */
  if ( lua_type(l, 3) == LUA_TNIL ) {
    data = NULL;
    len = 0;
  } else {
    data = tolstring(l, 3, &len);
  }

  err = sasl_server_start( ctx->conn, /* saved sasl connection              */
			   mech,   /* mech, which the client chose          */
			   data,   /* data that the client sent             */
			   len,    /* length of the client's data           */
			   &data,  /* data with which the server will reply */
			   &outlen /* size of the server's reply            */
			   );

  /* Form the reply and push onto the stack */
  lua_pushinteger(l, err);          /* SASL_CONTINUE, SASL_OK, et al  */
  lua_pushlstring(l, data, outlen); /* server's reply to the client   */
  return 2;                         /* returning 2 items on Lua stack */
}

/* (err, data) = cyrussasl.server_step(conn, data)
 *
 * Arguments:
 *   conn: the return result from a previous call to server_new
 *   data: any data that the client might have sent from the previous step.
 *         Note that data may still be an empty string or nil. (Like the 
 *         argument of the same name to server_start.)
 *
 * Return values:
 *   err: the (integer) SASL error code reflecting the state of the attempt 
 *        (e.g. SASL_OK, SASL_CONTINUE, SASL_BADMECH, ...)
 *   data: data that the server wants to send to the client in order 
 *         to continue the authN attempt. Returned as a Lua string object.
 */
static int cyrussasl_sasl_server_step(lua_State *l)
{
  int numargs = lua_gettop(l);
  int err;
  struct _sasl_ctx *ctx = NULL;
  const char *data = NULL;
  size_t len;
  unsigned outlen;

  if (numargs != 2) {
    lua_pushstring(l, 
		   "usage: (err, data) = cyrussasl.server_step(conn, data)");
    lua_error(l);
    return 0;
  }

  ctx = get_context(l, 1);
  data = tolstring(l, 2, &len);

  err = sasl_server_step( ctx->conn,
			  data,
			  len,
			  &data,
			  &outlen );

  /* Form the reply and push onto the stack */
  lua_pushinteger(l, err);          /* SASL_CONTINUE, SASL_OK, et al  */
  lua_pushlstring(l, data, outlen); /* server's reply to the client   */
  return 2;                         /* returning 2 items on Lua stack */
}

/* cyrussasl.setssf(conn, min_ssf, max_ssf)
 *
 * conn: the conn pointer from cyrussasl.server_new().
 * min_ssf, max_ssf: set the minimum and maximum security strength factor 
 *                   required for this AuthN.
 *
 * Throws Lua errors if it fails (as it should not typically fail).
 * Does not return any value.
 */
static int cyrussasl_setssf(lua_State *l)
{
  sasl_security_properties_t secprops;
  int err;
  int min_ssf, max_ssf;
  struct _sasl_ctx *ctx = NULL;

  int numargs = lua_gettop(l);
  if (numargs != 3) {
    lua_pushstring(l, "usage: cyrussasl.setssf(conn, min_ssf, max_ssf)");
    lua_error(l);
    return 0;
  }

  ctx     = get_context(l, 1);
  min_ssf = tointeger(l, 2);
  max_ssf = tointeger(l, 3);

  memset(&secprops, 0L, sizeof(secprops));
  secprops.min_ssf = min_ssf;
  secprops.max_ssf = max_ssf;

  err = sasl_setprop(ctx->conn, SASL_SEC_PROPS, &secprops);
  if ( err != SASL_OK ) {
    lua_pushstring(l, "setssf failed");
    lua_error(l);
    return 0;
  }

  return 0;
}


/* cyrussasl.setprop(conn, propnum, val)
 *
 * conn: the conn pointer from cyrussasl.server_new().
 * propnum: an integer corresponding to the property to set
 * val: a lua string object
 *
 * Throws Lua errors if it fails (as it should not typically fail).
 * Does not return any value.
 */
static int cyrussasl_sasl_setprop(lua_State *l)
{
  sasl_security_properties_t secprops;
  int err;
  int proptype;
  const void *proparg;
  struct _sasl_ctx *ctx = NULL;

  int numargs = lua_gettop(l);
  if (numargs != 3) {
    lua_pushstring(l, "usage: cyrussasl.setprop(conn, propnum, propval)");
    lua_error(l);
    return 0;
  }

  ctx      = get_context(l, 1);
  proptype = tointeger(l, 2);
  proparg  = tolstring(l, 3, NULL);

  memset(&secprops, 0L, sizeof(secprops));
  secprops.max_ssf = UINT_MAX;

  err = sasl_setprop(ctx->conn, proptype, &proparg);
  if ( err != SASL_OK ) {
    lua_pushstring(l, "sasl_setprop failed");
    lua_error(l);
    return 0;
  }

  return 0;
}

/* b64data = cyrussasl.encode64(data)
 *
 * A convenience method to use the Cyrus SASL library implementation of base64
 * encoding data. Takes, and returns, a Lua string object. Since Lua strings 
 * may contain true 8-bit data (including '\0'), the length of the data is 
 * obtained by examining the length of the string.
 */
static int cyrussasl_sasl_encode64(lua_State *l)
{
  unsigned len_out;
  int alloclen;
  char *buf = NULL;
  const char *data = NULL;
  size_t len;
  int err;

  int numargs = lua_gettop(l);
  if (numargs != 2) {
    lua_pushstring(l, "usage: b64data = cyrussasl.encode64(data, len)");
    lua_error(l);
    return 0;
  }

  len = 0;
  data = tolstring(l, 1, &len);

  /* Allocate a new buffer that will accommodate the data in its most-possibly-
   * expanded state. */
  alloclen = ((len / 3) + 1) * 4 + 1;
  buf = malloc(alloclen);
  if (!buf) {
    lua_pushstring(l, "malloc failed");
    lua_error(l);
    return 0;
  }

  err = sasl_encode64(data, len, buf, alloclen, &len_out);
  if ( err != SASL_OK ) {
    free(buf);
    lua_pushstring(l, "sasl_encode64 failed");
    lua_error(l);
    return 0;
  }

  lua_pushlstring(l, buf, len_out);
  free(buf);
  return 1;
}

/* data = cyrussasl.decode64(b64data)
 *
 * Partner function to encode64().
 */
static int cyrussasl_sasl_decode64(lua_State *l)
{
  const char *data = NULL;
  char *outdata = NULL;
  size_t len;
  unsigned outlen;
  int err;

  int numargs = lua_gettop(l);
  if (numargs != 1) {
    lua_pushstring(l, "usage: data = cyrussasl.decode64(b64data)");
    lua_error(l);
    return 0;
  }

  data = tostring(l, 1);
  len = strlen(data);

  outdata = malloc(len);
  if (!outdata) {
    lua_pushstring(l, "failed to malloc in decode64");
    lua_error(l);
    return 0;
  }

  /* Perform a decode-in-place, which is kosher according to docs. */
  err = sasl_decode64(data, len, outdata, len, &outlen);
  if ( err != SASL_OK ) {
    free(outdata);
    lua_pushstring(l, "sasl_decode64 failed");
    lua_error(l);
    return 0;
  }

  lua_pushlstring(l, outdata, outlen);
  free(outdata);
  return 1;
}

/* mechslist = cyrussasl.listmech((conn, authid, prefix, separator, suffix)
 *
 * Return all of the available mechanisms to the Cyrus SASL library.
 *
 * conn: the conn pointer from cyrussasl.server_new().
 * authid: the username trying to authenticate. May be nil.
 * prefix: prefix to prepend to the returned string. May be an empty string.
 * separator: the string to use to separate mechanisms. May be empty.
 * suffix: suffix to postpend to the returned string. May be empty.
 *
 * mechslist: a Lua string object containing the mechanisms (GSSAPI, et al)
 */
static int cyrussasl_sasl_listmech(lua_State *l)
{
  int err;
  struct _sasl_ctx *ctx = NULL;
  const char *ext_authid = NULL;
  const char *data = NULL;
  const char *prefix = NULL;
  const char *separator = NULL;
  const char *suffix = NULL;
  unsigned len;
  int count;

  int numargs = lua_gettop(l);
  if (numargs != 5) {
    lua_pushstring(l, 
		   "usage: "
		   "mechslist = cyrussasl.listmech"
		   "(conn, authid, prefix, separator, suffix)");
    lua_error(l);
    return 0;
  }

  ctx        = get_context(l, 1);
  ext_authid = tostring(l, 2);
  prefix     = tostring(l, 3);
  separator  = tostring(l, 4);
  suffix     = tostring(l, 5);

  err = sasl_listmech(ctx->conn,
		      ext_authid,
		      prefix,
		      separator,
		      suffix,
		      &data,
		      &len,
		      &count);
  if ( err != SASL_OK ) {
    lua_pushstring(l, "sasl_listmech failed");
    lua_error(l);
    return 0;
  }

  lua_pushlstring(l, data, len);
  return 1;
}

/* user = cyrussasl.get_username(conn)
 *
 * conn: the conn pointer from cyrussasl.server_new().
 * user: the username decoded from user data as part of the negotiation.
 *
 * Note that 'user' may be an empty string if the negotiation hasn't 
 * extracted a username for any reason (e.g. incomplete negotiation).
 * 
 * Typically used after negotation is successful to find the username 
 * associated with the authentication that just took place.
 */
static int cyrussasl_get_username(lua_State *l)
{
  struct _sasl_ctx *ctx = NULL;
  const char *ret;
  unsigned ulen;

  int numargs = lua_gettop(l);
  if (numargs != 1) {
    lua_pushstring(l, "usage: user = cyrussasl.get_username(conn)");
    lua_error(l);
    return 0;
  }

  ctx = get_context(l, 1);

  ret = get_context_user(ctx, &ulen);
  if (ret && ulen)
    lua_pushstring(l, ret); /* Not quite right - username might contain NULLs... */
  else
    lua_pushstring(l, "");

  return 1;
}

/* user = cyrussasl.get_authname(conn)
 *
 * conn: the conn pointer from cyrussasl.server_new().
 * user: the authname decoded from user data as part of the negotiation.
 */
static int cyrussasl_get_authname(lua_State *l)
{
  struct _sasl_ctx *ctx = NULL;
  const char *ret;
  int numargs = lua_gettop(l);

  if (numargs != 1) {
    lua_pushstring(l, "usage: user = cyrussasl.get_authname(conn)");
    lua_error(l);
    return 0;
  }

  ctx = get_context(l, 1);

  ret = get_context_authname(ctx);
  if (ret)
    lua_pushstring(l, ret);
  else
    lua_pushstring(l, "");

  return 1;
}

/* message = cyrussasl.get_message(conn)
 *
 * conn: the conn pointer from cyrussasl.server_new().
 * message: the last message emitted by the SASL library during the 
 *          negotiation. May be an empty string.
 *
 * Typically used to find the specifics about a failed negotation.
 */
static int cyrussasl_get_message(lua_State *l)
{
  struct _sasl_ctx *ctx = NULL;
  const char *ret;
  int numargs = lua_gettop(l);
  if (numargs != 1) {
    lua_pushstring(l, "usage: cyrussasl.get_message(conn)");
    lua_error(l);
    return 0;
  }

  ctx = get_context(l, 1);

  ret = get_context_message(ctx);
  if (ret)
    lua_pushstring(l, ret);
  else
    lua_pushstring(l, "");

  return 1;
}

/* old_cb = cyrussasl.set_canon_cb(conn, cb)
 *
 * conn: the conn pointer from cyrussasl.server_new()
 * cb: a function that is passed the username, realm (may be nil), and
 *     either "authcid", "authzid", or "both" to indicate whether
 *     the username is the authentication identity, the authorization
 *     identity, or both (both seems to happen under some circumstances).
 *     The function should return a normalized username.
 * old_cb: the previous callback (or nil)
 *
 * Used to canonicalize usernames.
 *
 */
static int cyrussasl_set_canon_cb(lua_State *l)
{
  struct _sasl_ctx *ctx = NULL;
  int numargs = lua_gettop(l);
  int old_ref;
  int type;

  if (numargs != 2) {
    lua_pushstring(l, "usage: cyrussasl.set_canon_cb(conn, cb)");
    lua_error(l);
    return 0;
  }

  ctx = get_context(l, 1);

  type = lua_type(l, 2);
  if (type != LUA_TFUNCTION && type != LUA_TNIL) {
    char err[256];
    snprintf(err, sizeof(err),
	      "expected function or nil, got %s",
	       lua_typename(l, type));
    lua_pushstring(l, err);
    lua_error(l);
    return 0;
  }

  old_ref = ctx->canon_cb_ref;
  /* Store the new function */
  ctx->canon_cb_ref = luaL_ref(l, LUA_REGISTRYINDEX);

  /* Push the old function onto the stack and free its ref */
  lua_rawgeti(l, LUA_REGISTRYINDEX, old_ref);
  luaL_unref(l, LUA_REGISTRYINDEX, old_ref);

  return 1;
}

/* old_cb = cyrussasl.set_log_cb(cb, minimum priority level)
 *
 * cb: a function that is passed the log message and priority.
 *     priority is one of "error", "fail" (auth failures), "warn",
 *     "note", "debug", "trace", and "pass" (includes passwords).
 * minimum priority level: the minimum severity level messages to call
 *     in the cb.  The default is "warn".
 * old_cb: the previous callback (or nil)
 *
 * Used for debug logging.
 */
static int cyrussasl_set_log_cb(lua_State *l)
{
  int old_ref;
  int numargs = lua_gettop(l);
  int type;

  type = lua_type(l, 1);
  if (type != LUA_TFUNCTION && type != LUA_TNIL) {
    char err[256];
    snprintf(err, sizeof(err),
	      "expected function or nil, got %s",
	       lua_typename(l, type));
    lua_pushstring(l, err);
    lua_error(l);
    return 0;
  }

  /* Store the minimum desired log level */
  minimum_log_prio = luaL_checkoption(l, 2, "warn", level_strings);
  if (numargs > 1)
    lua_pop(l, numargs - 1);

  old_ref = log_cb_ref;
  /* Store the new function */
  log_cb_ref = luaL_ref(l, LUA_REGISTRYINDEX);

  /* Push the old function onto the stack and free its ref */
  lua_rawgeti(l, LUA_REGISTRYINDEX, old_ref);
  luaL_unref(l, LUA_REGISTRYINDEX, old_ref);

  return 1;
}

/* metatable, hook for calling gc_context on context structs */
static const luaL_Reg meta[] = {
  { "__gc", gc_context },
  { NULL,   NULL        }
};

/* function table for this module */
static const struct luaL_Reg methods[] = {
  { "setssf",       cyrussasl_setssf            },
  { "setprop",      cyrussasl_sasl_setprop      },
  { "listmech",     cyrussasl_sasl_listmech     },
  { "encode64",     cyrussasl_sasl_encode64     },
  { "decode64",     cyrussasl_sasl_decode64     },
  { "get_version",  cyrussasl_get_version       },
  { "server_init",  cyrussasl_sasl_server_init  },
  { "server_new",   cyrussasl_sasl_server_new   },
  { "server_start", cyrussasl_sasl_server_start },
  { "server_step",  cyrussasl_sasl_server_step  },
  { "get_username", cyrussasl_get_username      },
  { "get_authname", cyrussasl_get_authname      },
  { "get_message",  cyrussasl_get_message       },
  { "set_canon_cb", cyrussasl_set_canon_cb      },
  { "set_log_cb",   cyrussasl_set_log_cb        },
  { NULL,           NULL                        }
};

/* Module initializer, called from Lua when the module is loaded. */
int luaopen_cyrussasl(lua_State *l)
{
  /* Construct a new namespace table for Lua, and register it as the global 
   * named "cyrussasl".
   */
  luaL_openlib(l, MODULENAME, methods, 0);

  /* Create metatable, which is used to tie C data structures to our garbage 
   * collection function. */
  luaL_newmetatable(l, MODULENAME);
  luaL_openlib(l, 0, meta, 0);
  lua_pushliteral(l, "__index");
  lua_pushvalue(l, -3);               /* dup methods table*/
  lua_rawset(l, -3);                  /* metatable.__index = methods */
  lua_pushliteral(l, "__metatable");
  lua_pushvalue(l, -3);               /* dup methods table*/
  lua_rawset(l, -3);                  /* hide metatable:
                                         metatable.__metatable = methods */
  lua_pop(l, 1);                      /* drop metatable */
  return 1;                           /* return methods on the stack */

}

