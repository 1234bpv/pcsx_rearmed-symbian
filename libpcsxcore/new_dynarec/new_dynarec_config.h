
// BENCHMARKED 2026-08-04 on N95 8GB (ARM1136): disabling this hack on ARMv6 yields
// nothing. Analysis predicted 0.4-1.6% of frame time (inverting backward branches supposedly useless
// where BTAC and static "backward - taken" exist), but two runs of 9000 frames gave
// 30.01 -> 30.02 ms without lighting and 31.96 -> 32.23 with it, i.e. zero or slightly
// worse. Keeping as in upstream. Do not reopen without new data.
// 2026-08-30: disabled again, this time with the instruction-cache miss counter
// as the measure rather than wall clock. The hack pads with add rN,rN,#0 for
// Cortex-A8 dual issue and writes the PC directly on the unlikely branch, both
// of which only cost code size on the single-issue ARM1136.
//#ifdef __arm__
//#define CORTEX_A8_BRANCH_PREDICTION_HACK 1
//#endif

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
