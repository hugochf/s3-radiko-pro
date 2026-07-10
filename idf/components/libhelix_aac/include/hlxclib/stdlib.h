#pragma once
/* Compatibility shim: the decoder uses helix_malloc/free (upstream's hlxclib). */
#include <stdlib.h>

#define helix_malloc(size)      malloc(size)
#define helix_free(ptr)         free(ptr)
#define helix_realloc(ptr, sz)  realloc(ptr, sz)
