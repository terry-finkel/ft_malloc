#ifndef MALLOC_H
# define MALLOC_H

/* For size_t. */
# include <stddef.h>

void	__attribute__((visibility("hidden"))) debug(int code, int flags, void *a, void *b, void *c);
void	__free(void *ptr);
void	*__malloc(size_t size);
void	*__realloc(void *ptr, size_t size);
void	show_alloc_mem(void);
void	show_alloc_mem_ex(void);

#endif /* MALLOC_H */
