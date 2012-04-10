/*
 * NAXSI, a web application firewall for NGINX
 * Copyright (C) 2011, Thibault 'bui' Koechlin
 *  
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "naxsi.h"


char	*
strnchr(const char *s, int c, int len)
{
  int	cpt;
  for (cpt = 0; cpt < len && s[cpt]; cpt++)
    if (s[cpt] == c) 
      return ((char *) s+cpt);
  return (NULL);
}

char	*
strncasechr(const char *s, int c, int len)
{
  int	cpt;
  for (cpt = 0; cpt < len && s[cpt]; cpt++)
    if (tolower(s[cpt]) == c) 
      return ((char *) s+cpt);
  return (NULL);
}

/*
** strstr: faster, stronger, harder
** (because strstr from libc is very slow)
*/
char *
strfaststr(unsigned char *haystack, unsigned int hl, 
		 unsigned char *needle, unsigned int nl)
{
  char	*cpt, *found, *end;
  if (hl < nl || !haystack || !needle || !nl || !hl) return (NULL);
  cpt = (char *) haystack;
  end = (char *) haystack + hl;
  while (cpt < end) {
      found = strncasechr((const char *) cpt, (int) needle[0], hl);
      if (!found) return (NULL);
      if (nl == 1) return (found);
      if (!strncasecmp((const char *)found+1, (const char *) needle+1, nl-1))
	return ((char *) found);
      else {
	  if (found+nl >= end)
	    break;
	  if (found+nl < end)
	    cpt = found+1;
	}
    }
  return (NULL);
}

/*
** Patched ngx_unescape_uri : 
** The original one does not care if the character following % is in valid range.
** For example, with the original one :
** '%uff' -> 'uff'
*/
void
naxsi_unescape_uri(u_char **dst, u_char **src, size_t size, ngx_uint_t type)
{
    u_char  *d, *s, ch, c, decoded;
    enum {
        sw_usual = 0,
        sw_quoted,
        sw_quoted_second
    } state;

    d = *dst;
    s = *src;

    state = 0;
    decoded = 0;

    while (size--) {

        ch = *s++;

        switch (state) {
        case sw_usual:
            if (ch == '?'
                && (type & (NGX_UNESCAPE_URI|NGX_UNESCAPE_REDIRECT)))
            {
                *d++ = ch;
                goto done;
            }

            if (ch == '%') {
                state = sw_quoted;
                break;
            }

            *d++ = ch;
            break;

        case sw_quoted:

            if (ch >= '0' && ch <= '9') {
                decoded = (u_char) (ch - '0');
                state = sw_quoted_second;
                break;
            }

            c = (u_char) (ch | 0x20);
            if (c >= 'a' && c <= 'f') {
                decoded = (u_char) (c - 'a' + 10);
                state = sw_quoted_second;
                break;
            }

            /* the invalid quoted character */

            state = sw_usual;
	    *d++ = '%';
            *d++ = ch;

            break;

        case sw_quoted_second:

            state = sw_usual;

            if (ch >= '0' && ch <= '9') {
                ch = (u_char) ((decoded << 4) + ch - '0');

                if (type & NGX_UNESCAPE_REDIRECT) {
                    if (ch > '%' && ch < 0x7f) {
                        *d++ = ch;
                        break;
                    }

                    *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);

                    break;
                }

                *d++ = ch;

                break;
            }

            c = (u_char) (ch | 0x20);
            if (c >= 'a' && c <= 'f') {
                ch = (u_char) ((decoded << 4) + c - 'a' + 10);

                if (type & NGX_UNESCAPE_URI) {
                    if (ch == '?') {
                        *d++ = ch;
                        goto done;
                    }

                    *d++ = ch;
                    break;
                }

                if (type & NGX_UNESCAPE_REDIRECT) {
                    if (ch == '?') {
                        *d++ = ch;
                        goto done;
                    }

                    if (ch > '%' && ch < 0x7f) {
                        *d++ = ch;
                        break;
                    }

                    *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);
                    break;
                }

                *d++ = ch;

                break;
            }

            /* the invalid quoted character */

            break;
        }
    }

done:

    *dst = d;
    *src = s;
}

//#define whitelist_heavy_debug

#ifdef whitelist_heavy_debug
#define whitelist_light_debug
#define whitelist_debug
#endif

#ifdef whitelist_debug
#define whitelist_light_debug
#endif


/* push rule into disabled rules. */
ngx_int_t 
ngx_http_wlr_push_disabled(ngx_conf_t *cf, ngx_http_dummy_loc_conf_t *dlc, 
			   ngx_http_rule_t *curr) {
  ngx_http_rule_t	**dr;
#ifdef whitelist_debug
  ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "[naxsi] rule %d disabled",
		     curr->wl_id[0]);
#endif
  if (!dlc->disabled_rules)
    dlc->disabled_rules = ngx_array_create(cf->pool, 4, 
					   sizeof(ngx_http_rule_t *));
  if (!dlc->disabled_rules)
    return (NGX_ERROR);
  dr = ngx_array_push(dlc->disabled_rules);
  if (!dr)
    return (NGX_ERROR);
  *dr = (ngx_http_rule_t *) curr;
  return (NGX_OK);
}

/* merge the two rules into father_wl, meaning 
   ids. Not locations, as we are getting rid of it */
ngx_int_t 
ngx_http_wlr_merge(ngx_conf_t *cf, ngx_http_whitelist_rule_t *father_wl, 
		   ngx_http_rule_t *curr) {
  uint i;
  ngx_int_t		*tmp_ptr;
  
  if (!father_wl->ids)
    {
      father_wl->ids = ngx_array_create(cf->pool, 3, sizeof(int));
      if (!father_wl->ids)
	return (NGX_ERROR);
    }
  for (i = 0; curr->wl_id[i] >= 0 ; i++) {
    tmp_ptr = ngx_array_push(father_wl->ids);
    if (!tmp_ptr)
      return (NGX_ERROR);
    *tmp_ptr = curr->wl_id[i];
  }
  return (NGX_OK);
}

/*check rule, returns associed zone, as well as location index.
  location index refers to $URL:bla or $ARGS_VAR:bla */
#define custloc_array(x) ((ngx_http_custom_rule_location_t *) x)

ngx_int_t 
ngx_http_wlr_identify(ngx_conf_t *cf, ngx_http_dummy_loc_conf_t *dlc, 
		      ngx_http_rule_t *curr, int *zone,
		      int *uri_idx, int *name_idx) {
  
  uint	i;

  /*
    identify global match zones (|ARGS|BODY|HEADERS|URL|FILE_EXT)
   */
  if (curr->br->body || curr->br->body_var)
    *zone = BODY;
  else if (curr->br->headers || curr->br->headers_var)
    *zone = HEADERS;
  else if (curr->br->args || curr->br->args_var)
    *zone = ARGS;
  else if (curr->br->url) /*don't assume that named $URL means zone is URL.*/
    *zone = URL;
  else if (curr->br->file_ext)
    *zone = FILE_EXT;
  /*
    if we're facing a WL in the style $URL:/bla|ARGS (or any other zone),
    push it to 
   */
  for (i = 0; i < curr->br->custom_locations->nelts; i++) {
    /*
      locate target URL if exists ($URL:/bla|ARGS) or ($URL:/bla|$ARGS_VAR:foo)
     */
    if (custloc_array(curr->br->custom_locations->elts)[i].specific_url) {
#ifdef whitelist_heavy_debug
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
			 "whitelist has URI %V", &(custloc_array(curr->br->custom_locations->elts)[i].target));
#endif
      *uri_idx = i;
    }
    /*
      identify named match zones ($ARGS_VAR:bla|$HEADERS_VAR:bla|$BODY_VAR:bla)
    */
    if (custloc_array(curr->br->custom_locations->elts)[i].body_var) {
#ifdef whitelist_heavy_debug
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
			 "whitelist has body_var %V", &(custloc_array(curr->br->custom_locations->elts)[i].target));
#endif
      *name_idx = i;
      *zone = BODY;
    }
    if (custloc_array(curr->br->custom_locations->elts)[i].headers_var) {
#ifdef whitelist_heavy_debug
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
			 "whitelist has header_var %V", &(custloc_array(curr->br->custom_locations->elts)[i].target));
#endif
      *name_idx = i;
      *zone = HEADERS;
    }
    if (custloc_array(curr->br->custom_locations->elts)[i].args_var) {
#ifdef whitelist_heavy_debug
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
			 "whitelist has arg_var %V", &(custloc_array(curr->br->custom_locations->elts)[i].target));
#endif
      *name_idx = i;
      *zone = ARGS;
    }
  }
  if (*zone == -1)
    return (NGX_ERROR);
  return (NGX_OK);
}


ngx_http_whitelist_rule_t *
ngx_http_wlr_find(ngx_conf_t *cf, ngx_http_dummy_loc_conf_t *dlc,
		  ngx_http_rule_t *curr, int zone, int uri_idx,
		  int name_idx, char **fullname) {
  uint i;
  
  /* Create unique string for rule, and try to find it in existing rules.*/
  /*name AND uri*/
  
  if (uri_idx != -1 && name_idx != -1) {
#ifdef whitelist_heavy_debug
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
		       "whitelist has uri + name");
#endif
    /* allocate one extra byte in case curr->br->target_name is set. */
    *fullname = ngx_pcalloc(cf->pool, custloc_array(curr->br->custom_locations->elts)[name_idx].target.len +
			    custloc_array(curr->br->custom_locations->elts)[uri_idx].target.len + 3);
    /* if WL targets variable name instead of content, prefix hash with '#' */
    if (curr->br->target_name)
      strncat(*fullname, (const char *) "#", 1);
    strncat(*fullname, (const char *) custloc_array(curr->br->custom_locations->elts)[uri_idx].target.data, 
	    custloc_array(curr->br->custom_locations->elts)[uri_idx].target.len);
    strncat(*fullname, (const char *) "#", 1);
    strncat(*fullname, (const char *) custloc_array(curr->br->custom_locations->elts)[name_idx].target.data, 
	    custloc_array(curr->br->custom_locations->elts)[name_idx].target.len);
  }
  /* only uri */
  else if (uri_idx != -1 && name_idx == -1) {
#ifdef whitelist_heavy_debug
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
		       "whitelist has uri");
#endif
    //XXX set flag only_uri
    *fullname = ngx_pcalloc(cf->pool, custloc_array(curr->br->custom_locations->elts)[uri_idx].target.len + 1);
    strncat(*fullname, (const char *) custloc_array(curr->br->custom_locations->elts)[uri_idx].target.data, 
	    custloc_array(curr->br->custom_locations->elts)[uri_idx].target.len);
  }
  /* only name */
  else if (name_idx != -1) {
#ifdef whitelist_heavy_debug
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
		       "whitelist has name");
#endif
    *fullname = ngx_pcalloc(cf->pool, custloc_array(curr->br->custom_locations->elts)[name_idx].target.len + 2);
    if (curr->br->target_name)
      strncat(*fullname, (const char *) "#", 1);
    strncat(*fullname, (const char *) custloc_array(curr->br->custom_locations->elts)[name_idx].target.data, 
	    custloc_array(curr->br->custom_locations->elts)[name_idx].target.len);
  }
  /* problem houston */
  else
    return (NULL);
  
  for (i = 0; i < dlc->tmp_wlr->nelts; i++)
    if (!strcmp((const char *)*fullname, (const char *)((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].name->data) && 
	((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].zone == (uint) zone)
      {
#ifdef whitelist_heavy_debug
	ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
			   "found existing 'same' WL : %V", ((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].name);
#endif
	return (&((ngx_http_whitelist_rule_t *)dlc->tmp_wlr->elts)[i]);
      }
  return (NULL);
}



#define httprule_array(x) ((ngx_http_rule_t *) x)


ngx_int_t
ngx_http_wlr_finalize_hashtables(ngx_conf_t *cf, ngx_http_dummy_loc_conf_t  *dlc) {
  int get_sz = 0, headers_sz = 0, body_sz = 0, uri_sz = 0;
  ngx_array_t *get_ar = NULL, *headers_ar = NULL, *body_ar = NULL, *uri_ar = NULL;
  ngx_hash_key_t *arr_node;
  ngx_hash_init_t hash_init;
  uint i;
  
#ifdef whitelist_heavy_debug
  ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
		     "finalizing hashtables");
#endif
  
  for (i = 0; i < dlc->tmp_wlr->nelts; i++) {
    switch (((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].zone) {
    case FILE_EXT:
    case BODY:
      body_sz++;
      break;
    case HEADERS:
      headers_sz++;
      break;
    case URL:
      uri_sz++;
      break;
    case ARGS:
      get_sz++;
      break;
    case UNKNOWN:
    default:
      return (NGX_ERROR);
    }
  }
#ifdef whitelist_heavy_debug
  ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
		     "nb items : body:%d headers:%d uri:%d get:%d",
		     body_sz, headers_sz, uri_sz, get_sz);
#endif

  if (get_sz)
    get_ar = ngx_array_create(cf->pool, get_sz, sizeof(ngx_hash_key_t));
  if (headers_sz)
    headers_ar = ngx_array_create(cf->pool, headers_sz, sizeof(ngx_hash_key_t));
  if (body_sz)
    body_ar = ngx_array_create(cf->pool, body_sz, sizeof(ngx_hash_key_t));
  if (uri_sz)
    uri_ar = ngx_array_create(cf->pool, uri_sz, sizeof(ngx_hash_key_t));
  for (i = 0; i < dlc->tmp_wlr->nelts; i++) {
    switch (((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].zone) {
    case FILE_EXT:
    case BODY:
      arr_node = (ngx_hash_key_t*) ngx_array_push(body_ar);
      break;
    case HEADERS:
      arr_node = (ngx_hash_key_t*) ngx_array_push(headers_ar);
      break;
    case URL:
      arr_node = (ngx_hash_key_t*) ngx_array_push(uri_ar);
      break;
    case ARGS:
      arr_node = (ngx_hash_key_t*) ngx_array_push(get_ar);
      break;
    default:
      return (NGX_ERROR);
    }
    ngx_memset(arr_node, 0, sizeof(ngx_hash_key_t));
    arr_node->key = *(((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].name);
    arr_node->key_hash = ngx_hash_key_lc(((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].name->data, 
					 ((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].name->len);
    arr_node->value = (void *) &(((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i]);
#ifdef whitelist_heavy_debug
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "pushing new WL, zone:%d, target:%V, %d IDs",
		       ((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].zone ,  ((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].name,
		        ((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].ids->nelts);
    unsigned int z;
    for (z = 0; z < ((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].ids->nelts; z++)
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "id:%d",
    			 ((int *)((ngx_http_whitelist_rule_t *) dlc->tmp_wlr->elts)[i].ids->elts)[z]);
#endif    
  }
  hash_init.key = &ngx_hash_key_lc;
  hash_init.pool = cf->pool;
  hash_init.temp_pool = NULL;
  hash_init.max_size  = 1024;
  hash_init.bucket_size = 512;
  
  if (body_ar) {
    dlc->wlr_body_hash =  (ngx_hash_t*) ngx_pcalloc(cf->pool, sizeof(ngx_hash_t));
    hash_init.hash = dlc->wlr_body_hash;
    hash_init.name = "wlr_body_hash";
    if (ngx_hash_init(&hash_init, (ngx_hash_key_t*) body_ar->elts, 
		      body_ar->nelts) != NGX_OK) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$BODY hashtable init failed");
      return (NGX_ERROR);
    }

#ifdef whitelist_debug
    else
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$BODY hashtable init successed !");
#endif
  }
  if (uri_ar) {
    dlc->wlr_url_hash =  (ngx_hash_t*) ngx_pcalloc(cf->pool, sizeof(ngx_hash_t));
    hash_init.hash = dlc->wlr_url_hash;
    hash_init.name = "wlr_url_hash";
    if (ngx_hash_init(&hash_init, (ngx_hash_key_t*) uri_ar->elts, 
		      uri_ar->nelts) != NGX_OK) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$URL hashtable init failed");
      return (NGX_ERROR);
    }
#ifdef whitelist_debug
    else
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$URL hashtable init successed !");
#endif
  }
  if (get_ar) {
    dlc->wlr_args_hash =  (ngx_hash_t*) ngx_pcalloc(cf->pool, sizeof(ngx_hash_t));
    hash_init.hash = dlc->wlr_args_hash;
    hash_init.name = "wlr_args_hash";
    if (ngx_hash_init(&hash_init, (ngx_hash_key_t*) get_ar->elts, 
		      get_ar->nelts) != NGX_OK) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$ARGS hashtable init failed");
      return (NGX_ERROR);
    }
#ifdef whitelist_debug
    else
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$ARGS hashtable init successed %d !",
			 dlc->wlr_args_hash->size);
#endif

  }
  if (headers_ar) {
    dlc->wlr_headers_hash =  (ngx_hash_t*) ngx_pcalloc(cf->pool, sizeof(ngx_hash_t));
    hash_init.hash = dlc->wlr_headers_hash;
    hash_init.name = "wlr_headers_hash";
    if (ngx_hash_init(&hash_init, (ngx_hash_key_t*) headers_ar->elts, 
		      headers_ar->nelts) != NGX_OK) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$HEADERS hashtable init failed");
      return (NGX_ERROR);
    }
#ifdef whitelist_debug
    else
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "$HEADERS hashtable init successed %d !",
			 dlc->wlr_headers_hash->size);
#endif

  }
  return (NGX_OK);
}


/*
** This function will take the whitelist basicrules generated during the configuration
** parsing phase, and aggregate them to build hashtables according to the matchzones.
** 
** As whitelist can be in the form :
** "mz:$URL:bla|$ARGS_VAR:foo"
** "mz:$URL:bla|ARGS"
** "mz:$HEADERS_VAR:Cookie"
** ...
**
** So, we will aggregate all the rules that are pointing to the same URL together, 
** as well as rules targetting the same argument name / zone.
*/

//#define whitelist_heavy_debug
ngx_int_t
ngx_http_dummy_create_hashtables_n(ngx_http_dummy_loc_conf_t *dlc, 
				   ngx_conf_t *cf)
{
  int				zone, uri_idx, name_idx, ret;
  ngx_http_rule_t		*curr_r/*, *father_r*/;
  ngx_http_whitelist_rule_t	*father_wlr;
  unsigned char			*fullname;
  uint	i;

  if (!dlc->whitelist_rules || dlc->whitelist_rules->nelts < 1) {
#ifdef whitelist_heavy_debug    
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
		       "No whitelist registred, but it's your call.");    
#endif
    return (NGX_OK);
  }
#ifdef whitelist_heavy_debug
  ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
		     "Building whitelist hashtables, %d items in list",
		     dlc->whitelist_rules->nelts);
#endif
  dlc->tmp_wlr = ngx_array_create(cf->pool, dlc->whitelist_rules->nelts,
  				  sizeof(ngx_http_whitelist_rule_t));
  /* iterate through each stored whitelist rule. */
  for (i = 0; i < dlc->whitelist_rules->nelts; i++) {
    uri_idx = name_idx = zone = -1;
    /*a whitelist is in fact just another basic_rule_t */
    curr_r = &(httprule_array(dlc->whitelist_rules->elts)[i]);
#ifdef whitelist_heavy_debug
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
		       "Processing wl %d/%p", i, curr_r);
#endif
    /*no custom location at all means that the rule is disabled */
    if (!curr_r->br->custom_locations) {
#ifdef whitelist_heavy_debug
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			 "WL %d is a disable rule.", i);
#endif
      if (ngx_http_wlr_push_disabled(cf, dlc, curr_r) == NGX_ERROR)
	return (NGX_ERROR);
      continue;
    }
    ret = ngx_http_wlr_identify(cf, dlc, curr_r, &zone, &uri_idx, &name_idx);
    if (ret != NGX_OK)
      {
	ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
			   "naxsi internal error in wlr_identify.");
	return (NGX_ERROR);
      }
    father_wlr = ngx_http_wlr_find(cf, dlc, curr_r, zone, uri_idx, name_idx, (char **) &fullname);
    if (!father_wlr) {
#ifdef whitelist_heavy_debug
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
			 "creating fresh WL [%s].", fullname);
#endif
      /* creates a new whitelist rule in the right place.
	 setup name and zone, create a new (empty) whitelist_location, as well
	 as a new (empty) id aray. */
      father_wlr = ngx_array_push(dlc->tmp_wlr);
      if (!father_wlr)
	return (NGX_ERROR);
      memset(father_wlr, 0, sizeof(ngx_http_whitelist_rule_t));
      father_wlr->name = ngx_pcalloc(cf->pool, sizeof(ngx_str_t));
      if (!father_wlr->name)
	return (NGX_ERROR);
      father_wlr->name->len = strlen((const char *) fullname);
      father_wlr->name->data = fullname;
      father_wlr->zone = zone;
      /* If there is URI and no name idx, specify it,
	 so that WL system won't get fooled by an argname like an URL */
      if (uri_idx != -1 && name_idx == -1)
	father_wlr->uri_only = 1;
      /* If target_name is present in son, report it. */
      if (curr_r->br && curr_r->br->target_name)
        father_wlr->target_name = curr_r->br->target_name; 
    }
    /*merges the two whitelist rules together, including custom_locations. */
    if (ngx_http_wlr_merge(cf, father_wlr, curr_r) != NGX_OK)
      return (NGX_ERROR);
  }
  
  /* and finally, build the hashtables for various zones. */
  if (ngx_http_wlr_finalize_hashtables(cf, dlc) != NGX_OK)
    return (NGX_ERROR);
  /* TODO : Free old whitelist_rules (dlc->whitelist_rules)*/
  return (NGX_OK);
}

