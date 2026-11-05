#ifndef SMOOTHAF_MENU_TOGGLE_H
#define SMOOTHAF_MENU_TOGGLE_H

/* Custom Legacy .select callbacks replace the default .priv toggle.  Keep the
 * small configuration transition separate so host tests can execute the same
 * priv/delta contract without importing the camera menu implementation. */
typedef void (*smoothaf_numeric_toggle_fn)(int *value, int delta,
                                           int min, int max);

static inline int smoothaf_logger_config_toggle(
    void *priv, int delta, smoothaf_numeric_toggle_fn toggle)
{
    if (!priv || !toggle) return 0;
    toggle((int *)priv, delta, 0, 1);
    return 1;
}

#endif
