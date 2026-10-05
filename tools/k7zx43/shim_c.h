#include <string.h>
#include <ctype.h>
static const char *shim_ext_lower(const char *f) {
    static char buf[16]; const char *d = strrchr(f, '.'), *sl = strrchr(f, '/');
    if (!d || (sl && d < sl) || strlen(d) >= sizeof buf) return "";
    size_t i; for (i = 0; d[i]; i++) buf[i] = (char)tolower((unsigned char)d[i]); buf[i] = 0; return buf;
}
static const char *shim_basename(const char *f) { const char *sl = strrchr(f, '/'); return sl ? sl + 1 : f; }
