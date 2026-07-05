#pragma once

#include "kobox/module.h"

kb_module_t *kb_loader_active_module(void);
void kb_loader_set_active_module(kb_module_t *module);
void *kb_loader_module_current_mm(const kb_module_t *module);
void *kb_loader_module_current_task(const kb_module_t *module);
kb_status_t kb_loader_enter_module_context(kb_module_t *module, unsigned long *out_old_gs);
void kb_loader_leave_module_context(unsigned long old_gs);
