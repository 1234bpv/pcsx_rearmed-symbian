
// ЗАМІРЯНО 2026-08-04 на N95 8GB (ARM1136): вимкнення цього хака під ARMv6 не дає
// нічого. Аналіз передбачав 0,4-1,6% кадру (інверсія зворотних гілок нібито марна
// там, де є BTAC і статичне "назад - взято"), але два прогони по 9000 кадрів дали
// 30,01 -> 30,02 мс без освітлення і 31,96 -> 32,23 з ним, тобто нуль або трохи
// гірше. Лишаємо як в upstream. Не переоткривати без нових даних.
#ifdef __arm__
#define CORTEX_A8_BRANCH_PREDICTION_HACK 1
#endif

#define USE_MINI_HT 1
//#define REG_PREFETCH 1

// options:
//#define NO_WRITE_EXEC 1
//#define BASE_ADDR_DYNAMIC 1
//#define TC_WRITE_OFFSET 1
//#define NDRC_CACHE_FLUSH_ALL 1

#if defined(__MACH__) || defined(HAVE_LIBNX)
#define NO_WRITE_EXEC 1
#endif
#if defined(VITA) || defined(HAVE_LIBNX)
#define BASE_ADDR_DYNAMIC 1
#endif
#if defined(HAVE_LIBNX)
#define TC_WRITE_OFFSET 1
#endif
#if defined(_3DS)
#define NDRC_CACHE_FLUSH_ALL 1
#endif
