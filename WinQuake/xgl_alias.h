/* Identity of a loaded alias model's command list. GPL-2.0-or-later.
 *
 * XGL_MakeAliasModelDisplayLists stores this record immediately before the
 * commands, inside the model's own (movable, evictable) cache block. A new
 * generation is issued every time a model is loaded, so the renderer's caches
 * can recognise the same loaded model by number instead of comparing the
 * whole command list on every draw. Models without the record, such as test
 * fixtures, still take the exact comparison path. */
#ifndef XGL_ALIAS_H
#define XGL_ALIAS_H

#define XGL_ALIAS_IDENTITY_MAGIC 0x58474c41U

typedef struct {
    unsigned int magic, generation, hash;
    int words;
} xgl_alias_identity_t;

/* FNV-1a over the command words, shared by the loader and the caches. */
static __inline__ unsigned int XGL_AliasCommandHash(const int *commands, int words)
{
    const byte *bytes;
    unsigned int hash;
    int i;
    hash = 2166136261U;
    bytes = (const byte *)commands;
    for (i = 0; i < words * (int)sizeof(int); ++i)
        hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}

static __inline__ const xgl_alias_identity_t *XGL_AliasIdentity(const aliashdr_t *header)
{
    const xgl_alias_identity_t *identity;
    if (header->commands < (int)(sizeof(aliashdr_t) + sizeof(*identity))) return NULL;
    identity = (const xgl_alias_identity_t *)((const byte *)header + header->commands) - 1;
    if (identity->magic != XGL_ALIAS_IDENTITY_MAGIC || !identity->generation ||
        identity->words < 1) return NULL;
    return identity;
}

#endif
