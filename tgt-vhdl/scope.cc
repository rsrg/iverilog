/*
 *  VHDL code generation for scopes.
 *
 *  Copyright (C) 2008  Nick Gasson (nick@nickg.me.uk)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "vhdl_target.h"
#include "vhdl_element.hh"
#include "state.hh"

#include <iostream>
#include <sstream>
#include <cassert>
#include <cstring>

static string make_safe_name(ivl_signal_t sig);


/*
 * This represents the portion of a nexus that is visible within
 * a VHDL scope. If that nexus portion does not contain a signal,
 * then `tmpname' gives the name of the temporary that will be
 * used when this nexus is used in `scope' (e.g. for LPMs that
 * appear in instantiations). The list `connect' lists all the
 * signals that should be joined together to re-create the net.
 */
struct scope_nexus_t {
   vhdl_scope *scope;
   ivl_signal_t sig;            // A real signal
   unsigned pin;                // The pin this signal is connected to
   string tmpname;              // A new temporary signal
   list<ivl_signal_t> connect;  // Other signals to wire together
};
   
/*
 * This structure is stored in the private part of each nexus.
 * It stores a scope_nexus_t for each VHDL scope which is
 * connected to that nexus. It's stored as a list so we can use
 * contained_within to allow several nested scopes to reference
 * the same signal.
 */
struct nexus_private_t {
   list<scope_nexus_t> signals;
   vhdl_expr *const_driver;
};

/*
 * Returns the scope_nexus_t of this nexus visible within scope.
 */
static scope_nexus_t *visible_nexus(nexus_private_t *priv, vhdl_scope *scope)
{
   list<scope_nexus_t>::iterator it;
   for (it = priv->signals.begin(); it != priv->signals.end(); ++it) {
      if (scope->contained_within((*it).scope))
         return &*it;
   }
   return NULL;
}

/*
 * Remember that a signal in `scope' is part of this nexus. The
 * first signal passed to this function for a scope will be used
 * as the canonical representation of this nexus when we need to
 * convert it to a variable reference (e.g. in a LPM input/output).
 */
static void link_scope_to_nexus_signal(nexus_private_t *priv, vhdl_scope *scope,
                                       ivl_signal_t sig, unsigned pin)
{   
   scope_nexus_t *sn;
   if ((sn = visible_nexus(priv, scope))) {
      assert(sn->tmpname == "");

      // Remember to connect this signal up later
      // If one of the signals is a input, make sure the input is not being driven
      if (ivl_signal_port(sn->sig) == IVL_SIP_INPUT) {
         sn->connect.push_back(sn->sig);
         sn->sig = sig;
      }
      else
         sn->connect.push_back(sig);
   }
   else {
      scope_nexus_t new_sn = { scope, sig, pin, "" };
      priv->signals.push_back(new_sn);
   }
}

/*
 * Make a temporary the representative of this nexus in scope.
 */
static void link_scope_to_nexus_tmp(nexus_private_t *priv, vhdl_scope *scope,
                                    const string &name)
{
   scope_nexus_t new_sn = { scope, NULL, 0, name };
   priv->signals.push_back(new_sn);
}

/*
 * Finds the name of the nexus signal within this scope.
 */
static string visible_nexus_signal_name(nexus_private_t *priv, vhdl_scope *scope,
                                        unsigned *pin)
{
   scope_nexus_t *sn = visible_nexus(priv, scope);
   assert(sn);

   *pin = sn->pin;
   return sn->sig ? get_renamed_signal(sn->sig) : sn->tmpname;
}

/*
 * Calculate the signal type of a nexus. This is modified from
 * draw_net_input in tgt-vvp. This also returns the width of
 * the signal(s) connected to the nexus.
 */
static ivl_signal_type_t signal_type_of_nexus(ivl_nexus_t nex, int &width)
{
   ivl_signal_type_t out = IVL_SIT_TRI;
   width = 0;
   
   for (unsigned idx = 0; idx < ivl_nexus_ptrs(nex); idx += 1) {
	    ivl_signal_type_t stype;
	    ivl_nexus_ptr_t ptr = ivl_nexus_ptr(nex, idx);
	    ivl_signal_t sig = ivl_nexus_ptr_sig(ptr);
	    if (sig == 0)
         continue;
      
      width = ivl_signal_width(sig);
      
	    stype = ivl_signal_type(sig);
	    if (stype == IVL_SIT_TRI)
         continue;
	    if (stype == IVL_SIT_NONE)
         continue;
	    out = stype;
   }
   
   return out;
}

/*
 * Generates VHDL code to fully represent a nexus.
 */
void draw_nexus(ivl_nexus_t nexus)
{   
   nexus_private_t *priv = new nexus_private_t;
   int nexus_signal_width = -1;
   priv->const_driver = NULL;
   
   int nptrs = ivl_nexus_ptrs(nexus);

   // Number of drivers for this nexus
   int ndrivers = 0;

   // First pass through connect all the signals up
   for (int i = 0; i < nptrs; i++) {
      ivl_nexus_ptr_t nexus_ptr = ivl_nexus_ptr(nexus, i);
      
      ivl_signal_t sig;
      if ((sig = ivl_nexus_ptr_sig(nexus_ptr))) { 
         vhdl_scope *scope = find_scope_for_signal(sig);
         if (scope) {
            unsigned pin = ivl_nexus_ptr_pin(nexus_ptr);
            link_scope_to_nexus_signal(priv, scope, sig, pin);
         }

         nexus_signal_width = ivl_signal_width(sig);
      }
   }

   // Second pass through make sure logic/LPMs have signal
   // inputs and outputs
   for (int i = 0; i < nptrs; i++) {
      ivl_nexus_ptr_t nexus_ptr = ivl_nexus_ptr(nexus, i);
      
      ivl_net_logic_t log;
      ivl_lpm_t lpm;
      ivl_net_const_t con;
      if ((log = ivl_nexus_ptr_log(nexus_ptr))) {
         ivl_scope_t log_scope = ivl_logic_scope(log);
         if (!is_default_scope_instance(log_scope))
            continue;
         
         vhdl_entity *ent = find_entity(log_scope);
         assert(ent);
         
         vhdl_scope *vhdl_scope = ent->get_arch()->get_scope();
         if (visible_nexus(priv, vhdl_scope)) {
            // Already seen this signal in vhdl_scope
         }
         else {
            // Create a temporary signal to connect it to the nexus
            vhdl_type *type =
               vhdl_type::type_for(ivl_logic_width(log), false);
            
            ostringstream ss;
            ss << "LO" << ivl_logic_basename(log);
            vhdl_scope->add_decl(new vhdl_signal_decl(ss.str().c_str(), type));

            link_scope_to_nexus_tmp(priv, vhdl_scope, ss.str());
         }

         // If this is connected to pin-0 then this nexus is driven
         // by this logic gate
         if (ivl_logic_pin(log, 0) == nexus)
            ndrivers++;
      }
      else if ((lpm = ivl_nexus_ptr_lpm(nexus_ptr))) {
         ivl_scope_t lpm_scope = ivl_lpm_scope(lpm);
         vhdl_entity *ent = find_entity(lpm_scope);
         assert(ent);
         
         vhdl_scope *vhdl_scope = ent->get_arch()->get_scope();
         if (visible_nexus(priv, vhdl_scope)) {
            // Already seen this signal in vhdl_scope
         }
         else {
            // Create a temporary signal to connect the nexus
            // TODO: we could avoid this for IVL_LPM_PART_PV

            // If we already know how wide the temporary should be
            // (i.e. because we've seen a signal it's connected to)
            // then use that, otherwise use the width of the LPM
            int lpm_temp_width;
            if (nexus_signal_width != -1)
               lpm_temp_width = nexus_signal_width;
            else
               lpm_temp_width = ivl_lpm_width(lpm);
            
            vhdl_type *type = vhdl_type::type_for(lpm_temp_width,
                                                  ivl_lpm_signed(lpm) != 0);
            ostringstream ss;
            ss << "LPM" << ivl_lpm_basename(lpm);
            vhdl_scope->add_decl(new vhdl_signal_decl(ss.str().c_str(), type));
            
            link_scope_to_nexus_tmp(priv, vhdl_scope, ss.str());
         }

         // If this is connected to the LPM output then this nexus
         // is driven by the LPM
         if (ivl_lpm_q(lpm, 0) == nexus)
            ndrivers++;
      }
      else if ((con = ivl_nexus_ptr_con(nexus_ptr))) {
         if (ivl_const_type(con) == IVL_VT_REAL) {
            error("No VHDL translation for real constant (%g)",
                  ivl_const_real(con));
            continue;
         }
         if (ivl_const_width(con) == 1)
            priv->const_driver = new vhdl_const_bit(ivl_const_bits(con)[0]);
         else
            priv->const_driver =
               new vhdl_const_bits(ivl_const_bits(con), ivl_const_width(con),
                                   ivl_const_signed(con) != 0);

         // A constant is a sort of driver
         ndrivers++;
      }
   }

   // Drive undriven nets with a constant
   if (ndrivers == 0) {
      char def = 0;
      int width;
      
      switch (signal_type_of_nexus(nexus, width)) {
      case IVL_SIT_TRI:
         def = 'Z';
         break;
      case IVL_SIT_TRI0:
         def = '0';
         break;
      case IVL_SIT_TRI1:
         def = '1';
         break;
      case IVL_SIT_TRIAND:
         error("No VHDL translation for triand nets");
         break;
      case IVL_SIT_TRIOR:
         error("No VHDL translation for trior nets");
         break;
      default:
         ;
      }

      if (def) {
         if (width > 1)
            priv->const_driver =
               new vhdl_bit_spec_expr(vhdl_type::std_logic(),
                                      new vhdl_const_bit(def));
         else
            priv->const_driver = new vhdl_const_bit(def);
      }
   }
   
   // Save the private data in the nexus
   ivl_nexus_set_private(nexus, priv);
}

/*
 * Ensure that a nexus has been initialised. I.e. all the necessary
 * statements, declarations, etc. have been generated.
 */
static void seen_nexus(ivl_nexus_t nexus)
{
   if (ivl_nexus_get_private(nexus) == NULL)      
      draw_nexus(nexus);
}
 
/*
 * Translate a nexus to a variable reference. Given a nexus and a
 * scope, this function returns a reference to a signal that is
 * connected to the nexus and within the given scope. This signal
 * might not exist in the original Verilog source (even as a
 * compiler-generated temporary). If this nexus hasn't been
 * encountered before, the necessary code to connect up the nexus
 * will be generated.
 */
vhdl_var_ref *nexus_to_var_ref(vhdl_scope *scope, ivl_nexus_t nexus)
{
   seen_nexus(nexus);
   
   nexus_private_t *priv =
      static_cast<nexus_private_t*>(ivl_nexus_get_private(nexus));
   unsigned pin;
   string renamed(visible_nexus_signal_name(priv, scope, &pin));
   
   vhdl_decl *decl = scope->get_decl(renamed);
   assert(decl);

   vhdl_type *type = new vhdl_type(*(decl->get_type()));
   vhdl_var_ref *ref = new vhdl_var_ref(renamed.c_str(), type);
   
   if (decl->get_type()->get_name() == VHDL_TYPE_ARRAY)
      ref->set_slice(new vhdl_const_int(pin), 0);

   return ref;
}

/*
 * Translate all the primitive logic gates into concurrent
 * signal assignments.
 */
static void declare_logic(vhdl_arch *arch, ivl_scope_t scope)
{
   debug_msg("Declaring logic in scope type %s", ivl_scope_tname(scope));
   
   int nlogs = ivl_scope_logs(scope);
   for (int i = 0; i < nlogs; i++)
      draw_logic(arch, ivl_scope_log(scope, i)); 
}

/*
 * Make sure a signal name conforms to VHDL naming rules.
 */
static string make_safe_name(ivl_signal_t sig)
{
   const char *base = ivl_signal_basename(sig);      
   if (base[0] == '_')
      return string("VL") + base;

   // This is the complete list of VHDL reserved words
   const char *vhdl_reserved[] = {
      "abs", "access", "after", "alias", "all", "and", "architecture",
      "array", "assert", "attribute", "begin", "block", "body", "buffer",
      "bus", "case", "component", "configuration", "constant", "disconnect",
      "downto", "else", "elsif", "end", "entity", "exit", "file", "for",
      "function", "generate", "generic", "group", "guarded", "if", "impure",
      "in", "inertial", "inout", "is", "label", "library", "linkage",
      "literal", "loop", "map", "mod", "nand", "new", "next", "nor", "not",
      "null", "of", "on", "open", "or", "others", "out", "package", "port",
      "postponed", "procedure", "process", "pure", "range", "record", "register",
      "reject", "rem", "report", "return", "rol", "ror", "select", "severity",
      "signal", "shared", "sla", "sll", "sra", "srl", "subtype", "then", "to",
      "transport", "type", "unaffected", "units", "until", "use", "variable",
      "wait", "when", "while", "with", "xnor", "xor",
      NULL
   };

   for (const char **p = vhdl_reserved; *p != NULL; p++) {
      if (strcasecmp(*p, base) == 0) {
         return string("VL_") + base;
         break;
      }
   }
   return string(base);
}

/*
 * Declare all signals and ports for a scope.
 */
static void declare_signals(vhdl_entity *ent, ivl_scope_t scope)
{
   debug_msg("Declaring signals in scope type %s", ivl_scope_tname(scope));
   
   int nsigs = ivl_scope_sigs(scope);
   for (int i = 0; i < nsigs; i++) {
      ivl_signal_t sig = ivl_scope_sig(scope, i);      
      remember_signal(sig, ent->get_arch()->get_scope());

      string name(make_safe_name(sig));
      rename_signal(sig, name);
      
      vhdl_type *sig_type;
      unsigned dimensions = ivl_signal_dimensions(sig);
      if (dimensions > 0) {
         // Arrays are implemented by generating a separate type
         // declaration for each array, and then declaring a
         // signal of that type

         if (dimensions > 1) {
            error("> 1 dimension arrays not implemented yet");
            return;
         }

         string type_name = name + "_Type";
         vhdl_type *base_type =
            vhdl_type::type_for(ivl_signal_width(sig), ivl_signal_signed(sig) != 0);

         int lsb = ivl_signal_array_base(sig);
         int msb = lsb + ivl_signal_array_count(sig) - 1;
         
         vhdl_type *array_type =
            vhdl_type::array_of(base_type, type_name, msb, lsb);
         vhdl_decl *array_decl = new vhdl_type_decl(type_name.c_str(), array_type);
         ent->get_arch()->get_scope()->add_decl(array_decl);
           
         sig_type = new vhdl_type(*array_type);
      }
      else
         sig_type =
            vhdl_type::type_for(ivl_signal_width(sig), ivl_signal_signed(sig) != 0);

      ivl_signal_port_t mode = ivl_signal_port(sig);
      switch (mode) {
      case IVL_SIP_NONE:
         {
            vhdl_decl *decl = new vhdl_signal_decl(name.c_str(), sig_type);

            ostringstream ss;
            if (ivl_signal_local(sig)) {
               ss << "Temporary created at " << ivl_signal_file(sig) << ":"
                  << ivl_signal_lineno(sig);
            } else {
               ss << "Declared at " << ivl_signal_file(sig) << ":"
                  << ivl_signal_lineno(sig);
            }
            decl->set_comment(ss.str().c_str());
            
            ent->get_arch()->get_scope()->add_decl(decl);
         }
         break;
      case IVL_SIP_INPUT:
         ent->get_scope()->add_decl
            (new vhdl_port_decl(name.c_str(), sig_type, VHDL_PORT_IN));
         break;
      case IVL_SIP_OUTPUT:
         {
            vhdl_port_decl *decl =
               new vhdl_port_decl(name.c_str(), sig_type, VHDL_PORT_OUT);            
            ent->get_scope()->add_decl(decl);
         }

         if (ivl_signal_type(sig) == IVL_SIT_REG) {
            // A registered output
            // In Verilog the output and reg can have the
            // same name: this is not valid in VHDL
            // Instead a new signal foo_Reg is created
            // which represents the register
            std::string newname(name);
            newname += "_Reg";
            rename_signal(sig, newname.c_str());

            vhdl_type *reg_type = new vhdl_type(*sig_type);
            ent->get_arch()->get_scope()->add_decl
               (new vhdl_signal_decl(newname.c_str(), reg_type));
            
            // Create a concurrent assignment statement to
            // connect the register to the output
            ent->get_arch()->add_stmt
               (new vhdl_cassign_stmt
                (new vhdl_var_ref(name.c_str(), NULL),
                 new vhdl_var_ref(newname.c_str(), NULL)));
         }
         break;
      case IVL_SIP_INOUT:
         ent->get_scope()->add_decl
            (new vhdl_port_decl(name.c_str(), sig_type, VHDL_PORT_INOUT));
         break;
      default:
         assert(false);
      }
   }
}

/*
 * Generate VHDL for LPM instances in a module.
 */
static void declare_lpm(vhdl_arch *arch, ivl_scope_t scope)
{
   int nlpms = ivl_scope_lpms(scope);
   for (int i = 0; i < nlpms; i++) {
      ivl_lpm_t lpm = ivl_scope_lpm(scope, i);
      if (draw_lpm(arch, lpm) != 0)
         error("Failed to translate LPM %s", ivl_lpm_name(lpm));
   }
}

/*
 * Map two signals together in an instantiation.
 * The signals are joined by a nexus.
 */
static void map_signal(ivl_signal_t to, vhdl_entity *parent,
                       vhdl_comp_inst *inst)
{   
   // TODO: Work for multiple words
   ivl_nexus_t nexus = ivl_signal_nex(to, 0);
   seen_nexus(nexus);

   vhdl_scope *arch_scope = parent->get_arch()->get_scope();

   nexus_private_t *priv =
      static_cast<nexus_private_t*>(ivl_nexus_get_private(nexus));
   assert(priv);
   if (!visible_nexus(priv, arch_scope)) {
      // This nexus isn't attached to anything in the parent
      return;
   }

   vhdl_var_ref *ref = nexus_to_var_ref(parent->get_arch()->get_scope(), nexus);

   string name = make_safe_name(to);
  
   // If we're mapping an output of this entity to an output of
   // the child entity, then VHDL will not let us read the value
   // of the signal (i.e. it must pass straight through).
   // However, Verilog allows the signal to be read in the parent.
   // The VHDL equivalent of this is to make *both* output ports
   // a `buffer'.
   vhdl_decl *decl =
      parent->get_arch()->get_scope()->get_decl(ref->get_name());
   vhdl_port_decl *pdecl;
   if ((pdecl = dynamic_cast<vhdl_port_decl*>(decl))
       && pdecl->get_mode() == VHDL_PORT_OUT) {

      // First change the mode in the parent entity
      //pdecl->set_mode(VHDL_PORT_BUFFER);

      // Now change the mode in the child entity
      /*      vhdl_port_decl *to_pdecl =
         dynamic_cast<vhdl_port_decl*>(find_scope_for_signal(to)->get_decl(name));
      assert(to_pdecl);
      to_pdecl->set_mode(VHDL_PORT_BUFFER);*/
   }
   
   inst->map_port(name.c_str(), ref);
}

/*
 * Find all the port mappings of a module instantiation.
 */
static void port_map(ivl_scope_t scope, vhdl_entity *parent,
                     vhdl_comp_inst *inst)
{
   // Find all the port mappings
   int nsigs = ivl_scope_sigs(scope);
   for (int i = 0; i < nsigs; i++) {
      ivl_signal_t sig = ivl_scope_sig(scope, i);
      
      ivl_signal_port_t mode = ivl_signal_port(sig);
      switch (mode) {
      case IVL_SIP_NONE:
         // Internal signals don't appear in the port map
         break;
      case IVL_SIP_INPUT:
      case IVL_SIP_OUTPUT:
      case IVL_SIP_INOUT:
         map_signal(sig, parent, inst);
         break;         
      default:
         assert(false);
      }      
   }
}

/*
 * Create a VHDL function from a Verilog function definition.
 */
static int draw_function(ivl_scope_t scope, ivl_scope_t parent)
{
   assert(ivl_scope_type(scope) == IVL_SCT_FUNCTION);

   debug_msg("Generating function %s (%s)", ivl_scope_tname(scope),
             ivl_scope_name(scope));

   // Find the containing entity
   vhdl_entity *ent = find_entity(parent);
   assert(ent);

   const char *funcname = ivl_scope_tname(scope);

   assert(!ent->get_arch()->get_scope()->have_declared(funcname));

   // The return type is worked out from the output port
   vhdl_function *func = new vhdl_function(funcname, NULL);

   // Set the parent scope of this function to be the containing
   // architecture. This allows us to look up non-local variables
   // referenced in the body, but if we do the `impure' flag must
   // be set on the function
   // (There are actually two VHDL scopes in a function: the local
   // variables and the formal parameters hence the call to get_parent)
   func->get_scope()->get_parent()->set_parent(ent->get_arch()->get_scope());
   
   int nsigs = ivl_scope_sigs(scope);
   for (int i = 0; i < nsigs; i++) {
      ivl_signal_t sig = ivl_scope_sig(scope, i);            
      vhdl_type *sigtype =
         vhdl_type::type_for(ivl_signal_width(sig),
                             ivl_signal_signed(sig) != 0);
      
      string signame(make_safe_name(sig));

      switch (ivl_signal_port(sig)) {
      case IVL_SIP_INPUT:
         func->add_param(new vhdl_param_decl(signame.c_str(), sigtype));
         break;
      case IVL_SIP_OUTPUT:
         // The magic variable <funcname>_Result holds the return value
         signame = funcname;
         signame += "_Result";
         func->set_type(new vhdl_type(*sigtype));
      default:
         func->get_scope()->add_decl
            (new vhdl_var_decl(signame.c_str(), sigtype));
      }
      
      remember_signal(sig, func->get_scope());
      rename_signal(sig, signame);
   }
   
   // Non-blocking assignment not allowed in functions
   func->get_scope()->set_allow_signal_assignment(false);

   set_active_entity(ent);
   {
      draw_stmt(func, func->get_container(), ivl_scope_def(scope));
   }
   set_active_entity(NULL);

   // Add a forward declaration too in case it is called by
   // another function that has already been added
   ent->get_arch()->get_scope()->add_forward_decl
      (new vhdl_forward_fdecl(func));

   ostringstream ss;
   ss << "Generated from function " << funcname << " at "
      << ivl_scope_def_file(scope) << ":" << ivl_scope_def_lineno(scope);
   func->set_comment(ss.str().c_str());
   
   ent->get_arch()->get_scope()->add_decl(func);   
   return 0;
}


/*
 * Create the signals necessary to expand this task later.
 */
static int draw_task(ivl_scope_t scope, ivl_scope_t parent)
{
   assert(ivl_scope_type(scope) == IVL_SCT_TASK);

   // Find the containing entity
   vhdl_entity *ent = find_entity(parent);
   assert(ent);

   const char *taskname = ivl_scope_tname(scope);
   
   int nsigs = ivl_scope_sigs(scope);
   for (int i = 0; i < nsigs; i++) {
      ivl_signal_t sig = ivl_scope_sig(scope, i);            
      vhdl_type *sigtype =
         vhdl_type::type_for(ivl_signal_width(sig),
                             ivl_signal_signed(sig) != 0);
      
      string signame(make_safe_name(sig));

      // Check this signal isn't declared in the outer scope
      if (ent->get_arch()->get_scope()->have_declared(signame)) {
         signame += "_";
         signame += taskname;
      }

      vhdl_signal_decl *decl = new vhdl_signal_decl(signame.c_str(), sigtype);

      ostringstream ss;
      ss << "Declared at " << ivl_signal_file(sig) << ":"
         << ivl_signal_lineno(sig) << " (in task " << taskname << ")";
      decl->set_comment(ss.str().c_str());
   
      ent->get_arch()->get_scope()->add_decl(decl);   
      
      remember_signal(sig, ent->get_arch()->get_scope());
      rename_signal(sig, signame);
   }
   
   return 0;
}

/*
 * Create an empty VHDL entity for a Verilog module.
 */
static void create_skeleton_entity_for(ivl_scope_t scope, int depth)
{
   assert(ivl_scope_type(scope) == IVL_SCT_MODULE);

   // The type name will become the entity name
   const char *tname = ivl_scope_tname(scope);;
   
   // Verilog does not have the entity/architecture distinction
   // so we always create a pair and associate the architecture
   // with the entity for convenience (this also means that we
   // retain a 1-to-1 mapping of scope to VHDL element)
   vhdl_arch *arch = new vhdl_arch(tname, "FromVerilog");
   vhdl_entity *ent = new vhdl_entity(tname, arch, depth);

   // Build a comment to add to the entity/architecture
   ostringstream ss;
   ss << "Generated from Verilog module " << ivl_scope_tname(scope)
      << " (" << ivl_scope_def_file(scope) << ":"
      << ivl_scope_def_lineno(scope) << ")";
   
   arch->set_comment(ss.str());
   ent->set_comment(ss.str());
   
   remember_entity(ent);
}

/*
 * A first pass through the hierarchy: create VHDL entities for
 * each unique Verilog module type.
 */
static int draw_skeleton_scope(ivl_scope_t scope, void *_unused)
{
   static int depth = 0;
   
   if (seen_this_scope_type(scope))
      return 0;  // Already generated a skeleton for this scope type
   
   debug_msg("Initial visit to scope type %s at depth %d",
             ivl_scope_tname(scope), depth);
   
   switch (ivl_scope_type(scope)) {
   case IVL_SCT_MODULE:
      create_skeleton_entity_for(scope, depth);
      break;
   case IVL_SCT_GENERATE:
      error("No translation for generate statements yet");
      return 1;
   case IVL_SCT_FORK:
      error("No translation for fork statements yet");
      return 1;
   default:
      // The other scope types are expanded later on
      break;
   }

   ++depth;
   int rc = ivl_scope_children(scope, draw_skeleton_scope, NULL);
   --depth;
   return rc;
}

static int draw_all_signals(ivl_scope_t scope, void *_parent)
{
   if (!is_default_scope_instance(scope))
      return 0;  // Not interested in this instance
     
   if (ivl_scope_type(scope) == IVL_SCT_MODULE) {
      vhdl_entity *ent = find_entity(scope);
      assert(ent);

      declare_signals(ent, scope);
   }

   return ivl_scope_children(scope, draw_all_signals, scope);
}

/*
 * Draw all tasks and functions in the hierarchy.
 */
static int draw_functions(ivl_scope_t scope, void *_parent)
{
   if (!is_default_scope_instance(scope))
      return 0;  // Not interested in this instance
   
   ivl_scope_t parent = static_cast<ivl_scope_t>(_parent);
   if (ivl_scope_type(scope) == IVL_SCT_FUNCTION) {
      if (draw_function(scope, parent) != 0)
         return 1;
   }
   else if (ivl_scope_type(scope) == IVL_SCT_TASK) {
      if (draw_task(scope, parent) != 0)
         return 1;
   }

   return ivl_scope_children(scope, draw_functions, scope);
}

/*
 * Make concurrent assignments for constants in nets. This works
 * bottom-up so that the driver is in the lowest instance it can.
 * This also has the side effect of generating all the necessary
 * nexus code.
 */
static int draw_constant_drivers(ivl_scope_t scope, void *_parent)
{
   if (!is_default_scope_instance(scope))
      return 0;  // Not interested in this instance
   
   ivl_scope_children(scope, draw_constant_drivers, scope);
   
   if (ivl_scope_type(scope) == IVL_SCT_MODULE) {
      vhdl_entity *ent = find_entity(scope);
      assert(ent);

      int nsigs = ivl_scope_sigs(scope);
      for (int i = 0; i < nsigs; i++) {
         ivl_signal_t sig = ivl_scope_sig(scope, i);
         
         for (unsigned j = ivl_signal_array_base(sig);
              j < ivl_signal_array_count(sig);
              j++) {
            // Make sure the nexus code is generated
            ivl_nexus_t nex = ivl_signal_nex(sig, j);
            seen_nexus(nex);
            
            nexus_private_t *priv =
               static_cast<nexus_private_t*>(ivl_nexus_get_private(nex));
            assert(priv);

            vhdl_scope *arch_scope = ent->get_arch()->get_scope();

            if (priv->const_driver
                && ivl_signal_port(sig) != IVL_SIP_INPUT) { // Don't drive inputs
               assert(j == 0);   // TODO: Make work for more words
               
               vhdl_var_ref *ref = nexus_to_var_ref(arch_scope, nex);
               
               ent->get_arch()->add_stmt
                  (new vhdl_cassign_stmt(ref, priv->const_driver));                  
               priv->const_driver = NULL;
            }

            // Connect up any signals which are wired together in the
            // same nexus
            scope_nexus_t *sn = visible_nexus(priv, arch_scope);

            // Make sure we don't drive inputs
            if (ivl_signal_port(sn->sig) != IVL_SIP_INPUT) { 
               for (list<ivl_signal_t>::const_iterator it = sn->connect.begin();
                    it != sn->connect.end();
                    ++it) {
                  vhdl_type* rtype =
                     vhdl_type::type_for(ivl_signal_width(sn->sig),
                                         ivl_signal_signed(sn->sig));
                  vhdl_type* ltype =
                     vhdl_type::type_for(ivl_signal_width(*it),
                                         ivl_signal_signed(*it));
                  
                  vhdl_var_ref *rref =
                     new vhdl_var_ref(get_renamed_signal(sn->sig).c_str(), rtype);
                  vhdl_var_ref *lref =
                     new vhdl_var_ref(get_renamed_signal(*it).c_str(), ltype);
                  
                  // Make sure the LHS and RHS have the same type
                  vhdl_expr* rhs = rref->cast(lref->get_type());
                  
                  ent->get_arch()->add_stmt(new vhdl_cassign_stmt(lref, rhs));
               }
            }
            sn->connect.clear();               
         }           
      }
   }

   return 0;
}

static int draw_all_logic_and_lpm(ivl_scope_t scope, void *_parent)
{
   if (!is_default_scope_instance(scope))
      return 0;  // Not interested in this instance
   
   if (ivl_scope_type(scope) == IVL_SCT_MODULE) {
      vhdl_entity *ent = find_entity(scope);
      assert(ent);

      set_active_entity(ent);
      {
         declare_logic(ent->get_arch(), scope);
         declare_lpm(ent->get_arch(), scope);
      }
      set_active_entity(NULL);
   }   

   return ivl_scope_children(scope, draw_all_logic_and_lpm, scope);
}

static int draw_hierarchy(ivl_scope_t scope, void *_parent)
{   
   if (ivl_scope_type(scope) == IVL_SCT_MODULE && _parent) {
      ivl_scope_t parent = static_cast<ivl_scope_t>(_parent);

      if (!is_default_scope_instance(parent))
         return 0;  // Not generating code for the parent instance so
                    // don't generate for the child
   
      vhdl_entity *ent = find_entity(scope);
      assert(ent);
      
      vhdl_entity *parent_ent = find_entity(parent);
      assert(parent_ent);

      vhdl_arch *parent_arch = parent_ent->get_arch();
      assert(parent_arch != NULL);
         
      // Create a forward declaration for it
      vhdl_scope *parent_scope = parent_arch->get_scope();
      if (!parent_scope->have_declared(ent->get_name())) {
         vhdl_decl *comp_decl = vhdl_component_decl::component_decl_for(ent);
         parent_arch->get_scope()->add_decl(comp_decl);
      }
         
      // And an instantiation statement
      string inst_name(ivl_scope_basename(scope));
      if (inst_name == ent->get_name() || parent_scope->have_declared(inst_name)) {
         // Cannot have instance name the same as type in VHDL
         inst_name += "_Inst";
      }

      // Need to replace any [ and ] characters that result
      // from generate statements
      string::size_type loc = inst_name.find('[', 0);
      if (loc != string::npos)
         inst_name.erase(loc, 1);
      
      loc = inst_name.find(']', 0);
      if (loc != string::npos)
         inst_name.erase(loc, 1);         
      
      vhdl_comp_inst *inst =
         new vhdl_comp_inst(inst_name.c_str(), ent->get_name().c_str());
      port_map(scope, parent_ent, inst);

      ostringstream ss;
      ss << "Generated from instantiation at "
         << ivl_scope_file(scope) << ":" << ivl_scope_lineno(scope);
      inst->set_comment(ss.str().c_str());
      
      parent_arch->add_stmt(inst);
   }   

   return ivl_scope_children(scope, draw_hierarchy, scope);
}

int draw_scope(ivl_scope_t scope, void *_parent)
{   
   int rc = draw_skeleton_scope(scope, _parent);
   if (rc != 0)
      return rc;

   rc = draw_all_signals(scope, _parent);
   if (rc != 0)
      return rc;

   rc = draw_all_logic_and_lpm(scope, _parent);
   if (rc != 0)
      return rc;      

   rc = draw_hierarchy(scope, _parent);
   if (rc != 0)
      return rc;

   rc = draw_functions(scope, _parent);
   if (rc != 0)
      return rc;

   rc = draw_constant_drivers(scope, _parent);
   if (rc != 0)
      return rc;
   
   return 0;
}
