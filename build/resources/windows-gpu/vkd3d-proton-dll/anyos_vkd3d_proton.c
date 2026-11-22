/* Copyright 2026 FULLSTACKHUB. SPDX-License-Identifier: MIT
 *
 * The one source file of anyos_vkd3d_proton.dll besides vkd3d-proton itself.
 * That DLL is vkd3d-proton (LGPL-2.1-or-later, with its MIT dxil-spirv /
 * dxbc-spirv subprojects) linked on its own, so the Any OS D3D12 user-mode
 * driver (anyos_d3d12_umd.dll, MIT) loads it at run time instead of carrying
 * it statically: anyos_vkd3d_proton.def exports the four entry points the
 * driver calls (vkd3d_create_instance, vkd3d_create_device and the two
 * root-signature serializers), and a rebuilt or replaced copy of this DLL
 * works with the same driver.
 *
 * vkd3d-proton's debug-control switches (test-suite hooks) live in its
 * d3d12core DLL entry file (libs/d3d12core/main.c), which this DLL does not
 * include; the defaults below are what a release d3d12core reports.  The
 * enum parameters are declared in vkd3d_core_interface.h; plain ints carry
 * the same ABI. */
#include <stdbool.h>

bool vkd3d_debug_control_is_test_suite(void) { return false; }
bool vkd3d_debug_control_explode_on_vvl_error(void) { return false; }
bool vkd3d_debug_control_has_out_of_spec_test_behavior(int behavior) { (void)behavior; return false; }
unsigned vkd3d_debug_control_get_behavior_flags(void) { return 0; }
bool vkd3d_debug_control_mute_message_id(const char *vuid) { (void)vuid; return false; }
