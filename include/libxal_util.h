#ifndef LIBXAL_UTIL_H
#define LIBXAL_UTIL_H

#ifdef XAL_DEBUG_ENABLED

#define XAL_DEBUG_FCALL(fn, ...) fn(__VA_ARGS__)

#define __FILENAME__ strrchr("/" __FILE__, '/') + 1

#define XAL_DEBUG(...)                                                                             \
	fprintf(stderr, "# DBG:%s:%s-%d: " FIRST(__VA_ARGS__) "\n", __FILENAME__, __func__,        \
		__LINE__ REST(__VA_ARGS__));                                                       \
	fflush(stderr);

#define FIRST(...) FIRST_HELPER(__VA_ARGS__, throwaway)
#define FIRST_HELPER(first, ...) first

#define REST(...) REST_HELPER(NUM(__VA_ARGS__), __VA_ARGS__)
#define REST_HELPER(qty, ...) REST_HELPER2(qty, __VA_ARGS__)
#define REST_HELPER2(qty, ...) REST_HELPER_##qty(__VA_ARGS__)
#define REST_HELPER_ONE(first)
#define REST_HELPER_TWOORMORE(first, ...) , __VA_ARGS__
#define NUM(...)                                                                                   \
	SELECT_10TH(__VA_ARGS__, TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE, \
		    TWOORMORE, TWOORMORE, ONE, throwaway)
#define SELECT_10TH(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, ...) a10

#else
#define XAL_DEBUG(...)
#define XAL_DEBUG_FCALL(fn, ...) do {} while (0)
#endif

#define XAL_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/**
 * Macro to suppress warnings on unused arguments, thanks to stackoverflow.
 */
#ifdef __GNUC__
#define XAL_UNUSED(x) UNUSED_##x __attribute__((__unused__))
#else
#define XAL_UNUSED(x) UNUSED_##x
#endif

#endif /* LIBXAL_UTIL_H */
