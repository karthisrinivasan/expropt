/*************************************************************************
 *
 *  Copyright (c) 2023 Rajit Manohar
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
 **************************************************************************
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <common/misc.h>
#include <common/config.h>
#include <common/list.h>
#include "abc_api.h"
#include <ctype.h>

AbcApi::AbcApi ()
{
  int parent_to_child[2], child_to_parent[2];

  if (pipe (parent_to_child) < 0) {
    fatal_error ("Could not create pipe()!\n");
  }
  if (pipe (child_to_parent) < 0) {
    fatal_error ("Could not create pipe()!\n");
  }
  
  _childpid = fork();
  _pAbc = NULL;
  if (_childpid < 0) {
    fatal_error ("fork() system call failed!");
  }
  if (_childpid == 0) {
    /* in child */
    _fd.from = parent_to_child[0];
    close (parent_to_child[1]);
    _fd.to = child_to_parent[1];
    close (child_to_parent[0]);
    _parent = false;

    for (int i=0; i < 256; i++) {
      if (i == _fd.from || i == _fd.to) {
	continue;
      }
      close (i);
    }
    _mainloop();
    exit (0);
  }
  else {
    _fd.from = child_to_parent[0];
    close (child_to_parent[1]);
    _fd.to = parent_to_child[1];
    close (parent_to_child[0]);
    _parent = true;
  }
}

AbcApi::~AbcApi()
{
  int stat;
  Assert (_parent == true, "What?");
  write (_fd.to, "$bye$", 5);
  if (waitpid (_childpid, &stat, 0) < 0) {
    fatal_error ("Error in waitpid() call!");
  }
  close (_fd.from);
  close (_fd.to);
  unlink ("abc.history");
}


/*
 * Child process that runs the abc engine
 */
void AbcApi::_mainloop()
{
  char *buf;
  int buf_max;
  int sz, pos;
  int cmd_end;

  buf_max = char_buf_sz_abc;
  MALLOC (buf, char, buf_max);

#define GET_MORE					\
  do {							\
    int res = read (_fd.from, buf+sz, buf_max-sz);	\
    if (res < 0) return;				\
    sz += res;						\
  } while (0)
  
  _pAbc = NULL;

  sz = 0;
  pos = 0;
  do {
    Assert (pos == 0, "What?");
    while (sz < 5) {
      GET_MORE;
    }

    if (sz < char_buf_sz_abc) {
      buf[sz] = '\0';
    }
    
    if (strncmp (buf, "$bye$", 5) == 0) {
      // we're done, exit
      FREE (buf);
      return;
    }
    else if (strncmp (buf, "$new$", 5) == 0) {
      // new session
      if (_pAbc) {
	// there is an existing session; we need to end it first!
	_endsession ();
      }
      Assert (!_pAbc, "What?");
      pos += 5;
      cmd_end = _get_full_cmd (&buf, &buf_max, &sz, &pos);
      buf[cmd_end] = '\0';
      if (_startsession (buf+pos)) {
	_ok ();
      }
      else {
	_error ();
      }
    }
    else if (strncmp (buf, "$cmd$", 5) == 0) {
      // abc commands
      pos += 5;
      cmd_end = _get_full_cmd (&buf, &buf_max, &sz, &pos);
      buf[cmd_end] = '\0';
      if (_run_abc (buf+pos)) {
	_ok ();
      }
      else {
	_error ();
      }
    }
    else if (strncmp (buf, "$end$", 5) == 0) {
      if (_endsession()) {
	_ok();
      }
      else {
	_error ();
      }
      cmd_end = 4;
    }
    else {
      _error ();
      cmd_end = 4;
    }
    cmd_end++;
    for (int i=0; i < sz - cmd_end; i++) {
      buf[i] = buf[i+cmd_end];
    }
    sz -= cmd_end;
    pos = 0;
  } while (1);
}


/*
 * Read input pipe until the end of command character '$' is seen. 
 * The buffer is reallocated as needed.
 * maxsz holds the size of the allocated buffer
 * pos is the current position in the buffer where the next unread
 * data starts
 * sz is the size of the data in the buffer 
 *
 * The returned value is the position of the '$' within the buffer.
 */
int AbcApi::_get_full_cmd (char **buf, int *maxsz, int *sz, int *pos)
{
  int i;
  int chkpos;

  for (i = *pos; i < *sz; i++) {
    if ((*buf)[i] == '$') {
      return i;
    }
  }
  if (*pos > 0) {
    for (i=0; i < (*sz - *pos); i++) {
      (*buf)[i] = (*buf)[i+*pos];
    }
    *sz = *sz - *pos;
    *pos = 0;
  }

  /* 
     - data starts from zero
     - we've checked that the end symbol has not been found up the
       number of received bytes. We need to receive more.
  */
  do {
    chkpos = *sz;
    if (*sz == *maxsz) {
      REALLOC (*buf, char, 2*(*maxsz));
      *maxsz = 2 * (*maxsz);
    }
    Assert (*sz < *maxsz, "What?");
    *sz += read (_fd.from, *buf+*sz, *maxsz - *sz);
    for (; chkpos < *sz; chkpos++) {
      if ((*buf)[chkpos] == '$') {
	return chkpos;
      }
    }
  } while (1);
}

#define RUN_ABC(cmdbuf)					\
  do {							\
  } while (0)


bool AbcApi::_run_abc (char *cmd)
{
  Assert (_parent == false, "What?");
  Assert (_pAbc, "What?");

  if ( Cmd_CommandExecute (_pAbc, cmd) ) {
    close (_logfd);
    Abc_Stop ();
    _pAbc = NULL;
    return false;
  }
  return true;
}

bool AbcApi::_startsession(char *args)
{
  char buf[char_buf_sz_abc];
  Assert (_parent == false, "What?");


  char *name = args;
  int argpos = 0;
  
  while (name[argpos] && name[argpos] != ' ') {
    argpos++;
  }
  if (argpos == 0) {
    return false;
  }
  name[argpos] = '\0';
  
  _name = Strdup (name);
  
  name = name + argpos + 1;
  argpos = 0;
  while (name[argpos] && name[argpos] != ' ') {
    argpos++;
  }
  if (argpos == 0) {
    return false;
  }
  name[argpos] = '\0';

  _vin = Strdup (name);
  name = name + argpos + 1;
  argpos = 0;

  _vout = Strdup (name);
      
  snprintf (buf, char_buf_sz_abc, "%s.log", _vout);
  _logfd = open (buf, O_CREAT|O_TRUNC|O_RDWR, S_IRUSR|S_IWUSR);

  if (_logfd < 0) {
    fatal_error ("Could not create log file %s", buf);
  }

  dup2 (_logfd, 1);
  dup2 (_logfd, 2);
  
  
  Abc_Start ();
  
  _pAbc = Abc_FrameGetGlobalFrame ();
  
  // read Verilog, blast it, and read in the liberty file
  snprintf (buf, char_buf_sz_abc, "%%read %s; %%blast; &put", _vin);
  if (!_run_abc (buf)) return false;

  char *lib = config_get_string("synth.liberty.typical");
  snprintf (buf, char_buf_sz_abc, "read_lib -v %s", lib);
  if (!_run_abc (buf)) return false;

  int constr = 0;
  if (config_exists ("synth.expropt.abc.use_constraints")) {
    if (config_get_int ("synth.expropt.abc.use_constraints") == 1) {
      constr = 1;
    }
  }
  
  if (constr) {
    int len;
    name = Strdup (_vin);
    len = strlen (name);
    if (len > 1) {
      name[len-1] = '\0';
      name[len-2] = '\0';
    }
    snprintf (buf, char_buf_sz_abc, "read_constr %s.sdc", name);
    FREE (name);
    if (!_run_abc (buf)) return false;
  }

  return true;
}



void AbcApi::_ok ()
{
  const char *buf = "$ok_$";
  Assert (_parent == false, "What?!");
  write (_fd.to, buf, strlen (buf));
}

void AbcApi::_error ()
{
  const char *buf = "$err$";
  Assert (_parent == false, "What?!");
  write (_fd.to, buf, strlen (buf));
}

int AbcApi::runCmd (const char *buf)
{
  Assert (_parent, "What?");

  if (write (_fd.to, "$cmd$", 5) < 0) {
    return 0;
  }
  if (write (_fd.to, buf, strlen (buf)) < 0) {
    return 0;
  }
  if (write (_fd.to, "$", 1) < 0) {
    return 0;
  }
  return _check_ok ();
}

int AbcApi::_check_ok ()
{
  char res[10];
  int sz = 0;

  do {
    sz = read (_fd.from, res + sz, 10-sz);
  } while (sz < 5);

  if (strncmp (res, "$ok_$", 5) == 0) {
    return 1;
  }
  else {
    return 0;
  }
}

int AbcApi::startSession (const char *v_in, const char *v_out, const char *name)
{
  Assert (_parent,"What?");

  if (write (_fd.to, "$new$", 5) < 0) {
    return 0;
  }
  if (write (_fd.to, name, strlen (name)) < 0) {
    return 0;
  }
  if (write (_fd.to, " ", 1) < 0) {
    return 0;
  }
  if (write (_fd.to, v_in, strlen (v_in)) < 0) {
    return 0;
  }
  if (write (_fd.to, " ", 1) < 0) {
    return 0;
  }
  if (write (_fd.to, v_out, strlen (v_out)) < 0) {
    return 0;
  }
  if (write (_fd.to, "$", 1) < 0) {
    return 0;
  }
  return _check_ok ();
}

int AbcApi::endSession ()
{
  Assert (_parent, "What?");
  
  if (write (_fd.to, "$end$", 5) < 0) {
    return 0;
  }
  
  return _check_ok();
}

int AbcApi::stdSynthesis ()
{
  Assert (_parent, "What?");

  if (!runCmd ("balance; rewrite -l; refactor -l; balance; rewrite -l; rewrite -lz; balance; refactor -lz; rewrite -lz; balance")) {
    return 0;
  }

  if (!runCmd ("strash; ifraig; dc2; strash; &get -n; &dch -f; &nf; &put; upsize; dnsize")) {
    return 0;
  }

  return 1;
}

int AbcApi::runTiming ()
{
  Assert (_parent, "What?");

  if (!runCmd ("stime -p")) {
    return 0;
  }

  return 1;
}

static int _get_id_array (char *buf, char *res, int *array)
{
  int pos = 0;
  while (isalnum (buf[pos]) || buf[pos] == '_') {
    pos++;
  }
  for (int i=0; i < pos; i++) {
    res[i] = buf[i];
  }
  res[pos] = '\0';
  if (buf[pos] == '[') {
    pos++;
    sscanf (buf+pos, "%d", array);
    while (isdigit (buf[pos]))
      pos++;
    if (buf[pos] != ']') {
      *array = -2;
    }
    pos++;
  }
  else {
    *array = -1;
  }
  return pos;
}
    
    

static void _parse_ports (char *buf, int nports, list_t *ports)
{
  int fresh = 1;
  char portname[128];
  int pos = 0;
  int local_idx = 0;

  for (int i=0; i < nports; i++) {
    int v;
    while (isspace (buf[pos])) pos++;
    if (sscanf (buf+pos, "%d=", &v) != 1) {
      fprintf (stderr, "Error parsing ABC output: %s\n", buf);
    }
    if (v != i) {
      fprintf (stderr, "Error parsing ABC output (v=%d, i=%d): %s\n", v, i, buf+pos);
      while (!isspace (buf[pos])) {
	pos++;
      }
      continue;
    }
    // skip to character after =
    while (buf[pos] != '=') pos++;
    pos++;

    pos += _get_id_array (buf+pos, portname, &v);
    if (v == -2) {
      fprintf (stderr, "Error parsing ABC output: %s\n", buf+pos);
      fresh = 1;
      local_idx = 0;
    }
    else if (v == -1) {
      // simple wire
      list_append (ports, Strdup (portname));
      list_iappend (ports, -1);
      fresh = 1;
      local_idx = 0;
    }
    else {
      if (v == 0) {
	if (local_idx != 0) {
	  list_iappend (ports, local_idx);
	  local_idx = 0;
	}
      }
      if (v != local_idx) {
	fprintf (stderr, "Error parsing ABC output: %s\n", buf+pos);
	fresh = 1;
	local_idx = 0;
	while (!isspace (buf[pos])) {
	  pos++;
	}
	continue;
      }
      if (local_idx == 0) {
	list_append (ports, Strdup (portname));
      }
      local_idx++;
    }
    while (!isspace (buf[pos])) {
      pos++;
    }
    continue;
  }
  if (local_idx > 0) {
    list_iappend (ports, local_idx);
  }
#if 0
  for (listitem_t *li = list_first (ports); li; li = list_next (li)) {
    fprintf (debug, "Port: %s ", (char *) list_value (li));
    if (list_next (li)) {
      li = list_next (li);
      fprintf (debug, "dim: %d\n", list_ivalue (li));
    }
    else {
      break;
    }
  }
#endif  
}

static bool my_fgets (char **buf, int *buf_max, FILE *fp)
{
  bool ret;
  int pos = 0;

  (*buf)[0] = '\0';
  (*buf)[*buf_max-1] = '\0';
  if (fgets (*buf, *buf_max, fp)) {
    while ((strlen ((*buf)+pos) == (*buf_max-1-pos)) &&
	   !feof (fp) && ((*buf)[*buf_max-2] != '\n')) {
      REALLOC (*buf, char, 2*(*buf_max));
      pos = *buf_max-1;
      *buf_max = 2*(*buf_max);
      if (!fgets (*buf + pos, *buf_max - pos, fp)) {
	break;
      }
    }
    return true;
  }
  else {
    return false;
  }
}

bool AbcApi::_endsession()
{
  int buf_max = char_buf_sz_abc;
  char *buf;

  if (!_pAbc) {
    return true;
  }

  MALLOC (buf, char, buf_max);

  snprintf (buf, char_buf_sz_abc, "write_verilog %s", _vout);
  _run_abc (buf);
  fflush (stdout);
  fflush (stderr);
  write (_logfd, "\n", 1);
  
  snprintf (buf, char_buf_sz_abc, "print_io; print_gates");
  _run_abc (buf);
  fflush (stdout);
  fflush (stderr);

  FILE *fp;
  FILE *vfp;
  snprintf (buf, char_buf_sz_abc, "%s.log", _vout);
  if (!(fp = fopen (buf, "r"))) {
    FREE (buf);
    return false;
  }

  list_t *iports = list_new ();
  list_t *oports = list_new ();

  while (my_fgets (&buf, &buf_max, fp)) {
    if (strncmp (buf, "Primary inputs ", 15) == 0) {
      int pos = 15;
      int inp;
      if (sscanf (buf + pos, "(%d):", &inp) != 1) {
	fprintf (stderr, "Error parsing ABC output: looking for (num)!\n");
      }
      while (buf[pos] && buf[pos] != ':') {
	pos++;
      }
      pos++;
      _parse_ports (buf+pos, inp, iports);
    }
    else if (strncmp (buf, "Primary outputs ", 16) == 0) {
      int pos = 16;
      int inp;
      if (sscanf (buf + pos, "(%d):", &inp) != 1) {
	fprintf (stderr, "Error parsing ABC output!\n");
      }
      while (buf[pos] && buf[pos] != ':') {
	pos++;
      }
      pos++;
      _parse_ports (buf+pos, inp, oports);
      break;
    }
    else {
      /* skip */
    }
  }
  fclose (fp);

  snprintf (buf, char_buf_sz_abc, "%s", _vout);
  vfp = fopen (buf, "a");
  fprintf (vfp, "module %s (", _name);

  int comma = 0;
  for (listitem_t *li = list_first (iports); li; li = list_next (li)) {
    if (comma) {
      fprintf (vfp, ", ");
    }
    comma = 1;
    fprintf (vfp, "%s", (char *) list_value (li));
    li = list_next (li);
  }
  
  for (listitem_t *li = list_first (oports); li; li = list_next (li)) {
    if (comma) {
      fprintf (vfp, ", ");
    }
    comma = 1;
    fprintf (vfp, "%s", (char *) list_value (li));
    li = list_next (li);
  }
  fprintf (vfp, ");\n");

  int vectorize = (config_get_int("synth.expropt.vectorize_all_ports") == 0) ? 0 : 1;

  for (listitem_t *li = list_first (iports); li; li = list_next (li)) {
    char *v = (char *) list_value (li);
    fprintf (vfp, "  input ");
    li = list_next (li);
    if (list_ivalue (li) > (1-vectorize)) {
      fprintf (vfp, "[%d:0] ", list_ivalue (li) - 1);
    }
    fprintf (vfp, "%s;\n", v);
  }
  
  for (listitem_t *li = list_first (oports); li; li = list_next (li)) {
    char *v = (char *) list_value (li);
    fprintf (vfp, "  output ");
    li = list_next (li);
    if (list_ivalue (li) > (1-vectorize)) {
      fprintf (vfp, "[%d:0] ", list_ivalue (li) - 1);
    }
    fprintf (vfp, "%s;\n", v);
  }

  // create instance!

  fprintf (vfp, " %stmp _passthru_ (", _name);
  comma = 0;

  for (listitem_t *li = list_first (iports); li; li = list_next (li)) {
    char *v = (char *) list_value (li);
    int dim;
    li = list_next (li);
    dim = list_ivalue (li);
      
    
    if (dim < (2-vectorize)) {
      if (comma) { fprintf (vfp, ", "); }
      comma = 1;
      fprintf (vfp, "%s", v);
    }
    else {
      for (int i=0; i < dim; i++) {
	if (comma) { fprintf (vfp, ", "); }
	comma = 1;
	fprintf (vfp, "%s[%d]", v, i);
      }
    }
    FREE (v);
  }
  
  for (listitem_t *li = list_first (oports); li; li = list_next (li)) {
    char *v = (char *) list_value (li);
    int dim;
    li = list_next (li);
    dim = list_ivalue (li);
      
    if (dim < (2-vectorize)) {
      if (comma) { fprintf (vfp, ", "); }
      comma = 1;
      fprintf (vfp, "%s", v);
    }
    else {
      for (int i=0; i < dim; i++) {
	if (comma) { fprintf (vfp, ", "); }
	comma = 1;
	fprintf (vfp, "%s[%d]", v, i);
      }
    }
    FREE (v);
  }

  list_free (iports);
  list_free (oports);
  fprintf (vfp, ");\n");
  
  fprintf (vfp, "\nendmodule\n\n");
  fclose (vfp);
  
  FREE (_name);
  FREE (_vin);
  FREE (_vout);
  fflush (stdout);
  fflush (stderr);
  close (_logfd);
  _pAbc = NULL;

  FREE (buf);
  Abc_Stop ();

  return true;
}


/*

  print_gates
  print_io


abc 12> print_gates
BUFX2      Fanin =  1   Instance =        2   Area =      48.00   100.00 %          0         0   A
_const0_   Fanin =  0   Instance =        2   Area =       0.00     0.00 %          0         0   CONST0
TOTAL                   Instance =        4   Area =      48.00   100.00 %   AbsDiff = 0
abc 12> print_io
Primary inputs (7):  0=a[0] 1=a[1] 2=a[2] 3=a[3] 4=c[0] 5=c[1] 6=c[2]
Primary outputs (4): 0=b[0] 1=b[1] 2=b[2] 3=b[3]
Latches (0):  
      */      
