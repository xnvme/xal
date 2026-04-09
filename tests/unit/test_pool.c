#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <xal_pool.h>

#include "test.h"

#define ELEMENT_SIZE 64
#define RESERVED 1024
#define GROWBY 16

static int
test_map_basic(void)
{
	struct xal_pool pool = {0};
	int err;

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(pool.reserved == RESERVED);
	TEST_ASSERT(pool.element_size == ELEMENT_SIZE);
	TEST_ASSERT(pool.growby == GROWBY);
	TEST_ASSERT(pool.free == 0);
	TEST_ASSERT(pool.memory != NULL);

	xal_pool_unmap(&pool, false);
	return 0;
}

static int
test_map_already_initialized(void)
{
	struct xal_pool pool = {0};
	int err;

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == 0);

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == -EINVAL);

	xal_pool_unmap(&pool, false);
	return 0;
}

static int
test_map_allocated_exceeds_reserved(void)
{
	struct xal_pool pool = {0};
	int err;

	err = xal_pool_map(&pool, RESERVED, RESERVED + 1, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == -EINVAL);

	return 0;
}

static int
test_claim_sequential_indices(void)
{
	struct xal_pool pool = {0};
	uint32_t idx;
	int err;

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == 0);

	err = xal_pool_claim_inodes(&pool, 1, &idx);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(idx == 0);

	err = xal_pool_claim_inodes(&pool, 1, &idx);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(idx == 1);

	err = xal_pool_claim_inodes(&pool, 3, &idx);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(idx == 2);
	TEST_ASSERT(pool.free == 5);

	xal_pool_unmap(&pool, false);
	return 0;
}

static int
test_claim_triggers_growth(void)
{
	struct xal_pool pool = {0};
	uint32_t idx;
	size_t i;
	int err;

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(pool.allocated == GROWBY);

	for (i = 0; i < GROWBY; i++) {
		err = xal_pool_claim_inodes(&pool, 1, &idx);
		TEST_ASSERT(err == 0);
	}

	TEST_ASSERT(pool.free == GROWBY);

	err = xal_pool_claim_inodes(&pool, 1, &idx);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(pool.allocated == GROWBY * 2);
	TEST_ASSERT(pool.free == GROWBY + 1);

	xal_pool_unmap(&pool, false);
	return 0;
}

static int
test_claim_count_exceeds_growby(void)
{
	struct xal_pool pool = {0};
	uint32_t idx;
	int err;

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == 0);

	err = xal_pool_claim_inodes(&pool, GROWBY + 1, &idx);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(idx == 0);
	TEST_ASSERT(pool.free == GROWBY + 1);
	TEST_ASSERT(pool.allocated == GROWBY * 2);

	xal_pool_unmap(&pool, false);
	return 0;
}

static int
test_claim_count_exceeds_reserved(void)
{
	struct xal_pool pool = {0};
	uint32_t idx;
	int err;

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == 0);

	err = xal_pool_claim_inodes(&pool, RESERVED + 1, &idx);
	TEST_ASSERT(err == -ENOMEM);

	xal_pool_unmap(&pool, false);
	return 0;
}

static int
test_clear_resets_pool(void)
{
	struct xal_pool pool = {0};
	uint8_t *mem;
	uint32_t idx;
	size_t i;
	int err;

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, NULL);
	TEST_ASSERT(err == 0);

	for (i = 0; i < 4; i++) {
		err = xal_pool_claim_inodes(&pool, 1, &idx);
		TEST_ASSERT(err == 0);
	}

	TEST_ASSERT(pool.free == 4);

	mem = pool.memory;
	memset(mem, 0xFF, 4 * ELEMENT_SIZE);

	err = xal_pool_clear(&pool);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(pool.free == 0);
	TEST_ASSERT(pool.allocated == GROWBY);

	for (i = 0; i < pool.allocated * ELEMENT_SIZE; i++) {
		TEST_ASSERT(mem[i] == 0);
	}

	xal_pool_unmap(&pool, false);
	return 0;
}

// The shm path commits the full reserved size upfront: allocated and growby are both
// reserved, not the growby argument
static int
test_shm_map_basic(void)
{
	struct xal_pool pool = {0};
	const char *name = "/xal_test_shm_basic";
	int err;

	shm_unlink(name); // cleanup for any failed previous tests

	err = xal_pool_map(&pool, RESERVED, GROWBY, ELEMENT_SIZE, name);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(pool.reserved == RESERVED);
	TEST_ASSERT(pool.element_size == ELEMENT_SIZE);
	TEST_ASSERT(pool.allocated == RESERVED);
	TEST_ASSERT(pool.growby == RESERVED);
	TEST_ASSERT(pool.free == 0);
	TEST_ASSERT(pool.memory != NULL);

	xal_pool_unmap(&pool, true);
	return 0;
}

int
main(void)
{
	int failures = 0;

	TEST_RUN(test_map_basic);
	TEST_RUN(test_map_already_initialized);
	TEST_RUN(test_map_allocated_exceeds_reserved);
	TEST_RUN(test_claim_sequential_indices);
	TEST_RUN(test_claim_triggers_growth);
	TEST_RUN(test_claim_count_exceeds_growby);
	TEST_RUN(test_claim_count_exceeds_reserved);
	TEST_RUN(test_clear_resets_pool);
	TEST_RUN(test_shm_map_basic);

	return failures;
}
