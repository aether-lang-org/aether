/*
 * Aether Programming Language - std.xml
 * Copyright (c) 2025 Aether Programming Language Contributors
 * Licensed under the MIT License. See LICENSE file in the project root.
 *
 * Small, dependency-free XML: a pull/SAX reader and an escaping builder
 * (issue #627). Scope is deliberately limited to what S3 / SOAP-ish /
 * config XML needs — see aether_xml.h.
 */
#include "aether_xml.h"
#include "../../runtime/aether_resource_caps.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
    int    oom;   /* sticky: a grow failed */
} Sb;

static void sb_init(Sb* s) { s->data = NULL; s->len = 0; s->cap = 0; s->oom = 0; }

static int sb_reserve(Sb* s, size_t extra) {
    if (s->oom) return 0;
    if (s->len + extra + 1 <= s->cap) return 1;
    size_t want = s->cap ? s->cap * 2 : 64;
    while (want < s->len + extra + 1) want *= 2;
    char* p = (char*)realloc(s->data, want);
    if (!p) { s->oom = 1; return 0; }
    s->data = p;
    s->cap = want;
    return 1;
}

static void sb_putc(Sb* s, char c) {
    if (!sb_reserve(s, 1)) return;
    s->data[s->len++] = c;
}

static void sb_append(Sb* s, const char* p, size_t n) {
    if (n == 0) return;
    if (!sb_reserve(s, n)) return;
    memcpy(s->data + s->len, p, n);
    s->len += n;
}

static void sb_puts(Sb* s, const char* str) { sb_append(s, str, strlen(str)); }

/* NUL-terminate and hand off the buffer (caller frees). */
static char* sb_finish(Sb* s) {
    if (s->oom) { free(s->data); return NULL; }
    if (!s->data) { return strdup(""); }
    s->data[s->len] = '\0';
    return s->data;  /* ownership transferred */
}

/* ------------------------------------------------------------------ */
/* Entity decode (& < > " ' and numeric &#NN; / &#xHH;)               */
/* ------------------------------------------------------------------ */

/* Encode a Unicode code point as UTF-8 into sb. */
static void sb_put_utf8(Sb* s, unsigned long cp) {
    if (cp <= 0x7F) {
        sb_putc(s, (char)cp);
    } else if (cp <= 0x7FF) {
        sb_putc(s, (char)(0xC0 | (cp >> 6)));
        sb_putc(s, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        sb_putc(s, (char)(0xE0 | (cp >> 12)));
        sb_putc(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(s, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        sb_putc(s, (char)(0xF0 | (cp >> 18)));
        sb_putc(s, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(s, (char)(0x80 | (cp & 0x3F)));
    }
    /* out-of-range code points are dropped */
}

/* Decode XML character data [p, p+n) into a freshly malloc'd string with
 * entities resolved. Returns NULL on alloc failure. Unknown/malformed
 * `&...;` runs are passed through verbatim (lenient — matches what real
 * S3/SOAP bodies need without rejecting odd-but-harmless input). */
static char* xml_decode(const char* p, size_t n) {
    Sb out;
    sb_init(&out);
    size_t i = 0;
    while (i < n) {
        char c = p[i];
        if (c != '&') { sb_putc(&out, c); i++; continue; }
        /* Find the terminating ';' within a sane window. */
        size_t semi = i + 1;
        while (semi < n && semi < i + 12 && p[semi] != ';') semi++;
        if (semi >= n || p[semi] != ';') { sb_putc(&out, c); i++; continue; }
        size_t elen = semi - (i + 1);
        const char* e = p + i + 1;
        if (elen == 3 && memcmp(e, "amp", 3) == 0)       sb_putc(&out, '&');
        else if (elen == 2 && memcmp(e, "lt", 2) == 0)   sb_putc(&out, '<');
        else if (elen == 2 && memcmp(e, "gt", 2) == 0)   sb_putc(&out, '>');
        else if (elen == 4 && memcmp(e, "quot", 4) == 0) sb_putc(&out, '"');
        else if (elen == 4 && memcmp(e, "apos", 4) == 0) sb_putc(&out, '\'');
        else if (elen >= 2 && e[0] == '#') {
            unsigned long cp = 0;
            int ok = 0;
            if (e[1] == 'x' || e[1] == 'X') {
                for (size_t k = 2; k < elen; k++) {
                    char h = e[k];
                    int d = (h >= '0' && h <= '9') ? h - '0'
                          : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                          : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (d < 0) { ok = 0; break; }
                    cp = cp * 16 + (unsigned long)d; ok = 1;
                }
            } else {
                for (size_t k = 1; k < elen; k++) {
                    if (e[k] < '0' || e[k] > '9') { ok = 0; break; }
                    cp = cp * 10 + (unsigned long)(e[k] - '0'); ok = 1;
                }
            }
            if (ok) sb_put_utf8(&out, cp);
            else { sb_append(&out, p + i, semi - i + 1); }  /* pass through */
        } else {
            sb_append(&out, p + i, semi - i + 1);  /* unknown entity: verbatim */
        }
        i = semi + 1;
    }
    return sb_finish(&out);
}

/* ------------------------------------------------------------------ */
/* Pull reader                                                        */
/* ------------------------------------------------------------------ */

typedef struct { char* name; char* value; } XmlAttr;

struct XmlParser {
    char*    buf;
    size_t   len;
    size_t   pos;
    char*    name;            /* current element name */
    char*    text;            /* current decoded text */
    XmlAttr* attrs;
    int      attr_count;
    int      attr_cap;
    char*    pending_end;     /* self-close: deferred </name> */
    int      errored;
    char     err[256];
};

static void clear_attrs(XmlParser* p) {
    for (int i = 0; i < p->attr_count; i++) {
        free(p->attrs[i].name);
        free(p->attrs[i].value);
    }
    p->attr_count = 0;
}

static void reset_event(XmlParser* p) {
    free(p->name); p->name = NULL;
    free(p->text); p->text = NULL;
    clear_attrs(p);
}

XmlParser* xml_parser_new(const char* data, size_t len) {
    XmlParser* p = (XmlParser*)aether_caps_calloc(1, sizeof(XmlParser));
    if (!p) return NULL;
    p->buf = (char*)malloc(len + 1);
    if (!p->buf) { aether_caps_free(p, sizeof(XmlParser)); return NULL; }
    if (len) memcpy(p->buf, data, len);
    p->buf[len] = '\0';
    p->len = len;
    return p;
}

/* Convenience for the Aether boundary: build a reader from a
 * NUL-terminated string (S3/SOAP/config bodies are text, so strlen is the
 * right length). For binary-safe input use xml_parser_new with an
 * explicit length. */
XmlParser* xml_parser_new_str(const char* s) {
    return xml_parser_new(s ? s : "", s ? strlen(s) : 0);
}

void xml_parser_free(XmlParser* p) {
    if (!p) return;
    reset_event(p);
    free(p->attrs);
    free(p->pending_end);
    free(p->buf);
    aether_caps_free(p, sizeof(XmlParser));
}

static int xml_fail(XmlParser* p, const char* msg) {
    p->errored = 1;
    snprintf(p->err, sizeof(p->err), "%s (at byte %zu)", msg, p->pos);
    return XML_EVENT_ERROR;
}

/* XML name chars (lenient): not whitespace, and not one of < > / = ? */
static int is_name_char(char c) {
    return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
             c == '<' || c == '>' || c == '/' || c == '=' || c == '?' ||
             c == '\0');
}

static void skip_ws(XmlParser* p) {
    while (p->pos < p->len) {
        char c = p->buf[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

/* Read a name into a freshly malloc'd string (advances pos). */
static char* read_name(XmlParser* p) {
    size_t start = p->pos;
    while (p->pos < p->len && is_name_char(p->buf[p->pos])) p->pos++;
    size_t n = p->pos - start;
    char* s = (char*)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, p->buf + start, n);
    s[n] = '\0';
    return s;
}

static int add_attr(XmlParser* p, char* name, char* value) {
    if (p->attr_count >= p->attr_cap) {
        int ncap = p->attr_cap ? p->attr_cap * 2 : 8;
        XmlAttr* na = (XmlAttr*)realloc(p->attrs, (size_t)ncap * sizeof(XmlAttr));
        if (!na) { free(name); free(value); return 0; }
        p->attrs = na;
        p->attr_cap = ncap;
    }
    p->attrs[p->attr_count].name = name;
    p->attrs[p->attr_count].value = value;
    p->attr_count++;
    return 1;
}

int xml_next(XmlParser* p) {
    if (!p) return XML_EVENT_EOF;
    if (p->errored) return XML_EVENT_ERROR;

    /* Deferred end from a self-closing element. */
    if (p->pending_end) {
        reset_event(p);
        p->name = p->pending_end;
        p->pending_end = NULL;
        return XML_EVENT_END_ELEMENT;
    }

    reset_event(p);

    for (;;) {
        if (p->pos >= p->len) return XML_EVENT_EOF;

        if (p->buf[p->pos] != '<') {
            /* Character data up to the next '<'. */
            size_t start = p->pos;
            while (p->pos < p->len && p->buf[p->pos] != '<') p->pos++;
            p->text = xml_decode(p->buf + start, p->pos - start);
            if (!p->text) return xml_fail(p, "out of memory decoding text");
            return XML_EVENT_TEXT;
        }

        /* Markup. Look at what follows '<'. */
        size_t remain = p->len - p->pos;
        if (remain >= 4 && memcmp(p->buf + p->pos, "<!--", 4) == 0) {
            const char* end = strstr(p->buf + p->pos + 4, "-->");
            if (!end) return xml_fail(p, "unterminated comment");
            p->pos = (size_t)(end - p->buf) + 3;
            continue;
        }
        if (remain >= 9 && memcmp(p->buf + p->pos, "<![CDATA[", 9) == 0) {
            size_t s = p->pos + 9;
            const char* end = strstr(p->buf + s, "]]>");
            if (!end) return xml_fail(p, "unterminated CDATA");
            size_t n = (size_t)(end - (p->buf + s));
            p->text = (char*)malloc(n + 1);
            if (!p->text) return xml_fail(p, "out of memory in CDATA");
            memcpy(p->text, p->buf + s, n);
            p->text[n] = '\0';
            p->pos = (size_t)(end - p->buf) + 3;
            return XML_EVENT_TEXT;
        }
        if (remain >= 2 && p->buf[p->pos + 1] == '?') {
            /* Prolog <?xml ...?> or processing instruction — skip. */
            const char* end = strstr(p->buf + p->pos + 2, "?>");
            if (!end) return xml_fail(p, "unterminated processing instruction");
            p->pos = (size_t)(end - p->buf) + 2;
            continue;
        }
        if (remain >= 2 && p->buf[p->pos + 1] == '!') {
            /* DOCTYPE or other declaration — skip to matching '>',
             * tolerating a single bracketed internal subset. */
            size_t i = p->pos + 2;
            int depth = 0;
            while (i < p->len) {
                char c = p->buf[i];
                if (c == '[') depth++;
                else if (c == ']') { if (depth > 0) depth--; }
                else if (c == '>' && depth == 0) break;
                i++;
            }
            if (i >= p->len) return xml_fail(p, "unterminated declaration");
            p->pos = i + 1;
            continue;
        }
        if (remain >= 2 && p->buf[p->pos + 1] == '/') {
            /* End element </name> */
            p->pos += 2;
            p->name = read_name(p);
            if (!p->name) return xml_fail(p, "out of memory reading end tag");
            skip_ws(p);
            if (p->pos >= p->len || p->buf[p->pos] != '>')
                return xml_fail(p, "malformed end tag");
            p->pos++;
            return XML_EVENT_END_ELEMENT;
        }

        /* Start element <name attr="v" ... > or <name/> */
        p->pos++;  /* past '<' */
        p->name = read_name(p);
        if (!p->name) return xml_fail(p, "out of memory reading start tag");
        if (p->name[0] == '\0') return xml_fail(p, "empty element name");

        for (;;) {
            skip_ws(p);
            if (p->pos >= p->len) return xml_fail(p, "unterminated start tag");
            char c = p->buf[p->pos];
            if (c == '>') { p->pos++; break; }
            if (c == '/') {
                if (p->pos + 1 >= p->len || p->buf[p->pos + 1] != '>')
                    return xml_fail(p, "malformed self-closing tag");
                p->pos += 2;
                /* Defer the matching END_ELEMENT to the next xml_next. */
                p->pending_end = strdup(p->name);
                if (!p->pending_end) return xml_fail(p, "out of memory");
                break;
            }
            /* attribute: name (ws) = (ws) quote value quote */
            char* aname = read_name(p);
            if (!aname) return xml_fail(p, "out of memory reading attribute");
            if (aname[0] == '\0') { free(aname); return xml_fail(p, "malformed attribute"); }
            skip_ws(p);
            if (p->pos >= p->len || p->buf[p->pos] != '=') {
                free(aname);
                return xml_fail(p, "expected '=' in attribute");
            }
            p->pos++;  /* past '=' */
            skip_ws(p);
            if (p->pos >= p->len || (p->buf[p->pos] != '"' && p->buf[p->pos] != '\'')) {
                free(aname);
                return xml_fail(p, "expected quoted attribute value");
            }
            char q = p->buf[p->pos++];
            size_t vstart = p->pos;
            while (p->pos < p->len && p->buf[p->pos] != q) p->pos++;
            if (p->pos >= p->len) { free(aname); return xml_fail(p, "unterminated attribute value"); }
            char* aval = xml_decode(p->buf + vstart, p->pos - vstart);
            p->pos++;  /* past closing quote */
            if (!aval) { free(aname); return xml_fail(p, "out of memory decoding attribute"); }
            if (!add_attr(p, aname, aval)) return xml_fail(p, "out of memory storing attribute");
        }
        return XML_EVENT_START_ELEMENT;
    }
}

const char* xml_event_name(XmlParser* p) { return (p && p->name) ? p->name : ""; }
const char* xml_event_text(XmlParser* p) { return (p && p->text) ? p->text : ""; }
int         xml_event_attr_count(XmlParser* p) { return p ? p->attr_count : 0; }

const char* xml_event_attr_name(XmlParser* p, int i) {
    if (!p || i < 0 || i >= p->attr_count) return "";
    return p->attrs[i].name;
}
const char* xml_event_attr_value(XmlParser* p, int i) {
    if (!p || i < 0 || i >= p->attr_count) return "";
    return p->attrs[i].value;
}
const char* xml_event_attr(XmlParser* p, const char* name) {
    if (!p || !name) return NULL;
    for (int i = 0; i < p->attr_count; i++)
        if (strcmp(p->attrs[i].name, name) == 0) return p->attrs[i].value;
    return NULL;
}
/* Aether-boundary variant: "" (never NULL) for an absent attribute, so the
 * caller can string_concat it without a NULL guard. */
const char* xml_event_attr_str(XmlParser* p, const char* name) {
    const char* v = xml_event_attr(p, name);
    return v ? v : "";
}
const char* xml_parser_error(XmlParser* p) { return (p && p->errored) ? p->err : ""; }

/* ------------------------------------------------------------------ */
/* Escaping                                                           */
/* ------------------------------------------------------------------ */

static void sb_put_escaped(Sb* s, const char* p) {
    if (!p) return;
    for (; *p; p++) {
        switch (*p) {
            case '&':  sb_puts(s, "&amp;");  break;
            case '<':  sb_puts(s, "&lt;");   break;
            case '>':  sb_puts(s, "&gt;");   break;
            case '"':  sb_puts(s, "&quot;"); break;
            case '\'': sb_puts(s, "&apos;"); break;
            default:   sb_putc(s, *p);       break;
        }
    }
}

char* xml_escape(const char* s) {
    Sb out; sb_init(&out);
    sb_put_escaped(&out, s);
    return sb_finish(&out);
}

/* ------------------------------------------------------------------ */
/* Builder                                                            */
/* ------------------------------------------------------------------ */

struct XmlBuilder {
    Sb   sb;
    int  tag_open;   /* an emitted start tag still awaits its closing '>' */
};

XmlBuilder* xml_builder_new(void) {
    XmlBuilder* b = (XmlBuilder*)aether_caps_calloc(1, sizeof(XmlBuilder));
    if (!b) return NULL;
    sb_init(&b->sb);
    return b;
}

void xml_builder_free(XmlBuilder* b) {
    if (!b) return;
    free(b->sb.data);
    aether_caps_free(b, sizeof(XmlBuilder));
}

static void close_open_tag(XmlBuilder* b) {
    if (b->tag_open) { sb_putc(&b->sb, '>'); b->tag_open = 0; }
}

void xml_builder_declaration(XmlBuilder* b) {
    if (!b) return;
    close_open_tag(b);
    sb_puts(&b->sb, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
}

void xml_builder_start(XmlBuilder* b, const char* name) {
    if (!b || !name) return;
    close_open_tag(b);
    sb_putc(&b->sb, '<');
    sb_puts(&b->sb, name);
    b->tag_open = 1;
}

void xml_builder_attr(XmlBuilder* b, const char* name, const char* value) {
    if (!b || !name || !b->tag_open) return;  /* only valid inside an open start tag */
    sb_putc(&b->sb, ' ');
    sb_puts(&b->sb, name);
    sb_puts(&b->sb, "=\"");
    sb_put_escaped(&b->sb, value);
    sb_putc(&b->sb, '"');
}

void xml_builder_text(XmlBuilder* b, const char* text) {
    if (!b) return;
    close_open_tag(b);
    sb_put_escaped(&b->sb, text);
}

void xml_builder_end(XmlBuilder* b, const char* name) {
    if (!b || !name) return;
    close_open_tag(b);
    sb_puts(&b->sb, "</");
    sb_puts(&b->sb, name);
    sb_putc(&b->sb, '>');
}

void xml_builder_element(XmlBuilder* b, const char* name, const char* text) {
    if (!b || !name) return;
    xml_builder_start(b, name);
    xml_builder_text(b, text);
    xml_builder_end(b, name);
}

char* xml_builder_finish(XmlBuilder* b) {
    if (!b) return NULL;
    close_open_tag(b);
    char* result = sb_finish(&b->sb);  /* transfers ownership of the buffer */
    /* Detach so a subsequent xml_builder_free() doesn't double-free the
     * buffer we just handed to the caller (sb_finish also frees it on OOM,
     * leaving a dangling pointer — nulling covers both). */
    b->sb.data = NULL;
    b->sb.len = 0;
    b->sb.cap = 0;
    return result;
}

