#include "mallocp.h"
#include <stdio.h>


t_arena_data	*g_arena_data = NULL;

static inline void *
user_area (t_alloc_chunk *chunk, size_t size, pthread_mutex_t *mutex) {

	/*
	   Populate chunk headers.
	   A chunk header precedes the user area with the size of the user area (to allow us to navigate between allocated
	   chunks which are not part of any linked list, and defragment the memory in free and realloc calls) and the size
	   of the previous chunk. We also use the previous size to store flags such as ALLOC_CHUNK to know if the chunk
	   is allocated or not.
	*/

	chunk->size = size;
	chunk->prev_size |= 1UL << ALLOC_CHUNK;
	((t_free_chunk *)(chunk->user_area + size))->prev_size = size;

	pthread_mutex_unlock(mutex);
	return (void *)chunk->user_area;
}

static t_pool *
create_new_pool (int type, int chunk_type, unsigned long size, long pagesize, pthread_mutex_t *mutex) {

	/*
	   Allocate memory using mmap. If the requested data isn't that big, we allocate enough memory to hold
	   CHUNKS_PER_POOL times this data to prepare for future malloc. If the requested data exceeds that value, nearest
	   page data will do.
	*/

	unsigned long mmap_size = size + sizeof(unsigned long);
	if (chunk_type == CHUNK_TYPE_LARGE) {
		mmap_size += sizeof(t_pool) + sizeof(t_alloc_chunk);
	} else {
		mmap_size = (chunk_type == CHUNK_TYPE_SMALL) ? SIZE_SMALL * 100 : SIZE_TINY * 100;
		mmap_size += sizeof(t_pool) + (MEM_ALIGN + sizeof(t_alloc_chunk)) * CHUNKS_PER_POOL;
	}

	mmap_size = mmap_size + (unsigned long)pagesize - (mmap_size % (unsigned long)pagesize);
	t_pool *new_pool = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	if (__builtin_expect(new_pool == MAP_FAILED, 0)) {
		errno = ENOMEM;
		pthread_mutex_unlock(mutex);
		return MAP_FAILED;
	}

	/* Keep track of the size and free size available. */
	new_pool->size = mmap_size | (1UL << chunk_type);
	new_pool->free_size = mmap_size - (size + sizeof(t_alloc_chunk) + sizeof(t_pool) + sizeof(unsigned long));

	if (type == MAIN_POOL) new_pool->size |= (1UL << MAIN_POOL);

	return new_pool;
}

void *
__malloc (size_t size) {

	static long 			pagesize = 0;
	static pthread_mutex_t	main_arena_mutex = PTHREAD_MUTEX_INITIALIZER,
							new_arena_mutex = PTHREAD_MUTEX_INITIALIZER;
	static t_arena_data		arena_data = { .arena_count = 1 };
	bool					creator = false;
	t_arena					*current_arena = NULL;
	t_pool					*current_pool = NULL;


	int chunk_type = 0;
	if (size > SIZE_SMALL) {
		chunk_type = CHUNK_TYPE_LARGE;
	} else {
		size = (size + MEM_ALIGN) & ~MEM_ALIGN;

		if (size >= (1UL << FLAG_THRESHOLD)) return NULL;

		chunk_type = (size <= SIZE_TINY) ? CHUNK_TYPE_TINY : CHUNK_TYPE_SMALL;
	}

	/*
	   First malloc may be called simultaneously by different threads. In order to prevent the main arena being created
	   more than once, we place a mutex before reading if main_arena already exists or not. If it doesn't, we release
	   the mutex after it's creation to enable other thread access. If it does, the mutex is released instantly.
	   GCC's builtin expect further improves performance as we know there will only be a single time in the program
	   execution where the comparison holds false.
	*/

	pthread_mutex_lock(&main_arena_mutex);

	if (__builtin_expect(g_arena_data == NULL, 0)) {

		pagesize = sysconf(_SC_PAGESIZE);

		current_pool = create_new_pool(MAIN_POOL, chunk_type, size, pagesize, &main_arena_mutex);
		if (current_pool == MAP_FAILED) return NULL;

		arena_data.arenas[0] = (t_arena){ .main_pool = current_pool };
		pthread_mutex_init(&arena_data.arenas[0].mutex, NULL);
		pthread_mutex_lock(&arena_data.arenas[0].mutex);
		g_arena_data = &arena_data;
		pthread_mutex_unlock(&main_arena_mutex);

		return user_area((t_alloc_chunk *)current_pool->chunk, size, &arena_data.arenas[0].mutex);
	}

	/* Main arena exists, we can proceed. */
	pthread_mutex_unlock(&main_arena_mutex);

	/*
	   In order to prevent threads to compete for the same memory area, multiple arenas can be created to allow
	   for faster memory allocation in multi-threaded programs. Each arena has it's own mutex that will allow each
	   thread to operate independently. If M_ARENA_MAX is reached, threads will loop over all arenas until one
	   is available.
	*/

	/* Look for an open arena. */
	current_arena = &arena_data.arenas[0];
	int arena_index = 0;
	while (pthread_mutex_trylock(&current_arena->mutex) != 0) {

		if (arena_index == arena_data.arena_count - 1) {

			if (arena_data.arena_count == M_ARENA_MAX || pthread_mutex_trylock(&new_arena_mutex) != 0) {
				current_arena = &arena_data.arenas[(arena_index = 0)];
				continue;
			}

			creator = true;
			break;
		}

		current_arena = &arena_data.arenas[arena_index++];
	}

	/* All arenas are occupied by other threads but M_ARENA_MAX isn't reached. Let's just create a new one. */
	if (creator == true) {

		current_pool = create_new_pool(MAIN_POOL, chunk_type, size, pagesize, &new_arena_mutex);
		if (current_pool == MAP_FAILED) return NULL;

		++arena_data.arena_count;
		arena_data.arenas[arena_index + 1] = (t_arena){ .main_pool = current_pool };
		pthread_mutex_init(&arena_data.arenas[arena_index + 1].mutex, NULL);
		pthread_mutex_lock(&arena_data.arenas[arena_index + 1].mutex);
		pthread_mutex_unlock(&new_arena_mutex);

		return user_area((t_alloc_chunk *)current_pool->chunk, size, &current_arena->mutex);
	}

	/*
	   Otherwise, thread has accessed an arena and locked it. Now let's try to find a chunk of memory that is big
	   enough to accommodate the user-requested size.
	*/

	current_pool = arena_data.arenas[arena_index].main_pool;
	unsigned long required_size = size + sizeof(t_alloc_chunk);

	/*
	   Look for a pool with a matching chunk type and enough space to accommodate user request. This applies only if
	   the chunk type isn't large, as large chunks have their dedicated pool.
	*/

	if (chunk_type != CHUNK_TYPE_LARGE) {
		while (pool_type_match(current_pool, chunk_type) == 0 || current_pool->free_size < required_size) {

			if (current_pool->right == NULL) {
				creator = true;
				break;
			}

			current_pool = current_pool->right;
		}
	}

	/* A suitable pool could not be found, we need to create one. */
	if (chunk_type == CHUNK_TYPE_LARGE || creator == true) {

		current_pool = create_new_pool(0, chunk_type, size, pagesize, &current_arena->mutex);
		if (current_pool == MAP_FAILED) return NULL;

		/*
		   As large pools don't need to be searched for empty space, and because we want to place fresh tiny and small
		   pools the closest to the main pool to reduce access time, we are using a non-circular double linked list
		   anchored to the main pool. Large chunks pools will be placed to the left of the main pool, tiny and small
		   chunks to it's right.
		*/

		t_pool	*main_pool = arena_data.arenas[arena_index].main_pool,
				*tmp = (chunk_type == CHUNK_TYPE_LARGE) ? main_pool->left : main_pool->right ;

		/* Insert the new pool into the pool list. */
		if (chunk_type == CHUNK_TYPE_LARGE) {
			main_pool->left = current_pool;
			current_pool->right = main_pool;
			current_pool->left = tmp;

			if (tmp != NULL) tmp->right = current_pool;

		} else {
			main_pool->right = current_pool;
			current_pool->left = main_pool;
			current_pool->right = tmp;

			if (tmp != NULL) tmp->left = current_pool;
		}

		return user_area((t_alloc_chunk *)current_pool->chunk, size, &current_arena->mutex);
	}

	/* Find the first free memory chunk and look for a chunk large enough to accommodate user request. */
	t_alloc_chunk *chunk = (t_alloc_chunk *)current_pool->chunk;
	while (chunk_is_allocated(chunk)) {
		chunk = (t_alloc_chunk *)(chunk->user_area + chunk->size);
	}

	t_free_chunk *free_chunk = (t_free_chunk *)chunk, *anchor = NULL;
	while (free_chunk->next != NULL && free_chunk->size < required_size) {
		anchor = free_chunk;
		free_chunk = free_chunk->next;
	}

	current_pool->free_size -= required_size;
	if (anchor != NULL) anchor->next = free_chunk->next;

	return user_area((t_alloc_chunk *)free_chunk, size, &current_arena->mutex);
}
