#pragma once

#include "kobox/module.h"

kb_module_t *kb_loader_active_module(void);
void kb_loader_set_active_module(kb_module_t *module);
void *kb_loader_module_current_mm(const kb_module_t *module);
void *kb_loader_module_current_task(const kb_module_t *module);
void *kb_loader_default_current_task(void);
void *kb_loader_current_task(void);
void *kb_loader_clone_execution_task(void);
void *kb_loader_task_journal_info(const void *task);
unsigned int kb_loader_task_state(const void *task);
void kb_loader_set_current_task_for_all_modules(void *task);
void kb_loader_refresh_page_model_for_all_modules(void);
kb_status_t kb_loader_enter_module_context(kb_module_t *module, unsigned long *out_old_gs);
void kb_loader_leave_module_context(unsigned long old_gs);
