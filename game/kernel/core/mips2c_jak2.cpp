/*!
 * @file mips2c_jak2.cpp
 * The Jak 2 half of the mips2c seam: every hand-translated Jak 2 function, registered by name.
 * Compiled only into jak2-kernel-core; the table itself is in mips2c_seam.cpp.
 *
 * The list is upstream's Jak 2 section of game/mips2c/mips2c_table.cpp, deduplicated:
 * upstream lists method_33_sky_work under both sky-tng and spatial-hash.
 */

#include <mutex>

#include "game/kernel/core/mips2c_seam.h"
#include "game/mips2c/mips2c_table.h"

// jak2_functions/merc_blend_shape.cpp guards blerc data against upstream's render thread with this
// mutex, which upstream defines in the OpenGL renderer (Merc2.cpp). That renderer is not part of
// this library, so the definition lives here; nothing else locks it in a headless runtime.
std::mutex g_merc_data_mutex;

namespace Mips2C {

// mips2c_seam.cpp
void reserve_mips2c_stack();
void forget_mips2c_registrations();

namespace jak2 {
namespace collide_do_primitives { extern void link(); }
namespace moving_sphere_triangle_intersect { extern void link(); }
namespace calc_animation_from_spr { extern void link(); }
namespace cspace_parented_transformq_joint { extern void link(); }
namespace get_string_length { extern void link(); }
namespace draw_string_asm { extern void link(); }
namespace adgif_shader_texture_with_update { extern void link(); }
namespace debug_line_clip { extern void link(); }
namespace init_boundary_regs { extern void link(); }
namespace render_boundary_quad { extern void link(); }
namespace render_boundary_tri { extern void link(); }
namespace set_sky_vf27 { extern void link(); }
namespace draw_boundary_polygon { extern void link(); }
namespace particle_adgif { extern void link(); }
namespace sp_launch_particles_var { extern void link(); }
namespace sparticle_motion_blur { extern void link(); }
namespace sp_process_block_2d { extern void link(); }
namespace sp_process_block_3d { extern void link(); }
namespace set_tex_offset { extern void link(); }
namespace draw_large_polygon { extern void link(); }
namespace render_sky_quad { extern void link(); }
namespace render_sky_tri { extern void link(); }
namespace method_16_sky_work { extern void link(); }
namespace method_17_sky_work { extern void link(); }
namespace method_32_sky_work { extern void link(); }
namespace method_33_sky_work { extern void link(); }
namespace method_28_sky_work { extern void link(); }
namespace method_29_sky_work { extern void link(); }
namespace method_30_sky_work { extern void link(); }
namespace set_sky_vf23_value { extern void link(); }
namespace method_11_collide_hash { extern void link(); }
namespace method_12_collide_hash { extern void link(); }
namespace fill_bg_using_box_new { extern void link(); }
namespace fill_bg_using_line_sphere_new { extern void link(); }
namespace method_12_collide_mesh { extern void link(); }
namespace method_14_collide_mesh { extern void link(); }
namespace method_15_collide_mesh { extern void link(); }
namespace method_10_collide_edge_hold_list { extern void link(); }
namespace method_19_collide_edge_work { extern void link(); }
namespace method_9_edge_grab_info { extern void link(); }
namespace method_16_collide_edge_work { extern void link(); }
namespace method_17_collide_edge_work { extern void link(); }
namespace method_18_collide_edge_work { extern void link(); }
namespace method_16_ocean { extern void link(); }
namespace method_15_ocean { extern void link(); }
namespace method_14_ocean { extern void link(); }
namespace init_ocean_far_regs { extern void link(); }
namespace draw_large_polygon_ocean { extern void link(); }
namespace render_ocean_quad { extern void link(); }
namespace method_18_grid_hash { extern void link(); }
namespace method_19_grid_hash { extern void link(); }
namespace method_20_grid_hash { extern void link(); }
namespace method_22_grid_hash { extern void link(); }
namespace method_28_sphere_hash { extern void link(); }
namespace method_29_sphere_hash { extern void link(); }
namespace method_30_sphere_hash { extern void link(); }
namespace method_31_sphere_hash { extern void link(); }
namespace method_32_sphere_hash { extern void link(); }
namespace method_33_sphere_hash { extern void link(); }
namespace method_33_spatial_hash { extern void link(); }
namespace method_35_spatial_hash { extern void link(); }
namespace method_36_spatial_hash { extern void link(); }
namespace method_37_spatial_hash { extern void link(); }
namespace method_39_spatial_hash { extern void link(); }
namespace method_10_collide_shape_prim_mesh { extern void link(); }
namespace method_10_collide_shape_prim_sphere { extern void link(); }
namespace method_10_collide_shape_prim_group { extern void link(); }
namespace method_11_collide_shape_prim_mesh { extern void link(); }
namespace method_11_collide_shape_prim_sphere { extern void link(); }
namespace method_11_collide_shape_prim_group { extern void link(); }
namespace method_9_collide_cache_prim { extern void link(); }
namespace method_10_collide_cache_prim { extern void link(); }
namespace method_17_collide_cache { extern void link(); }
namespace method_9_collide_puss_work { extern void link(); }
namespace method_10_collide_puss_work { extern void link(); }
namespace bones_mtx_calc { extern void link(); }
namespace foreground_check_longest_edge_asm { extern void link(); }
namespace foreground_merc { extern void link(); }
namespace foreground_generic_merc { extern void link(); }
namespace foreground_draw_hud { extern void link(); }
namespace add_light_sphere_to_light_group { extern void link(); }
namespace light_hash_add_items { extern void link(); }
namespace light_hash_count_items { extern void link(); }
namespace light_hash_get_bucket_index { extern void link(); }
namespace method_39_nav_state { extern void link(); }
namespace nav_state_patch_pointers { extern void link(); }
namespace method_45_nav_mesh { extern void link(); }
namespace method_20_nav_engine { extern void link(); }
namespace method_43_nav_mesh { extern void link(); }
namespace nav_dma_send_to_spr_no_flush { extern void link(); }
namespace nav_dma_send_from_spr_no_flush { extern void link(); }
namespace method_17_nav_engine { extern void link(); }
namespace method_18_nav_engine { extern void link(); }
namespace method_21_nav_engine { extern void link(); }
namespace setup_blerc_chains_for_one_fragment { extern void link(); }
namespace blerc_execute { extern void link(); }
namespace ripple_execute_init { extern void link(); }
namespace ripple_create_wave_table { extern void link(); }
namespace ripple_apply_wave_table { extern void link(); }
namespace ripple_matrix_scale { extern void link(); }
namespace method_53_squid { extern void link(); }
namespace init_vortex_regs { extern void link(); }
namespace draw_large_polygon_vortex { extern void link(); }
namespace render_vortex_quad { extern void link(); }
namespace generic_merc_init_asm { extern void link(); }
namespace mercneric_convert { extern void link(); }
namespace high_speed_reject { extern void link(); }
namespace generic_translucent { extern void link(); }
namespace generic_merc_query { extern void link(); }
namespace generic_merc_death { extern void link(); }
namespace generic_merc_execute_asm { extern void link(); }
namespace generic_merc_do_chain { extern void link(); }
namespace generic_light_proc { extern void link(); }
namespace generic_envmap_proc { extern void link(); }
namespace generic_prepare_dma_double { extern void link(); }
namespace generic_prepare_dma_single { extern void link(); }
namespace generic_warp_source_proc { extern void link(); }
namespace generic_warp_dest_proc { extern void link(); }
namespace generic_warp_dest { extern void link(); }
namespace generic_warp_envmap_dest { extern void link(); }
namespace generic_no_light_proc { extern void link(); }
namespace shadow_execute { extern void link(); }
namespace shadow_add_double_edges { extern void link(); }
namespace shadow_add_double_tris { extern void link(); }
namespace shadow_add_single_tris { extern void link(); }
namespace shadow_add_single_edges { extern void link(); }
namespace shadow_add_facing_single_tris { extern void link(); }
namespace shadow_add_verts { extern void link(); }
namespace shadow_find_double_edges { extern void link(); }
namespace shadow_find_facing_double_tris { extern void link(); }
namespace shadow_find_single_edges { extern void link(); }
namespace shadow_find_facing_single_tris { extern void link(); }
namespace shadow_init_vars { extern void link(); }
namespace shadow_scissor_top { extern void link(); }
namespace shadow_scissor_edges { extern void link(); }
namespace shadow_calc_dual_verts { extern void link(); }
namespace shadow_xform_verts { extern void link(); }
}  // namespace jak2

}  // namespace Mips2C

/*!
 * Register every Jak 2 mips2c function. Must run after the symbol table and heaps exist, because
 * each registration allocates a GOAL function object.
 */
void goal_mips2c_register_jak2(void) {
  Mips2C::forget_mips2c_registrations();
  Mips2C::reserve_mips2c_stack();
  using namespace Mips2C::jak2;
  collide_do_primitives::link();
  moving_sphere_triangle_intersect::link();
  calc_animation_from_spr::link();
  cspace_parented_transformq_joint::link();
  get_string_length::link();
  draw_string_asm::link();
  adgif_shader_texture_with_update::link();
  debug_line_clip::link();
  init_boundary_regs::link();
  render_boundary_quad::link();
  render_boundary_tri::link();
  set_sky_vf27::link();
  draw_boundary_polygon::link();
  particle_adgif::link();
  sp_launch_particles_var::link();
  sparticle_motion_blur::link();
  sp_process_block_2d::link();
  sp_process_block_3d::link();
  set_tex_offset::link();
  draw_large_polygon::link();
  render_sky_quad::link();
  render_sky_tri::link();
  method_16_sky_work::link();
  method_17_sky_work::link();
  method_32_sky_work::link();
  method_33_sky_work::link();
  method_28_sky_work::link();
  method_29_sky_work::link();
  method_30_sky_work::link();
  set_sky_vf23_value::link();
  method_11_collide_hash::link();
  method_12_collide_hash::link();
  fill_bg_using_box_new::link();
  fill_bg_using_line_sphere_new::link();
  method_12_collide_mesh::link();
  method_14_collide_mesh::link();
  method_15_collide_mesh::link();
  method_10_collide_edge_hold_list::link();
  method_19_collide_edge_work::link();
  method_9_edge_grab_info::link();
  method_16_collide_edge_work::link();
  method_17_collide_edge_work::link();
  method_18_collide_edge_work::link();
  method_16_ocean::link();
  method_15_ocean::link();
  method_14_ocean::link();
  init_ocean_far_regs::link();
  draw_large_polygon_ocean::link();
  render_ocean_quad::link();
  method_18_grid_hash::link();
  method_19_grid_hash::link();
  method_20_grid_hash::link();
  method_22_grid_hash::link();
  method_28_sphere_hash::link();
  method_29_sphere_hash::link();
  method_30_sphere_hash::link();
  method_31_sphere_hash::link();
  method_32_sphere_hash::link();
  method_33_sphere_hash::link();
  method_33_spatial_hash::link();
  method_35_spatial_hash::link();
  method_36_spatial_hash::link();
  method_37_spatial_hash::link();
  method_39_spatial_hash::link();
  method_10_collide_shape_prim_mesh::link();
  method_10_collide_shape_prim_sphere::link();
  method_10_collide_shape_prim_group::link();
  method_11_collide_shape_prim_mesh::link();
  method_11_collide_shape_prim_sphere::link();
  method_11_collide_shape_prim_group::link();
  method_9_collide_cache_prim::link();
  method_10_collide_cache_prim::link();
  method_17_collide_cache::link();
  method_9_collide_puss_work::link();
  method_10_collide_puss_work::link();
  bones_mtx_calc::link();
  foreground_check_longest_edge_asm::link();
  foreground_merc::link();
  foreground_generic_merc::link();
  foreground_draw_hud::link();
  add_light_sphere_to_light_group::link();
  light_hash_add_items::link();
  light_hash_count_items::link();
  light_hash_get_bucket_index::link();
  method_39_nav_state::link();
  nav_state_patch_pointers::link();
  method_45_nav_mesh::link();
  method_20_nav_engine::link();
  method_43_nav_mesh::link();
  nav_dma_send_to_spr_no_flush::link();
  nav_dma_send_from_spr_no_flush::link();
  method_17_nav_engine::link();
  method_18_nav_engine::link();
  method_21_nav_engine::link();
  setup_blerc_chains_for_one_fragment::link();
  blerc_execute::link();
  ripple_execute_init::link();
  ripple_create_wave_table::link();
  ripple_apply_wave_table::link();
  ripple_matrix_scale::link();
  method_53_squid::link();
  init_vortex_regs::link();
  draw_large_polygon_vortex::link();
  render_vortex_quad::link();
  generic_merc_init_asm::link();
  mercneric_convert::link();
  high_speed_reject::link();
  generic_translucent::link();
  generic_merc_query::link();
  generic_merc_death::link();
  generic_merc_execute_asm::link();
  generic_merc_do_chain::link();
  generic_light_proc::link();
  generic_envmap_proc::link();
  generic_prepare_dma_double::link();
  generic_prepare_dma_single::link();
  generic_warp_source_proc::link();
  generic_warp_dest_proc::link();
  generic_warp_dest::link();
  generic_warp_envmap_dest::link();
  generic_no_light_proc::link();
  shadow_execute::link();
  shadow_add_double_edges::link();
  shadow_add_double_tris::link();
  shadow_add_single_tris::link();
  shadow_add_single_edges::link();
  shadow_add_facing_single_tris::link();
  shadow_add_verts::link();
  shadow_find_double_edges::link();
  shadow_find_facing_double_tris::link();
  shadow_find_single_edges::link();
  shadow_find_facing_single_tris::link();
  shadow_init_vars::link();
  shadow_scissor_top::link();
  shadow_scissor_edges::link();
  shadow_calc_dual_verts::link();
  shadow_xform_verts::link();
}
