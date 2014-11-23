/* lines2insns - determine the positions of machine instructions 
 * corresponding to memory accesses in the given source locations.
 * 
 * See 'lines2insns --help' for usage details. */
/* ========================================================================
 * Copyright (C) 2014, NTC IT ROSA
 *
 * Author: 
 *      Eugene A. Shatokhin
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 ======================================================================== */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <stdexcept>
#include <sstream>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <getopt.h>
#include <libgen.h>	/* basename() */

#include <libelf.h>
#include <dwarf.h>
#include <elfutils/libdwfl.h>

#include <kedr/asm/insn.h>

#include "config_lines2insns.h"
/* ====================================================================== */

#define APP_USAGE \
 "Usage:\n" \
 "\t" APP_NAME " [options] <module_file> < <source_locations> " \
 "> <insn_locations>\n" \
 "Execute \'" APP_NAME " --help\' for more information.\n\n"

#define APP_HELP \
 APP_NAME " [options] <module_file_with_debug_info>\n\n" \
 APP_NAME " reads the list of source locations for a kernel or a given\n" \
 "kernel module from stdin and outputs the list of machine instructions\n" \
 "corresponding to these locations that may access memory to stdout.\n\n" \
 "" \
 "Each input line must have the following format:\n\n" \
 "<path_to_source_file>:<line_no>[:{read|write}]\n\n" \
 "" \
 "Examples:\n\n" \
 "examples/sample_target/cfake.c:173:read\n" \
 "test.c:144\n" \
 "test2/test2.c:55:write\n\n" \
 "" \
 APP_NAME " reads the list of sections and debug info from the given\n" \
 "<module_file_with_debug_info> (it may be *.ko file for a kernel module\n" \
 "or vmlinu* for the kernel proper).\n" \
 "" \
 "The tool outputs the list of the corresponding machine instructions\n" \
 "in the following format, one per line:\n\n" \
 "[<module>:]{init|core}+0xoffset\n\n" \
 "" \
 "'init' and 'core' are the two main areas of the loaded kernel or\n" \
 "module. 'core' contains all code sections, without gaps, in the\n" \
 "order they appear in the ELF file (*.ko or vmlinu*) except *.init.text*\n" \
 "sections, which belong to 'init' area and go in order too.\n\n" \
 "" \
 "If the given file is a module, its name without .ko followed by\n" \
 "a colon (with '-' characters replaced by underscores like the kernel\n" \
 "does) will be output first. No such prefix will be output for the\n" \
 "kernel proper or the built-in modules.\n\n" \
 "" \
 "Example:\n\n" \
 "$ echo 'my-driver/main.c:126' | " APP_NAME " my-driver.ko\n" \
 "my_driver:core+0x568\n" \
 "my_driver:core+0x575\n\n" \
 "" \
 "Options:\n\n" \
 "" \
 "--help\n\t" \
 "Show this help and exit.\n\n" \
 "" \
 "--with-stack\n\t" \
 "By default, %esp/%rsp-based memory accesses are not output. This option\n\t" \
 "tells " APP_NAME " to output such instructions too.\n" \
 "" \
 "--with-locked\n\t" \
 "By default, locked operations are not output. This option tells\n\t" \
 APP_NAME " to output such instructions too.\n" \
 "" \
 "-v, --verbose\n\t" \
 "Output more messages to stderr about what is being done.\n"
/* ====================================================================== */
 
using namespace std;
/* ====================================================================== */

/* See the command-line options. */
static bool with_stack = false;
static bool with_locked = false;
static bool verbose = false;

/* Name of the kernel module (the part before .ko), empty for the kernel
 * proper (vmlinu*). */
static string kmodule_name;
static bool
is_kernel_image()
{
	return kmodule_name.empty();
}

/* The path to the module's file specified by the user. */
static string kmodule_path;
/* ====================================================================== */

static void
show_usage()
{
	cerr << APP_USAGE;
}

static void
show_help()
{
	cerr << APP_HELP;
}

static bool
extract_kmodule_name()
{
	static string kernel_image = "vmlinu";
	static string suffix = ".ko";
	
	/* basename() needs a string that can be changed. */
	char *str = strdup(kmodule_path.c_str());
	if (str == NULL) {
		cerr << "Not enough memory." << endl;
		return false;
	}
	
	string kmodule_file = basename(str);
	free(str);
	
	if (kmodule_file.compare(0, kernel_image.size(), 
				 kernel_image) == 0) {
		/* A kernel image. */
		return true;
	}
	
	if (kmodule_file.size() <= suffix.size() || 
	    kmodule_file.substr(
		    kmodule_file.size() - suffix.size()) != suffix) {
		cerr << "Invalid name of the kernel module: \"" 
			<< kmodule_file << "\"." << endl;
		return false;
	}
	
	kmodule_name = kmodule_file.substr(
		0, kmodule_file.size() - suffix.size());
	
	/* Within the kernel, all modules have dashes replaced with 
	 * underscores in their names. */
	for (size_t i = 0; i < kmodule_name.size(); ++i) {
		if (kmodule_name[i] == '-')
			kmodule_name[i] = '_';
	}
	return true;	
}

/* Process the command line arguments. Returns true if successful, false 
 * otherwise. */
static bool
process_args(int argc, char *argv[])
{
	int c;
	string module_dir = ".";
	
	struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"with-stack", no_argument, NULL, 's'},
		{"with-locked", no_argument, NULL, 'l'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};
	
	while (true) {
		int index = 0;
		c = getopt_long(argc, argv, "v", long_options, &index);
		if (c == -1)
			break;  /* all options have been processed */
		
		switch (c) {
		case 0:
			break;
		case 'h':
			show_help();
			exit(EXIT_SUCCESS);
			break;
		case 's':
			with_stack = true;
			break;
		case 'l':
			with_locked = true;
			break;
		case 'v':
			verbose = true;
			break;
		case '?':
			/* Unknown option, getopt_long() should have already 
			printed an error message. */
			return false;
		default: 
			assert(false); /* Should not get here. */
		}
	}
	
	if (optind == argc) {
		cerr << "Please specify the file of the kernel or module." 
			<< endl;
		return false;
	}
	
	kmodule_path = argv[optind];
	return extract_kmodule_name();
}
/* ====================================================================== */

/* It is not needed for libdw to search itself for the files with debug 
 * info. So, a stub is used instead of the default callback of this kind. */
static int
find_debuginfo(Dwfl_Module *mod __attribute__ ((unused)),
	       void **userdata __attribute__ ((unused)),
	       const char *modname __attribute__ ((unused)),
	       GElf_Addr base __attribute__ ((unused)),
	       const char *file_name __attribute__ ((unused)),
	       const char *debuglink_file __attribute__ ((unused)),
	       GElf_Word debuglink_crc __attribute__ ((unused)),
	       char **debuginfo_file_name __attribute__ ((unused)))
{
	return -1; /* as if found nothing */ 
}

/* .find_elf callback should not be called by libdw because we use 
 * dwfl_report_elf() to inform the library about the file with debug info.
 * The callback is still provided in case something in libdw expects it to 
 * be. */
static int
find_elf(Dwfl_Module *mod __attribute__ ((unused)),
	 void **userdata __attribute__ ((unused)),
	 const char *modname __attribute__ ((unused)),
	 Dwarf_Addr base __attribute__ ((unused)),
	 char **file_name __attribute__ ((unused)), 
	 Elf **elfp __attribute__ ((unused)))
{
	return -1; /* as if found nothing */ 
}

/* A wrapper around a handle to libdw/libdwfl that closes the handle on 
 * exit. */
class DwflWrapper
{
	Dwfl *dwfl_handle;
	Dwfl_Callbacks cb;

public:	
	/* An object to access DWARF info of the kernel module. */
	Dwfl_Module *dwfl_mod;
	int nrelocs;
	
public:
	DwflWrapper() 
	{
		cb.section_address = dwfl_offline_section_address;
		cb.find_debuginfo = find_debuginfo;
		cb.find_elf = find_elf;
		
		dwfl_handle = dwfl_begin(&cb);
		if (dwfl_handle == NULL) {
			throw runtime_error(string(
				"Failed to initialize DWARF facilities: ") +
				dwfl_errmsg(-1));
		}
	}
	
	~DwflWrapper()
	{
		dwfl_end(dwfl_handle);
	}
	
	Dwfl *get_handle() const
	{
		return dwfl_handle;
	}
};
/* ====================================================================== */

static void
process_elf_file(DwflWrapper &wr_dwfl,
		 void (*func)(DwflWrapper &, Elf *, int /* fd */))
{
	int fd;
	Elf *e;
	Elf_Kind ek;
	
	errno = 0;
	fd = open(kmodule_path.c_str(), O_RDONLY, 0);
	if (fd == -1) {
		ostringstream err;
		err << "Failed to open \"" << kmodule_path << "\": " <<
			strerror(errno) << endl;
		throw runtime_error(err.str());
	}
	
	e = elf_begin(fd, ELF_C_READ, NULL);
	if (e == NULL) {
		close(fd);
		ostringstream err;
		err << "elf_begin() failed for " << kmodule_path << ": " <<
			elf_errmsg(-1) << endl;
		throw runtime_error(err.str());
	}
	
	ek = elf_kind(e);
	if (ek != ELF_K_ELF) {
		elf_end(e);
		close(fd);
		throw runtime_error(
			string("Not an ELF object file: ") + kmodule_path);
	}
	
	try {
		func(wr_dwfl, e, fd);
	}
	catch (runtime_error &err) {
		elf_end(e);
		close(fd);
		throw;
	}
			
	elf_end(e);
	close(fd);
}

static void
load_dwarf_info(DwflWrapper &wr_dwfl, Elf * /* unused */, int fd)
{
	/* dwfl_report_*() functions close the file descriptor passed there 
	 * if successful, so make a duplicate first. */
	int dwfl_fd = dup(fd);
	if (dwfl_fd < 0) {
		throw runtime_error(
			"Failed to duplicate a file descriptor.");
	}
	
	wr_dwfl.dwfl_mod = my_dwfl_report_elf(
		wr_dwfl.get_handle(), kmodule_name.c_str(), 
		kmodule_path.c_str(), dwfl_fd, 0 /* base address */);
	
	if (wr_dwfl.dwfl_mod == NULL) {
		/* Not always an error but worth notifying the user. 
		 * Missing debug info, perhaps? */
		cerr << "No debug info is present in or can be loaded from "
			<< kmodule_path << ". " << dwfl_errmsg(-1) << endl;
		close(dwfl_fd);
		return;
	}
	
	dwfl_report_end(wr_dwfl.get_handle(), NULL, NULL);
	
	wr_dwfl.nrelocs = dwfl_module_relocations(wr_dwfl.dwfl_mod);
	if (wr_dwfl.nrelocs < 0) {
		throw runtime_error(string(
			"Failed to get the number of relocs: ") +
			dwfl_errmsg(-1));
	}
	else if (wr_dwfl.nrelocs == 0) {
		throw runtime_error(string(
			"No relocations - is section info valid?"));
	}
}
/* ====================================================================== */

enum EAccessType {
	AT_NONE = 0, /* means "not specified" */
	AT_READ,
	AT_WRITE
};

struct InsnInfo
{
	/* Offset of the instruction in the section it belongs to. */
	unsigned int offset;
	
	/* The access type of interest. */
	EAccessType atype;
	
	friend bool operator<(const InsnInfo &left, const InsnInfo &right)
	{
		if (left.offset < right.offset)
			return true;
		
		if (left.offset == right.offset && left.atype < right.atype)
			return true;
		
		return false;
	}
};



/* Instruction locations in the given ELF section and other data for that
 * section. */
struct SectionInfo
{
	/* Start offset of this section in the area ('init' or 'core') it 
	 * belongs to. */
	unsigned int start_offset;
	
	/* true if the section belongs to 'init' area, false otherwise. */
	bool belongs_to_init;
	
	/* The instructions in this section corresponding to the input 
	 * source lines. */
	set<InsnInfo> insns;
};

/* map: section_index => section_info */
typedef map<unsigned int, SectionInfo> TSectionMap;
static TSectionMap sections;
/* ====================================================================== */

static void
output_insns(const SectionInfo &si)
{
	set<InsnInfo>::const_iterator it;
	for (it = si.insns.begin(); it != si.insns.end(); ++it) {
		if (!is_kernel_image())
			cout << kmodule_name << ":";
		cout << (si.belongs_to_init ? "init+0x" : "core+0x") << hex
			<< si.start_offset + it->offset << dec << endl;
	}
}
/* ====================================================================== */

/* [NB] Some portions of this code are based on "line2addr" test from 
 * elfutils. */
static void 
find_insns(DwflWrapper &wr_dwfl, string src_file, int line, EAccessType at)
{
	Dwfl_Line **lines = NULL;
	size_t nlines = 0;
	int col = 0;
	
	/* Create the prefix for the messages first. */
	ostringstream os;
	os << src_file << ":" << line << ":" << col << ": ";
	string prefix = os.str();
	
	int ret = dwfl_module_getsrc_file(
		wr_dwfl.dwfl_mod, src_file.c_str(), line, col, 
		&lines, &nlines);
	
	if (ret != 0) {
		if (verbose) {
			cerr << prefix << "no data found." << endl;
		}
		return;
	}
	
	for (size_t inner = 0; inner < nlines; ++inner) {
		Dwarf_Addr addr;
		const char *file = dwfl_lineinfo(
			lines[inner], &addr, &line, &col, NULL, NULL);
		if (file == NULL)
			continue;
		
		/* Find the section and the offset in it. */
		int idx = dwfl_module_relocate_address(wr_dwfl.dwfl_mod, 
						       &addr);
		if (idx < 0) {
			ostringstream err;
			err << prefix
				<< "failed to relocate the address: "
				<< dwfl_errmsg(-1);
			throw runtime_error(err.str());
		}
		
		GElf_Word sec_idx = (GElf_Word)(-1);
		const char *secname = dwfl_module_relocation_info (
			wr_dwfl.dwfl_mod, idx, &sec_idx);
		if (secname == NULL || secname[0] == '\0' || 
		    sec_idx == (GElf_Word)(-1)) {
			ostringstream err;
			err << prefix
			<< "failed to find the name of the ELF section";
			throw runtime_error(err.str());
		}
		/* What we've got now:
		 * addr - it is now the offset into the section,
		 * secname - name of the section,
		 * sec_idx - index of the section in the ELF file. */
		InsnInfo ii = {
			.offset = (unsigned int)addr,
			.atype = at
		};
		sections[(unsigned int)sec_idx].insns.insert(ii);
	}
	free(lines); /* See line2addr test in elfutils */
}

static void 
process_input_line(DwflWrapper &wr_dwfl, const string &str)
{
	/* trim first */
	static string sep = " \t\n\r";
	size_t first = str.find_first_not_of(sep);
	if (first == string::npos)
		return; /* empty string */

	size_t last = str.find_last_not_of(sep);
	assert(first <= last);
	
	string s = str.substr(first, last - first + 1);
	size_t pos = s.find_first_of(':');
	if (pos == string::npos) {
		throw runtime_error(string("Invalid input line: ") + str);
	}
	
	EAccessType at = AT_NONE;
	string fpath = s.substr(0, pos);
	string str_line;
	
	s = s.substr(pos + 1);
	if (s.empty())
		throw runtime_error(string("Invalid input line: ") + str);
	
	pos = s.find_first_of(':');
	if (pos == string::npos) {
		/* No access type specified */
		str_line = s;
	}
	else {
		str_line = s.substr(0, pos);
		string str_atype = s.substr(pos + 1);
		
		if (str_atype == "read") {
			at = AT_READ;
		}
		else if (str_atype == "write") {
			at = AT_WRITE;
		}
		else {
			throw runtime_error(string(
				"Invalid access type in the input line: ") 
				+ str);
		}
	}
	
	char *rest = NULL;
	int line_no = (int)strtol(str_line.c_str(), &rest, 10);
	if (rest == NULL || rest[0] != 0)
		throw runtime_error(string("Invalid input line: ") + str);
	
	find_insns(wr_dwfl, fpath, line_no, at);
}
/* ====================================================================== */

int
main(int argc, char *argv[])
{
	if (argc == 1) {
		show_usage();
		return EXIT_FAILURE;
	}
	
	if (elf_version(EV_CURRENT) == EV_NONE) {
		cerr << "Failed to initialize libelf: " << elf_errmsg(-1) 
			<< endl;
		return EXIT_FAILURE;
	}
	
	if (!process_args(argc, argv))
		return EXIT_FAILURE;
	
	if (verbose) {
		cerr << "With stack: " << with_stack << endl;
		cerr << "With locked: " << with_locked << endl;
		cerr << "Module path: " << kmodule_path << endl;
		cerr << "Is kernel image? " << is_kernel_image() << endl;
		cerr << "Module name: " << kmodule_name << endl;
	}
	
	try {
		DwflWrapper wr_dwfl;
		process_elf_file(wr_dwfl, load_dwarf_info);
		
		/* Process the input and populate 'sections' map and the 
		 * sets of instructions to inspect. */
		string str;
		while (getline(cin, str)) {
			process_input_line(wr_dwfl, str);
		}
		
		/* Find which area each section belongs to and what offset
		 * the section has there. */
		// TODO: find which area each section belongs to, find the start offset of the section

		/* Filter the insns */
		// TODO: decode the insns in the sections, filter out the insns that do not access memory (remove them from the sets)
		// TODO: if requested, filter out %esp/%rsp-based accesses and locked ops.
		
		/* Output the results, sorted by offset, init area first. */
		TSectionMap::iterator it;
		for (it = sections.begin(); it != sections.end(); ++it) {
			if (it->second.belongs_to_init)
				output_insns(it->second);
		}
		
		/* 'core' area */
		for (it = sections.begin(); it != sections.end(); ++it) {
			if (!it->second.belongs_to_init)
				output_insns(it->second);
		}
	}
	catch (runtime_error &e) {
		cerr << e.what() << endl;
		return EXIT_FAILURE;
	}
	return 0;
}