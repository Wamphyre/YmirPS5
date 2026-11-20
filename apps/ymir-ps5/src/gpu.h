// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ymir_gpu_open(unsigned width, unsigned height, unsigned canvasWidth, unsigned canvasHeight, bool prefer120Hz,
                   char *error, size_t size);
void ymir_gpu_close(void);
bool ymir_gpu_resize(unsigned width, unsigned height);
bool ymir_gpu_matches(unsigned width, unsigned height);
const char *ymir_gpu_description(void);
double ymir_gpu_refresh(void);

bool ymir_gpu_menu(const uint32_t *pixels);
bool ymir_gpu_game(const uint32_t *pixels, unsigned width, unsigned height, bool fresh, bool frameTick, float x,
                   float y, float extentWidth, float extentHeight, bool fsr, unsigned generated, double coreHz);

#ifdef __cplusplus
}
#endif
