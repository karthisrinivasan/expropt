/*************************************************************************
 *
 *  This file is part of act expropt
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *
 **************************************************************************/

#include "expropt.h"
#include "abc_api.h"

#define VERILOG_FILE_PREFIX "exprop_"
#define MAPPED_FILE_SUFFIX "_mapped"

/**
 * Destroy the External Expr Opt:: External Expr Opt object
 * 
 * no memory allocation on global vars, nothing to be done
 */
ExternalExprOpt::~ExternalExprOpt()
{
  if (_abc_api) {
    AbcApi *x = (AbcApi *) _abc_api;
    delete x;
  }
  if (_syn_dlib) {
    dlclose (_syn_dlib);
    _syn_dlib = NULL;
  }
}

bool ExternalExprOpt::engineExists (const char *s)
{
  // if a mapper string has been specified, find the library!
  char buf[10240];
  
  if (!getenv ("ACT_HOME")) return false;
  
  snprintf (buf, 10240, "%s/lib/act_extsyn_%s.so", getenv ("ACT_HOME"), s);

  FILE *fp = fopen (buf, "r");
  if (!fp) {
    return false;
  }
  fclose (fp);
  return true;
}

/**
 * Initialize configuration parameters
 */
void ExternalExprOpt::_init_defaults ()
{
  // clear tmp files by default
  config_set_default_int ("synth.expropt.clean_tmp_files", 1);

  // do not vectorize all ports; single entry ports are bools, while
  // ports with an array of bits are arrays
  config_set_default_int ("synth.expropt.vectorize_all_ports", 0);

  // verbosity level. 1 = display dots
  config_set_default_int ("synth.expropt.verbose", 1);

  // namespace for the cell library for qdi and bundled data datapaths
  config_set_default_string ("synth.qdi.cell_lib_namespace", "syn");
  config_set_default_string ("synth.bundled.cell_lib_namespace", "syn");

  // core type for a bit in qdi v/s bundled data
  config_set_default_string ("synth.qdi.cell_lib_wire_type", "sdtexprchan<1>");
  config_set_default_string ("synth.bundled.cell_lib_wire_type", "bool");
  
  // capacitance table for the technology
  config_set_default_string ("synth.expropt.captable", "none");

  // LEF files
  config_set_default_string ("expropt.lef", "none");

  // .lib files for other corners; omitted by default
  config_set_default_string ("synth.liberty.max_power", "none");
  config_set_default_string ("synth.liberty.min_delay", "none");
  config_set_default_string ("synth.liberty.max_delay", "none");

  // default load cap
  config_set_default_real ("synth.expropt.default_load", 1.0);

  config_read("expropt.conf");

  _syn_dlib = NULL;
  _syn_run = NULL;
  _syn_get_metric = NULL;
  _syn_cleanup = NULL;

  if (!mapper) {
    mapper = "abc";
  }

  // if a mapper string has been specified, find the library!
  char buf[10240];
  snprintf (buf, 10240, "%s/lib/act_extsyn_%s.so",
	    getenv ("ACT_HOME"),
	    mapper);
  _syn_dlib = dlopen (buf, RTLD_LAZY);
  if (!_syn_dlib) {
    fatal_error ("could not open `%s' external logic synthesis library!", buf);
  }

  snprintf (buf, 10240, "%s_run", mapper);
  *((void **)&_syn_run)= dlsym (_syn_dlib, buf);
  if (!_syn_run) {
    fatal_error ("Expression synthesis library `%s': missing %s!", mapper, buf);
  }
  snprintf (buf, 10240, "%s_get_metric", mapper);
  *((void **)&_syn_get_metric) = dlsym (_syn_dlib, buf);
  if (!_syn_get_metric) {
    fatal_error ("Expression synthesis library `%s': missing %s", mapper, buf);
  }
  snprintf (buf, 10240, "%s_cleanup", mapper);
  *((void **)&_syn_cleanup) = dlsym (_syn_dlib, buf);
  if (!_syn_cleanup) {
    fatal_error ("Expression synthesis library `%s': missing %s", mapper, buf);
  }

  _filenum = 0;
}


/*------------------------------------------------------------------------
 * Simplest expression optimization run
 *------------------------------------------------------------------------
 */
ExprBlockInfo* ExternalExprOpt::run_external_opt (int expr_set_number,
						  int targetwidth,
						  Expr *expr,
						  list_t *in_expr_list,
						  iHashtable *in_expr_map,
						  iHashtable *in_width_map,
              bool __cleanup)
{
  // build the data structures needed

  ExprBlockInfo* info;
  iHashtable *outexprmap = ihash_new(0);
  iHashtable *inexprmap = ihash_new(0);
  iHashtable *outwidthmap = ihash_new(0);
  list_t *outlist = list_new();

  // convert input list, reverse searching nessesary, should always
  // use last on multimatching
  listitem_t *li;
  Expr* e = NULL;
  for (li = list_first (in_expr_list); li; li = list_next (li)) { 
    e = (Expr *) list_value(li);
    // change from int to C string
    ihash_bucket_t *b_map,*b_new;
    b_map = ihash_lookup(in_expr_map, (long) e);
    char *charbuf = (char *) malloc( sizeof(char) * ( 1024 + 1 ) );
    snprintf(charbuf, 1024, "%s%u",expr_prefix.data(),b_map->i);
    b_new = ihash_add(inexprmap, (long) e);
    b_new->v = charbuf;
  }

  ihash_bucket_t *b_map;
  char *charbuf = (char *) malloc( sizeof(char) * ( 1024 + 1 ) );

  // the output should be called "out"
  snprintf(charbuf,1024, "out");
  b_map = ihash_add(outexprmap,(long) expr);
  b_map->v = charbuf;

  // add the Expr * to the output list
  list_append(outlist, expr);

  // add the bitwidth to the bitwidth hash table
  ihash_bucket_t *b_width;
  b_width = ihash_add(outwidthmap,(long) expr);
  b_width->i = targetwidth;

  // generate module name 
  char expr_set_name[1024];
  snprintf(expr_set_name, 1024, "%s%u",module_prefix.data(), expr_set_number);
  
  // and send off
  info = run_external_opt(expr_set_name,
			  in_expr_list,
			  inexprmap,
			  in_width_map,
			  outlist,
			  outexprmap,
			  outwidthmap,
        NULL,
        __cleanup);

  // after completerion clean up memory, the generated char names will
  // leak they are not cleaned up atm.
  list_free(outlist);
  ihash_free(outwidthmap);

  // delete the strings in outexprmap and inexprmap
  ihash_iter_t it;
  ihash_iter_init (outexprmap, &it); // ok this can be done in a much
				     // simpler manner...
  while ((b_map = ihash_iter_next (outexprmap, &it))) {
    FREE (b_map->v);
  }
  ihash_free(outexprmap);

  ihash_iter_init (inexprmap, &it);
  while ((b_map = ihash_iter_next (inexprmap, &it))) {
    FREE (b_map->v);
  }
  ihash_free(inexprmap);

  return info;
}


/**
 * first constuct the filenames for the temporary files and than
 * generate the verilog, exc the external tool, read out the results
 * and convert them back to act.
 * 
 * the printing of the verilog is seperate
 */
ExprBlockInfo* ExternalExprOpt::run_external_opt (const char* expr_set_name,
						  list_t *in_expr_list,
						  iHashtable *in_expr_map,
						  iHashtable *in_width_map,
						  list_t *out_expr_list,
						  iHashtable *out_expr_map,
						  iHashtable *out_width_map,
						  list_t *hidden_expr_list,
              bool __cleanup)
{
  //build the data structures need
  ExprBlockInfo* info;
  list_t *out_name_list = list_new();
  list_t *hidden_name_list = NULL;
  listitem_t *li;
  
  if (hidden_expr_list) 
  {
    hidden_name_list = list_new();
    for (li = list_first (hidden_expr_list); li; li = list_next (li))
    { 
      ihash_bucket_t *b;
      b = ihash_lookup (out_expr_map, (long) list_value(li));
      Assert (b, "variable not found in variable map");
      list_append(hidden_name_list, b->v);
    }
  }

  for (li = list_first (out_expr_list); li; li = list_next (li))
  { 
    ihash_bucket_t *b;
    b = ihash_lookup (out_expr_map, (long) list_value(li));
    Assert (b, "variable not found in variable map");
    list_append(out_name_list, b->v);
  }

  info = run_external_opt(expr_set_name,
			  in_expr_list,
			  in_expr_map,
			  in_width_map,
			  out_expr_list,
			  out_name_list,
			  out_width_map,
			  hidden_expr_list,
			  hidden_name_list,
        __cleanup);
  
  list_free(out_name_list);
  list_free(hidden_name_list);
  return info;
}


/**
 * The only function that does actual work!
 *
 * First constuct the filenames for the temporary files and than
 * generate the verilog, exc the external tool, read out the results
 * and convert them back to act.
 * 
 * the printing of the verilog is seperate
 */
ExprBlockInfo* ExternalExprOpt::run_external_opt (const char* expr_set_name,
						  list_t *in_expr_list,
						  iHashtable *in_expr_map,
						  iHashtable *in_width_map,
						  list_t *out_expr_list,
						  list_t *out_expr_name_list,
						  iHashtable *out_width_map,
						  list_t *hidden_expr_list,
						  list_t *hidden_expr_name_list,
              bool __cleanup)
{
  ExprBlockInfo* info = NULL;

  /*
    Check if this call has been cached; if so, use the saved results!

    Cache organization:

     $ACT_CACHE -- if it exists, otherwise $HOME/.act_cache

     For this, the cache is:
        $ACT_CACHE/syn/<mapper>/$ACT_TECH/

     index.db : list of proc names + metrics
     <procname>.act : for each proc name
  */


  // consruct files names for the temp files
  std::string verilog_file = "./";
  verilog_file.append(VERILOG_FILE_PREFIX);
  //verilog_file.append(expr_set_name);
  verilog_file.append (std::to_string (_filenum++));
  
  std::string mapped_file = verilog_file.data();
  mapped_file.append(MAPPED_FILE_SUFFIX);
  
  mapped_file.append(".v");
  verilog_file.append(".v");
  
  // char cmd[4096] = "";
  FILE *verilog_stream;

  std::string sdc_file = "./";
  sdc_file.append(VERILOG_FILE_PREFIX);
  sdc_file.append(expr_set_name);
  sdc_file.append(".sdc");

  // open temp verilog file to be syntesised
  verilog_stream = fopen(verilog_file.data(), "w");
  if (!verilog_stream) {
    fatal_error("ExternalExprOpt::run_external_opt: "
		"verilog file %s is not writable", verilog_file.data());
  }

  std::chrono::microseconds io_duration(0);

  // generate verilog module
  {
    char buf[1024];
    const char *module_name = expr_set_name;
    
    if (strcmp (mapper, "abc") == 0) {
      /* abc is going to mess up the port names, so we need to fix
	 that; we do this by changing the Verilog module name, and then
	 creating a dummy passthru instance in the abc API (abc_api.cc) */
      snprintf (buf, 1024, "%stmp", expr_set_name);
      module_name = buf;
    }
    auto start_print_verilog = high_resolution_clock::now();
    print_expr_verilog(verilog_stream, module_name,
		       in_expr_list,
		       in_expr_map,
		       in_width_map,
		       out_expr_list,
		       out_expr_name_list,
		       out_width_map,
		       hidden_expr_list,
		       hidden_expr_name_list);
    auto stop_print_verilog = high_resolution_clock::now();
    io_duration += duration_cast<microseconds>(stop_print_verilog - start_print_verilog);
  }

  // close Verilog file
  fclose(verilog_stream);

  // generate the exec command for the sysntesis tool and run the syntesis
  int exec_failure = 1;


  char *configreturn;

  /*
    A synthesis/mapping tool translates and maps a Verilog description
    into a Verilog netlist.  It also provides a method to analyze the
    result and return metrics. The functionality is provided through
    dynamically loaded functions (see yosys.cc for an example).
  */

  // parameters to be passed to the logic synthesis engine 
  __syn.v_in = verilog_file.c_str();
  __syn.v_out = mapped_file.c_str();
  __syn.toplevel = expr_set_name;
  __syn.use_tie_cells = use_tie_cells;
  __syn.space = NULL;
  
  configreturn = config_get_string("synth.liberty.typical");
  if (strcmp (configreturn, "none") == 0) {
    fatal_error("please define \"liberty.typical\" in synthesis configuration file 2");
  }

  if (strcmp (mapper, "abc") == 0) {
    // Intrnal abc logic synthesis
    if (!_abc_api) {
      _abc_api = new AbcApi ();
    }
    __syn.space  = _abc_api;
  }
  
  auto start_mapper = high_resolution_clock::now();
  if (!(*_syn_run) (&__syn)) {
    fatal_error ("Synthesis %s failed.", mapper);
  }
  auto stop_mapper = high_resolution_clock::now();
  auto duration = duration_cast<microseconds>(stop_mapper - start_mapper);
  auto ebi = backend(mapped_file, io_duration, duration);
  if (__cleanup) cleanup_tmp_files();
  return ebi;
}

ExprBlockInfo *ExternalExprOpt::backend(std::string _mapped_file,
                                std::chrono::microseconds io_duration,
                                std::chrono::microseconds duration)
{
  // read the resulting netlist and map it back to act, if the
  // wire_type is not bool use the async mode the specify a wire type
  // as a channel.  skip if run was just for extraction of properties
  // => output filename empty
  if (!expr_output_file.empty()) {
    auto start_v2act = high_resolution_clock::now();
    run_v2act(_mapped_file, __syn.use_tie_cells);
    auto stop_v2act = high_resolution_clock::now();
    io_duration += duration_cast<microseconds>(stop_v2act - start_v2act);
  }
  
  // parse block info - WORK IN PROGRESS
  metric_triplet delay, static_power, dynamic_power, total_power;
  double area = 0.0;

  area = (*_syn_get_metric) (&__syn, metadata_area);

  if (area == 0.0) {
    delay.set_metrics (0, 0, 0);
    static_power.set_metrics (0, 0, 0);
    dynamic_power.set_metrics (0, 0, 0);
    total_power.set_metrics (0, 0, 0);
  }
  else if (area == -1) {
    // not found!
  }
  else {
    delay.
      set_metrics ((*_syn_get_metric) (&__syn, metadata_delay_min),
		   (*_syn_get_metric) (&__syn, metadata_delay_typ),
		   (*_syn_get_metric) (&__syn, metadata_delay_max)
		   );

    static_power.
      set_metrics ((*_syn_get_metric) (&__syn, metadata_power_typ_static),
		   (*_syn_get_metric) (&__syn, metadata_power_typ_static),
		   (*_syn_get_metric) (&__syn, metadata_power_max_static)
		   );

    dynamic_power.
      set_metrics ((*_syn_get_metric) (&__syn, metadata_power_typ_dynamic),
		   (*_syn_get_metric) (&__syn, metadata_power_typ_dynamic),
		   (*_syn_get_metric) (&__syn, metadata_power_max_dynamic)
		   );
    
    total_power.
      set_metrics ((*_syn_get_metric) (&__syn, metadata_power_typ),
		   (*_syn_get_metric) (&__syn, metadata_power_typ),
		   (*_syn_get_metric) (&__syn, metadata_power_max)
		   );
  }

  auto info =  new ExprBlockInfo(delay,
              total_power,
              static_power,
              dynamic_power,
              area,
              duration.count(),
              io_duration.count(),
              _mapped_file
              );

  return info;
}

void ExternalExprOpt::run_v2act(std::string _mapped_file, bool tie_cells)
{
  char cmd[4096] = "";
  // read the resulting netlist and map it back to act, if the
  // wire_type is not bool use the async mode the specify a wire type
  // as a channel.  skip if run was just for extraction of properties
  // => output filename empty
  if (wire_encoding == qdi) {
    // QDI, so we don't add tie cells
    snprintf(cmd, 4096, "v2act -a -C \"%s\" -l %s -n %s %s >> %s",
        expr_channel_type.data(),
        cell_act_file.data(),
        cell_namespace.data(),
        _mapped_file.data(),
        expr_output_file.data());
  }
  else {
    if (!(tie_cells)) {
      snprintf(cmd, 4096, "v2act -t -l %s -n %s %s >> %s",
        cell_act_file.data(),
        cell_namespace.data(),
        _mapped_file.data(),
        expr_output_file.data());
    }
    else {
      snprintf(cmd, 4096, "v2act -l %s -n %s %s >> %s",
        cell_act_file.data(),
        cell_namespace.data(),
        _mapped_file.data(),
        expr_output_file.data());
    }
  }
  
  if (config_get_int("synth.expropt.verbose") == 2) {
    printf("running: %s \n",cmd);
  }
  else if (config_get_int("synth.expropt.verbose") == 1) {
    printf(".");
    fflush(stdout);
  }

  int exec_failure = system(cmd);
  if (exec_failure != 0) {
    fatal_error("external program call \"%s\" failed.", cmd);
  }
}

void ExternalExprOpt::cleanup_tmp_files()
{
  // clean up temporary files
  if (_cleanup) {
    (*_syn_cleanup) (&__syn);
  }
}