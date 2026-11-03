#ifndef SMOOTHAF_DIRECTION_H
#define SMOOTHAF_DIRECTION_H

/* Vlog Direction has its own persisted vocabulary.  Focus Test keeps its
 * existing AUTO/A/B setting and state; this header only describes the Vlog
 * menu and its legacy-config migration. */
enum smoothaf_vlog_direction
{
    SMOOTHAF_VLOG_DIRECTION_AUTO = 0,
    SMOOTHAF_VLOG_DIRECTION_A = 1,
    SMOOTHAF_VLOG_DIRECTION_B = 2,
};

enum smoothaf_vlog_auto_phase
{
    SMOOTHAF_VLOG_AUTO_IDLE = 0,
    SMOOTHAF_VLOG_AUTO_FIRST_MOVE,
    SMOOTHAF_VLOG_AUTO_FIRST_RETURN,
    SMOOTHAF_VLOG_AUTO_OPPOSITE_MOVE,
    SMOOTHAF_VLOG_AUTO_OPPOSITE_RETURN,
    SMOOTHAF_VLOG_AUTO_DONE,
};

static inline int smoothaf_vlog_direction_normalize(int direction)
{
    return direction >= SMOOTHAF_VLOG_DIRECTION_AUTO &&
           direction <= SMOOTHAF_VLOG_DIRECTION_B
         ? direction : SMOOTHAF_VLOG_DIRECTION_AUTO;
}

static inline int smoothaf_vlog_direction_is_auto(int direction)
{
    return smoothaf_vlog_direction_normalize(direction) ==
           SMOOTHAF_VLOG_DIRECTION_AUTO;
}

static inline int smoothaf_vlog_direction_forced_sign(int direction)
{
    return smoothaf_vlog_direction_normalize(direction) ==
           SMOOTHAF_VLOG_DIRECTION_B ? 1 : -1;
}

/* Before V2.1, smoothaf.af_dir used 0=A and 1=B.  New configurations use
 * smoothaf.vlog_direction with AUTO as the default.  Migrate only when the
 * new key is still untouched and the legacy key was explicitly changed; a
 * normal new configuration therefore remains AUTO. */
static inline int smoothaf_vlog_direction_migrate(
        int selected, int selected_changed, int legacy, int legacy_changed)
{
    if (!selected_changed && legacy_changed)
    {
        if (legacy == 0) return SMOOTHAF_VLOG_DIRECTION_A;
        if (legacy == 1) return SMOOTHAF_VLOG_DIRECTION_B;
    }
    return smoothaf_vlog_direction_normalize(selected);
}

#endif
