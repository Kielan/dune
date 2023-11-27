#include "lib_math_base_safe.h"
#include "lib_rand.hh"

#include "dune_cxt.hh"
#include "dune_curves.hh"
#include "dune_node.hh"
#include "dune_node_runtime.hh"

#include "lang.h"

#include "ed_curves.hh"
#include "ed_node.hh"
#include "ed_ob.hh"

#include "types_mod.h"
#include "types_node.h"
#include "types_ob.h"

namespace dune::ed::curves {

static bool has_surface_deformation_node(const NodeTree &ntree)
{
  if (!ntree.nodes_by_type("GeometryNodeDeformCurvesOnSurface").is_empty()) {
    return true;
  }
  for (const Node *node : ntree.group_nodes()) {
    if (const NodeTree *sub_tree = reinterpret_cast<const NodeTree *>(node->id)) {
      if (has_surface_deformation_node(*sub_tree)) {
        return true;
      }
    }
  }
  return false;
}

static bool has_surface_deformation_node(const Ob &curves_ob)
{
  LIST_FOREACH (const ModData *, md, &curves_ob.mods) {
    if (md->type != eModTypeNodes) {
      continue;
    }
    const NodesModData *nmd = reinterpret_cast<const NodesModData *>(md);
    if (nmd->node_group == nullptr) {
      continue;
    }
    if (has_surface_deformation_node(*nmd->node_group)) {
      return true;
    }
  }
  return false;
}

void ensure_surface_deformation_node_exists(Cxt &C, Ob &curves_ob)
{
  if (has_surface_deformation_node(curves_ob)) {
    return;
  }

  Main *main = cxt_data_main(&C);
  Scene *scene = cxt_data_scene(&C);

  ModData *md = ed_ob_mod_add(
      nullptr, main, scene, &curves_ob, DATA_("Surface Deform"), eModTypeNodes);
  NodesModData &nmd = *reinterpret_cast<NodesModData *>(md);
  nmd.node_group = ntreeAddTree(main, DATA_("Surface Deform"), "GeometryNodeTree");

  NodeTree *ntree = nmd.node_group;
  ntree->tree_interface.add_socket("Geometry",
                                   "",
                                   "NodeSocketGeometry",
                                   NODE_INTERFACE_SOCKET_INPUT | NODE_INTERFACE_SOCKET_OUTPUT,
                                   nullptr);
  Node *group_input = nodeAddStaticNode(&C, ntree, NODE_GROUP_INPUT);
  Node *group_output = nodeAddStaticNode(&C, ntree, NODE_GROUP_OUTPUT);
  Node *deform_node = nodeAddStaticNode(&C, ntree, GEO_NODE_DEFORM_CURVES_ON_SURFACE);

  ed_node_tree_propagate_change(&C, bmain, nmd.node_group);

  nodeAddLink(ntree,
              group_input,
              static_cast<bNodeSocket *>(group_input->outputs.first),
              deform_node,
              nodeFindSocket(deform_node, SOCK_IN, "Curves"));
  nodeAddLink(ntree,
              deform_node,
              nodeFindSocket(deform_node, SOCK_OUT, "Curves"),
              group_output,
              static_cast<bNodeSocket *>(group_output->inputs.first));

  group_input->locx = -200;
  group_output->locx = 200;
  deform_node->locx = 0;

  ED_node_tree_propagate_change(&C, main, nmd.node_group);
}

dune::CurvesGeometry primitive_random_sphere(const int curves_size, const int points_per_curve)
{
  dune::CurvesGeometry curves(points_per_curve * curves_size, curves_size);

  MutableSpan<int> offsets = curves.offsets_for_write();
  MutableSpan<float3> positions = curves.positions_for_write();
  dune::MutableAttributeAccessor attributes = curves.attributes_for_write();
  dune::SpanAttributeWriter<float> radius = attributes.lookup_or_add_for_write_only_span<float>(
      "radius", ATTR_DOMAIN_POINT);

  for (const int i : offsets.index_range()) {
    offsets[i] = points_per_curve * i;
  }

  RandomNumberGenerator rng;

  const OffsetIndices points_by_curve = curves.points_by_curve();
  for (const int i : curves.curves_range()) {
    const IndexRange points = points_by_curve[i];
    MutableSpan<float3> curve_positions = positions.slice(points);
    MutableSpan<float> curve_radii = radius.span.slice(points);

    const float theta = 2.0f * M_PI * rng.get_float();
    const float phi = safe_acosf(2.0f * rng.get_float() - 1.0f);

    float3 no = {std::sin(theta) * std::sin(phi), std::cos(theta) * std::sin(phi), std::cos(phi)};
    no = math::normalize(no);

    float3 co = no;
    for (int key = 0; key < points_per_curve; key++) {
      float t = key / float(points_per_curve - 1);
      curve_positions[key] = co;
      curve_radii[key] = 0.02f * (1.0f - t);

      float3 offset = float3(rng.get_float(), rng.get_float(), rng.get_float()) * 2.0f - 1.0f;
      co += (offset + no) / points_per_curve;
    }
  }

  radius.finish();

  return curves;
}

}  // namespace dune::ed::curves
