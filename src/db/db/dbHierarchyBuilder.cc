
/*

  KLayout Layout Viewer
  Copyright (C) 2006-2018 Matthias Koefferlein

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#include "dbRecursiveShapeIterator.h"
#include "dbHierarchyBuilder.h"
#include "dbClip.h"
#include "dbRegion.h"
#include "dbPolygonTools.h"

namespace db
{

static HierarchyBuilderShapeInserter def_inserter;

// -------------------------------------------------------------------------------------------

int
compare_iterators_with_respect_to_target_hierarchy (const db::RecursiveShapeIterator &iter1, const db::RecursiveShapeIterator &iter2)
{
  //  basic source (layout, top_cell) needs to be the same of course
  if (iter1.layout () != iter2.layout ()) {
    //  NOTE: pointer compare :-(
    return iter1.layout () < iter2.layout () ? -1 : 1;
  }
  if (iter1.top_cell ()->cell_index () != iter2.top_cell ()->cell_index ()) {
    return iter1.top_cell ()->cell_index () < iter2.top_cell ()->cell_index () ? -1 : 1;
  }

  //  max depth controls the main hierarchical appearance
  if (iter1.max_depth () != iter2.max_depth ()) {
    return iter1.max_depth () < iter2.max_depth () ? -1 : 1;
  }

  //  if a region is set, the hierarchical appearance is the same only if the layers and
  //  complex region are indentical
  if ((iter1.region () == db::Box::world ()) != (iter2.region () == db::Box::world ())) {
    return (iter1.region () == db::Box::world ()) < (iter2.region () == db::Box::world ()) ? -1 : 1;
  }
  if (iter1.region () != db::Box::world ()) {
    if (iter1.has_complex_region () != iter2.has_complex_region ()) {
      return iter1.has_complex_region () < iter2.has_complex_region () ? -1 : 1;
    }
    if (iter1.has_complex_region () && iter1.complex_region () != iter2.complex_region ()) {
      return iter1.complex_region () < iter2.complex_region () ? -1 : 1;
    }
    if (iter1.multiple_layers () != iter2.multiple_layers ()) {
      return iter1.multiple_layers () < iter2.multiple_layers () ? -1 : 1;
    }
    if (iter1.multiple_layers ()) {
      if (iter1.layers () != iter2.layers ()) {
        return iter1.layers () < iter2.layers ();
      }
    } else {
      if (iter1.layer () != iter2.layer ()) {
        return iter1.layer () < iter2.layer ();
      }
    }
  }

  return 0;
}

// -------------------------------------------------------------------------------------------

/**
 *  @brief Computes the clip variant (a box set) from a cell bbox, a region and a complex region (optional)
 */
static std::pair<bool, std::set<db::Box> > compute_clip_variant (const db::Box &cell_bbox, const db::ICplxTrans &trans, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region)
{
  if (region == db::Box::world ()) {
    return std::make_pair (true, std::set<db::Box> ());
  }

  db::ICplxTrans trans_inv (trans.inverted ());
  db::Box region_in_cell = region.transformed (trans_inv);

  std::set<db::Box> clip_variant;
  if (! cell_bbox.overlaps (region_in_cell)) {
    //  an empty clip variant should not happen, but who knows
    return std::make_pair (false, std::set<db::Box> ());
  }

  db::Box rect_box = region_in_cell & cell_bbox;

  if (complex_region) {

    for (db::RecursiveShapeReceiver::box_tree_type::overlapping_iterator cr = complex_region->begin_overlapping (region, db::box_convert<db::Box> ()); ! cr.at_end (); ++cr) {
      db::Box cr_in_cell = (*cr).transformed (trans_inv);
      if (rect_box.overlaps (cr_in_cell)) {
        clip_variant.insert (rect_box * cr_in_cell);
      }
    }

    if (clip_variant.empty ()) {
      //  an empty clip variant should not happen, but who knows
      return std::make_pair (false, std::set<db::Box> ());
    }

  } else {
    clip_variant.insert (rect_box);
  }

  return std::make_pair (true, clip_variant);
}

HierarchyBuilder::HierarchyBuilder (db::Layout *target, unsigned int target_layer, HierarchyBuilderShapeReceiver *pipe)
  : mp_target (target), m_initial_pass (true), m_target_layer (target_layer)
{
  set_shape_receiver (pipe);
}

HierarchyBuilder::HierarchyBuilder (db::Layout *target, HierarchyBuilderShapeReceiver *pipe)
  : mp_target (target), m_initial_pass (true), m_target_layer (0)
{
  set_shape_receiver (pipe);
}

HierarchyBuilder::~HierarchyBuilder ()
{
  //  .. nothing yet ..
}

void
HierarchyBuilder::set_shape_receiver (HierarchyBuilderShapeReceiver *pipe)
{
  mp_pipe = pipe ? pipe : &def_inserter;
}

void
HierarchyBuilder::reset ()
{
  m_initial_pass = true;
  mp_initial_cell = 0;

  m_cell_map.clear ();
  m_cells_seen.clear ();
  m_cell_stack.clear ();
  m_cm_entry = cell_map_type::const_iterator ();
}

void
HierarchyBuilder::begin (const RecursiveShapeIterator *iter)
{
  if (m_initial_pass) {
    m_ref_iter = *iter;
  } else {
    tl_assert (compare_iterators_with_respect_to_target_hierarchy (m_ref_iter, *iter) == 0);
  }

  m_cell_stack.clear ();
  m_cells_seen.clear ();

  std::pair<db::cell_index_type, std::set<db::Box> > key (iter->top_cell ()->cell_index (), std::set<db::Box> ());
  m_cm_entry = m_cell_map.find (key);
  if (m_cm_entry == m_cell_map.end ()) {

    db::cell_index_type new_top_index = mp_target->add_cell (iter->layout ()->cell_name (key.first));
    m_cm_entry = m_cell_map.insert (std::make_pair (key, new_top_index)).first;

  }

  db::Cell &new_top = mp_target->cell (m_cm_entry->second);
  m_cells_seen.insert (key);

  m_cell_stack.push_back (&new_top);
}

void
HierarchyBuilder::end (const RecursiveShapeIterator * /*iter*/)
{
  tl_assert (m_cell_stack.size () == 1);

  m_initial_pass = false;
  m_cells_seen.clear ();
  mp_initial_cell = m_cell_stack.front ();
  m_cell_stack.clear ();
  m_cm_entry = cell_map_type::const_iterator ();
}

void
HierarchyBuilder::enter_cell (const RecursiveShapeIterator * /*iter*/, const db::Cell * /*cell*/, const db::Box & /*region*/, const box_tree_type * /*complex_region*/)
{
  tl_assert (m_cm_entry != m_cell_map.end () && m_cm_entry != cell_map_type::const_iterator ());
  m_cells_seen.insert (m_cm_entry->first);

  m_cell_stack.push_back (&mp_target->cell (m_cm_entry->second));
}

void
HierarchyBuilder::leave_cell (const RecursiveShapeIterator * /*iter*/, const db::Cell * /*cell*/)
{
  m_cell_stack.pop_back ();
}

HierarchyBuilder::new_inst_mode
HierarchyBuilder::new_inst (const RecursiveShapeIterator *iter, const db::CellInstArray &inst, const db::Box & /*region*/, const box_tree_type * /*complex_region*/, bool all)
{
  if (all) {

    std::pair<db::cell_index_type, std::set<db::Box> > key (inst.object ().cell_index (), std::set<db::Box> ());
    m_cm_entry = m_cell_map.find (key);

    if (m_initial_pass) {

      if (m_cm_entry == m_cell_map.end ()) {
        db::cell_index_type new_cell = mp_target->add_cell (iter->layout ()->cell_name (inst.object ().cell_index ()));
        m_cm_entry = m_cell_map.insert (std::make_pair (key, new_cell)).first;
      }

      db::CellInstArray new_inst = inst;
      new_inst.object () = db::CellInst (m_cm_entry->second);
      m_cell_stack.back ()->insert (new_inst);

    }

    //  To see the cell once, use NI_single. If we did see the cell already, skip the whole instance array.
    return (m_cells_seen.find (key) == m_cells_seen.end ()) ? NI_single : NI_skip;

  } else {

    //  Iterate by instance array members
    return NI_all;

  }
}

bool
HierarchyBuilder::new_inst_member (const RecursiveShapeIterator *iter, const db::CellInstArray &inst, const db::ICplxTrans &trans, const db::Box &region, const box_tree_type *complex_region, bool all)
{
  if (all) {

    return true;

  } else {

    db::Box cell_bbox = iter->layout ()->cell (inst.object ().cell_index ()).bbox ();
    std::pair<bool, std::set<db::Box> > clip_variant = compute_clip_variant (cell_bbox, trans, region, complex_region);
    if (! clip_variant.first) {
      return false;
    }

    std::pair<db::cell_index_type, std::set<db::Box> > key (inst.object ().cell_index (), clip_variant.second);
    m_cm_entry = m_cell_map.find (key);

    if (m_initial_pass) {

      if (m_cm_entry == m_cell_map.end ()) {
        std::string suffix;
        if (! key.second.empty ()) {
          suffix = "$CLIP_VAR";
        }
        db::cell_index_type new_cell = mp_target->add_cell ((std::string (iter->layout ()->cell_name (inst.object ().cell_index ())) + suffix).c_str ());
        m_cm_entry = m_cell_map.insert (std::make_pair (key, new_cell)).first;
      }

      db::CellInstArray new_inst (db::CellInst (m_cm_entry->second), trans);
      m_cell_stack.back ()->insert (new_inst);

    }

    return (m_cells_seen.find (key) == m_cells_seen.end ());

  }
}

void
HierarchyBuilder::shape (const RecursiveShapeIterator * /*iter*/, const db::Shape &shape, const db::ICplxTrans & /*trans*/, const db::Box &region, const box_tree_type *complex_region)
{
  db::Shapes &shapes = m_cell_stack.back ()->shapes (m_target_layer);
  mp_pipe->push (shape, region, complex_region, &shapes);
}

// ---------------------------------------------------------------------------------------------

ClippingHierarchyBuilderShapeReceiver::ClippingHierarchyBuilderShapeReceiver (HierarchyBuilderShapeReceiver *pipe)
  : mp_pipe (pipe ? pipe : &def_inserter)
{
  //  .. nothing yet ..
}

void
ClippingHierarchyBuilderShapeReceiver::push (const db::Shape &shape, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  static db::Box world = db::Box::world ();

  if (region == world || is_inside (shape.bbox (), region, complex_region)) {

    mp_pipe->push (shape, world, 0, target);

  } else if (! is_outside (shape.bbox (), region, complex_region)) {

    //  clip the shape if required
    if (shape.is_text () || shape.is_edge () || shape.is_edge_pair ()) {
      mp_pipe->push (shape, world, 0, target);
    } else if (shape.is_box ()) {
      insert_clipped (shape.box (), region, complex_region, target);
    } else if (shape.is_polygon () || shape.is_simple_polygon () || shape.is_path ()) {
      db::Polygon poly;
      shape.polygon (poly);
      insert_clipped (poly, region, complex_region, target);
    }

  }
}

void
ClippingHierarchyBuilderShapeReceiver::push (const db::Box &shape, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  static db::Box world = db::Box::world ();

  if (! complex_region) {
    db::Box r = shape & region;
    if (! r.empty()) {
      mp_pipe->push (r, world, 0, target);
    }
  } else {
    insert_clipped (shape, region, complex_region, target);
  }
}

void
ClippingHierarchyBuilderShapeReceiver::push (const db::Polygon &shape, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  static db::Box world = db::Box::world ();

  if (region == world || (shape.box ().inside (region) && ! complex_region)) {
    mp_pipe->push (shape, world, 0, target);
  } else {
    insert_clipped (shape, region, complex_region, target);
  }
}

bool
ClippingHierarchyBuilderShapeReceiver::is_inside (const db::Box &box, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region)
{
  if (region == db::Box::world ()) {
    return true;
  }

  if (box.inside (region)) {

    db::Box rect_box = region & box;

    if (complex_region) {

      //  TODO: this is not a real test for being inside a complex region
      for (db::RecursiveShapeReceiver::box_tree_type::overlapping_iterator cr = complex_region->begin_overlapping (rect_box, db::box_convert<db::Box> ()); ! cr.at_end (); ++cr) {
        if (rect_box.inside (*cr)) {
          return true;
        }
      }

    }

  }

  return false;
}

bool
ClippingHierarchyBuilderShapeReceiver::is_outside (const db::Box &box, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region)
{
  if (region == db::Box::world ()) {
    return false;
  }

  if (box.overlaps (region)) {

    db::Box rect_box = region & box;

    if (complex_region) {
      for (db::RecursiveShapeReceiver::box_tree_type::overlapping_iterator cr = complex_region->begin_overlapping (rect_box, db::box_convert<db::Box> ()); ! cr.at_end (); ++cr) {
        if (rect_box.overlaps (*cr)) {
          return false;
        }
      }
    } else {
      return false;
    }

  }

  return true;
}

void
ClippingHierarchyBuilderShapeReceiver::insert_clipped (const db::Box &box, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  db::Box bb = box & region;
  static db::Box world = db::Box::world ();

  if (complex_region) {
    for (db::RecursiveShapeReceiver::box_tree_type::overlapping_iterator cr = complex_region->begin_overlapping (bb, db::box_convert<db::Box> ()); ! cr.at_end (); ++cr) {
      mp_pipe->push (*cr & bb, world, 0, target);
    }
  } else {
    mp_pipe->push (bb, world, 0, target);
  }
}

void
ClippingHierarchyBuilderShapeReceiver::insert_clipped (const db::Polygon &poly, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  std::vector<db::Polygon> clipped_poly;
  static db::Box world = db::Box::world ();

  if (complex_region) {
    //  TODO: is this good way to clip a polygon at a complex boundary?
    for (db::RecursiveShapeReceiver::box_tree_type::overlapping_iterator cr = complex_region->begin_overlapping (region, db::box_convert<db::Box> ()); ! cr.at_end (); ++cr) {
      db::clip_poly (poly, *cr & region, clipped_poly);
    }
  } else {
    db::clip_poly (poly, region, clipped_poly);
  }

  for (std::vector<db::Polygon>::const_iterator p = clipped_poly.begin (); p != clipped_poly.end (); ++p) {
    mp_pipe->push (*p, world, 0, target);
  }
}

// ---------------------------------------------------------------------------------------------

ReducingHierarchyBuilderShapeReceiver::ReducingHierarchyBuilderShapeReceiver (HierarchyBuilderShapeReceiver *pipe, double area_ratio, size_t max_vertex_count)
  : mp_pipe (pipe ? pipe : &def_inserter), m_area_ratio (area_ratio), m_max_vertex_count (max_vertex_count)
{
  //  .. nothing yet ..
}

void
ReducingHierarchyBuilderShapeReceiver::push (const db::Shape &shape, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  if (shape.is_text () || shape.is_edge () || shape.is_edge_pair ()) {
    mp_pipe->push (shape, region, complex_region, target);
  } else if (shape.is_box ()) {
    mp_pipe->push (shape.box (), region, complex_region, target);
  } else if (shape.is_polygon () || shape.is_simple_polygon () || shape.is_path ()) {
    db::Polygon poly;
    shape.polygon (poly);
    reduce (poly, region, complex_region, target);
  }
}

void
ReducingHierarchyBuilderShapeReceiver::push (const db::Box &shape, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  mp_pipe->push (shape, region, complex_region, target);
}

void
ReducingHierarchyBuilderShapeReceiver::push (const db::Polygon &shape, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  reduce (shape, region, complex_region, target);
}

static double area_ratio (const db::Polygon &poly)
{
  return double (poly.box ().area ()) / double (poly.area ());
}

void
ReducingHierarchyBuilderShapeReceiver::reduce (const db::Polygon &poly, const db::Box &region, const db::RecursiveShapeReceiver::box_tree_type *complex_region, db::Shapes *target)
{
  size_t npoints = 0;
  for (unsigned int c = 0; c < poly.holes () + 1; ++c) {
    npoints += poly.contour (c).size ();
  }

  if (npoints > m_max_vertex_count || area_ratio (poly) > m_area_ratio) {

    std::vector <db::Polygon> split_polygons;
    db::split_polygon (poly, split_polygons);
    for (std::vector <db::Polygon>::const_iterator sp = split_polygons.begin (); sp != split_polygons.end (); ++sp) {
      reduce (*sp, region, complex_region, target);
    }

  } else {
    mp_pipe->push (poly, region, complex_region, target);
  }
}

// ---------------------------------------------------------------------------------------------

PolygonReferenceHierarchyBuilderShapeReceiver::PolygonReferenceHierarchyBuilderShapeReceiver (db::Layout *layout)
  : mp_layout (layout)
{
  //  nothing yet ..
}

void PolygonReferenceHierarchyBuilderShapeReceiver::push (const db::Shape &shape, const db::Box &, const db::RecursiveShapeReceiver::box_tree_type *, db::Shapes *target)
{
  if (shape.is_box () || shape.is_polygon () || shape.is_simple_polygon () || shape.is_path ()) {
    db::Polygon poly;
    shape.polygon (poly);
    target->insert (db::PolygonRef (poly, mp_layout->shape_repository ()));
  }
}

void PolygonReferenceHierarchyBuilderShapeReceiver::push (const db::Box &shape, const db::Box &, const db::RecursiveShapeReceiver::box_tree_type *, db::Shapes *target)
{
  target->insert (db::PolygonRef (db::Polygon (shape), mp_layout->shape_repository ()));
}

void PolygonReferenceHierarchyBuilderShapeReceiver::push (const db::Polygon &shape, const db::Box &, const db::RecursiveShapeReceiver::box_tree_type *, db::Shapes *target)
{
  target->insert (db::PolygonRef (shape, mp_layout->shape_repository ()));
}

}
