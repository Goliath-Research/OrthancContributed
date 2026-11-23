/* Included ahead of Plugin.cpp so fopen in that file is observable. */
#ifndef VPI_TEST_HOOKS_H
#define VPI_TEST_HOOKS_H

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE* vpi_test_fopen(const char* path, const char* mode);

#ifdef __cplusplus
}
#endif

#define fopen vpi_test_fopen

#endif
