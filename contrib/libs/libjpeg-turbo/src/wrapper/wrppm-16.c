/*
 * wrppm-16.c
 *
 * Copyright (C) 2025, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file is a wrapper for compiling wrppm.c to support 13 to 16 bits of
 * data precision.  wrppm.c should not be compiled directly.
 */

#define BITS_IN_JSAMPLE  16

#include "../wrppm.c"
