/*
 * xslt_ext.c: XSLT extension functions needed by the stylesheets
 *
 * Copyright (C) 2009, 2014, 2015 Red Hat Inc.
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
#include "internal.h"
#include "dutil.h"

#include <errno.h>
#include <arpa/inet.h>

#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxslt/xsltutils.h>
#include <libxslt/extensions.h>

// FIXME: Find a better URI for this
#define XSLT_EXT_IPCALC_NS                                  \
    (BAD_CAST "http://redhat.com/xslt/netcf/ipcalc/1.0")
#define XSLT_EXT_BOND_NS                                \
    (BAD_CAST "http://redhat.com/xslt/netcf/bond/1.0")
#define XSLT_EXT_PATHCOMPONENT_NS                       \
    (BAD_CAST "http://redhat.com/xslt/netcf/pathcomponent/1.0")

/* Given an IP prefix like "24", compute the netmask "255.255.255.0"
 */
static void ipcalc_netmask(xmlXPathParserContextPtr ctxt, int nargs) {
    double raw_prefix;
    unsigned long prefix = 0;
    struct in_addr netmask;
    xmlChar netmask_str[16];

    if (nargs != 1) {
        xmlXPathSetArityError(ctxt);
        goto error;
    }

    raw_prefix = xmlXPathPopNumber(ctxt);
    if (xmlXPathCheckError(ctxt)) {
        xsltTransformError(xsltXPathGetTransformContext(ctxt), NULL, NULL,
                    "ipcalc:netmask: failed to get prefix as number");
        goto error;
    }

    prefix = raw_prefix;
    if ((double) prefix != raw_prefix) {
        xsltTransformError(xsltXPathGetTransformContext(ctxt), NULL, NULL,
                    "ipcalc:netmask: failed to convert prefix to int");
        goto error;
    }

    if (prefix == 0 || prefix > 32) {
        xsltTransformError(xsltXPathGetTransformContext(ctxt), NULL, NULL,
                           "ipcalc:netmask: prefix %lu not in the range 1 to 32", prefix);
        goto error;
    }

    /* We need to special-case 32, because the result of "x >> 32"
     * is undefined when the number of bits in x's type is == 32.
     */
    if (prefix == 32)
        netmask.s_addr = 0xffffffffu;
    else
        netmask.s_addr = htonl(~(0xffffffffu >> prefix));

    if (! inet_ntop(AF_INET, &netmask,
                    (char *) netmask_str, sizeof(netmask_str))) {
        xsltTransformError(xsltXPathGetTransformContext(ctxt), NULL, NULL,
                    "ipcalc:netmask: internal error: inet_ntop failed");
        goto error;
    }
    netmask_str[sizeof(netmask_str)-1] = '\0';
    xmlXPathReturnString(ctxt, xmlStrdup(netmask_str));
 error:
    return;
}

/* Given a netmask like "255.255.0.0" convert it into a prefix, e.g. "/16".
 * If the netmask is the empty string, return the empty string.
 */
static void ipcalc_prefix(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlChar *netmask_str = NULL;
    char *prefix_str = NULL;
    struct in_addr netmask;
    unsigned int prefix = 8*sizeof(netmask.s_addr);
    int r;

    if (nargs != 1) {
        xmlXPathSetArityError(ctxt);
        goto error;
    }

    netmask_str = xmlXPathPopString(ctxt);
    if (xmlStrlen(netmask_str) == 0) {
        xmlXPathReturnEmptyString(ctxt);
        goto error;
    }

    r = inet_pton(AF_INET, (char *) netmask_str, &netmask);
    if (r < 0) {
        xsltTransformError(xsltXPathGetTransformContext(ctxt), NULL, NULL,
                           "ipcalc:prefix: illegal netmask '%s'",
                           netmask_str);
        goto error;
    }

    for (int i = 0; i < 8*sizeof(netmask.s_addr); i++) {
        if (!(ntohl(netmask.s_addr) & ((2 << i) - 1)))
            prefix--;
    }

    if (asprintf(&prefix_str, "%d", prefix) < 0) {
        prefix_str = NULL;
        goto error;
    }

    xmlXPathReturnString(ctxt, BAD_CAST prefix_str);
    prefix_str = NULL;
 error:
    xmlFree(netmask_str);
    free(prefix_str);
    return;
}

/* Given BONDING_OPTS, fish out the 'mode=FOO' token and return 'FOO'
 */
static void bond_option(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlChar *bond_opts = NULL;
    xmlChar *name = NULL;
    const xmlChar *val, *val_end;

    if (nargs != 2) {
        xmlXPathSetArityError(ctxt);
        return;
    }

    name = xmlXPathPopString(ctxt);
    bond_opts = xmlXPathPopString(ctxt);
    val = xmlStrstr(bond_opts, name);
    if (val == NULL) {
        xmlXPathReturnEmptyString(ctxt);
        goto done;
    }
    val += xmlStrlen(name);
    if (*val != '=') {
        // FIXME: We should really go look for the next occurrence of name
        xmlXPathReturnEmptyString(ctxt);
        goto done;
    }
    val += 1;
    for (val_end = val;
         *val_end != '\0' && xmlStrchr(BAD_CAST " \t'\"", *val_end) == NULL;
         val_end++);

    int len = xmlStrlen(val) - xmlStrlen(val_end);
    xmlXPathReturnString(ctxt, xmlStrndup(val, len));
 done:
    xmlFree(name);
    xmlFree(bond_opts);
}


static void
pathcomponent_escape(xmlXPathParserContextPtr ctxt, int nargs) {
    xmlChar *orig_xmlstr = NULL;
    char *escaped_str = NULL;
    int r;

    if (nargs != 1) {
        xmlXPathSetArityError(ctxt);
        goto error;
    }

    orig_xmlstr = xmlXPathPopString(ctxt);
    if (xmlStrlen(orig_xmlstr) == 0) {
        xmlXPathReturnEmptyString(ctxt);
        goto error;
    }

    r = aug_escape_name_base((const char *)orig_xmlstr, &escaped_str);
    if (r < 0) {
        xsltTransformError(xsltXPathGetTransformContext(ctxt), NULL, NULL,
                           "pathcomponent:escape: Out of Memory");
        goto error;
    }

    if (escaped_str)
        xmlXPathReturnString(ctxt, BAD_CAST escaped_str);
    else
        xmlXPathReturnString(ctxt, xmlStrdup(orig_xmlstr));
    escaped_str = NULL;
 error:
    xmlFree(orig_xmlstr);
    free(escaped_str);
    return;
}


int xslt_register_exts(xsltTransformContextPtr ctxt) {
    int r;

    r = xsltRegisterExtFunction(ctxt, BAD_CAST "netmask",
                                XSLT_EXT_IPCALC_NS, ipcalc_netmask);
    if (r < 0)
        return r;

    r = xsltRegisterExtFunction(ctxt, BAD_CAST "prefix",
                                XSLT_EXT_IPCALC_NS, ipcalc_prefix);
    if (r < 0)
        return r;

    r = xsltRegisterExtFunction(ctxt, BAD_CAST "option",
                                XSLT_EXT_BOND_NS, bond_option);
    if (r < 0)
        return r;

    r = xsltRegisterExtFunction(ctxt, BAD_CAST "escape",
                                XSLT_EXT_PATHCOMPONENT_NS,
                                pathcomponent_escape);
    if (r < 0)
        return r;
    return 0;
}
