#include "git-compat-util.h"
#include "strmap.h"
#include "mem-pool.h"

static int cmp_strmap_entry(const void *hashmap_cmp_fn_data,
			    const struct hashmap_entry *entry1,
			    const struct hashmap_entry *entry2,
			    const void *keydata)
{
	const struct strmap_entry *e1, *e2;

	e1 = container_of(entry1, const struct strmap_entry, ent);
	e2 = container_of(entry2, const struct strmap_entry, ent);
	return strcmp(e1->key, e2->key);
}

static struct strmap_entry *find_strmap_entry(struct strmap *map,
					      const char *str)
{
	struct strmap_entry entry;
	hashmap_entry_init(&entry.ent, strhash(str));
	entry.key = str;
	return hashmap_get_entry(&map->map, &entry, ent, NULL);
}

void strmap_init(struct strmap *map)
{
	strmap_ocd_init(map, NULL, 1);
}

void strmap_ocd_init(struct strmap *map,
		     struct mem_pool *pool,
		     int strdup_strings)
{
	hashmap_init(&map->map, cmp_strmap_entry, NULL, 0);
	map->pool = pool;
	map->strdup_strings = strdup_strings;
}

static void strmap_free_entries_(struct strmap *map, int free_util)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;

	if (!map)
		return;

	if (!free_util && map->pool)
		/* Memory other than util is owned by and freed with the pool */
		return;

	/*
	 * We need to iterate over the hashmap entries and free
	 * e->key and e->value ourselves; hashmap has no API to
	 * take care of that for us.  Since we're already iterating over
	 * the hashmap, though, might as well free e too and avoid the need
	 * to make some call into the hashmap API to do that.
	 */
	hashmap_for_each_entry(&map->map, &iter, e, ent) {
		if (free_util)
			free(e->value);
		if (!map->pool) {
			if (map->strdup_strings)
				free((char*)e->key);
			free(e);
		}
	}
}

void strmap_clear(struct strmap *map, int free_util)
{
	strmap_free_entries_(map, free_util);
	hashmap_free(&map->map);
}

void strmap_partial_clear(struct strmap *map, int free_util)
{
	strmap_free_entries_(map, free_util);
	hashmap_partial_clear(&map->map);
}

void *strmap_put(struct strmap *map, const char *str, void *data)
{
	struct strmap_entry *entry = find_strmap_entry(map, str);
	void *old = NULL;

	if (entry) {
		old = entry->value;
		entry->value = data;
	} else {
		/*
		 * We won't modify entry->key so it really should be const.
		 */
		const char *key = str;

		entry = map->pool ? mem_pool_alloc(map->pool, sizeof(*entry))
				  : xmalloc(sizeof(*entry));
		hashmap_entry_init(&entry->ent, strhash(str));

		if (map->strdup_strings)
			key = map->pool ? mem_pool_strdup(map->pool, str)
					: xstrdup(str);
		entry->key = key;
		entry->value = data;
		hashmap_add(&map->map, &entry->ent);
	}
	return old;
}

struct strmap_entry *strmap_get_entry(struct strmap *map, const char *str)
{
	return find_strmap_entry(map, str);
}

void *strmap_get(struct strmap *map, const char *str)
{
	struct strmap_entry *entry = find_strmap_entry(map, str);
	return entry ? entry->value : NULL;
}

int strmap_contains(struct strmap *map, const char *str)
{
	return find_strmap_entry(map, str) != NULL;
}

void strmap_remove(struct strmap *map, const char *str, int free_util)
{
	struct strmap_entry entry, *ret;
	hashmap_entry_init(&entry.ent, strhash(str));
	entry.key = str;
	ret = hashmap_remove_entry(&map->map, &entry, ent, NULL);
	if (!ret)
		return;
	if (free_util)
		free(ret->value);
	if (!map->pool) {
		if (map->strdup_strings)
			free((char*)ret->key);
		free(ret);
	}
}

void strintmap_incr(struct strintmap *map, const char *str, intptr_t amt)
{
	struct strmap_entry *entry = find_strmap_entry(&map->map, str);
	if (entry) {
		intptr_t *whence = (intptr_t*)&entry->value;
		*whence += amt;
	}
	else
		strintmap_set(map, str, amt);
}
