/*
 * Stubs for linking upstream GEMU's machine_nes.c on its own (the reference
 * build of tests/nes_exact.c).  The display and monitor stubs run its loop:
 * input from the test script, one frame hashed per render, quit after the
 * last.  Everything else (FDS, cheats, CRT, hex editor, ...) is inert.
 */
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include <string.h>

uint32_t nesx_poll(void);
void     nesx_render(const uint32_t *argb);
bool     nesx_done(void);

static int dummy;

bool gemu_display_wanted(GemuDisplayType t) { (void)t; return true; }
GemuDisplay *gemu_display_create(GemuDisplayType t, const GemuDisplayConfig *c)
{ (void)t; (void)c; return (GemuDisplay *)&dummy; }
void gemu_display_destroy(GemuDisplay *d) { (void)d; }
uint32_t gemu_display_poll(GemuDisplay *d) { (void)d; return nesx_poll(); }
void gemu_display_render(GemuDisplay *d, const uint32_t *argb, int w, int h)
{ (void)d; (void)w; (void)h; nesx_render(argb); }
GemuPointerState gemu_display_get_pointer(const GemuDisplay *d)
{ (void)d; GemuPointerState p; memset(&p, 0, sizeof(p)); p.x = p.y = -1; return p; }
bool gemu_display_should_quit(const GemuDisplay *d) { (void)d; return false; }
bool gemu_display_reset_requested(const GemuDisplay *d) { (void)d; return false; }
void gemu_display_clear_flags(GemuDisplay *d) { (void)d; }
void gemu_display_set_paused(GemuDisplay *d, bool p) { (void)d; (void)p; }
void gemu_display_add_debug_item(GemuDisplay *d, const char *l, void (*cb)(void *), void *ud)
{ (void)d; (void)l; (void)cb; (void)ud; }
void gemu_display_reset_input_bindings_ud(void *d) { (void)d; }

GemuMonitor *gemu_monitor_create(void) { return (GemuMonitor *)&dummy; }
GemuMonCmd gemu_monitor_poll(GemuMonitor *m) { (void)m; return nesx_done() ? GEMU_MON_QUIT : GEMU_MON_NONE; }
bool gemu_monitor_is_paused(const GemuMonitor *m) { (void)m; return false; }
bool gemu_monitor_check_exec(GemuMonitor *m, uint32_t a) { (void)m; (void)a; return false; }
bool gemu_monitor_check_read(GemuMonitor *m, uint32_t a) { (void)m; (void)a; return false; }
bool gemu_monitor_check_write(GemuMonitor *m, uint32_t a) { (void)m; (void)a; return false; }
uint32_t gemu_monitor_step_count(const GemuMonitor *m) { (void)m; return 0; }
void gemu_monitor_destroy(GemuMonitor *m) { (void)m; }
void gemu_monitor_printf(const GemuMonitor *m, const char *f, ...) { (void)m; (void)f; }
bool gemu_monitor_bind(GemuMonitor *m, const char *n, GemuMonHandler fn, void *ud)
{ (void)m; (void)n; (void)fn; (void)ud; return true; }
bool gemu_monitor_register_media(GemuMonitor *m, const GemuMediaDevice *d) { (void)m; (void)d; return true; }
void gemu_monitor_set_cpu_state_cb(GemuMonitor *m, void (*cb)(void *, char *, size_t), void *ud)
{ (void)m; (void)cb; (void)ud; }
void gemu_monitor_set_input_reset_cb(GemuMonitor *m, void (*cb)(void *), void *ud) { (void)m; (void)cb; (void)ud; }
void gemu_monitor_set_memory(GemuMonitor *m, const GemuMonMemory *mem) { (void)m; (void)mem; }
void gemu_monitor_set_screendump_cb(GemuMonitor *m, bool (*cb)(void *, const char *), void *ud)
{ (void)m; (void)cb; (void)ud; }
void gemu_monitor_shutdown_or_pause(GemuMonitor *m, bool n) { (void)m; (void)n; }
void gemu_monitor_start(GemuMonitor *m) { (void)m; }
void gemu_monitor_stop(GemuMonitor *m) { (void)m; }

/* The rest only need to exist (C linkage doesn't check types): void, or a
 * zero / NULL result. */
#define INERT(name) long name(void) { return 0; }
INERT(cheat_dialog_sdl_create) INERT(nes_pin_window_sdl_create)
INERT(crt_tube_apply) INERT(crt_tube_profile_generic) INERT(nes_ntsc_encode) INERT(ntsc_decode)
INERT(fds_audio_tick) INERT(fds_bios_load) INERT(fds_disk_eject) INERT(fds_disk_flip)
INERT(fds_disk_load) INERT(fds_disk_load_save) INERT(fds_disk_save) INERT(fds_hle_boot)
INERT(fds_hle_build_rom) INERT(fds_reg_read) INERT(fds_reg_write) INERT(fds_tick)
INERT(gemu_config_path) INERT(gemu_ensure_parent_dir) INERT(gemu_screendump)
INERT(gemu_screendump_argb) INERT(romdb_print_needed)
INERT(hex_editor_create) INERT(hex_editor_destroy) INERT(hex_editor_hide)
INERT(hex_editor_is_visible) INERT(hex_editor_refresh) INERT(hex_editor_show)
INERT(nes_cheats_add) INERT(nes_cheats_find) INERT(nes_cheats_init) INERT(nes_cheats_remove)
INERT(nes_cheats_set_enabled) INERT(nes_gg_decode)
