#pragma once

unsigned long long parse_size(const char *s);

// sanitize header values: strip CR/LF and control chars, collapse whitespace, NUL-terminate
char *sanitize_header(const char *src, char *dst, size_t dstlen);
