// Native mouse cursor (type_cursor, vtable 0x59e18c; notes/18 s7, card 191):
// create 0x4b7a10, commands vf14 0x4b7d70, tick vf11 0x4b79e0 -> 0x4b8430.
#pragma once
#include <stdint.h>

#include "game/runtime.h"

namespace game {

// 0x4b7a10 vf3: the typed state for a `native cursor` instance.
void cursor_create(Runtime &rt, Runtime::Resource *inst);
// 0x4b7d70 vf14: returns 1 for every code on a native cursor, 0 otherwise.
int cursor_command(Runtime &rt, Runtime::Resource *cur, uint32_t code, const Value &param);
// 0x4b79e0 vf11 -> 0x4b8430 for every active cursor.
void cursor_tick(Runtime &rt, float dt);
// INVENTED harness hook (vmboot): body id of the last LCLICK sent to a ref, 0 if none.
uint32_t cursor_last_click_body();

}  // namespace game
