/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   malloc.h                                           :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: nfinkel <nfinkel@42.fr>                    +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2019/03/25 18:27:12 by nfinkel           #+#    #+#             */
/*   Updated: 2019/03/25 18:29:44 by nfinkel          ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include "arenap.h"

void	ft_memset(void *b, int c, size_t len)
{
	size_t	k;

	k = 0;
	while (k < len)
	{
		((unsigned char *)b)[k] = (unsigned char)c;
		k++;
	}
}

size_t	malloc_good_size(size_t size)
{
	return ((size + 0xfULL) & ~0xfULL);
}

size_t	malloc_size(const void *ptr)
{
	if (ptr == NULL)
		return (0);
	return ((size_t)(((t_chunk *)ptr - 1)->size - sizeof(t_chunk)));
}
