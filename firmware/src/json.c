#include <stdio.h>
#include "json.h"

void json_escape(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 6 < n; s++) {   // room for \u00XX + NUL
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20 || ch == 0x7f) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = 0;
}
