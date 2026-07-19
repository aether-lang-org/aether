#include "test_harness.h"
#include "../../std/collections/aether_collections.h"
#include "../../std/string/aether_string.h"

TEST_CATEGORY(list_create_and_free, TEST_CATEGORY_COLLECTIONS) {
    ArrayList* list = list_new();
    ASSERT_NOT_NULL(list);
    ASSERT_EQ(0, list_size(list));
    list_free(list);
}

TEST_CATEGORY(list_add_and_get, TEST_CATEGORY_COLLECTIONS) {
    ArrayList* list = list_new();

    int val1 = 42;
    int val2 = 100;
    list_add_raw(list, &val1);
    list_add_raw(list, &val2);

    ASSERT_EQ(2, list_size(list));
    ASSERT_EQ(&val1, list_get_raw(list, 0));
    ASSERT_EQ(&val2, list_get_raw(list, 1));

    list_free(list);
}

/* Regression: list_get_raw must reject a non-list pointer with a safe NULL
 * rather than faulting on list->size / list->items[index]. A dangling /
 * type-confused pointer (wrong _kind_magic) reaching the accessor used to
 * SIGSEGV deep inside it; the kind + low-address guard now catches it. */
TEST_CATEGORY(list_get_raw_rejects_invalid_pointers, TEST_CATEGORY_COLLECTIONS) {
    /* NULL list. */
    ASSERT_NULL(list_get_raw(NULL, 0));

    /* Low-address scalar: a small int intptr-cast to `ptr`. Below the
     * mmap-min guard, so it must never be dereferenced. */
    ASSERT_NULL(list_get_raw((ArrayList*)(uintptr_t)42, 0));

    /* Another kind mistaken for a list: a HashMap has a valid struct at a
     * valid address but carries AETHER_KIND_MAP_MAGIC, not the list magic —
     * exactly the shape that crashed (valid pointer, wrong magic). */
    HashMap* map = map_new();
    ASSERT_NOT_NULL(map);
    ASSERT_NULL(list_get_raw((ArrayList*)map, 0));
    map_free(map);

    /* The guard must not regress valid lists. */
    ArrayList* list = list_new();
    int v = 7;
    list_add_raw(list, &v);
    ASSERT_EQ(&v, list_get_raw(list, 0));
    list_free(list);
}

TEST_CATEGORY(list_set_and_remove, TEST_CATEGORY_COLLECTIONS) {
    ArrayList* list = list_new();

    int val1 = 1, val2 = 2, val3 = 3;
    list_add_raw(list, &val1);
    list_add_raw(list, &val2);
    list_add_raw(list, &val3);

    int val4 = 99;
    list_set(list, 1, &val4);
    ASSERT_EQ(&val4, list_get_raw(list, 1));

    list_remove(list, 0);
    ASSERT_EQ(2, list_size(list));
    ASSERT_EQ(&val4, list_get_raw(list, 0));

    list_free(list);
}

TEST_CATEGORY(map_create_and_free, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();
    ASSERT_NOT_NULL(map);
    ASSERT_EQ(0, map_size(map));
    map_free(map);
}

TEST_CATEGORY(map_put_and_get, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();

    int val1 = 42;
    int val2 = 100;

    map_put_raw(map, "name", &val1);
    map_put_raw(map, "age", &val2);

    ASSERT_EQ(2, map_size(map));
    ASSERT_EQ(&val1, map_get_raw(map, "name"));
    ASSERT_EQ(&val2, map_get_raw(map, "age"));

    map_free(map);
}

TEST_CATEGORY(map_has_and_remove, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();

    int val = 123;

    map_put_raw(map, "test", &val);
    ASSERT_TRUE(map_has(map, "test"));

    map_remove(map, "test");
    ASSERT_FALSE(map_has(map, "test"));
    ASSERT_EQ(0, map_size(map));

    map_free(map);
}

TEST_CATEGORY(map_keys, TEST_CATEGORY_COLLECTIONS) {
    HashMap* map = map_new();

    int val = 1;
    map_put_raw(map, "a", &val);
    map_put_raw(map, "b", &val);

    MapKeys* keys = map_keys_raw(map);
    ASSERT_NOT_NULL(keys);
    ASSERT_EQ(2, keys->count);

    map_keys_free(keys);
    map_free(map);
}
