// BeginLicense:
// Part of: xcsg - XML based Constructive Solid Geometry
// Copyright (C) 2017-2020 Carsten Arnholm
// All rights reserved
//
// This file may be used under the terms of either the GNU General
// Public License version 2 or 3 (at your option) as published by the
// Free Software Foundation and appearing in the files LICENSE.GPL2
// and LICENSE.GPL3 included in the packaging of this file.
//
// This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
// INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE. ALL COPIES OF THIS FILE MUST INCLUDE THIS LICENSE.
// EndLicense:

#include "xcsg_main.h"

#include <boost/date_time.hpp>

#include <sstream>
#include <stdexcept>
using namespace std;
#include "csg_parser/cf_xmlTree.h"

#include "xshape2d.h"

#include "clipper_boolean.h"
#include "carve_boolean.h"
#include "carve_triangulate.h"
#include "mesh_utils.h"
#include "xpolyhedron.h"
#include "xcsg_factory.h"
#include "boolean_timer.h"

#include "openscad_csg.h"
#include "out_triangles.h"
#include "amf_file.h"
#include "dxf_file.h"
#include "svg_file.h"

#include "std_filename.h"

#include "csg_parser/csg_parser.h"

static std::string DisplayName(const std_filename& fname, bool show_path)
{
   return ((show_path)? fname.GetFullPath() : fname.GetFullName());
}

xcsg_main::xcsg_main(const boost_command_line& cmd)
: m_cmd(cmd)
{}

xcsg_main::~xcsg_main()
{}

bool xcsg_main::run()
{
   if(!m_cmd.parsed_ok())return false;
   if(m_cmd.count("xcsg-file")==0) {
      cout << endl << "Error, missing required input parameter <xcsg-file>" << endl;
      return false;
   }

   std::string xcsg_file;
   try {
      xcsg_file = m_cmd.get<std::string>("xcsg-file");
      std::replace(xcsg_file.begin(),xcsg_file.end(), '\\', '/');
   }
   catch(std::exception& ex) {
      ostringstream sout;
      sout << "xcsg  command line processing error: " << ex.what() << ", please report. "<< endl;
      throw std::logic_error(sout.str());
   }

   if(!std_filename::Exists(xcsg_file)) throw std::runtime_error("File does not exist: " + xcsg_file);

   // determine if we shall display full file paths
   bool show_path = m_cmd.count("fullpath")>0;

   cf_xmlTree tree;
   std_filename file(xcsg_file);

   if(file.GetExt() == ".csg") {

      cout << "Converting from OpenSCAD " << xcsg_file << endl;
      std::ifstream csg(xcsg_file);
      csg_parser parser(csg);
      parser.to_xcsg(tree);

      file.SetExt("xcsg");
      xcsg_file = file.GetFullPath();
      tree.write_xml(xcsg_file);
   }

   if(tree.read_xml(xcsg_file)) {

      cout << "xcsg processing: " << DisplayName(file,show_path) << endl;

      cf_xmlNode root;
      if(tree.get_root(root)) {
         if("xcsg" == root.tag()) {

            // set the global secant tolerance,
            mesh_utils::set_secant_tolerance(root.get_property("secant_tolerance",mesh_utils::secant_tolerance()));

            size_t icount = 0;
            for(auto i=root.begin(); i!=root.end(); i++) {
               cf_xmlNode child(i);
               if(!child.is_attribute_node()) {
                  if(xcsg_factory::singleton().is_solid(child)) {
                     run_xsolid(child,xcsg_file);
                     icount++;
                  }
                  else if(xcsg_factory::singleton().is_shape2d(child)) {
                     run_xshape2d(child,xcsg_file);
                     icount++;
                  }
               }
               if(icount > 0)break;
            }
         }
      }
   }
   else {
      cout << "error: xcsg input file not found: " << xcsg_file << endl;
   }
   return true;
}


bool xcsg_main::run_xsolid(cf_xmlNode& node,const std::string& xcsg_file)
{
   cout << "processing solid: " << node.tag() << endl;
   std::shared_ptr<xsolid> obj = xcsg_factory::singleton().make_solid(node);
   if(obj.get()) {

      // determine if we shall display full file paths
      bool show_path = m_cmd.count("fullpath")>0;

      size_t nbool = obj->nbool();
      cout << "...completed CSG tree: " <<  nbool << " boolean operations to process." << endl;
      if(nbool > m_cmd.max_bool()) {
         ostringstream sout;
         sout << "Max " << m_cmd.max_bool() << " boolean operations allowed in this configuration.";
         throw std::logic_error(sout.str());
      }


      if(nbool > 0) {
         cout << "...starting boolean operations" << endl;
      }

      boost::posix_time::ptime time_0 = boost::posix_time::microsec_clock::universal_time();
      carve_boolean csg;
      try {

         boolean_timer::singleton().init(static_cast<int>(nbool));
         csg.compute(obj->create_carve_mesh(),carve::csg::CSG::OP::UNION);
         boost::posix_time::time_duration  ptime_diff = boost::posix_time::microsec_clock::universal_time() - time_0;
         double elapsed_sec = 0.001*ptime_diff.total_milliseconds();

         cout << "...completed boolean operations in " << setprecision(5) << elapsed_sec << " [sec] " << endl;
      }
      catch(carve::exception& ex ) {

         // rethrow as std::exception
         string msg("(carve error): ");
         msg += ex.str();
         cout << "WARNING: " << msg << endl;
//         throw std::exception(msg.c_str());
      }

      size_t nmani = csg.size();
      cout << "...result model contains " << nmani << ((nmani==1)? " lump.": " lumps.") << endl;

      // we export only triangles
       boost::posix_time::ptime time_1 = boost::posix_time::microsec_clock::universal_time();
      carve_triangulate triangulate;
      for(size_t imani=0; imani<nmani; imani++) {

         // create & check lump
         std::shared_ptr<xpolyhedron> poly = csg.create_manifold(imani);
         cout << "...lump " << imani+1 << ": " <<poly->v_size() << " vertices, " << poly->f_size() << " polygon faces." << endl;

         size_t num_non_tri = 0;
         poly->check_polyhedron(cout,num_non_tri);

         if(num_non_tri > 0) {
            cout << "...Triangulating lump ... " << std::endl;
            cout << "...Triangulation completed with " << triangulate.compute2d(poly->create_carve_polyhedron())<< " triangle faces ";

            boost::posix_time::ptime time_2 = boost::posix_time::microsec_clock::universal_time();
            double elapsed_2 = 0.001*(time_2 - time_1).total_milliseconds();
            cout << "in " << elapsed_2 << " [sec]" << endl;

         }
         else {
            // triangulation not required
            triangulate.add(poly->create_carve_polyhedron());
         }
      }
      cout <<    "...Exporting results " << endl;

      // create object for file export
      out_triangles exporter(triangulate.carve_polyset());

      amf_file amf;
      if(m_cmd.count("csg")>0)       cout << "Created OpenSCAD file: " << DisplayName(std_filename(exporter.write_csg(xcsg_file)),show_path) << endl;
      if(m_cmd.count("amf")>0)       cout << "Created AMF file     : " << DisplayName(std_filename(amf.write(triangulate.carve_polyset(),xcsg_file)),show_path) << endl;
      if(m_cmd.count("obj")>0)       cout << "Created OBJ file     : " << DisplayName(std_filename(exporter.write_obj(xcsg_file)),show_path) << endl;
      if(m_cmd.count("off")>0)       cout << "Created OFF file(s)  : " << DisplayName(std_filename(exporter.write_off(xcsg_file)),show_path) << endl;
      // write STL last so it is the most recent updated format
      if(m_cmd.count("stl")>0)       cout << "Created STL file     : " << DisplayName(std_filename(exporter.write_stl(xcsg_file,true)),show_path) << endl;
      else if(m_cmd.count("astl")>0) cout << "Created STL file     : " << DisplayName(std_filename(exporter.write_stl(xcsg_file,false)),show_path) << endl;

   }
   else {
      throw logic_error("xcsg tree contains no data. ");
   }
   return true;
}


bool xcsg_main::run_xshape2d(cf_xmlNode& node,const std::string& xcsg_file)
{
   cout << "processing shape2d: " << node.tag() << endl;
   std::shared_ptr<xshape2d> obj = xcsg_factory::singleton().make_shape2d(node);
   if(obj.get()) {

      // determine if we shall display full file paths
      bool show_path = m_cmd.count("fullpath")>0;

      size_t nbool = obj->nbool();
      cout << "...completed CSG tree: " <<  nbool << " boolean operations to process." << endl;
      if(nbool > m_cmd.max_bool()) {
         ostringstream sout;
         sout << "Max " << m_cmd.max_bool() << " boolean operations allowed in this configuration.";
         throw std::logic_error(sout.str());
      }

      if(nbool > 0) {
         cout << "...starting boolean operations" << endl;
      }
      clipper_boolean csg;
      csg.compute(obj->create_clipper_profile(),ClipperLib::ctUnion);

      std::shared_ptr<polyset2d> polyset = csg.profile()->polyset();
      size_t nmani = polyset->size();
      cout << "...result model contains " << nmani << ((nmani==1)? " lump.": " lumps.") << endl;

      if(m_cmd.count("csg")>0) {
         openscad_csg openscad(xcsg_file);
         size_t imani = 0;
         for(auto i=polyset->begin(); i!=polyset->end(); i++) {
            std::shared_ptr<polygon2d> poly = *i;
            openscad.write_polygon(poly);
         }
         cout << "Created OpenSCAD file: " << DisplayName(std_filename(openscad.path()),show_path) << endl;
      }

      // write SVG?
      if(m_cmd.count("svg")>0) {
         svg_file svg;
         cout << "Created SVG      file: " << DisplayName(std_filename(svg.write(polyset,xcsg_file)),show_path) << endl;
      }

      // write DXF last so it is the most recent updated format
      if(m_cmd.count("dxf")>0) {
         dxf_file dxf;
         cout << "Created DXF      file: " << DisplayName(std_filename(dxf.write(polyset,xcsg_file)),show_path) << endl;
      }

   }
   else {
      throw logic_error("xcsg tree contains no data. ");
   }
   return true;

}
