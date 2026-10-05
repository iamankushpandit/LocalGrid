/*
 * LocalGrid's side of Codec2's embedded build. With __EMBEDDED__ defined Codec2 keeps its tables in
 * flash and asks the port for its memory through these three; here that is the ordinary heap.
 * This file is ours (GPL-3.0-or-later, like the rest of LocalGrid); everything in src/ is Codec2's.
 */
#include <stdlib.h>

void *codec2_malloc(size_t size)
{
    return malloc(size);
}

void *codec2_calloc(size_t nmemb, size_t size)
{
    return calloc(nmemb, size);
}

void codec2_free(void *ptr)
{
    free(ptr);
}
