#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

#include "inet.h"
#include "coap.h"

/* --- PRIVATE -------------------------------------------------------------- */
static const coap_option_t *_find_options(const coap_packet_t *pkt,
                                          const coap_option_num_t num,
                                          uint8_t *count);
static void _option_nibble(const uint32_t value, uint8_t *nibble);

/*
 * options are always stored consecutively,
 * so can return a block with same option num
 */
static const coap_option_t *_find_options(const coap_packet_t *pkt,
                                          const coap_option_num_t num,
                                          uint8_t *count)
{
    const coap_option_t * first = NULL;
    /* loop through packet opts */
    *count = 0;
    for (size_t i = 0; i < pkt->numopts; ++i) {
        if (pkt->opts[i].num == num) {
            if (!first) {
                first = &pkt->opts[i];
            }
            (*count)++;
        }
        /* options are ordered by num, skip if greater */
        else if (pkt->opts[i].num > num) {
            break;
        }
        /* single block for same option num, skip on first match */
        else if (first) {
            break;
        }
    }
    return first;
}

/* https://tools.ietf.org/html/rfc7252#section-3.1 */
static void _option_nibble(const uint32_t value, uint8_t *nibble)
{
    if (value < 13) {
        *nibble = (0xFF & value);
    }
    else if (value <= 0xFF+13) {
        *nibble = 13;
    }
    else if (value <= 0xFFFF+269) {
        *nibble = 14;
    }
}

/* --- PUBLIC --------------------------------------------------------------- */
int coap_build(const coap_packet_t *pkt, uint8_t *buf, size_t *buflen)
{
    // build header
    if (*buflen < (sizeof(coap_raw_header_t) + pkt->hdr.tkl)) {
        return COAP_ERR_BUFFER_TOO_SMALL;
    }
    coap_raw_header_t *r = (coap_raw_header_t *)buf;
    r->hdr.ver = pkt->hdr.ver;
    r->hdr.t = pkt->hdr.t;
    r->hdr.tkl = pkt->hdr.tkl;
    r->hdr.code = pkt->hdr.code;
    r->hdr.id = htons(pkt->hdr.id);
    // inject token
    uint8_t *p = buf + sizeof(coap_raw_header_t);
    if ((pkt->hdr.tkl > 0) && (pkt->hdr.tkl != pkt->tok.len)) {
        return COAP_ERR_UNSUPPORTED;
    }
    if (pkt->hdr.tkl > 0) {
        memcpy(p, pkt->tok.p, pkt->hdr.tkl);
    }
    p += pkt->hdr.tkl;
    // inject options, http://tools.ietf.org/html/rfc7252#section-3.1
    uint16_t running_delta = 0;
    for (size_t i = 0; i < pkt->numopts; ++i) {
        if (((size_t)(p - buf)) > *buflen) {
            return COAP_ERR_BUFFER_TOO_SMALL;
        }
        uint32_t optDelta = pkt->opts[i].num - running_delta;
        uint8_t delta = 0;
        _option_nibble(optDelta, &delta);
        uint8_t len = 0;
        _option_nibble((uint32_t)pkt->opts[i].buf.len, &len);

        *p++ = (0xFF & (delta << 4 | len));
        if (delta == 13) {
            *p++ = (optDelta - 13);
        }
        else if (delta == 14) {
            *p++ = ((optDelta-269) >> 8);
            *p++ = (0xFF & (optDelta-269));
        }
        if (len == 13) {
            *p++ = (pkt->opts[i].buf.len - 13);
        }
        else if (len == 14) {
            *p++ = (pkt->opts[i].buf.len >> 8);
            *p++ = (0xFF & (pkt->opts[i].buf.len-269));
        }

        memcpy(p, pkt->opts[i].buf.p, pkt->opts[i].buf.len);
        p += pkt->opts[i].buf.len;
        running_delta = pkt->opts[i].num;
    }
    // calc number of bytes used by options
    size_t opts_len = (p - buf) - sizeof(coap_raw_header_t);
    if (pkt->payload.len > 0) {
        if (*buflen < sizeof(coap_raw_header_t) + 1 + pkt->payload.len + opts_len) {
            return COAP_ERR_BUFFER_TOO_SMALL;
        }
        buf[sizeof(coap_raw_header_t) + opts_len] = 0xFF;  // payload marker
        memcpy(buf + sizeof(coap_raw_header_t) + opts_len + 1,
               pkt->payload.p, pkt->payload.len);
        *buflen = sizeof(coap_raw_header_t) + opts_len + 1 + pkt->payload.len;
    }
    else {
        *buflen = opts_len + sizeof(coap_raw_header_t);
    }
    return COAP_SUCCESS;
}
int coap_make_request(const uint16_t msgid, const coap_buffer_t* tok,
                      const coap_msgtype_t msgtype,
                      const coap_resource_t *resource,
                      const uint8_t *content, const size_t content_len,
                      coap_packet_t *pkt)
{
    const coap_resource_path_t *path = resource->path;
    // check if path elements + content type fit into option array
    if ((path->count + 1) > COAP_MAX_OPTIONS)
        return COAP_ERR_BUFFER_TOO_SMALL;
    // init request header
    pkt->hdr.ver = 0x01;
    pkt->hdr.t = msgtype;
    pkt->hdr.tkl = 0;
    pkt->hdr.code = resource->method;
    pkt->hdr.id = msgid;
    pkt->numopts = path->count;
    // set token
    if (tok) {
        pkt->hdr.tkl = tok->len;
        pkt->tok = *tok;
    }
    /*
     * NOTE: coap options MUST be in ascending order, i.e.,
     *       COAP_OPTION_URI_PATH (11) < COAP_OPTION_CONTENT_FORMAT (12)
     */
    // copy path to options, first
    int i;
    for (i=0; i < path->count; ++i) {
        pkt->opts[i].num = COAP_OPTION_URI_PATH;
        pkt->opts[i].buf.p = (const uint8_t *) path->elems[i];
        pkt->opts[i].buf.len = strlen(path->elems[i]);
    }
    // set content type, if present afterwards
    if (COAP_GET_CONTENTTYPE(resource->content_type, 2) != COAP_CONTENTTYPE_NONE) {
        pkt->numopts++;
        pkt->opts[i].num = COAP_OPTION_CONTENT_FORMAT;
        pkt->opts[i].buf.p = resource->content_type;
        pkt->opts[i].buf.len = 2;
    }
    // attach payload
    pkt->payload.p = content;
    pkt->payload.len = content_len;
    return COAP_SUCCESS;
}

int coap_make_ack(const uint16_t msgid, const coap_buffer_t* tok,
                  coap_packet_t *pkt)
{
    return coap_make_response(msgid, tok,
                              COAP_TYPE_ACK, COAP_RSPCODE_EMPTY,
                              NULL, NULL, 0, pkt);
}

int coap_make_response(const uint16_t msgid, const coap_buffer_t* tok,
                       const coap_msgtype_t msgtype,
                       const coap_responsecode_t rspcode,
                       const uint8_t *content_type,
                       const uint8_t *content, const size_t content_len,
                       coap_packet_t *pkt)
{
    pkt->hdr.ver = 0x01;
    pkt->hdr.t = msgtype;
    pkt->hdr.tkl = 0;
    pkt->hdr.code = rspcode;
    pkt->hdr.id = msgid;
    pkt->numopts = 0;
    // need token in response
    if (tok) {
        pkt->hdr.tkl = tok->len;
        pkt->tok = *tok;
    }
    if (content_type) {
        pkt->numopts = 1;
        // safe because 1 < COAP_MAX_OPTIONS
        pkt->opts[0].num = COAP_OPTION_CONTENT_FORMAT;
        pkt->opts[0].buf.p = content_type;
        pkt->opts[0].buf.len = 2;
    }
    pkt->payload.p = content;
    pkt->payload.len = content_len;
    return COAP_SUCCESS;
}

int coap_handle_request(const coap_resource_t *resources, size_t resources_len,
                        const coap_packet_t *inpkt,
                        coap_packet_t *pkt)
{
    uint8_t count;
    coap_responsecode_t rspcode = COAP_RSPCODE_NOT_IMPLEMENTED;
    const coap_option_t *opt = coap_find_uri_path(inpkt, &count);
    // find handler for requested resource
    for (size_t j = 0; (j < resources_len) && opt; ++j) {
        if ((resources[j].method == inpkt->hdr.code) && (count == resources[j].path->count)){
            int i;
            for (i = 0; i < count; ++i) {
                if (opt[i].buf.len != strlen(resources[j].path->elems[i])) {
                    break;
                }
                if (memcmp(resources[j].path->elems[i], opt[i].buf.p, opt[i].buf.len)) {
                    break;
                }
            }
            if (i == count) {
                return resources[j].handler(&resources[j], inpkt, pkt);
            }
            rspcode = COAP_RSPCODE_NOT_FOUND;
        }
        else {
            rspcode = COAP_RSPCODE_METHOD_NOT_ALLOWED;
        }
    }
    return coap_make_response(inpkt->hdr.id, &inpkt->tok,
                        COAP_TYPE_ACK, rspcode,
                        NULL, NULL, 0, pkt);
}

int coap_make_link_format(const coap_resource_t *resources, size_t resources_len,
                         char *buf, size_t buflen)
{
    if (buflen < 4) { // <>;
        return COAP_ERR_BUFFER_TOO_SMALL;
    }
    memset(buf, 0, buflen);
    // loop over resources
    int len = buflen - 1;
    for (size_t i = 0; i < resources_len; ++i) {
        if (0 > len)
            return COAP_ERR_BUFFER_TOO_SMALL;
        // skip if missing content type
        if (COAP_CONTENTTYPE_NONE == COAP_GET_CONTENTTYPE(resources[i].content_type, 2))
            continue;
        // comma separated list
        if (0 < strlen(buf)) {
            strncat(buf, ",", len--);
        }
        // insert < at path beginning
        strncat(buf, "<", len--);
        // insert path by elements
        for (size_t j = 0; j < resources[i].path->count; ++j) {
            strncat(buf, "/", len--);
            strncat(buf, resources[i].path->elems[j], len);
            len -= strlen(resources[i].path->elems[j]);
        }
        // insert >; after path
        strncat(buf, ">;", len);
        len -= 2;
        // append content type
        len -= sprintf(buf + (buflen - len - 1), "ct=%d",
                       COAP_GET_CONTENTTYPE(resources[i].content_type, 2));
    }

    return COAP_SUCCESS;
}

const coap_option_t *coap_find_uri_path(const coap_packet_t *pkt,
                                          uint8_t *count)
{
    return _find_options(pkt, COAP_OPTION_URI_PATH, count);
}
