
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

#include "dbNetlistDeviceExtractor.h"
#include "dbNetlistProperty.h"
#include "dbRegion.h"
#include "dbHierNetworkProcessor.h"
#include "dbDeepRegion.h"

namespace db
{

// ----------------------------------------------------------------------------------------
//  NetlistDeviceExtractorError implementation

NetlistDeviceExtractorError::NetlistDeviceExtractorError ()
{
  //  .. nothing yet ..
}

NetlistDeviceExtractorError::NetlistDeviceExtractorError (const std::string &cell_name, const std::string &msg)
  : m_cell_name (cell_name), m_message (msg)
{
  //  .. nothing yet ..
}

// ----------------------------------------------------------------------------------------
//  NetlistDeviceExtractor implementation

NetlistDeviceExtractor::NetlistDeviceExtractor (const std::string &name)
  : mp_layout (0), m_cell_index (0), mp_circuit (0)
{
  m_name = name;
  m_propname_id = 0;
}

NetlistDeviceExtractor::~NetlistDeviceExtractor ()
{
  //  .. nothing yet ..
}

const tl::Variant &NetlistDeviceExtractor::terminal_property_name ()
{
  static tl::Variant name ("TERMINAL");
  return name;
}

void NetlistDeviceExtractor::initialize (db::Netlist *nl)
{
  m_layer_definitions.clear ();
  mp_device_class = 0;
  m_propname_id = 0;
  m_netlist.reset (nl);

  setup ();
}

static void insert_into_region (const db::PolygonRef &s, const db::ICplxTrans &tr, db::Region &region)
{
  region.insert (s.obj ().transformed (tr * db::ICplxTrans (s.trans ())));
}

void NetlistDeviceExtractor::extract (db::DeepShapeStore &dss, const NetlistDeviceExtractor::input_layers &layer_map, db::Netlist *nl)
{
  initialize (nl);

  std::vector<unsigned int> layers;
  layers.reserve (m_layer_definitions.size ());

  for (layer_definitions::const_iterator ld = begin_layer_definitions (); ld != end_layer_definitions (); ++ld) {

    input_layers::const_iterator l = layer_map.find (ld->name);
    if (l == layer_map.end ()) {
      throw tl::Exception (tl::to_string (tr ("Missing input layer for device extraction: ")) + ld->name);
    }

    tl_assert (l->second != 0);
    db::DeepRegion *dr = dynamic_cast<db::DeepRegion *> (l->second->delegate ());
    if (dr == 0) {
      throw tl::Exception (tl::sprintf (tl::to_string (tr ("Invalid region passed to input layer '%s' for device extraction: must be of deep region kind")), ld->name));
    }

    if (&dr->deep_layer ().layout () != &dss.layout () || &dr->deep_layer ().initial_cell () != &dss.initial_cell ()) {
      throw tl::Exception (tl::sprintf (tl::to_string (tr ("Invalid region passed to input layer '%s' for device extraction: not originating from the same source")), ld->name));
    }

    layers.push_back (dr->deep_layer ().layer ());

  }

  extract_without_initialize (dss.layout (), dss.initial_cell (), layers);
}

void NetlistDeviceExtractor::extract (db::Layout &layout, db::Cell &cell, const std::vector<unsigned int> &layers, db::Netlist *nl)
{
  initialize (nl);
  extract_without_initialize (layout, cell, layers);
}

void NetlistDeviceExtractor::extract_without_initialize (db::Layout &layout, db::Cell &cell, const std::vector<unsigned int> &layers)
{
  tl_assert (layers.size () == m_layer_definitions.size ());

  typedef db::PolygonRef shape_type;
  db::ShapeIterator::flags_type shape_iter_flags = db::ShapeIterator::Polygons;

  mp_layout = &layout;
  m_layers = layers;

  //  terminal properties are kept in a property with the terminal_property_name name
  m_propname_id = mp_layout->properties_repository ().prop_name_id (terminal_property_name ());

  tl_assert (m_netlist.get () != 0);

  //  build a cell-id-to-circuit lookup table
  std::map<db::cell_index_type, db::Circuit *> circuits_by_cell;
  for (db::Netlist::circuit_iterator c = m_netlist->begin_circuits (); c != m_netlist->end_circuits (); ++c) {
    circuits_by_cell.insert (std::make_pair (c->cell_index (), c.operator-> ()));
  }

  //  collect the cells below the top cell
  std::set<db::cell_index_type> called_cells;
  called_cells.insert (cell.cell_index ());
  cell.collect_called_cells (called_cells);

  //  build the device clusters
  db::Connectivity device_conn = get_connectivity (layout, layers);
  db::hier_clusters<shape_type> device_clusters;
  device_clusters.build (layout, cell, shape_iter_flags, device_conn);

  //  for each cell investigate the clusters
  for (std::set<db::cell_index_type>::const_iterator ci = called_cells.begin (); ci != called_cells.end (); ++ci) {

    m_cell_index = *ci;

    std::map<db::cell_index_type, db::Circuit *>::const_iterator c2c = circuits_by_cell.find (*ci);
    if (c2c != circuits_by_cell.end ()) {

      //  reuse existing circuit
      mp_circuit = c2c->second;

    } else {

      //  create a new circuit for this cell
      mp_circuit = new db::Circuit ();
      mp_circuit->set_cell_index (*ci);
      mp_circuit->set_name (layout.cell_name (*ci));
      m_netlist->add_circuit (mp_circuit);

    }

    //  investigate each cluster
    db::connected_clusters<shape_type> cc = device_clusters.clusters_per_cell (*ci);
    for (db::connected_clusters<shape_type>::all_iterator c = cc.begin_all (); !c.at_end(); ++c) {

      //  take only root clusters - others have upward connections and are not "whole"
      if (! cc.is_root (*c)) {
        continue;
      }

      //  build layer geometry from the cluster found

      std::vector<db::Region> layer_geometry;
      layer_geometry.resize (layers.size ());

      for (std::vector<unsigned int>::const_iterator l = layers.begin (); l != layers.end (); ++l) {
        db::Region &r = layer_geometry [l - layers.begin ()];
        for (db::recursive_cluster_shape_iterator<shape_type> si (device_clusters, *l, *ci, *c); ! si.at_end(); ++si) {
          insert_into_region (*si, si.trans (), r);
        }
      }

      //  do the actual device extraction
      extract_devices (layer_geometry);

    }

  }
}

void NetlistDeviceExtractor::setup ()
{
  //  .. the default implementation does nothing ..
}

db::Connectivity NetlistDeviceExtractor::get_connectivity (const db::Layout & /*layout*/, const std::vector<unsigned int> & /*layers*/) const
{
  //  .. the default implementation does nothing ..
  return db::Connectivity ();
}

void NetlistDeviceExtractor::extract_devices (const std::vector<db::Region> & /*layer_geometry*/)
{
  //  .. the default implementation does nothing ..
}

void NetlistDeviceExtractor::register_device_class (DeviceClass *device_class)
{
  if (mp_device_class != 0) {
    throw tl::Exception (tl::to_string (tr ("Device class already set")));
  }

  tl_assert (device_class != 0);
  mp_device_class = device_class;
  mp_device_class->set_name (m_name);

  tl_assert (m_netlist.get () != 0);
  m_netlist->add_device_class (device_class);
}

void NetlistDeviceExtractor::define_layer (const std::string &name, const std::string &description)
{
  m_layer_definitions.push_back (db::NetlistDeviceExtractorLayerDefinition (name, description, m_layer_definitions.size ()));
}

Device *NetlistDeviceExtractor::create_device ()
{
  if (mp_device_class == 0) {
    throw tl::Exception (tl::to_string (tr ("No device class registered")));
  }

  tl_assert (mp_circuit != 0);
  Device *device = new Device (mp_device_class);
  mp_circuit->add_device (device);
  return device;
}

void NetlistDeviceExtractor::define_terminal (Device *device, size_t terminal_id, size_t geometry_index, const db::Polygon &polygon)
{
  tl_assert (mp_layout != 0);
  tl_assert (geometry_index < m_layers.size ());
  unsigned int layer_index = m_layers [geometry_index];

  //  Build a property set for the DeviceTerminalProperty
  db::PropertiesRepository::properties_set ps;
  tl::Variant &v = ps.insert (std::make_pair (m_propname_id, tl::Variant ()))->second;
  v = tl::Variant (new db::DeviceTerminalProperty (device->id (), terminal_id), db::NetlistProperty::variant_class (), true);
  db::properties_id_type pi = mp_layout->properties_repository ().properties_id (ps);

  db::PolygonRef pr (polygon, mp_layout->shape_repository ());
  mp_layout->cell (m_cell_index).shapes (layer_index).insert (db::PolygonRefWithProperties (pr, pi));
}

void NetlistDeviceExtractor::define_terminal (Device *device, size_t terminal_id, size_t layer_index, const db::Box &box)
{
  define_terminal (device, terminal_id, layer_index, db::Polygon (box));
}

void NetlistDeviceExtractor::define_terminal (Device *device, size_t terminal_id, size_t layer_index, const db::Point &point)
{
  //  NOTE: we add one DBU to the "point" to prevent it from vanishing
  db::Vector dv (1, 1);
  define_terminal (device, terminal_id, layer_index, db::Polygon (db::Box (point - dv, point + dv)));
}

std::string NetlistDeviceExtractor::cell_name () const
{
  if (layout ()) {
    return layout ()->cell_name (cell_index ());
  } else {
    return std::string ();
  }
}

void NetlistDeviceExtractor::error (const std::string &msg)
{
  m_errors.push_back (db::NetlistDeviceExtractorError (cell_name (), msg));
}

void NetlistDeviceExtractor::error (const std::string &msg, const db::Polygon &poly)
{
  error (msg);
  m_errors.back ().set_geometry (db::Region (poly));
}

void NetlistDeviceExtractor::error (const std::string &msg, const db::Region &region)
{
  error (msg);
  m_errors.back ().set_geometry (region);
}

void NetlistDeviceExtractor::error (const std::string &category_name, const std::string &category_description, const std::string &msg)
{
  error (msg);
  m_errors.back ().set_category_name (category_name);
  m_errors.back ().set_category_description (category_description);
}

void NetlistDeviceExtractor::error (const std::string &category_name, const std::string &category_description, const std::string &msg, const db::Polygon &poly)
{
  error (category_name, category_description, msg);
  m_errors.back ().set_geometry (db::Region (poly));
}

void NetlistDeviceExtractor::error (const std::string &category_name, const std::string &category_description, const std::string &msg, const db::Region &region)
{
  error (category_name, category_description, msg);
  m_errors.back ().set_geometry (region);
}

}