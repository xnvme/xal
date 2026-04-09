#ifndef TEST_H
#define TEST_H

#include <stdio.h>

#define TEST_ASSERT(cond)                                                                          \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
			return -1;                                                                 \
		}                                                                                  \
	} while (0)

#define TEST_RUN(fn)                                                                               \
	do {                                                                                       \
		int _err = fn();                                                                   \
		if (_err) {                                                                        \
			fprintf(stderr, "FAIL %s\n", #fn);                                         \
			failures++;                                                                \
		} else {                                                                           \
			printf("PASS %s\n", #fn);                                                  \
		}                                                                                  \
	} while (0)

#endif /* TEST_H */
