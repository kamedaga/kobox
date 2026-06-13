#include "linux_subsystem/usb/port_state.h"
#include "kobox/shim.h"

#include <stdint.h>
#include <string.h>

static kb_usb_port_state_t *usb_port_states;

static int pointer_is_error_or_low(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

kb_usb_port_state_t *kb_usb_port_state_for_hcd(void *hcd)
{
    if (pointer_is_error_or_low(hcd)) {
        return NULL;
    }
    for (kb_usb_port_state_t *state = usb_port_states; state != NULL; state = state->next) {
        if (state->hcd == hcd) {
            return state;
        }
    }

    kb_usb_port_state_t *state = kb_kzalloc(sizeof(*state), 0);
    if (state == NULL) {
        return NULL;
    }
    state->hcd = hcd;
    state->next = usb_port_states;
    usb_port_states = state;
    return state;
}

void kb_usb_port_state_release(void *hcd)
{
    if (pointer_is_error_or_low(hcd)) {
        return;
    }

    kb_usb_port_state_t **cursor = &usb_port_states;
    while (*cursor != NULL) {
        kb_usb_port_state_t *state = *cursor;
        if (state->hcd == hcd) {
            *cursor = state->next;
            memset(state, 0, sizeof(*state));
            kb_kfree(state);
            return;
        }
        cursor = &state->next;
    }
}
