#pragma once

#include <stdio.h>

#include "goalc/aot/goal_c_runtime.h"

void goal_test_loader_init(void);
void goal_test_load_statics(const goal_static_desc* statics, int count);
void goal_test_load_functions(const void* const* natives, int count);
uint64_t goal_test_empty_pair(void);
