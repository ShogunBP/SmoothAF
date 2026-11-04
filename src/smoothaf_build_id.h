#ifndef SMOOTHAF_BUILD_ID_H
#define SMOOTHAF_BUILD_ID_H

/* The ARM build script replaces this header in its staging tree with a
 * deterministic commit/configuration identifier.  The source fallback makes
 * host-only builds and development modules visibly distinguishable. */
#define SMOOTHAF_BUILD_ID "DEV-UNBUILT"
#define SMOOTHAF_BUILD_CONFIG "600D.102"

static inline const char *smoothaf_build_id_value(void)
{
    return SMOOTHAF_BUILD_ID;
}

#endif
