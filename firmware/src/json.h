#pragma once
#include <stddef.h>

// Copy s into out as JSON string content (no surrounding quotes). Escapes
// quote, backslash and control bytes (\u00XX); passes UTF-8 through.
// Worst case output is 6 x strlen(s) + 1 bytes.
void json_escape(char *out, size_t n, const char *s);
