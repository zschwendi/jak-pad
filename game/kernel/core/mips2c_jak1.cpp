/*!
 * @file mips2c_jak1.cpp
 * The Jak 1 half of the mips2c seam: every hand-translated Jak 1 function, registered by name.
 * Compiled only into jak1-kernel-core; the table itself is in mips2c_seam.cpp.
 */

#include "game/kernel/core/mips2c_seam.h"
#include "game/mips2c/mips2c_table.h"

namespace Mips2C {

// mips2c_seam.cpp
void reserve_mips2c_stack();
void forget_mips2c_registrations();

namespace jak1 {
namespace draw_string { extern void link(); }
namespace particle_adgif { extern void link(); }
namespace sp_launch_particles_var { extern void link(); }
namespace sp_process_block_3d { extern void link(); }
namespace sp_process_block_2d { extern void link(); }
namespace draw_large_polygon { extern void link(); }
namespace init_sky_regs { extern void link(); }
namespace clip_polygon_against_positive_hyperplane { extern void link(); }
namespace render_sky_quad { extern void link(); }
namespace render_sky_tri { extern void link(); }
namespace set_tex_offset { extern void link(); }
namespace set_sky_vf27 { extern void link(); }
namespace set_sky_vf23_value { extern void link(); }
namespace adgif_shader_texture_with_update { extern void link(); }
namespace init_boundary_regs { extern void link(); }
namespace render_boundary_quad { extern void link(); }
namespace render_boundary_tri { extern void link(); }
namespace draw_boundary_polygon { extern void link(); }
namespace draw_inline_array_tfrag { extern void link(); }
namespace stats_tfrag_asm { extern void link(); }
namespace time_of_day_interp_colors_scratch { extern void link(); }
namespace collide_do_primitives { extern void link(); }
namespace moving_sphere_triangle_intersect { extern void link(); }
namespace method_12_collide_mesh { extern void link(); }
namespace method_11_collide_mesh { extern void link(); }
namespace collide_probe_node { extern void link(); }
namespace collide_probe_instance_tie { extern void link(); }
namespace method_26_collide_cache { extern void link(); }
namespace method_32_collide_cache { extern void link(); }
namespace pc_upload_collide_frag { extern void link(); }
namespace method_28_collide_cache { extern void link(); }
namespace method_27_collide_cache { extern void link(); }
namespace method_29_collide_cache { extern void link(); }
namespace method_12_collide_shape_prim_mesh { extern void link(); }
namespace method_14_collide_shape_prim_mesh { extern void link(); }
namespace method_13_collide_shape_prim_mesh { extern void link(); }
namespace method_30_collide_cache { extern void link(); }
namespace method_9_collide_cache_prim { extern void link(); }
namespace method_10_collide_cache_prim { extern void link(); }
namespace method_10_collide_puss_work { extern void link(); }
namespace method_9_collide_puss_work { extern void link(); }
namespace method_15_collide_mesh { extern void link(); }
namespace method_14_collide_mesh { extern void link(); }
namespace method_16_collide_edge_work { extern void link(); }
namespace method_15_collide_edge_work { extern void link(); }
namespace method_10_collide_edge_hold_list { extern void link(); }
namespace method_18_collide_edge_work { extern void link(); }
namespace calc_animation_from_spr { extern void link(); }
namespace bones_mtx_calc { extern void link(); }
namespace cspace_parented_transformq_joint { extern void link(); }
namespace draw_bones_merc { extern void link(); }
namespace draw_bones_check_longest_edge_asm { extern void link(); }
namespace blerc_execute { extern void link(); }
namespace setup_blerc_chains_for_one_fragment { extern void link(); }
namespace generic_merc_init_asm { extern void link(); }
namespace generic_merc_execute_asm { extern void link(); }
namespace mercneric_convert { extern void link(); }
namespace generic_prepare_dma_double { extern void link(); }
namespace generic_light_proc { extern void link(); }
namespace generic_envmap_proc { extern void link(); }
namespace high_speed_reject { extern void link(); }
namespace generic_prepare_dma_single { extern void link(); }
namespace ripple_create_wave_table { extern void link(); }
namespace ripple_execute_init { extern void link(); }
namespace ripple_apply_wave_table { extern void link(); }
namespace ripple_matrix_scale { extern void link(); }
namespace init_ocean_far_regs { extern void link(); }
namespace render_ocean_quad { extern void link(); }
namespace draw_large_polygon_ocean { extern void link(); }
namespace ocean_interp_wave { extern void link(); }
namespace ocean_generate_verts { extern void link(); }
namespace shadow_execute { extern void link(); }
namespace shadow_add_double_edges { extern void link(); }
namespace shadow_add_double_tris { extern void link(); }
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
namespace draw_inline_array_instance_tie { extern void link(); }
namespace draw_inline_array_prototype_tie_generic_asm { extern void link(); }
namespace generic_tie_dma_to_spad_sync { extern void link(); }
namespace generic_envmap_dproc { extern void link(); }
namespace generic_interp_dproc { extern void link(); }
namespace generic_no_light_dproc { extern void link(); }
namespace generic_tie_convert { extern void link(); }
}  // namespace jak1

}  // namespace Mips2C

/*!
 * Register every Jak 1 mips2c function. Must run after the symbol table and heaps exist, because
 * each registration allocates a GOAL function object.
 */
void goal_mips2c_register_jak1(void) {
  Mips2C::forget_mips2c_registrations();
  Mips2C::reserve_mips2c_stack();
  using namespace Mips2C::jak1;
  draw_string::link();
  particle_adgif::link();
  sp_launch_particles_var::link();
  sp_process_block_3d::link();
  sp_process_block_2d::link();
  draw_large_polygon::link();
  init_sky_regs::link();
  clip_polygon_against_positive_hyperplane::link();
  render_sky_quad::link();
  render_sky_tri::link();
  set_tex_offset::link();
  set_sky_vf27::link();
  set_sky_vf23_value::link();
  adgif_shader_texture_with_update::link();
  init_boundary_regs::link();
  render_boundary_quad::link();
  render_boundary_tri::link();
  draw_boundary_polygon::link();
  draw_inline_array_tfrag::link();
  stats_tfrag_asm::link();
  time_of_day_interp_colors_scratch::link();
  collide_do_primitives::link();
  moving_sphere_triangle_intersect::link();
  method_12_collide_mesh::link();
  method_11_collide_mesh::link();
  collide_probe_node::link();
  collide_probe_instance_tie::link();
  method_26_collide_cache::link();
  method_32_collide_cache::link();
  pc_upload_collide_frag::link();
  method_28_collide_cache::link();
  method_27_collide_cache::link();
  method_29_collide_cache::link();
  method_12_collide_shape_prim_mesh::link();
  method_14_collide_shape_prim_mesh::link();
  method_13_collide_shape_prim_mesh::link();
  method_30_collide_cache::link();
  method_9_collide_cache_prim::link();
  method_10_collide_cache_prim::link();
  method_10_collide_puss_work::link();
  method_9_collide_puss_work::link();
  method_15_collide_mesh::link();
  method_14_collide_mesh::link();
  method_16_collide_edge_work::link();
  method_15_collide_edge_work::link();
  method_10_collide_edge_hold_list::link();
  method_18_collide_edge_work::link();
  calc_animation_from_spr::link();
  bones_mtx_calc::link();
  cspace_parented_transformq_joint::link();
  draw_bones_merc::link();
  draw_bones_check_longest_edge_asm::link();
  blerc_execute::link();
  setup_blerc_chains_for_one_fragment::link();
  generic_merc_init_asm::link();
  generic_merc_execute_asm::link();
  mercneric_convert::link();
  generic_prepare_dma_double::link();
  generic_light_proc::link();
  generic_envmap_proc::link();
  high_speed_reject::link();
  generic_prepare_dma_single::link();
  ripple_create_wave_table::link();
  ripple_execute_init::link();
  ripple_apply_wave_table::link();
  ripple_matrix_scale::link();
  init_ocean_far_regs::link();
  render_ocean_quad::link();
  draw_large_polygon_ocean::link();
  ocean_interp_wave::link();
  ocean_generate_verts::link();
  shadow_execute::link();
  shadow_add_double_edges::link();
  shadow_add_double_tris::link();
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
  draw_inline_array_instance_tie::link();
  draw_inline_array_prototype_tie_generic_asm::link();
  generic_tie_dma_to_spad_sync::link();
  generic_envmap_dproc::link();
  generic_interp_dproc::link();
  generic_no_light_dproc::link();
  generic_tie_convert::link();
}

