#include <stdlib.h>
#include <string.h>
#include "utils.h"

#include <ctype.h>

unsigned long long parse_size(const char *s) {
    if (!s) return 0ULL;
    char *endptr = NULL;
    unsigned long long v = strtoull(s, &endptr, 10);
    if (endptr && *endptr) {
        if (*endptr == 'K' || *endptr == 'k') v *= 1024ULL;
        else if (*endptr == 'M' || *endptr == 'm') v *= 1024ULL*1024ULL;
        else if (*endptr == 'G' || *endptr == 'g') v *= 1024ULL*1024ULL*1024ULL;
    }
    return v;
}

char *sanitize_header(const char *src, char *dst, size_t dstlen) {
    if (!src || !dst || dstlen == 0) return dst;
    size_t wi = 0; int last_space = 0;
    for (size_t i = 0; src[i] != '\0' && wi + 1 < dstlen; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r' || c == '\n') break; // stop at header end
        if (iscntrl(c)) continue;
        if (isspace(c)) {
            if (!last_space && wi + 1 < dstlen) { dst[wi++] = ' '; last_space = 1; }
        } else {
            dst[wi++] = (char)c; last_space = 0;
        }
    }
    if (wi == 0) { dst[0] = '\0'; return dst; }
    dst[wi] = '\0';
    return dst;
}
