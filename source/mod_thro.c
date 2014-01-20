/* ====================================================================
 * Copyright (c) 1995-2000 The Apache Group.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache"
 *    nor may "Apache" appear in their names without prior written
 *    permission of the Apache Group.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 * Author           : Christian Gilmore
 * Created On       : Mon Mar  6 14:04:05 EST 2000
 * Status           : Functional
 * Last Modified By : Christian Gilmore
 *
 * NAME
 *       mod_throttle_access.c
 *
 * PURPOSE 
 *       Limit (throttle) access by one of the following methods:
 *         * concurrent request restrictions
 */

#define VERSION 0.2

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_request.h"
#include "http_conf_globals.h"
#include "scoreboard.h"

typedef struct {
  int limited;
  int max_clients;
} throttle_access_dir_conf;

module MODULE_VAR_EXPORT throttle_access_module;

static void *create_throttle_access_dir_config(pool *p, char *d) {
  throttle_access_dir_conf *conf = (throttle_access_dir_conf *)
    ap_pcalloc(p, sizeof(throttle_access_dir_conf));
  int i;

  conf->limited = 0;
  conf->max_clients = HARD_SERVER_LIMIT;
  return conf;
}

static const char *max_concurrent_reqs(cmd_parms *cmd, void *dv, char *arg) {
  throttle_access_dir_conf *d = (throttle_access_dir_conf *)dv;
  int val;

  val = atoi(arg);
  if (val < 1)
    return "MaxConcurrentReqs must be an integer greater than 0";
  d->max_clients = val;
  d->limited = cmd->limited;

  return NULL;
}

static const command_rec throttle_access_cmds[] = {
  {"MaxConcurrentReqs", max_concurrent_reqs, NULL, OR_LIMIT, TAKE1,
   "'MaxConcurrentReqs' followed by non-negative integer"},
  {NULL}
};

static int check_dir_throttle_access(request_rec *r) {
  throttle_access_dir_conf *a = (throttle_access_dir_conf *)
    ap_get_module_config(r->per_dir_config, &throttle_access_module);

  int clients = 0;
  int i, score_methnum;
  short_score score_record;
  const char *score_method, *score_request, *score_uri;

  if  (!(a->limited & (1 << r->method_number)))
    return OK;

  if (!ap_exists_scoreboard_image()) {
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_CRIT, r,
		  "Server status unavailable in inetd mode");
    return HTTP_INTERNAL_SERVER_ERROR;
  }

  ap_sync_scoreboard_image();
  for (i = 0; i < HARD_SERVER_LIMIT; ++i) {
    score_record = ap_scoreboard_image->servers[i];
    score_request = ap_pstrdup(r->pool, score_record.request);
    score_method = ap_getword_white(r->pool, &score_request);
    score_methnum = ap_method_number_of(score_method);
    score_uri = ap_getword_white(r->pool, &score_request);
    /* only do length of r->uri, else we'd need to dissect
     * score_uri with call to ap_parse_uri_components */
    if (strncmp(r->uri, score_uri, strlen(r->uri)) == 0 &&
	/* make sure only limited methods are counted */
	(a->limited & (1 << score_methnum)) &&
	/* make sure the child is actually doing something */
	(score_record.status == SERVER_BUSY_READ ||
	 score_record.status == SERVER_BUSY_WRITE ||
	 score_record.status == SERVER_BUSY_KEEPALIVE))
      clients++;
  }
  
  /* add one to account for this current request */
  if (clients >= a->max_clients + 1) {
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, r,
		  "client access to %s deferred, MaxConcurrentReqs %d reached",
		  r->uri, a->max_clients);
    return HTTP_SERVICE_UNAVAILABLE;
  }
  
  return OK;
}

module MODULE_VAR_EXPORT throttle_access_module =
{
    STANDARD_MODULE_STUFF,
    NULL,			       /* initializer */
    create_throttle_access_dir_config, /* dir config creater */
    NULL,			       /* dir merger: default is to override */
    NULL,			       /* server config */
    NULL,			       /* merge server config */
    throttle_access_cmds,	       /* command table */
    NULL,			       /* handlers */
    NULL,			       /* filename translation */
    NULL,		               /* check_user_id */
    NULL,                              /* check auth */
    check_dir_throttle_access,	       /* check access */
    NULL,			       /* type_checker */
    NULL,			       /* fixups */
    NULL,			       /* logger */
    NULL,			       /* header parser */
    NULL,			       /* child_init */
    NULL,			       /* child_exit */
    NULL			       /* post read-request */
};

/* 
$Id: mod_throttle_access.c,v 1.5 2000/05/09 20:58:34 cgilmore Exp $
*/

/*
 * $Log: mod_throttle_access.c,v $
 * Revision 1.5  2000/05/09 20:58:34  cgilmore
 * removed use of ap_mmn as it didn't work as advertised
 *
 * Revision 1.4  2000/03/13 22:52:26  cgilmore
 * updated copyright dates
 *
 * Revision 1.3  2000/03/13 22:51:20  cgilmore
 * updated license
 *
 * Revision 1.2  2000/03/09 18:12:46  cgilmore
 * made a host of corrections and cosmetic cleanups, including two new
 * additions/alterations to the client-counting test.
 *
 * Revision 1.1  2000/03/08 21:27:30  cgilmore
 * Initial revision
 *
 */
