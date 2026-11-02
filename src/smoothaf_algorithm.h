#ifndef SMOOTHAF_ALGORITHM_H
#define SMOOTHAF_ALGORITHM_H

/* One persisted vocabulary is shared by Vlog episodes and Focus Test.  The
 * Current Vlog controller is the historical monitor policy; the other values
 * select the real reusable core controllers. */
enum smoothaf_algorithm
{
    SMOOTHAF_ALGORITHM_CURRENT_VLOG = 0,
    SMOOTHAF_ALGORITHM_LEGACY_HC = 1,
    SMOOTHAF_ALGORITHM_ADAPTIVE_HC = 2,
    SMOOTHAF_ALGORITHM_NORMAL_AF = 3,
};

static inline int smoothaf_algorithm_normalize(int algorithm)
{
    return algorithm >= SMOOTHAF_ALGORITHM_CURRENT_VLOG &&
           algorithm <= SMOOTHAF_ALGORITHM_NORMAL_AF
         ? algorithm : SMOOTHAF_ALGORITHM_CURRENT_VLOG;
}

static inline const char *smoothaf_algorithm_name_for(int algorithm)
{
    switch (smoothaf_algorithm_normalize(algorithm))
    {
        case SMOOTHAF_ALGORITHM_LEGACY_HC: return "Legacy HC";
        case SMOOTHAF_ALGORITHM_ADAPTIVE_HC: return "Adaptive HC";
        case SMOOTHAF_ALGORITHM_NORMAL_AF: return "Normal AF";
        default: return "Current Vlog";
    }
}

/* Current Vlog is a monitor-native adapter.  Focus Test deliberately does not
 * advertise it until a separate wrapper can reproduce its episode contract. */
static inline int smoothaf_algorithm_focus_test_supported(int algorithm)
{
    return smoothaf_algorithm_normalize(algorithm) !=
           SMOOTHAF_ALGORITHM_CURRENT_VLOG;
}

#endif
