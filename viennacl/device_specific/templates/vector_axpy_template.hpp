#ifndef VIENNACL_DEVICE_SPECIFIC_TEMPLATES_VECTOR_AXPY_HPP
#define VIENNACL_DEVICE_SPECIFIC_TEMPLATES_VECTOR_AXPY_HPP

/* =========================================================================
   Copyright (c) 2010-2013, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
   Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                  ViennaCL - The Vienna Computing Library
                            -----------------

   Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

   (A list of authors and contributors can be found in the PDF manual)

   License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */


/** @file viennacl/generator/vector_axpy.hpp
 *
 * Kernel template for the vector axpy-like operations
*/

#include <vector>
#include <cmath>

#include "viennacl/scheduler/forwards.h"

#include "viennacl/device_specific/mapped_objects.hpp"
#include "viennacl/device_specific/tree_parsing.hpp"
#include "viennacl/device_specific/forwards.h"
#include "viennacl/device_specific/utils.hpp"

#include "viennacl/device_specific/templates/template_base.hpp"
#include "viennacl/device_specific/templates/utils.hpp"

#include "viennacl/tools/tools.hpp"

namespace viennacl
{

  namespace device_specific
  {

    class vector_axpy_template : public template_base
    {

    public:
      class parameters_type : public template_base::parameters_type
      {
      public:
        parameters_type(unsigned int _simd_width,
                   unsigned int _group_size, unsigned int _num_groups,
                   fetching_policy_type _fetching_policy) : template_base::parameters_type(_simd_width, _group_size, 1, 1), num_groups(_num_groups), fetching_policy(_fetching_policy){ }



        unsigned int num_groups;
        fetching_policy_type fetching_policy;
      };

    private:
      virtual int check_invalid_impl(viennacl::ocl::device const & /*dev*/) const
      {
          if(p_.fetching_policy==FETCH_FROM_LOCAL)
            return TEMPLATE_INVALID_FETCHING_POLICY_TYPE;
          return TEMPLATE_VALID;
      }

      std::vector<std::string> generate_impl(std::string const & kernel_prefix, statements_container const & statements, std::vector<mapping_type> const & mappings) const
      {
        std::vector<std::string> result;
        for(unsigned int i = 0 ; i < 2 ; ++i)
        {
          utils::kernel_generation_stream stream;
          unsigned int simd_width = (i==0)?1:p_.simd_width;
          std::string suffix = (i==0)?"_strided":"";
          stream << " __attribute__((reqd_work_group_size(" << p_.local_size_0 << ",1,1)))" << std::endl;
          generate_prototype(stream, kernel_prefix + suffix, "unsigned int N,", mappings, statements);
          stream << "{" << std::endl;
          stream.inc_tab();

          tree_parsing::process(stream, PARENT_NODE_TYPE, "scalar", "#scalartype #namereg = *#pointer;", statements, mappings);
          tree_parsing::process(stream, PARENT_NODE_TYPE, "matrix", "#pointer += $OFFSET{#start1, #start2};", statements, mappings);
          tree_parsing::process(stream, PARENT_NODE_TYPE, "vector", "#pointer += #start;", statements, mappings);

          struct loop_body : public loop_body_base
          {
            loop_body(statements_container const & _statements, std::vector<mapping_type> const & _mappings) : statements(_statements), mappings(_mappings) { }

            void operator()(utils::kernel_generation_stream & stream, unsigned int simd_width) const
            {
              std::string process_str;
              std::string i = (simd_width==1)?"i*#stride":"i";

              process_str = utils::append_width("#scalartype",simd_width) + " #namereg = " + utils::append_width("vload" , simd_width) + "(" + i + ", #pointer);";
              tree_parsing::process(stream, PARENT_NODE_TYPE, "vector", process_str, statements, mappings);
              tree_parsing::process(stream, PARENT_NODE_TYPE, "matrix_row", "#scalartype #namereg = vload($OFFSET{#row*#stride1, i*#stride2}, #pointer);", statements, mappings);
              tree_parsing::process(stream, PARENT_NODE_TYPE, "matrix_column", "#scalartype #namereg = vload($OFFSET{i*#stride1,#column*#stride2}, #pointer);", statements, mappings);
              tree_parsing::process(stream, PARENT_NODE_TYPE, "matrix_diag", "#scalartype #namereg = vload(#diag_offset<0?$OFFSET{(i - #diag_offset)*#stride1, i*#stride2}:$OFFSET{i*#stride1, (i + #diag_offset)*#stride2}, #pointer);", statements, mappings);

              std::map<std::string, std::string> accessors;
              accessors["vector"] = "#namereg";
              accessors["matrix_row"] = "#namereg";
              accessors["matrix_column"] = "#namereg";
              accessors["matrix_diag"] = "#namereg";
              accessors["scalar"] = "#namereg";
              tree_parsing::evaluate(stream, PARENT_NODE_TYPE, accessors, statements, mappings);

              process_str = utils::append_width("vstore", simd_width) +"(#namereg, " + i + ", #pointer);";
              tree_parsing::process(stream, LHS_NODE_TYPE, "vector", process_str, statements, mappings);
              tree_parsing::process(stream, LHS_NODE_TYPE, "matrix_row", "vstore(#namereg, $OFFSET{#row, i}, #pointer);", statements, mappings);
              tree_parsing::process(stream, LHS_NODE_TYPE, "matrix_column", "vstore(#namereg, $OFFSET{i, #column}, #pointer);", statements, mappings);
              tree_parsing::process(stream, LHS_NODE_TYPE, "matrix_diag", "vstore(#namereg, #diag_offset<0?$OFFSET{i - #diag_offset, i}:$OFFSET{i, i + #diag_offset}, #pointer);", statements, mappings);

            }

          private:
            statements_container const & statements;
            std::vector<mapping_type> const & mappings;
          };

          element_wise_loop_1D(stream, loop_body(statements, mappings), p_.fetching_policy, simd_width, "i", "N", "get_global_id(0)", "get_global_size(0)");

          stream.dec_tab();
          stream << "}" << std::endl;
          result.push_back(stream.str());
        }
        return result;
      }

    public:
      vector_axpy_template(vector_axpy_template::parameters_type const & parameters, binding_policy_t binding_policy = BIND_ALL_UNIQUE) : template_base(p_, binding_policy), up_to_internal_size_(false), p_(parameters){ }

      void up_to_internal_size(bool v) { up_to_internal_size_ = v; }
      vector_axpy_template::parameters_type const & parameters() const { return p_; }

      void enqueue(std::string const & kernel_prefix, std::vector<lazy_program_compiler> & programs,  statements_container const & statements)
      {
        viennacl::ocl::kernel * kernel;
        if(has_strided_access(statements) && p_.simd_width > 1)
          kernel = &programs[0].program().get_kernel(kernel_prefix+"_strided");
        else
          kernel = &programs[1].program().get_kernel(kernel_prefix);

        kernel->local_work_size(0, p_.local_size_0);
        kernel->global_work_size(0, p_.local_size_0*p_.num_groups);
        unsigned int current_arg = 0;
        scheduler::statement const & statement = statements.data().front();
        cl_uint size = static_cast<cl_uint>(vector_size(lhs_most(statement.array(), statement.root()), up_to_internal_size_));
        kernel->arg(current_arg++, size);
        set_arguments(statements, *kernel, current_arg);
        viennacl::ocl::enqueue(*kernel);
      }

    private:
      bool up_to_internal_size_;
      vector_axpy_template::parameters_type p_;
    };

  }

}

#endif
