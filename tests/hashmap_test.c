/*
 * Hash map test summary:
 * - Verifies insert/get/contains for empty and populated maps.
 * - Verifies collision chains support update, try_insert, and removal from the
 *   head, middle, and tail.
 * - Verifies both stack-backed destroy and heap-backed free paths.
 */

#include "../kernel/hashmap.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

static int value_a = 11;
static int value_b = 22;
static int value_c = 33;
static int value_d = 44;
static int value_e = 55;
static int value_f = 66;

static void fail_lookup(unsigned key, void* got, void* expected) {
  int args[3] = { (int)key, (int)got, (int)expected };
  say("***hashmap FAIL key=%u got=0x%X expected=0x%X\n", args);
  panic("hashmap test: lookup mismatch\n");
}

static void fail_contains(unsigned key, bool got, bool expected) {
  int args[3] = { (int)key, got, expected };
  say("***hashmap FAIL contains key=%u got=%d expected=%d\n", args);
  panic("hashmap test: contains mismatch\n");
}

static void check_lookup(struct HashMap* map, unsigned key, void* expected_value, bool expected_contains) {
  void* got = hash_map_get(map, key);
  if (got != expected_value) {
    fail_lookup(key, got, expected_value);
  }

  bool contains = hash_map_contains(map, key);
  if (contains != expected_contains) {
    fail_contains(key, contains, expected_contains);
  }
}

static void expect_removed(unsigned key, void* got, void* expected) {
  if (got != expected) {
    fail_lookup(key, got, expected);
  }
}

void kernel_main(void) {
  say("***hashmap test start\n", NULL);

  struct HashMap map;
  hash_map_init(&map, 4);

  check_lookup(&map, 1, NULL, false);
  check_lookup(&map, 5, NULL, false);
  check_lookup(&map, 9, NULL, false);

  expect_removed(1, hash_map_insert(&map, 1, &value_a), NULL);
  expect_removed(5, hash_map_insert(&map, 5, &value_b), NULL);
  expect_removed(9, hash_map_insert(&map, 9, &value_c), NULL);
  expect_removed(13, hash_map_insert(&map, 13, &value_d), NULL);

  check_lookup(&map, 1, &value_a, true);
  check_lookup(&map, 5, &value_b, true);
  check_lookup(&map, 9, &value_c, true);
  check_lookup(&map, 13, &value_d, true);

  expect_removed(5, hash_map_insert(&map, 5, &value_e), &value_b);
  check_lookup(&map, 5, &value_e, true);

  expect_removed(5, hash_map_try_insert(&map, 5, &value_f), &value_e);
  check_lookup(&map, 5, &value_e, true);

  expect_removed(17, hash_map_try_insert(&map, 17, &value_f), NULL);
  check_lookup(&map, 17, &value_f, true);

  say("***hashmap insert ok\n", NULL);

  expect_removed(5, hash_map_remove(&map, 5), &value_e);
  check_lookup(&map, 5, NULL, false);
  check_lookup(&map, 1, &value_a, true);
  check_lookup(&map, 9, &value_c, true);
  check_lookup(&map, 13, &value_d, true);
  check_lookup(&map, 17, &value_f, true);

  expect_removed(1, hash_map_remove(&map, 1), &value_a);
  check_lookup(&map, 1, NULL, false);
  check_lookup(&map, 9, &value_c, true);
  check_lookup(&map, 13, &value_d, true);
  check_lookup(&map, 17, &value_f, true);

  expect_removed(17, hash_map_remove(&map, 17), &value_f);
  check_lookup(&map, 17, NULL, false);
  check_lookup(&map, 9, &value_c, true);
  check_lookup(&map, 13, &value_d, true);

  expect_removed(42, hash_map_remove(&map, 42), NULL);
  expect_removed(13, hash_map_remove(&map, 13), &value_d);
  expect_removed(9, hash_map_remove(&map, 9), &value_c);
  check_lookup(&map, 9, NULL, false);
  check_lookup(&map, 13, NULL, false);

  say("***hashmap remove ok\n", NULL);

  hash_map_destroy(&map);

  struct HashMap* heap_map = malloc(sizeof(struct HashMap));
  assert(heap_map != NULL, "hashmap test: heap map allocation failed.\n");

  hash_map_init(heap_map, 2);
  expect_removed(2, hash_map_insert(heap_map, 2, &value_a), NULL);
  expect_removed(4, hash_map_insert(heap_map, 4, &value_b), NULL);
  check_lookup(heap_map, 2, &value_a, true);
  check_lookup(heap_map, 4, &value_b, true);

  hash_map_free(heap_map);

  say("***hashmap free ok\n", NULL);
  say("***hashmap test complete\n", NULL);
}
