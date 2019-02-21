#!/bin/sh
export DYLD_LIBRARY_PATH=.
export DYLD_INSERT_LIBRARIES="libft_malloc.so"
export DYLD_FORCE_FLAT_NAMESPACE=1
export M_ABORT_ON_ERROR=0
export M_SHOW_HEXDUMP=1
export M_SHOW_UNALLOCATED=1
$@
