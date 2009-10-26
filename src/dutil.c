/*
 * dutil.c: Global utility functions for driver backends.
 *
 * Copyright (C) 2009 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: David Lutterkort <lutter@redhat.com>
 */

#include <config.h>
#include <internal.h>

#include <augeas.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "safe-alloc.h"
#include "ref.h"
#include "list.h"
#include "netcf.h"
#include "dutil.h"

#include <netlink/socket.h>
#include <netlink/cache.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>

#include <libxml/parser.h>
#include <libxml/relaxng.h>
#include <libxml/tree.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>

/* Like asprintf, but set *STRP to NULL on error */
int xasprintf(char **strp, const char *format, ...) {
  va_list args;
  int result;

  va_start (args, format);
  result = vasprintf (strp, format, args);
  va_end (args);
  if (result < 0)
      *strp = NULL;
  return result;
}

int add_augeas_xfm_table(struct netcf *ncf,
                         const struct augeas_xfm_table *xfm) {
    int slot, r;
    struct driver *d = ncf->driver;

    if (d->augeas_xfm_num_tables == 0) {
        slot = 0;
        r = ALLOC(d->augeas_xfm_tables);
        ERR_NOMEM(r < 0, ncf);
        d->augeas_xfm_num_tables = 1;
    } else {
        for (slot =0;
             slot < d->augeas_xfm_num_tables
                 && d->augeas_xfm_tables[slot] != NULL;
             slot++);
        if (slot == d->augeas_xfm_num_tables) {
            r = REALLOC_N(ncf->driver->augeas_xfm_tables, slot + 1);
            ERR_NOMEM(r < 0, ncf);
            d->augeas_xfm_num_tables = slot + 1;
        }
    }

    ncf->driver->augeas_xfm_tables[slot] = xfm;
    ncf->driver->copy_augeas_xfm = 1;
    return 0;
 error:
    return -1;
}

int remove_augeas_xfm_table(struct netcf *ncf,
                            const struct augeas_xfm_table *xfm) {
    int slot;
    const int last = ncf->driver->augeas_xfm_num_tables;
    const struct augeas_xfm_table **tables =
        ncf->driver->augeas_xfm_tables;

    for (slot = 0; slot < last && tables[slot] != xfm; slot++);
    if (tables[slot] == xfm) {
        tables[slot] = NULL;
        ncf->driver->copy_augeas_xfm = 1;
    }
    return 0;
}

/* Get the Augeas instance; if we already initialized it, just return
 * it. Otherwise, create a new one and return that.
 */
struct augeas *get_augeas(struct netcf *ncf) {
    int r;

    if (ncf->driver->augeas == NULL) {
        struct augeas *aug;
        char *path;

        r = xasprintf(&path, "%s/lenses", ncf->data_dir);
        ERR_NOMEM(r < 0, ncf);

        aug = aug_init(ncf->root, path, AUG_NO_MODL_AUTOLOAD);
        FREE(path);
        ERR_THROW(aug == NULL, ncf, EOTHER, "aug_init failed");
        ncf->driver->augeas = aug;
        ncf->driver->copy_augeas_xfm = 1;
    }

    if (ncf->driver->copy_augeas_xfm) {
        struct augeas *aug = ncf->driver->augeas;
        /* Only look at a few config files */
        r = aug_rm(aug, "/augeas/load/*");
        ERR_THROW(r < 0, ncf, EOTHER, "aug_rm failed in get_augeas");

        for (int slot = 0; slot < ncf->driver->augeas_xfm_num_tables; slot++) {
            const struct augeas_xfm_table *t =
                ncf->driver->augeas_xfm_tables[slot];
            if (t == NULL)
                continue;
            for (int i=0; i < t->size; i++) {
                r = aug_set(aug, t->pv[i].path, t->pv[i].value);
                ERR_THROW(r < 0, ncf, EOTHER,
                          "transform setup failed to set %s",
                          t->pv[i].path);
            }
        }
        ncf->driver->copy_augeas_xfm = 0;
        ncf->driver->load_augeas = 1;
    }

    if (ncf->driver->load_augeas) {
        struct augeas *aug = ncf->driver->augeas;
        /* Undefine all our variables to work around bug 79 in Augeas */
        aug_defvar(aug, "iptables", NULL);
        aug_defvar(aug, "fw", NULL);
        aug_defvar(aug, "fw_custom", NULL);
        aug_defvar(aug, "ipt_filter", NULL);

        r = aug_load(aug);
        ERR_THROW(r < 0, ncf, EOTHER, "failed to load config files");

        /* FIXME: we need to produce _much_ better diagnostics here - need
         * to analyze what came back in /augeas//error; ultimately, we need
         * to understand whether this is harmless or a real error. For real
         * errors, we need to return an error.
        */
        r = aug_match(aug, "/augeas//error", NULL);
        if (r > 0 && NCF_DEBUG(ncf)) {
            fprintf(stderr, "warning: augeas initialization had errors\n");
            fprintf(stderr, "please file a bug with the following lines in the bug report:\n");
            aug_print(aug, stderr, "/augeas//error");
        }
        ERR_THROW(r > 0, ncf, EOTHER, "errors in loading some config files");
        ncf->driver->load_augeas = 0;
    }
    return ncf->driver->augeas;
 error:
    aug_close(ncf->driver->augeas);
    ncf->driver->augeas = NULL;
    return NULL;
}

ATTRIBUTE_FORMAT(printf, 4, 5)
int defnode(struct netcf *ncf, const char *name, const char *value,
                   const char *format, ...) {
    struct augeas *aug = get_augeas(ncf);
    va_list ap;
    char *expr = NULL;
    int r, created;

    va_start(ap, format);
    r = vasprintf (&expr, format, ap);
    va_end (ap);
    if (r < 0)
        expr = NULL;
    ERR_NOMEM(r < 0, ncf);

    r = aug_defnode(aug, name, expr, value, &created);
    ERR_THROW(r < 0, ncf, EOTHER, "failed to define node %s", name);

    /* Fallthrough intentional */
 error:
    free(expr);
    return (r < 0) ? -1 : created;
}

int aug_fmt_match(struct netcf *ncf, char ***matches, const char *fmt, ...) {
    struct augeas *aug = NULL;
    char *path = NULL;
    va_list args;
    int r;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    va_start(args, fmt);
    r = vasprintf(&path, fmt, args);
    va_end(args);
    if (r < 0) {
        path = NULL;
        ERR_NOMEM(1, ncf);
    }

    r = aug_match(aug, path, matches);
    ERR_COND_BAIL(r < 0, ncf, EOTHER);

    free(path);
    return r;
 error:
    free(path);
    return -1;
}

void free_matches(int nint, char ***intf) {
    if (*intf != NULL) {
        for (int i=0; i < nint; i++)
            FREE((*intf)[i]);
        FREE(*intf);
    }
}

xsltStylesheetPtr parse_stylesheet(struct netcf *ncf,
                                          const char *fname) {
    xsltStylesheetPtr result = NULL;
    char *path = NULL;
    int r;

    r = xasprintf(&path, "%s/xml/%s", ncf->data_dir, fname);
    ERR_NOMEM(r < 0, ncf);

    if (access(path, R_OK) < 0) {
        report_error(ncf, NETCF_EFILE,
                     "Stylesheet %s does not exist or is not readable",
                     path);
        goto error;
    }

    result = xsltParseStylesheetFile(BAD_CAST path);
    ERR_THROW(result == NULL, ncf, EFILE,
              "Could not parse stylesheet %s", path);

 error:
    free(path);
    return result;
}

ATTRIBUTE_FORMAT(printf, 2, 3)
static void apply_stylesheet_error(void *ctx, const char *format, ...) {
    struct netcf *ncf = ctx;
    va_list ap;

    va_start(ap, format);
    vreport_error(ncf, NETCF_EXSLTFAILED, format, ap);
    va_end(ap);
}

xmlDocPtr apply_stylesheet(struct netcf *ncf, xsltStylesheetPtr style,
                           xmlDocPtr doc) {
    xsltTransformContextPtr ctxt;
    xmlDocPtr res = NULL;
    int r;

    ctxt = xsltNewTransformContext(style, doc);
    ERR_NOMEM(ctxt == NULL, ncf);

    xsltSetTransformErrorFunc(ctxt, ncf, apply_stylesheet_error);

    r = xslt_register_exts(ctxt);
    ERR_NOMEM(r < 0, ncf);

    res = xsltApplyStylesheetUser(style, doc, NULL, NULL, NULL, ctxt);
    if ((ctxt->state == XSLT_STATE_ERROR) ||
        (ctxt->state == XSLT_STATE_STOPPED)) {
        xmlFreeDoc(res);
        res = NULL;
        /* Fallback, in case our error handler isn't called */
        report_error(ncf, NETCF_EXSLTFAILED, NULL);
    }

error:
    xsltFreeTransformContext(ctxt);
    return res;
}

char *apply_stylesheet_to_string(struct netcf *ncf, xsltStylesheetPtr style,
                                 xmlDocPtr doc) {
    xmlDocPtr doc_xfm = NULL;
    char *result = NULL;
    int r, result_len;

    doc_xfm = apply_stylesheet(ncf, style, doc);
    ERR_BAIL(ncf);

    r = xsltSaveResultToString((xmlChar **) &result, &result_len,
                               doc_xfm, style);
    ERR_NOMEM(r < 0, ncf);
    xmlFreeDoc(doc_xfm);
    return result;

 error:
    FREE(result);
    xmlFreeDoc(doc_xfm);
    return NULL;
}

/* Callback for reporting RelaxNG errors */
void rng_error(void *ctx, const char *format, ...) {
    struct netcf *ncf = ctx;
    va_list ap;

    va_start(ap, format);
    vreport_error(ncf, NETCF_EXMLINVALID, format, ap);
    va_end(ap);
}

xmlRelaxNGPtr rng_parse(struct netcf *ncf, const char *fname) {
    char *path = NULL;
    xmlRelaxNGPtr result = NULL;
    xmlRelaxNGParserCtxtPtr ctxt = NULL;
    int r;

    r = xasprintf(&path, "%s/xml/%s", ncf->data_dir, fname);
    ERR_NOMEM(r < 0, ncf);

    if (access(path, R_OK) < 0) {
        report_error(ncf, NETCF_EFILE,
                     "File %s does not exist or is not readable", path);
        goto error;
    }

    ctxt = xmlRelaxNGNewParserCtxt(path);
    xmlRelaxNGSetParserErrors(ctxt, rng_error, rng_error, ncf);

    result = xmlRelaxNGParse(ctxt);

 error:
    xmlRelaxNGFreeParserCtxt(ctxt);
    free(path);
    return result;
}

void rng_validate(struct netcf *ncf, xmlDocPtr doc) {
	xmlRelaxNGValidCtxtPtr ctxt;
	int r;

	ctxt = xmlRelaxNGNewValidCtxt(ncf->driver->rng);
	xmlRelaxNGSetValidErrors(ctxt, rng_error, rng_error, ncf);

    r = xmlRelaxNGValidateDoc(ctxt, doc);
    if (r != 0 && ncf->errcode == NETCF_NOERROR)
        report_error(ncf, NETCF_EXMLINVALID,
           "Interface definition fails to validate");

	xmlRelaxNGFreeValidCtxt(ctxt);
}

/* Called from SAX on parsing errors in the XML. */
void catch_xml_error(void *ctx, const char *msg ATTRIBUTE_UNUSED, ...) {
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    if (ctxt != NULL) {
        struct netcf *ncf = ctxt->_private;

        if (ctxt->lastError.level == XML_ERR_FATAL &&
            ctxt->lastError.message != NULL) {
            report_error(ncf, NETCF_EXMLPARSER,
                         "at line %d: %s",
                         ctxt->lastError.line,
                         ctxt->lastError.message);
        }
    }
}

xmlDocPtr parse_xml(struct netcf *ncf, const char *xml_str) {
    xmlParserCtxtPtr pctxt;
    xmlDocPtr xml = NULL;

    /* Set up a parser context so we can catch the details of XML errors. */
    pctxt = xmlNewParserCtxt();
    ERR_NOMEM(pctxt == NULL || pctxt->sax == NULL, ncf);

    pctxt->sax->error = catch_xml_error;
    pctxt->_private = ncf;

    xml = xmlCtxtReadDoc (pctxt, BAD_CAST xml_str, "netcf.xml", NULL,
                          XML_PARSE_NOENT | XML_PARSE_NONET |
                          XML_PARSE_NOWARNING);
    ERR_THROW(xml == NULL, ncf, EXMLPARSER,
              "failed to parse xml document");
    ERR_THROW(xmlDocGetRootElement(xml) == NULL, ncf, EINTERNAL,
              "missing root element");

    xmlFreeParserCtxt(pctxt);
    return xml;
error:
    xmlFreeParserCtxt (pctxt);
    xmlFreeDoc (xml);
    return NULL;
}

char *xml_prop(xmlNodePtr node, const char *name) {
    return (char *) xmlGetProp(node, BAD_CAST name);
}

int init_ioctl_fd(struct netcf *ncf) {
    int ioctl_fd;
    int flags;

    ioctl_fd = socket(AF_INET, SOCK_STREAM, 0);
    ERR_THROW(ioctl_fd < 0, ncf, EINTERNAL, "failed to open socket for interface ioctl");

    flags = fcntl(ioctl_fd, F_GETFD);
    ERR_THROW(flags < 0, ncf, EINTERNAL, "failed to get flags for ioctl socket");

    flags = fcntl(ioctl_fd, F_SETFD, flags | FD_CLOEXEC);
    ERR_THROW(flags < 0, ncf, EINTERNAL, "failed to set FD_CLOEXEC flag on ioctl socket");
    return ioctl_fd;

error:
    if (ioctl_fd >= 0)
        close(ioctl_fd);
    return -1;
}

int netlink_init(struct netcf *ncf) {

    ncf->driver->nl_sock = nl_handle_alloc();
    if (ncf->driver->nl_sock == NULL)
        goto error;
    if (nl_connect(ncf->driver->nl_sock, NETLINK_ROUTE) < 0) {
        goto error;
    }

    ncf->driver->link_cache = rtnl_link_alloc_cache(ncf->driver->nl_sock);
    if (ncf->driver->link_cache == NULL) {
        goto error;
    }
    nl_cache_mngt_provide(ncf->driver->link_cache);

    ncf->driver->addr_cache = rtnl_addr_alloc_cache(ncf->driver->nl_sock);
    if (ncf->driver->addr_cache == NULL) {
        goto error;
    }
    nl_cache_mngt_provide(ncf->driver->addr_cache);

    return 0;

error:
    netlink_close(ncf);
    return -1;
}

int netlink_close(struct netcf *ncf) {

    if (ncf->driver->addr_cache) {
        nl_cache_free(ncf->driver->addr_cache);
        ncf->driver->addr_cache = NULL;
    }
    if (ncf->driver->link_cache) {
        nl_cache_free(ncf->driver->link_cache);
        ncf->driver->link_cache = NULL;
    }
    if (ncf->driver->nl_sock) {
        nl_close(ncf->driver->nl_sock);
        ncf->driver->nl_sock = NULL;
    }
    return 0;
}

int if_is_active(struct netcf *ncf, const char *intf) {
    struct ifreq ifr;

    MEMZERO(&ifr, 1);
    strncpy(ifr.ifr_name, intf, sizeof(ifr.ifr_name));
    ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';
    if (ioctl(ncf->driver->ioctl_fd, SIOCGIFFLAGS, &ifr))  {
        return 0;
    }
    return ((ifr.ifr_flags & IFF_UP) == IFF_UP);
}

const char *if_type(struct netcf *ncf, const char *intf) {
    char *path;
    struct stat stats;
    const char *ret = NULL;

    xasprintf(&path, "/proc/net/vlan/%s", intf);
    ERR_NOMEM(path == NULL, ncf);
    if ((stat (path, &stats) == 0) && S_ISREG (stats.st_mode)) {
        ret = "vlan";
    }
    FREE(path);

    if (ret == NULL) {
        xasprintf(&path, "/sys/class/net/%s/bridge", intf);
        ERR_NOMEM(path == NULL, ncf);
        if (stat (path, &stats) == 0 && S_ISDIR (stats.st_mode))
            ret = "bridge";
        FREE(path);
    }
    if (ret == NULL) {
        xasprintf(&path, "/sys/class/net/%s/bonding", intf);
        ERR_NOMEM(path == NULL, ncf);
        if (stat (path, &stats) == 0 && S_ISDIR (stats.st_mode))
            ret = "bond";
        FREE(path);
    }
    if (ret == NULL)
        ret = "ethernet";

error:
    FREE(path);
    return ret;
}

/* Create a new netcf if instance for interface NAME */
struct netcf_if *make_netcf_if(struct netcf *ncf, char *name) {
    int r;
    struct netcf_if *result = NULL;

    r = make_ref(result);
    ERR_NOMEM(r < 0, ncf);
    result->ncf = ref(ncf);
    result->name = name;
    return result;

 error:
    unref(result, netcf_if);
    return result;
}

/*
 * Test interface
 */
int dutil_get_aug(struct netcf *ncf, const char *ncf_xml, char **aug_xml) {
    xmlDocPtr ncf_doc = NULL, aug_doc = NULL;
    int result = -1;

    ncf_doc = parse_xml(ncf, ncf_xml);
    ERR_BAIL(ncf);

    rng_validate(ncf, ncf_doc);
    ERR_BAIL(ncf);

    *aug_xml = apply_stylesheet_to_string(ncf, ncf->driver->get, ncf_doc);
    ERR_BAIL(ncf);

    /* fallthrough intentional */
    result = 0;
 error:
    xmlFreeDoc(ncf_doc);
    xmlFreeDoc(aug_doc);
    return result;
}

/* Transform the Augeas XML AUG_XML into interface XML NCF_XML */
int dutil_put_aug(struct netcf *ncf, const char *aug_xml, char **ncf_xml) {
    xmlDocPtr ncf_doc = NULL, aug_doc = NULL;
    int result = -1;

    aug_doc = parse_xml(ncf, aug_xml);
    ERR_BAIL(ncf);

    *ncf_xml = apply_stylesheet_to_string(ncf, ncf->driver->put, aug_doc);
    ERR_BAIL(ncf);

    /* fallthrough intentional */
    result = 0;
 error:
    xmlFreeDoc(ncf_doc);
    xmlFreeDoc(aug_doc);
    return result;
}

/* Data that needs to be preserved between calls to the libnl iterator
 * callback.
 */
struct nl_callback_data {
    xmlDocPtr doc;
    xmlNodePtr root;
    xmlNodePtr protov4;
    xmlNodePtr protov6;
    xmlNodePtr mac;
    struct netcf_if *nif;
};

/* add all ip addresses for the given interface to the xml document
*/
static void _add_ips_cb(struct nl_object *obj, void *arg) {
    struct nl_callback_data *cb_data = arg;
    struct rtnl_addr *addr = (struct rtnl_addr *)obj;
    struct netcf *ncf = cb_data->nif->ncf;

    struct nl_addr *local_addr;
    int family, prefix;
    const char *family_str;
    char ip_str[48];
    char prefix_str[16];
    xmlNodePtr *proto, ip_node, cur;
    xmlAttrPtr prop = NULL;

    local_addr = rtnl_addr_get_local(addr);
    family = nl_addr_get_family(local_addr);
    switch (family) {
    case AF_INET:
        family_str = "ipv4";
        proto = &cb_data->protov4;
        break;
    case AF_INET6:
        family_str = "ipv6";
        proto = &cb_data->protov6;
        break;

    default:
        /* Nothing that interests us in this entry */
        return;
    }

    inet_ntop(family, nl_addr_get_binary_addr(local_addr),
              ip_str, sizeof(ip_str));
    prefix = nl_addr_get_prefixlen(local_addr);

    if (*proto == NULL) {
        /* We haven't dont anything with this proto yet. Search for an
         * existing node.
         */
        for (cur = cb_data->root->children; cur != NULL; cur = cur->next) {
            if ((cur->type == XML_ELEMENT_NODE) &&
                xmlStrEqual(cur->name, BAD_CAST "protocol")) {
                xmlChar *node_family = xmlGetProp(cur, BAD_CAST "family");
                if (node_family != NULL) {
                    if (xmlStrEqual(node_family, BAD_CAST family_str))
                        *proto = cur;
                    xmlFree(node_family);
                    if (*proto != NULL) {
                        break;
                    }
                }
            }
        }
    }

    if (*proto == NULL) {
        /* No node exists for this protocol family. Create one.
         */
        *proto = xmlNewDocNode(cb_data->doc, NULL, BAD_CAST "protocol", NULL);
        ERR_NOMEM(*proto == NULL, ncf);

        cur = xmlAddChild(cb_data->root, *proto);
        if (cur == NULL) {
            xmlFreeNode(*proto);
            *proto = NULL;
            report_error(ncf, NETCF_ENOMEM, NULL);
            goto error;
        }
        prop = xmlSetProp(*proto, BAD_CAST "family", BAD_CAST family_str);
        ERR_NOMEM(prop == NULL, ncf);

    }

    /* Create a new ip node for this address/prefix, and set the
     * properties
     */
    ip_node = xmlNewDocNode(cb_data->doc, NULL, BAD_CAST "ip", NULL);
    ERR_NOMEM((ip_node == NULL), ncf);
    cur = xmlAddChild(*proto, ip_node);
    if (cur == NULL) {
        xmlFreeNode(ip_node);
        report_error(ncf, NETCF_ENOMEM, NULL);
        goto error;
    }
    prop = xmlSetProp(ip_node, BAD_CAST "address", BAD_CAST ip_str);
    ERR_NOMEM((prop == NULL), ncf);
    snprintf(prefix_str, sizeof(prefix_str), "%d", prefix);
    prop = xmlSetProp(ip_node, BAD_CAST "prefix", BAD_CAST prefix_str);
    ERR_NOMEM((prop == NULL), ncf);

error:
    return;
}

static void add_mac_cb(struct nl_object *obj, void *arg) {
    struct nl_callback_data *cb_data = arg;
    struct rtnl_link *iflink = (struct rtnl_link *)obj;
    struct netcf *ncf = cb_data->nif->ncf;

    struct nl_addr *addr;
    char mac_str[64];
    xmlNodePtr cur;
    xmlAttrPtr prop = NULL;

    if (cb_data->mac != NULL)
        return;

    addr = rtnl_link_get_addr(iflink);
    if ((addr == NULL) || nl_addr_iszero(addr))
        return;

    nl_addr2str(addr, mac_str, sizeof(mac_str));

    for (cur = cb_data->root->children; cur != NULL; cur = cur->next) {
        if ((cur->type == XML_ELEMENT_NODE) &&
            xmlStrEqual(cur->name, BAD_CAST "mac")) {
            cb_data->mac = cur;
            break;
        }
    }

    if (cb_data->mac == NULL) {
        /* No mac node exists in the document, create a new one.
         */
        cb_data->mac = xmlNewDocNode(cb_data->doc, NULL, BAD_CAST "mac", NULL);
        ERR_NOMEM(cb_data->mac == NULL, ncf);

        cur = xmlAddChild(cb_data->root, cb_data->mac);
        if (cur == NULL) {
            xmlFreeNode(cb_data->mac);
            cb_data->mac = NULL;
            report_error(ncf, NETCF_ENOMEM, NULL);
            goto error;
        }
    }

    prop = xmlSetProp(cb_data->mac, BAD_CAST "address", BAD_CAST mac_str);
    ERR_NOMEM(prop == NULL, ncf);

error:
    return;
}

void add_state_to_xml_doc(struct netcf_if *nif, xmlDocPtr doc) {

    struct nl_callback_data cb_data = { doc, NULL, NULL, NULL, NULL, nif };
    struct rtnl_addr *filter_addr = NULL;
    struct rtnl_link *filter_link = NULL;
    int ifindex, code;

    cb_data.root = xmlDocGetRootElement(doc);
    ERR_THROW((cb_data.root == NULL), nif->ncf, EINTERNAL,
              "failed to get document root element");
    ERR_THROW(!xmlStrEqual(cb_data.root->name, BAD_CAST "interface"),
              nif->ncf, EINTERNAL, "root document is not an interface");

    /* Update the caches with any recent changes */
    code = nl_cache_refill(nif->ncf->driver->nl_sock,
                           nif->ncf->driver->link_cache);
    ERR_THROW((code < 0), nif->ncf, ENETLINK,
              "failed to refill interface index cache");
    code = nl_cache_refill(nif->ncf->driver->nl_sock,
                           nif->ncf->driver->addr_cache);
    ERR_THROW((code < 0), nif->ncf, ENETLINK,
              "failed to refill interface address cache");

    /* The addr cache only knows about ifindex, not name */
    ifindex = rtnl_link_name2i(nif->ncf->driver->link_cache, nif->name);
    ERR_THROW((ifindex == RTNL_LINK_NOT_FOUND), nif->ncf, ENETLINK,
              "Could find ifindex for interface `%s`", nif->name);

    /* Build an rtnl_link with the interface index set, and use it to
     * find the entry for this interface and extract the mac
     * address.
     */
    filter_link = rtnl_link_alloc();
    ERR_NOMEM((filter_link == NULL), nif->ncf);

    rtnl_link_set_ifindex(filter_link, ifindex);
    nl_cache_foreach_filter(nif->ncf->driver->link_cache,
                            OBJ_CAST(filter_link), add_mac_cb,
                            &cb_data);

    /* Build an rtnl_addr with the interface name set. This is used by
     * the iterator to filter the contents of the address cache.
     */
    filter_addr = rtnl_addr_alloc();
    ERR_NOMEM((filter_addr == NULL), nif->ncf);

    rtnl_addr_set_ifindex(filter_addr, ifindex);

    nl_cache_foreach_filter(nif->ncf->driver->addr_cache,
                            OBJ_CAST(filter_addr), _add_ips_cb,
                            &cb_data);
error:
    if (filter_addr)
        rtnl_addr_put(filter_addr);
    if (filter_link)
        rtnl_link_put(filter_link);
    return;
}

/*
 * Bringing interfaces up/down
 */

/* Run the program PROG with the single argument ARG */
void run1(struct netcf *ncf, const char *prog, const char *arg) {
    const char *const argv[] = {
        prog, arg, NULL
    };

    run_program(ncf, argv);
}

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
/* vim: set ts=4 sw=4 et: */