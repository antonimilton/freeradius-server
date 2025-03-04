/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file cf_file.c
 * @brief Read the radiusd.conf file.
 *
 * @note  Yep I should learn to use lex & yacc, or at least
 *	  write a decent parser. I know how to do that, really :)
 *	  miquels@cistron.nl
 *
 * @copyright 2017 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2000,2006 The FreeRADIUS server project
 * @copyright 2000 Miquel van Smoorenburg (miquels@cistron.nl)
 * @copyright 2000 Alan DeKok (aland@freeradius.org)
 */
RCSID("$Id$")

#include <freeradius-devel/server/cf_file.h>
#include <freeradius-devel/server/cf_priv.h>
#include <freeradius-devel/server/log.h>
#include <freeradius-devel/server/tmpl.h>
#include <freeradius-devel/server/util.h>
#include <freeradius-devel/server/virtual_servers.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/file.h>
#include <freeradius-devel/util/misc.h>
#include <freeradius-devel/util/perm.h>
#include <freeradius-devel/util/syserror.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_DIRENT_H
#  include <dirent.h>
#endif

#ifdef HAVE_GLOB_H
#  include <glob.h>
#endif

#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif

#include <ctype.h>
#include <fcntl.h>

#include <freeradius-devel/server/main_config.h>

bool check_config = false;
static uid_t conf_check_uid = (uid_t)-1;
static gid_t conf_check_gid = (gid_t)-1;

typedef enum conf_property {
	CONF_PROPERTY_INVALID = 0,
	CONF_PROPERTY_NAME,
	CONF_PROPERTY_INSTANCE,
} CONF_PROPERTY;

static fr_table_num_sorted_t const conf_property_name[] = {
	{ L("instance"),	CONF_PROPERTY_INSTANCE	},
	{ L("name"),	CONF_PROPERTY_NAME	}
};
static size_t conf_property_name_len = NUM_ELEMENTS(conf_property_name);

typedef enum {
	CF_STACK_FILE = 0,
#ifdef HAVE_DIRENT_H
	CF_STACK_DIR,
#endif
#ifdef HAVE_GLOB_H
	CF_STACK_GLOB
#endif
} cf_stack_file_t;

#define MAX_STACK (32)
typedef struct {
	cf_stack_file_t type;

	char const     	*filename;		//!< filename we're reading
	int		lineno;			//!< line in that filename

	union {
		struct {
			FILE		*fp;		//!< FP we're reading
		};

#ifdef HAVE_DIRENT_H
		struct {
			fr_heap_t	*heap;		//!< sorted heap of files
			char		*directory;	//!< directory name we're reading
		};
#endif

#ifdef HAVE_GLOB_H
		struct {
			size_t		gl_current;
			glob_t		glob;		//! reading glob()
			bool		required;
		};
#endif
	};

	CONF_SECTION	*parent;		//!< which started this file
	CONF_SECTION	*current;		//!< sub-section we're reading
	CONF_SECTION	*special;		//!< map / update section

	bool		require_edits;		//!< are we required to do edits?

	int		braces;
	bool		from_dir;		//!< this file was read from $include foo/
} cf_stack_frame_t;

/*
 *	buff[0] is the data we read from the file
 *	buff[1] is name
 *	buff[2] is name2 OR value for pair
 *	buff[3] is a temporary buffer
 */
typedef struct {
	char		**buff;			//!< buffers for reading / parsing
	size_t		bufsize;		//!< size of the buffers
	int		depth;			//!< stack depth
	char const	*ptr;			//!< current parse pointer
	char		*fill;			//!< where we start filling the buffer from
	cf_stack_frame_t frame[MAX_STACK];	//!< stack frames
} cf_stack_t;


static inline CC_HINT(always_inline) int cf_tmpl_rules_verify(CONF_SECTION *cs, tmpl_rules_t const *rules)
{
	if (cf_section_has_parent(cs, "policy", NULL)) {
		if (!fr_cond_assert_msg(!rules->attr.dict_def || (rules->attr.dict_def == fr_dict_internal()),
					"Protocol dictionary must be NULL not %s",
					fr_dict_root(rules->attr.dict_def)->name)) return -1;

	} else {
		if (!fr_cond_assert_msg(rules->attr.dict_def, "No protocol dictionary set")) return -1;
		if (!fr_cond_assert_msg(rules->attr.dict_def != fr_dict_internal(), "rules->attr.dict_def must not be the internal dictionary")) return -1;
	}

	if (!fr_cond_assert_msg(!rules->attr.allow_foreign, "rules->allow_foreign must be false")) return -1;
	if (!fr_cond_assert_msg(!rules->at_runtime, "rules->at_runtime must be false")) return -1;

	return 0;
}

#define RULES_VERIFY(_cs, _rules) if (cf_tmpl_rules_verify(_cs, _rules) < 0) return NULL

static ssize_t fr_skip_xlat(char const *start, char const *end);

/*
 *	Expand the variables in an input string.
 *
 *	Input and output should be two different buffers, as the
 *	output may be longer than the input.
 */
char const *cf_expand_variables(char const *cf, int lineno,
				CONF_SECTION *outer_cs,
				char *output, size_t outsize,
				char const *input, ssize_t inlen, bool *soft_fail)
{
	char *p;
	char const *end, *next, *ptr;
	CONF_SECTION const *parent_cs;
	char name[8192];

	if (soft_fail) *soft_fail = false;

	/*
	 *	Find the master parent conf section.
	 *	We can't use main_config->root_cs, because we're in the
	 *	process of re-building it, and it isn't set up yet...
	 */
	parent_cs = cf_root(outer_cs);

	p = output;
	ptr = input;

	if (inlen < 0) {
		end = NULL;
	} else {
		end = input + inlen;
	}

	/*
	 *	Note that this CAN go over "end" if the input string
	 *	is malformed.  e.g. pass "${foo.bar}", and pass
	 *	"inlen=5".  Well, too bad.
	 */
	while (*ptr && (!end || (ptr < end))) {
		/*
		 *	Ignore anything other than "${"
		 */
		if ((*ptr == '$') && (ptr[1] == '{')) {
			CONF_ITEM *ci;
			CONF_PAIR *cp;
			char *q;
			ssize_t len;

			len = fr_skip_xlat(ptr, end);
			if (len <= 0) {
				ERROR("%s[%d]: Failed parsing variable expansion '%s''",
				      cf, lineno, input);
				return NULL;
			}

			next = ptr + len;
			ptr += 2;

			/*
			 *	Can't really happen because input lines are
			 *	capped at 8k, which is sizeof(name)
			 */
			if ((size_t) len >= sizeof(name)) {
				ERROR("%s[%d]: Reference string is too large",
				      cf, lineno);
				return NULL;
			}

			memcpy(name, ptr, len - 3);
			name[len - 3] = '\0';

			/*
			 *	Read configuration value from a file.
			 *
			 *	Note that this is "read binary data", and the contents aren't stripped of
			 *	CRLF.
			 */
			if (name[0] == '/') {
				int fd = open(name, O_RDONLY);
				struct stat buf;

				if (fd < 0) {
					ERROR("%s[%d]: Reference \"${%s}\" failed opening file - %s", cf, lineno, name, fr_syserror(errno));
					return NULL;
				}

				if (fstat(fd, &buf) < 0) {
				fail_fd:
					close(fd);
					ERROR("%s[%d]: Reference \"${%s}\" failed reading file - %s", cf, lineno, name, fr_syserror(errno));
					return NULL;
				}

				if (buf.st_size >= ((output + outsize) - p)) {
					close(fd);
					ERROR("%s[%d]: Reference \"${%s}\" file is too large (%zd >= %zd)", cf, lineno, name,
					      (size_t) buf.st_size, ((output + outsize) - p));
					return NULL;
				}

				len = read(fd, p, (output + outsize) - p);
				if (len < 0) goto fail_fd;

				close(fd);
				p += len;
				*p = '\0';
				ptr = next;
				goto check_eos;
			}

			q = strchr(name, ':');
			if (q) {
				*(q++) = '\0';
			}

			ci = cf_reference_item(parent_cs, outer_cs, name);
			if (!ci) {
				if (soft_fail) *soft_fail = true;
				ERROR("%s[%d]: Reference \"${%s}\" not found", cf, lineno, name);
				return NULL;
			}

			/*
			 *	The expansion doesn't refer to another item or section
			 *	it's the property of a section.
			 */
			if (q) {
				CONF_SECTION *find = cf_item_to_section(ci);

				if (ci->type != CONF_ITEM_SECTION) {
					ERROR("%s[%d]: Can only reference properties of sections", cf, lineno);
					return NULL;
				}

				switch (fr_table_value_by_str(conf_property_name, q, CONF_PROPERTY_INVALID)) {
				case CONF_PROPERTY_NAME:
					strcpy(p, find->name1);
					break;

				case CONF_PROPERTY_INSTANCE:
					strcpy(p, find->name2 ? find->name2 : find->name1);
					break;

				default:
					ERROR("%s[%d]: Invalid property '%s'", cf, lineno, q);
					return NULL;
				}
				p += strlen(p);
				ptr = next;

			} else if (ci->type == CONF_ITEM_PAIR) {
				/*
				 *  Substitute the value of the variable.
				 */
				cp = cf_item_to_pair(ci);

				/*
				 *	If the thing we reference is
				 *	marked up as being expanded in
				 *	pass2, don't expand it now.
				 *	Let it be expanded in pass2.
				 */
				if (cp->pass2) {
					if (soft_fail) *soft_fail = true;

					ERROR("%s[%d]: Reference \"%s\" points to a variable which has not been expanded.",
					      cf, lineno, input);
					return NULL;
				}

				if (!cp->value) {
					ERROR("%s[%d]: Reference \"%s\" has no value",
					      cf, lineno, input);
					return NULL;
				}

				if (p + strlen(cp->value) >= output + outsize) {
					ERROR("%s[%d]: Reference \"%s\" is too long",
					      cf, lineno, input);
					return NULL;
				}

				strcpy(p, cp->value);
				p += strlen(p);
				ptr = next;

			} else if (ci->type == CONF_ITEM_SECTION) {
				CONF_SECTION *subcs;

				/*
				 *	Adding an entry again to a
				 *	section is wrong.  We don't
				 *	want an infinite loop.
				 */
				if (cf_item_to_section(ci->parent) == outer_cs) {
					ERROR("%s[%d]: Cannot reference different item in same section", cf, lineno);
					return NULL;
				}

				/*
				 *	Copy the section instead of
				 *	referencing it.
				 */
				subcs = cf_item_to_section(ci);
				subcs = cf_section_dup(outer_cs, outer_cs, subcs,
						       cf_section_name1(subcs), cf_section_name2(subcs),
						       false);
				if (!subcs) {
					ERROR("%s[%d]: Failed copying reference %s", cf, lineno, name);
					return NULL;
				}

				cf_filename_set(subcs, ci->filename);
				cf_lineno_set(subcs, ci->lineno);

				ptr = next;

			} else {
				ERROR("%s[%d]: Reference \"%s\" type is invalid", cf, lineno, input);
				return NULL;
			}
		} else if (strncmp(ptr, "$ENV{", 5) == 0) {
			char *env;

			ptr += 5;

			/*
			 *	Look for trailing '}', and log a
			 *	warning for anything that doesn't match,
			 *	and exit with a fatal error.
			 */
			next = strchr(ptr, '}');
			if (next == NULL) {
				*p = '\0';
				ERROR("%s[%d]: Environment variable expansion missing }",
				     cf, lineno);
				return NULL;
			}

			/*
			 *	Can't really happen because input lines are
			 *	capped at 8k, which is sizeof(name)
			 */
			if ((size_t) (next - ptr) >= sizeof(name)) {
				ERROR("%s[%d]: Environment variable name is too large",
				      cf, lineno);
				return NULL;
			}

			memcpy(name, ptr, next - ptr);
			name[next - ptr] = '\0';

			/*
			 *	Get the environment variable.
			 *	If none exists, then make it an empty string.
			 */
			env = getenv(name);
			if (env == NULL) {
				*name = '\0';
				env = name;
			}

			if (p + strlen(env) >= output + outsize) {
				ERROR("%s[%d]: Reference \"%s\" is too long",
				      cf, lineno, input);
				return NULL;
			}

			strcpy(p, env);
			p += strlen(p);
			ptr = next + 1;

		} else {
			/*
			 *	Copy it over verbatim.
			 */
			*(p++) = *(ptr++);
		}

	check_eos:
		if (p >= (output + outsize)) {
			ERROR("%s[%d]: Reference \"%s\" is too long",
			      cf, lineno, input);
			return NULL;
		}
	} /* loop over all of the input string. */

	*p = '\0';

	return output;
}

/*
 *	Merge the template so everything else "just works".
 */
static bool cf_template_merge(CONF_SECTION *cs, CONF_SECTION const *template)
{
	if (!cs || !template) return true;

	cs->template = NULL;

	/*
	 *	Walk over the template, adding its' entries to the
	 *	current section.  But only if the entries don't
	 *	already exist in the current section.
	 */
	cf_item_foreach(&template->item, ci) {
		if (ci->type == CONF_ITEM_PAIR) {
			CONF_PAIR *cp1, *cp2;

			/*
			 *	It exists, don't over-write it.
			 */
			cp1 = cf_item_to_pair(ci);
			if (cf_pair_find(cs, cp1->attr)) {
				continue;
			}

			/*
			 *	Create a new pair with all of the data
			 *	of the old one.
			 */
			cp2 = cf_pair_dup(cs, cp1);
			if (!cp2) return false;

			cf_filename_set(cp2, cp1->item.filename);
			cf_lineno_set(cp2, cp1->item.lineno);
			continue;
		}

		if (ci->type == CONF_ITEM_SECTION) {
			CONF_SECTION *subcs1, *subcs2;

			subcs1 = cf_item_to_section(ci);
			fr_assert(subcs1 != NULL);

			subcs2 = cf_section_find(cs, subcs1->name1, subcs1->name2);
			if (subcs2) {
				/*
				 *	sub-sections get merged.
				 */
				if (!cf_template_merge(subcs2, subcs1)) {
					return false;
				}
				continue;
			}

			/*
			 *	Our section doesn't have a matching
			 *	sub-section.  Copy it verbatim from
			 *	the template.
			 */
			subcs2 = cf_section_dup(cs, cs, subcs1,
						cf_section_name1(subcs1), cf_section_name2(subcs1),
						false);
			if (!subcs2) return false;

			cf_filename_set(subcs2, subcs1->item.filename);
			cf_lineno_set(subcs2, subcs1->item.lineno);
			continue;
		}

		/* ignore everything else */
	}

	return true;
}

/*
 *	Functions for tracking files by inode
 */
static int8_t _inode_cmp(void const *one, void const *two)
{
	cf_file_t const *a = one, *b = two;

	CMP_RETURN(a, b, buf.st_dev);

	return CMP(a->buf.st_ino, b->buf.st_ino);
}

static int cf_file_open(CONF_SECTION *cs, char const *filename, bool from_dir, FILE **fp_p)
{
	cf_file_t *file;
	CONF_SECTION *top;
	fr_rb_tree_t *tree;
	int fd = -1;
	FILE *fp;

	top = cf_root(cs);
	tree = cf_data_value(cf_data_find(top, fr_rb_tree_t, "filename"));
	fr_assert(tree);

	/*
	 *	If we're including a wildcard directory, then ignore
	 *	any files the users has already explicitly loaded in
	 *	that directory.
	 */
	if (from_dir) {
		cf_file_t my_file;
		char const *r;
		int dirfd;

		my_file.cs = cs;
		my_file.filename = filename;

		/*
		 *	Find and open the directory containing filename so we can use
		 * 	 the "at"functions to avoid time of check/time of use insecurities.
		 */
		if (fr_dirfd(&dirfd, &r, filename) < 0) {
			ERROR("Failed to open directory containing %s", filename);
			return -1;
		}

		if (fstatat(dirfd, r, &my_file.buf, 0) < 0) goto error;

		file = fr_rb_find(tree, &my_file);

		/*
		 *	The file was previously read by including it
		 *	explicitly.  After it was read, we have a
		 *	$INCLUDE of the directory it is in.  In that
		 *	case, we ignore the file.
		 *
		 *	However, if the file WAS read from a wildcard
		 *	$INCLUDE directory, then we read it again.
		 */
		if (file && !file->from_dir) {
			if (dirfd != AT_FDCWD) close(dirfd);
			return 1;
		}
		fd = openat(dirfd, r, O_RDONLY, 0);
		fp = (fd < 0) ? NULL : fdopen(fd, "r");
		if (dirfd != AT_FDCWD) close(dirfd);
	} else {
		fp = fopen(filename, "r");
		if (fp) fd = fileno(fp);
	}

	DEBUG2("including configuration file %s", filename);

	if (!fp) {
	error:
		ERROR("Unable to open file \"%s\": %s", filename, fr_syserror(errno));
		return -1;
	}

	MEM(file = talloc(tree, cf_file_t));

	file->filename = talloc_strdup(file, filename);	/* The rest of the code expects this to be a talloced buffer */
	file->cs = cs;
	file->from_dir = from_dir;

	if (fstat(fd, &file->buf) == 0) {
#ifdef S_IWOTH
		if ((file->buf.st_mode & S_IWOTH) != 0) {
			ERROR("Configuration file %s is globally writable.  "
			      "Refusing to start due to insecure configuration.", filename);

			fclose(fp);
			talloc_free(file);
			return -1;
		}
#endif
	}

	/*
	 *	We can include the same file twice.  e.g. when it
	 *	contains common definitions, such as for SQL.
	 *
	 *	Though the admin should really use templates for that.
	 */
	if (!fr_rb_insert(tree, file)) talloc_free(file);

	*fp_p = fp;
	return 0;
}

/** Do some checks on the file as an "input" file.  i.e. one read by a module.
 *
 * @note Must be called with super user privileges.
 *
 * @param cp		currently being processed.
 * @param check_perms	If true - will return false if file is world readable,
 *			or not readable by the unprivileged user/group.
 * @return
 *	- true if permissions are OK, or the file exists.
 *	- false if the file does not exist or the permissions are incorrect.
 */
bool cf_file_check(CONF_PAIR *cp, bool check_perms)
{
	cf_file_t	*file;
	CONF_SECTION	*top;
	fr_rb_tree_t	*tree;
	char const 	*filename = cf_pair_value(cp);
	int		fd = -1;

	top = cf_root(cp);
	tree = cf_data_value(cf_data_find(top, fr_rb_tree_t, "filename"));
	if (!tree) return false;

	file = talloc(tree, cf_file_t);
	if (!file) return false;

	file->filename = talloc_strdup(file, filename);	/* The rest of the code expects this to be talloced */
	file->cs = cf_item_to_section(cf_parent(cp));

	if (!check_perms) {
		if (stat(filename, &file->buf) < 0) {
		perm_error:
			fr_perm_file_error(errno);	/* Write error and euid/egid to error buff */
			cf_log_perr(cp, "Unable to open file \"%s\"", filename);
		error:
			if (fd >= 0) close(fd);
			talloc_free(file);
			return false;
		}
		talloc_free(file);
		return true;
	}

	/*
	 *	This really does seem to be the simplest way
	 *	to check that the file can be read with the
	 *	euid/egid.
	 */
	{
		uid_t euid = (uid_t)-1;
		gid_t egid = (gid_t)-1;

		if ((conf_check_gid != (gid_t)-1) && ((egid = getegid()) != conf_check_gid)) {
			if (setegid(conf_check_gid) < 0) {
				cf_log_perr(cp, "Failed setting effective group ID (%i) for file check: %s",
					    conf_check_gid, fr_syserror(errno));
				goto error;
			}
		}
		if ((conf_check_uid != (uid_t)-1) && ((euid = geteuid()) != conf_check_uid)) {
			if (seteuid(conf_check_uid) < 0) {
				cf_log_perr(cp, "Failed setting effective user ID (%i) for file check: %s",
					    conf_check_uid, fr_syserror(errno));
				goto error;
			}
		}
		fd = open(filename, O_RDONLY);
		if (conf_check_uid != euid) {
			if (seteuid(euid) < 0) {
				cf_log_perr(cp, "Failed restoring effective user ID (%i) after file check: %s",
					    euid, fr_syserror(errno));
				goto error;
			}
		}
		if (conf_check_gid != egid) {
			if (setegid(egid) < 0) {
				cf_log_perr(cp, "Failed restoring effective group ID (%i) after file check: %s",
					    egid, fr_syserror(errno));
				goto error;
			}
		}
	}

	if (fd < 0) goto perm_error;
	if (fstat(fd, &file->buf) < 0) goto perm_error;

	close(fd);

#ifdef S_IWOTH
	if ((file->buf.st_mode & S_IWOTH) != 0) {
		cf_log_perr(cp, "Configuration file %s is globally writable.  "
		            "Refusing to start due to insecure configuration.", filename);
		talloc_free(file);
		return false;
	}
#endif

	/*
	 *	It's OK to include the same file twice...
	 */
	if (!fr_rb_insert(tree, file)) talloc_free(file);

	return true;
}

/*
 *	Do variable expansion in pass2.
 *
 *	This is a breadth-first expansion.  "deep
 */
int cf_section_pass2(CONF_SECTION *cs)
{
	cf_item_foreach(&cs->item, ci) {
		char const	*value;
		CONF_PAIR	*cp;
		char		buffer[8192];

		if (ci->type != CONF_ITEM_PAIR) continue;

		cp = cf_item_to_pair(ci);
		if (!cp->value || !cp->pass2) continue;

		fr_assert((cp->rhs_quote == T_BARE_WORD) ||
			  (cp->rhs_quote == T_HASH) ||
			  (cp->rhs_quote == T_DOUBLE_QUOTED_STRING) ||
			  (cp->rhs_quote == T_BACK_QUOTED_STRING));

		value = cf_expand_variables(ci->filename, ci->lineno, cs, buffer, sizeof(buffer), cp->value, -1, NULL);
		if (!value) return -1;

		talloc_const_free(cp->value);
		cp->value = talloc_typed_strdup(cp, value);
	}

	cf_item_foreach(&cs->item, ci) {
		if (ci->type != CONF_ITEM_SECTION) continue;

		if (cf_section_pass2(cf_item_to_section(ci)) < 0) return -1;
	}

	return 0;
}


static char const *cf_local_file(char const *base, char const *filename,
				 char *buffer, size_t bufsize)
{
	size_t	dirsize;
	char	*p;

	strlcpy(buffer, base, bufsize);

	p = strrchr(buffer, FR_DIR_SEP);
	if (!p) return filename;
	if (p[1]) {		/* ./foo */
		p[1] = '\0';
	}

	dirsize = (p - buffer) + 1;

	if ((dirsize + strlen(filename)) >= bufsize) {
		return NULL;
	}

	strlcpy(p + 1, filename, bufsize - dirsize);

	return buffer;
}

/*
 *	Like gettoken(), but uses the new API which seems better for a
 *	host of reasons.
 */
static int cf_get_token(CONF_SECTION *parent, char const **ptr_p, fr_token_t *token, char *buffer, size_t buflen,
			char const *filename, int lineno)
{
	char const *ptr = *ptr_p;
	ssize_t slen;
	char const *out;
	size_t outlen;

	/*
	 *	Discover the string content, returning what kind of
	 *	string it is.
	 *
	 *	Don't allow casts or regexes.  But do allow bare
	 *	%{...} expansions.
	 */
	slen = tmpl_preparse(&out, &outlen, ptr, strlen(ptr), token, NULL, false, true);
	if (slen <= 0) {
		char *spaces, *text;

		fr_canonicalize_error(parent, &spaces, &text, slen, ptr);

		ERROR("%s[%d]: %s", filename, lineno, text);
		ERROR("%s[%d]: %s^ - %s", filename, lineno, spaces, fr_strerror());

		talloc_free(spaces);
		talloc_free(text);
		return -1;
	}

	if ((size_t) slen >= buflen) {
		ERROR("%s[%d]: Name is too long", filename, lineno);
		return -1;
	}

	/*
	 *	Unescape it or copy it verbatim as necessary.
	 */
	if (!cf_expand_variables(filename, lineno, parent, buffer, buflen,
				 out, outlen, NULL)) {
		return -1;
	}

	ptr += slen;
	fr_skip_whitespace(ptr);

	*ptr_p = ptr;
	return 0;
}

typedef struct cf_file_heap_t {
	char const		*filename;
	fr_heap_index_t		heap_id;
} cf_file_heap_t;

static int8_t filename_cmp(void const *one, void const *two)
{
	int ret;
	cf_file_heap_t const *a = one;
	cf_file_heap_t const *b = two;

	ret = strcmp(a->filename, b->filename);
	return CMP(ret, 0);
}


static int process_include(cf_stack_t *stack, CONF_SECTION *parent, char const *ptr, bool required, bool relative)
{
	char const *value;
	cf_stack_frame_t *frame = &stack->frame[stack->depth];

	/*
	 *	Can't do this inside of update / map.
	 */
	if (frame->special) {
		ERROR("%s[%d]: Parse error: Invalid location for $INCLUDE",
		      frame->filename, frame->lineno);
		return -1;
	}

	fr_skip_whitespace(ptr);

	/*
	 *	Grab all of the non-whitespace text.
	 */
	value = ptr;
	while (*ptr && !isspace((uint8_t) *ptr)) ptr++;

	/*
	 *	We're OK with whitespace after the filename.
	 */
	fr_skip_whitespace(ptr);

	/*
	 *	But anything else after the filename is wrong.
	 */
	if (*ptr) {
		ERROR("%s[%d]: Unexpected text after $INCLUDE", frame->filename, frame->lineno);
		return -1;
	}

	/*
	 *	Hack for ${confdir}/foo
	 */
	if (*value == '$') relative = false;

	value = cf_expand_variables(frame->filename, frame->lineno, parent, stack->buff[1], stack->bufsize,
				    value, ptr - value, NULL);
	if (!value) return -1;

	if (!FR_DIR_IS_RELATIVE(value)) relative = false;

	if (relative) {
		value = cf_local_file(frame->filename, value, stack->buff[2], stack->bufsize);
		if (!value) {
			ERROR("%s[%d]: Directories too deep", frame->filename, frame->lineno);
			return -1;
		}
	}

	if (strchr(value, '*') != 0) {
#ifndef HAVE_GLOB_H
		ERROR("%s[%d]: Filename globbing is not supported.", frame->filename, frame->lineno);
		return -1;
#else
		stack->depth++;
		frame = &stack->frame[stack->depth];
		memset(frame, 0, sizeof(*frame));

		frame->type = CF_STACK_GLOB;
		frame->required = required;
		frame->parent = parent;
		frame->current = parent;
		frame->special = NULL;

		/*
		 *	For better debugging.
		 */
		frame->filename = frame[-1].filename;
		frame->lineno = frame[-1].lineno;

		if (glob(value, GLOB_ERR | GLOB_NOESCAPE, NULL, &frame->glob) < 0) {
			stack->depth--;
			ERROR("%s[%d]: Failed expanding '%s' - %s", frame->filename, frame->lineno,
				value, fr_syserror(errno));
			return -1;
		}

		/*
		 *	If nothing matches, that may be an error.
		 */
		if (frame->glob.gl_pathc == 0) {
			if (!required) {
				stack->depth--;
				return 0;
			}

			ERROR("%s[%d]: Failed expanding '%s' - No matchin files", frame->filename, frame->lineno,
			      value);
			return -1;
		}

		return 1;
#endif
	}

	/*
	 *	Allow $-INCLUDE for directories, too.
	 */
	if (!required) {
		struct stat statbuf;

		if (stat(value, &statbuf) < 0) {
			WARN("Not including file %s: %s", value, fr_syserror(errno));
			return 0;
		}
	}

	/*
	 *	The filename doesn't end in '/', so it must be a file.
	 */
	if (value[strlen(value) - 1] != '/') {
		if ((stack->depth + 1) >= MAX_STACK) {
			ERROR("%s[%d]: Directories too deep", frame->filename, frame->lineno);
			return -1;
		}

		stack->depth++;
		frame = &stack->frame[stack->depth];
		memset(frame, 0, sizeof(*frame));

		frame->type = CF_STACK_FILE;
		frame->fp = NULL;
		frame->parent = parent;
		frame->current = parent;
		frame->filename = talloc_strdup(frame->parent, value);
		frame->special = NULL;
		return 1;
	}

#ifdef HAVE_DIRENT_H
	/*
	 *	$INCLUDE foo/
	 *
	 *	Include ALL non-"dot" files in the directory.
	 *	careful!
	 */
	{
		char		*directory;
		DIR		*dir;
		struct dirent	*dp;
		struct stat	stat_buf;
		cf_file_heap_t	*h;

		/*
		 *	We need to keep a copy of parent while the
		 *	included files mangle our buff[] array.
		 */
		directory = talloc_strdup(parent, value);

		cf_log_debug(parent, "Including files in directory \"%s\"", directory);

		dir = opendir(directory);
		if (!dir) {
			ERROR("%s[%d]: Error reading directory %s: %s",
			      frame->filename, frame->lineno, value,
			      fr_syserror(errno));
		error:
			talloc_free(directory);
			return -1;
		}
#ifdef S_IWOTH
		/*
		 *	Security checks.
		 */
		if (fstat(dirfd(dir), &stat_buf) < 0) {
			ERROR("%s[%d]: Failed reading directory %s: %s", frame->filename, frame->lineno,
			      directory, fr_syserror(errno));
			goto error;
		}

		if ((stat_buf.st_mode & S_IWOTH) != 0) {
			ERROR("%s[%d]: Directory %s is globally writable.  Refusing to start due to "
			      "insecure configuration", frame->filename, frame->lineno, directory);
			goto error;
		}
#endif

		/*
		 *	Directory plus next filename.
		 */
		if ((stack->depth + 2) >= MAX_STACK) {
			ERROR("%s[%d]: Directories too deep", frame->filename, frame->lineno);
			goto error;
		}

		stack->depth++;
		frame = &stack->frame[stack->depth];
		*frame = (cf_stack_frame_t){
			.type = CF_STACK_DIR,
			.directory = directory,
			.parent = parent,
			.current = parent,
			.from_dir = true
		};

		MEM(frame->heap = fr_heap_alloc(frame->directory, filename_cmp, cf_file_heap_t, heap_id, 0));

		/*
		 *	Read the whole directory before loading any
		 *	individual file.  We stat() files to ensure
		 *	that they're readable.  We ignore
		 *	subdirectories and files with odd filenames.
		 */
		while ((dp = readdir(dir)) != NULL) {
			char const *p;

			if (dp->d_name[0] == '.') continue;

			/*
			 *	Check for valid characters
			 */
			for (p = dp->d_name; *p != '\0'; p++) {
				if (isalpha((uint8_t)*p) ||
				    isdigit((uint8_t)*p) ||
				    (*p == '-') ||
				    (*p == '_') ||
				    (*p == '.')) continue;
				break;
			}
			if (*p != '\0') continue;

			snprintf(stack->buff[1], stack->bufsize, "%s%s",
				 frame->directory, dp->d_name);

			if (stat(stack->buff[1], &stat_buf) != 0) {
				ERROR("%s[%d]: Failed checking file %s: %s",
				      (frame - 1)->filename, (frame - 1)->lineno,
				      stack->buff[1], fr_syserror(errno));
				continue;
			}

			if (S_ISDIR(stat_buf.st_mode)) {
				WARN("%s[%d]: Ignoring directory %s",
				     (frame - 1)->filename, (frame - 1)->lineno,
				     stack->buff[1]);
				continue;
			}

			MEM(h = talloc_zero(frame->heap, cf_file_heap_t));
			MEM(h->filename = talloc_typed_strdup(h, stack->buff[1]));
			h->heap_id = FR_HEAP_INDEX_INVALID;
			(void) fr_heap_insert(&frame->heap, h);
		}
		closedir(dir);

		/*
		 *	No "$INCLUDE dir/" inside of update / map.  That's dumb.
		 */
		frame->special = NULL;
		return 1;
	}
#else
	ERROR("%s[%d]: Error including %s: No support for directories!",
	      frame->filename, frame->lineno, value);
	return -1;
#endif
}


static int process_template(cf_stack_t *stack)
{
	CONF_ITEM *ci;
	CONF_SECTION *parent_cs, *templatecs;
	fr_token_t token;
	cf_stack_frame_t *frame = &stack->frame[stack->depth];
	CONF_SECTION	*parent = frame->current;

	token = getword(&stack->ptr, stack->buff[2], stack->bufsize, true);
	if (token != T_EOL) {
		ERROR("%s[%d]: Unexpected text after $TEMPLATE", frame->filename, frame->lineno);
		return -1;
	}

	if (!parent) {
		ERROR("%s[%d]: Internal sanity check error in template reference", frame->filename, frame->lineno);
		return -1;
	}

	if (parent->template) {
		ERROR("%s[%d]: Section already has a template", frame->filename, frame->lineno);
		return -1;
	}

	/*
	 *	Allow in-line templates.
	 */
	templatecs = cf_section_find(cf_item_to_section(cf_parent(parent)), "template", stack->buff[2]);
	if (templatecs) {
		parent->template = templatecs;
		return 0;
	}

	parent_cs = cf_root(parent);

	templatecs = cf_section_find(parent_cs, "templates", NULL);
	if (!templatecs) {
		ERROR("%s[%d]: Cannot find template \"%s\", as no 'templates' section exists.",
		      frame->filename, frame->lineno, stack->buff[2]);
		return -1;
	}

	ci = cf_reference_item(parent_cs, templatecs, stack->buff[2]);
	if (!ci || (ci->type != CONF_ITEM_SECTION)) {
		ERROR("%s[%d]: No such template \"%s\" in the 'templates' section.",
		      frame->filename, frame->lineno, stack->buff[2]);
		return -1;
	}

	parent->template = cf_item_to_section(ci);
	return 0;
}


static int cf_file_fill(cf_stack_t *stack);


/**  Skip an xlat expression.
 *
 *  This is a simple "peek ahead" parser which tries to not be wrong.  It may accept
 *  some things which will later parse as invalid (e.g. unknown attributes, etc.)
 *  But it also rejects all malformed expressions.
 *
 *  It's used as a quick hack because the full parser isn't always available.
 *
 *  @param[in] start	start of the expression, MUST point to the "%{" or "%("
 *  @param[in] end	end of the string (or NULL for zero-terminated strings)
 *  @return
 *	>0 length of the string which was parsed
 *	<=0 on error
 */
static ssize_t fr_skip_xlat(char const *start, char const *end)
{
	int	depth = 1;		/* caller skips '{' */
	ssize_t slen;
	char	quote, end_quote;
	char const *p = start;

	/*
	 *	At least %{1}
	 */
	if (end && ((start + 4) > end)) {
		fr_strerror_const("Invalid expansion");
		return 0;
	}

	if ((*p != '%') && (*p != '$')) {
		fr_strerror_const("Unexpected character in expansion");
		return -(p - start);
	}

	p++;
	if ((*p != '{') && (*p != '(')) {
		char const *q = p;

		/*
		 *	New xlat syntax: %foo(...)
		 */
		while (isalnum((int) *q) || (*q == '.') || (*q == '_') || (*q == '-')) {
			q++;
		}
		if (*q == '(') {
			p = q;
			goto do_quote;
		}

		fr_strerror_const("Invalid character after '%'");
		return -(p - start);
	}

do_quote:
	quote = *(p++);
	if (quote == '{') {
		end_quote = '}';
	} else {
		end_quote = ')';
	}

	while ((end && (p < end)) || (*p >= ' ')) {
		if (*p == quote) {
			p++;
			depth++;
			continue;
		}

		if (*p == end_quote) {
			p++;
			depth--;
			if (!depth) return p - start;

			continue;
		}

		/*
		 *	Nested expansion.
		 */
		if ((p[0] == '$') || (p[0] == '%')) {
			if (end && (p + 2) >= end) break;

			if ((p[1] == '{') || ((p[0] == '$') && (p[1] == '('))) {
				slen = fr_skip_xlat(p, end);

			check:
				if (slen <= 0) return -(p - start) + slen;

				p += slen;
				continue;
			}

			/*
			 *	Bare $ or %, just leave it alone.
			 */
			p++;
			continue;
		}

		/*
		 *	A quoted string.
		 */
		if ((*p == '"') || (*p == '\'') || (*p == '`')) {
			slen = fr_skip_string(p, end);
			goto check;
		}

		/*
		 *	@todo - bare '(' is a condition or nested
		 *	expression.  The brackets need to balance
		 *	here, too.
		 */

		if (*p != '\\') {
			p++;
			continue;
		}

		if (end && ((p + 2) >= end)) break;

		/*
		 *	Escapes here are only one-character escapes.
		 */
		if (p[1] < ' ') break;
		p += 2;
	}

	/*
	 *	Unexpected end of xlat
	 */
	fr_strerror_const("Unexpected end of expansion");
	return -(p - start);
}

static const bool terminal_end_section[UINT8_MAX + 1] = {
	['{'] = true,
};

static const bool terminal_end_line[UINT8_MAX + 1] = {
	[0] = true,

	['\r'] = true,
	['\n'] = true,

	['#'] = true,
	[','] = true,
	[';'] = true,
	['}'] = true,
};

/**  Skip a conditional expression.
 *
 *  This is a simple "peek ahead" parser which tries to not be wrong.  It may accept
 *  some things which will later parse as invalid (e.g. unknown attributes, etc.)
 *  But it also rejects all malformed expressions.
 *
 *  It's used as a quick hack because the full parser isn't always available.
 *
 *  @param[in] start	start of the condition.
 *  @param[in] end	end of the string (or NULL for zero-terminated strings)
 *  @param[in] terminal	terminal character(s)
 *  @param[out] eol	did the parse error happen at eol?
 *  @return
 *	>0 length of the string which was parsed.  *eol is false.
 *	<=0 on error, *eol may be set.
 */
static ssize_t fr_skip_condition(char const *start, char const *end, bool const terminal[static UINT8_MAX + 1], bool *eol)
{
	char const *p = start;
	bool was_regex = false;
	int depth = 0;
	ssize_t slen;

	if (eol) *eol = false;

	/*
	 *	Keep parsing the condition until we hit EOS or EOL.
	 */
	while ((end && (p < end)) || *p) {
		if (isspace((uint8_t) *p)) {
			p++;
			continue;
		}

		/*
		 *	In the configuration files, conditions end with ") {" or just "{"
		 */
		if ((depth == 0) && terminal && terminal[(uint8_t) *p]) {
			return p - start;
		}

		/*
		 *	"recurse" to get more conditions.
		 */
		if (*p == '(') {
			p++;
			depth++;
			was_regex = false;
			continue;
		}

		if (*p == ')') {
			if (!depth) {
				fr_strerror_const("Too many ')'");
				return -(p - start);
			}

			p++;
			depth--;
			was_regex = false;
			continue;
		}

		/*
		 *	Parse xlats.  They cannot span EOL.
		 */
		if ((*p == '$') || (*p == '%')) {
			if (end && ((p + 2) >= end)) {
				fr_strerror_const("Expansions cannot extend across end of line");
				return -(p - start);
			}

			if ((p[1] == '{') || ((p[0] == '$') && (p[1] == '('))) {
				slen = fr_skip_xlat(p, end);

			check:
				if (slen <= 0) return -(p - start) + slen;

				p += slen;
				continue;
			}

			/*
			 *	Bare $ or %, just leave it alone.
			 */
			p++;
			was_regex = false;
			continue;
		}

		/*
		 *	Parse quoted strings.  They cannot span EOL.
		 */
		if ((*p == '"') || (*p == '\'') || (*p == '`') || (was_regex && (*p == '/'))) {
			was_regex = false;

			slen = fr_skip_string((char const *) p, end);
			goto check;
		}

		/*
		 *	192.168/16 is a netmask.  So we only
		 *	allow regex after a regex operator.
		 *
		 *	This isn't perfect, but is good enough
		 *	for most purposes.
		 */
		if ((p[0] == '=') || (p[0] == '!')) {
			if (end && ((p + 2) >= end)) {
				fr_strerror_const("Operators cannot extend across end of line");
				return -(p - start);
			}

			if (p[1] == '~') {
				was_regex = true;
				p += 2;
				continue;
			}

			/*
			 *	Some other '==' or '!=', just leave it alone.
			 */
			p++;
			was_regex = false;
			continue;
		}

		/*
		 *	Any control characters (other than \t) cause an error.
		 */
		if (*p < ' ') break;

		was_regex = false;

		/*
		 *	Normal characters just get skipped.
		 */
		if (*p != '\\') {
			p++;
			continue;
		}

		/*
		 *	Backslashes at EOL are ignored.
		 */
		if (end && ((p + 2) >= end)) break;

		/*
		 *	Escapes here are only one-character escapes.
		 */
		if (p[1] < ' ') break;
		p += 2;
	}

	/*
	 *	We've fallen off of the end of a string.  It may be OK?
	 */
	if (eol) *eol = (depth > 0);

	if (terminal && terminal[(uint8_t) *p]) return p - start;

	fr_strerror_const("Unexpected end of condition");
	return -(p - start);
}

static CONF_ITEM *process_if(cf_stack_t *stack)
{
	ssize_t		slen = 0;
	fr_dict_t const	*dict = NULL;
	CONF_SECTION	*cs;
	uint8_t const   *p;
	char const	*ptr = stack->ptr;
	cf_stack_frame_t *frame = &stack->frame[stack->depth];
	CONF_SECTION	*parent = frame->current;
	char		*buff[4];
	tmpl_rules_t	t_rules;

	/*
	 *	Short names are nicer.
	 */
	buff[1] = stack->buff[1];
	buff[2] = stack->buff[2];
	buff[3] = stack->buff[3];

	dict = virtual_server_dict_by_child_ci(cf_section_to_item(parent));

	t_rules = (tmpl_rules_t) {
		.attr = {
			.dict_def = dict,
			.list_def = request_attr_request,
			.allow_unresolved = true,
			.allow_unknown = true
		}
	};

	/*
	 *	Create the CONF_SECTION.  We don't pass a name2, as it
	 *	hasn't yet been parsed.
	 */
	cs = cf_section_alloc(parent, parent, buff[1], NULL);
	if (!cs) {
		cf_log_err(parent, "Failed allocating memory for section");
		return NULL;
	}
	cf_filename_set(cs, frame->filename);
	cf_lineno_set(cs, frame->lineno);

	RULES_VERIFY(cs, &t_rules);

	/*
	 *	Keep "parsing" the condition until we hit EOL.
	 *
	 *
	 */
	while (true) {
		int rcode;
		bool eol;

		/*
		 *	Try to parse the condition.  We can have a parse success, or a parse failure.
		 */
		slen = fr_skip_condition(ptr, NULL, terminal_end_section, &eol);

		/*
		 *	Parse success means we stop reading more data.
		 */
		if (slen > 0) break;

		/*
		 *	Parse failures not at EOL are real errors.
		 */
		if (!eol) {
			char *spaces, *text;

			slen = 0;
			fr_strerror_const("Unexpected EOF");
	error:
			fr_canonicalize_error(cs, &spaces, &text, slen, ptr);

			cf_log_err(cs, "Parse error in condition");
			cf_log_err(cs, "%s", text);
			cf_log_err(cs, "%s^ %s", spaces, fr_strerror());

			talloc_free(spaces);
			talloc_free(text);
			talloc_free(cs);
			return NULL;
		}

		/*
		 *	Parse failures at EOL means that we read more data.
		 */
		p = (uint8_t const *) ptr + (-slen);

		/*
		 *	Auto-continue across CR LF until we reach the
		 *	end of the string.  We mash everything into one line.
		 */
		if (*p && (*p < ' ')) {
			while ((*p == '\r') || (*p == '\n')) {
				char *q;

				q = UNCONST(char *, p);
				*q = ' ';
				p++;
				continue;
			}

			/*
			 *	Hopefully the next line is already in
			 *	the buffer, and we don't have to read
			 *	more data.
			 */
			continue;
		}

		/*
		 *	Anything other than EOL is a problem at this point.
		 */
		if (*p) {
			fr_strerror_const("Unexpected text");
			goto error;
		}

		/*
		 *	We hit EOL, so the parse error is really "read more data".
		 */
		stack->fill = UNCONST(char *, p);
		rcode = cf_file_fill(stack);
		if (rcode < 0) return NULL;
	}

	fr_assert((size_t) slen < (stack->bufsize - 1));

	/*
	 *	Save the parsed condition (minus trailing whitespace)
	 *	into a buffer.
	 */
	memcpy(buff[2], stack->ptr, slen);
	buff[2][slen] = '\0';

	while (slen > 0) {
		if (!isspace((uint8_t) buff[2][slen])) break;

		buff[2][slen] = '\0';
		slen--;
	}

	/*
	 *	Expand the variables in the pre-parsed condition.
	 */
	if (!cf_expand_variables(frame->filename, frame->lineno, parent,
				 buff[3], stack->bufsize, buff[2], slen, NULL)) {
		fr_strerror_const("Failed expanding configuration variable");
		return NULL;
	}

	MEM(cs->name2 = talloc_typed_strdup(cs, buff[3]));
	cs->name2_quote = T_BARE_WORD;

	ptr += slen;
	fr_skip_whitespace(ptr);

	if (*ptr != '{') {
		cf_log_err(cs, "Expected '{' instead of %s", ptr);
		talloc_free(cs);
		return NULL;
	}
	ptr++;

	stack->ptr = ptr;

	cs->allow_unlang = cs->allow_locals = true;
	return cf_section_to_item(cs);
}

static CONF_ITEM *process_map(cf_stack_t *stack)
{
	char const *mod;
	char const *value = NULL;
	CONF_SECTION *css;
	fr_token_t token;
	char const	*ptr = stack->ptr;
	cf_stack_frame_t *frame = &stack->frame[stack->depth];
	CONF_SECTION	*parent = frame->current;
	char		*buff[4];

	/*
	 *	Short names are nicer.
	 */
	buff[1] = stack->buff[1];
	buff[2] = stack->buff[2];

	if (cf_get_token(parent, &ptr, &token, buff[1], stack->bufsize,
			 frame->filename, frame->lineno) < 0) {
		return NULL;
	}

	if (token != T_BARE_WORD) {
		ERROR("%s[%d]: Invalid syntax for 'map' - module name must not be a quoted string",
		      frame->filename, frame->lineno);
		return NULL;
	}
	mod = buff[1];

        /*
	 *      Maps without an expansion string are allowed, though
	 *      it's not clear why.
	 */
	if (*ptr == '{') {
		ptr++;
		goto alloc_section;
	}

	/*
	 *	Now get the expansion string.
	 */
	if (cf_get_token(parent, &ptr, &token, buff[2], stack->bufsize,
			 frame->filename, frame->lineno) < 0) {
		return NULL;
	}
	if (!fr_str_tok[token]) {
		ERROR("%s[%d]: Expecting string expansions in 'map' definition",
		      frame->filename, frame->lineno);
		return NULL;
	}

	if (*ptr != '{') {
		ERROR("%s[%d]: Expecting section start brace '{' in 'map' definition",
		      frame->filename, frame->lineno);
		return NULL;
	}
	ptr++;
	value = buff[2];

	/*
	 *	Allocate the section
	 */
alloc_section:
	css = cf_section_alloc(parent, parent, "map", mod);
	if (!css) {
		ERROR("%s[%d]: Failed allocating memory for section",
		      frame->filename, frame->lineno);
		return NULL;
	}
	cf_filename_set(css, frame->filename);
	cf_lineno_set(css, frame->lineno);
	css->name2_quote = T_BARE_WORD;

	css->argc = 0;
	if (value) {
		css->argv = talloc_array(css, char const *, 1);
		css->argv[0] = talloc_typed_strdup(css->argv, value);
		css->argv_quote = talloc_array(css, fr_token_t, 1);
		css->argv_quote[0] = token;
		css->argc++;
	}
	stack->ptr = ptr;
	frame->special = css;

	return cf_section_to_item(css);
}


static CONF_ITEM *process_subrequest(cf_stack_t *stack)
{
	char const *mod = NULL;
	CONF_SECTION *css;
	fr_token_t token;
	char const	*ptr = stack->ptr;
	cf_stack_frame_t *frame = &stack->frame[stack->depth];
	CONF_SECTION	*parent = frame->current;
	char		*buff[4];
	int		values = 0;

	/*
	 *	Short names are nicer.
	 */
	buff[1] = stack->buff[1];
	buff[2] = stack->buff[2];
	buff[3] = stack->buff[3];

	/*
	 *	subrequest { ... } is allowed.
	 */
	fr_skip_whitespace(ptr);
	if (*ptr == '{') {
		ptr++;
		goto alloc_section;
	}

	/*
	 *	Get the name of the Packet-Type.
	 */
	if (cf_get_token(parent, &ptr, &token, buff[1], stack->bufsize,
			 frame->filename, frame->lineno) < 0) {
		return NULL;
	}

	if (token != T_BARE_WORD) {
		ERROR("%s[%d]: The first argument to 'subrequest' must be a name or an attribute reference",
		      frame->filename, frame->lineno);
		return NULL;
	}
	mod = buff[1];

        /*
	 *	subrequest Access-Request { ... } is allowed.
	 */
	if (*ptr == '{') {
		ptr++;
		goto alloc_section;
	}

	/*
	 *	subrequest Access-Request &foo { ... }
	 */
	if (cf_get_token(parent, &ptr, &token, buff[2], stack->bufsize,
			 frame->filename, frame->lineno) < 0) {
		return NULL;
	}

	if (token != T_BARE_WORD) {
		ERROR("%s[%d]: The second argument to 'subrequest' must be an attribute reference",
		      frame->filename, frame->lineno);
		return NULL;
	}
	values++;

	if (*ptr == '{') {
		ptr++;
		goto alloc_section;
	}

	/*
	 *	subrequest Access-Request &foo &bar { ... }
	 */
	if (cf_get_token(parent, &ptr, &token, buff[3], stack->bufsize,
			 frame->filename, frame->lineno) < 0) {
		return NULL;
	}

	if (token != T_BARE_WORD) {
		ERROR("%s[%d]: The third argument to 'subrequest' must be an attribute reference",
		      frame->filename, frame->lineno);
		return NULL;
	}
	values++;

	if (*ptr != '{') {
		ERROR("%s[%d]: Expecting section start brace '{' in 'subrequest' definition",
		      frame->filename, frame->lineno);
		return NULL;
	}
	ptr++;

	/*
	 *	Allocate the section
	 */
alloc_section:
	css = cf_section_alloc(parent, parent, "subrequest", mod);
	if (!css) {
		ERROR("%s[%d]: Failed allocating memory for section",
		      frame->filename, frame->lineno);
		return NULL;
	}
	cf_filename_set(css, frame->filename);
	cf_lineno_set(css, frame->lineno);
	if (mod) css->name2_quote = T_BARE_WORD;

	css->argc = values;
	if (values) {
		int i;

		css->argv = talloc_array(css, char const *, values);
		css->argv_quote = talloc_array(css, fr_token_t, values);

		for (i = 0; i < values; i++) {
			css->argv[i] = talloc_typed_strdup(css->argv, buff[2 + i]);
			css->argv_quote[i] = T_BARE_WORD;
		}
	}

	stack->ptr = ptr;
	frame->special = css;

	css->allow_unlang = css->allow_locals = true;
	return cf_section_to_item(css);
}

static int add_section_pair(CONF_SECTION **parent, char const **attr, char const *dot, char *buffer, size_t buffer_len, char const *filename, int lineno)
{
	CONF_SECTION *cs;
	char const *name1 = *attr;
	char *name2 = NULL;
	char const *next;
	char *p;

	if (!dot[1] || (dot[1] == '.')) {
		ERROR("%s[%d]: Invalid name", filename, lineno);
		return -1;
	}

	if ((size_t) (dot - name1) >= buffer_len) {
		ERROR("%s[%d]: Name too long", filename, lineno);
		return -1;
	}

	memcpy(buffer, name1, dot - name1);
	buffer[dot - name1] = '\0';
	next = dot + 1;

	/*
	 *	Look for name1[name2]
	 */
	p = strchr(buffer, '[');
	if (p) {
		name2 = p;

		p = strchr(p, ']');
		if (!p || ((p[1] != '\0') && (p[1] != '.'))) {
			cf_log_err(*parent, "Could not parse name2 in '%s', expected 'name1[name2]", name1);
			return -1;
		}

		*p = '\0';
		*(name2++) = '\0';
	}

	/*
	 *	Reference a subsection which already exists.  If not,
	 *	create it.
	 */
	cs = cf_section_find(*parent, buffer, name2);
	if (!cs) {
		cs = cf_section_alloc(*parent, *parent, buffer, NULL);
		if (!cs) {
			cf_log_err(*parent, "Failed allocating memory for section");
			return -1;
		}
		cf_filename_set(cs, filename);
		cf_lineno_set(cs, lineno);
	}

	*parent = cs;
	*attr = next;

	p = strchr(next + 1, '.');
	if (!p) return 0;

	return add_section_pair(parent, attr, p, buffer, buffer_len, filename, lineno);
}

static int add_pair(CONF_SECTION *parent, char const *attr, char const *value,
		    fr_token_t name1_token, fr_token_t op_token, fr_token_t value_token,
		    char *buff, char const *filename, int lineno)
{
	CONF_DATA const *cd;
	CONF_PARSER *rule;
	CONF_PAIR *cp;
	bool pass2 = false;

	/*
	 *	For laziness, allow specifying paths for CONF_PAIRs
	 *
	 *		section.subsection.pair = value
	 *
	 *	Note that we do this ONLY for CONF_PAIRs which have
	 *	values, so that we don't pick up module references /
	 *	methods.  And we don't do this for things which begin
	 *	with '&' (or %), so we don't pick up attribute
	 *	references.
	 */
	if ((*attr >= 'A') && (name1_token == T_BARE_WORD) && value && !parent->attr) {
		char const *p = strchr(attr, '.');

		if (p && (add_section_pair(&parent, &attr, p, buff, talloc_array_length(buff), filename, lineno) < 0)) {
			return -1;
		}
	}

	/*
	 *	If we have the value, expand any configuration
	 *	variables in it.
	 */
	if (value && *value) {
		bool		soft_fail;
		char const	*expanded;

		expanded = cf_expand_variables(filename, lineno, parent, buff, talloc_array_length(buff), value, -1, &soft_fail);
		if (expanded) {
			value = expanded;

		} else if (!soft_fail) {
			return -1;

		} else {
			/*
			 *	References an item which doesn't exist,
			 *	or which is already marked up as being
			 *	expanded in pass2.  Wait for pass2 to
			 *	do the expansions.
			 *
			 *	Leave the input value alone.
			 */
			pass2 = true;
		}
	}

	cp = cf_pair_alloc(parent, attr, value, op_token, name1_token, value_token);
	if (!cp) return -1;
	cf_filename_set(cp, filename);
	cf_lineno_set(cp, lineno);
	cp->pass2 = pass2;

	cd = cf_data_find(CF_TO_ITEM(parent), CONF_PARSER, attr);
	if (!cd) return 0;

	rule = cf_data_value(cd);
	if (!rule->on_read) return 0;

	/*
	 *	Do the on_read callback after adding the value.
	 */
	return rule->on_read(parent, NULL, NULL, cf_pair_to_item(cp), rule);
}

static fr_table_ptr_sorted_t unlang_keywords[] = {
	{ L("elsif"),		(void *) process_if },
	{ L("if"),		(void *) process_if },
	{ L("map"),		(void *) process_map },
	{ L("subrequest"),	(void *) process_subrequest }
};
static int unlang_keywords_len = NUM_ELEMENTS(unlang_keywords);

typedef CONF_ITEM *(*cf_process_func_t)(cf_stack_t *);

static int parse_input(cf_stack_t *stack)
{
	fr_token_t	name1_token, name2_token, value_token, op_token;
	char const	*value;
	CONF_SECTION	*css;
	char const	*ptr = stack->ptr;
	cf_stack_frame_t *frame = &stack->frame[stack->depth];
	CONF_SECTION	*parent = frame->current;
	char		*buff[4];
	cf_process_func_t process;

	/*
	 *	Short names are nicer.
	 */
	buff[0] = stack->buff[0];
	buff[1] = stack->buff[1];
	buff[2] = stack->buff[2];
	buff[3] = stack->buff[3];

	fr_assert(parent != NULL);

	/*
	 *	Catch end of a subsection.
	 */
	if (*ptr == '}') {
		/*
		 *	We're already at the parent section
		 *	which loaded this file.  We cannot go
		 *	back up another level.
		 *
		 *	This limitation means that we cannot
		 *	put half of a CONF_SECTION in one
		 *	file, and then the second half in
		 *	another file.  That's fine.
		 */
		if (parent == frame->parent) {
			ERROR("%s[%d]: Too many closing braces", frame->filename, frame->lineno);
			return -1;
		}

		fr_assert(frame->braces > 0);
		frame->braces--;

		/*
		 *	Merge the template into the existing
		 *	section.  parent uses more memory, but
		 *	means that templates now work with
		 *	sub-sections, etc.
		 */
		if (!cf_template_merge(parent, parent->template)) return -1;

		if (parent == frame->special) frame->special = NULL;

		frame->current = cf_item_to_section(parent->item.parent);

		ptr++;
		stack->ptr = ptr;
		return 1;
	}

	/*
	 *	Found nothing to get excited over.  It MUST be
	 *	a key word.
	 */
	if (cf_get_token(parent, &ptr, &name1_token, buff[1], stack->bufsize,
			 frame->filename, frame->lineno) < 0) {
		return -1;
	}

	/*
	 *	See which unlang keywords are allowed
	 *
	 *	0 - no unlang keywords are allowed.
	 *	1 - unlang keywords are allowed
	 *	2 - unlang keywords are allowed only in sub-sections
	 *	  i.e. policy { ... } doesn't allow "if".  But the "if"
	 *	  keyword is allowed in children of "policy".
	 */
	if (parent->allow_unlang != 1) {
		if ((strcmp(buff[1], "if") == 0) ||
		    (strcmp(buff[1], "elsif") == 0)) {
			ERROR("%s[%d]: Invalid location for '%s'",
			      frame->filename, frame->lineno, buff[1]);
			return -1;
		}

	} else if ((name1_token == T_BARE_WORD) && isalpha((uint8_t) *buff[1])) {
		fr_type_t type;

		/*
		 *	The next thing should be a keyword.
		 */
		process = (cf_process_func_t) fr_table_value_by_str(unlang_keywords, buff[1], NULL);
		if (process) {
			CONF_ITEM *ci;

			stack->ptr = ptr;
			ci = process(stack);
			ptr = stack->ptr;
			if (!ci) return -1;
			if (cf_item_is_section(ci)) {
				parent->allow_locals = false;
				css = cf_item_to_section(ci);
				goto add_section;
			}

			/*
			 *	Else the item is a pair, and it's already added to the section.
			 */
			goto added_pair;
		}

		/*
		 *	The next token is an assignment operator, so we ignore it.
		 */
		if (!isalnum((int) *ptr)) goto check_for_eol;

		/*
		 *	It's not a keyword, check for a data type, which means we have a local variable
		 *	definition.
		 */
		type = fr_table_value_by_str(fr_type_table, buff[1], FR_TYPE_NULL);
		if (type == FR_TYPE_NULL) {
			parent->allow_locals = false;
			goto check_for_eol;
		}

		if (!parent->allow_locals && (cf_section_find_in_parent(parent, "dictionary", NULL) == NULL)) {
			ERROR("%s[%d]: Parse error: Invalid location for variable definition",
			      frame->filename, frame->lineno);
			return -1;
		}

		if (type == FR_TYPE_TLV) goto parse_name2;

		/*
		 *	We don't have an operator, so set it to a magic value.
		 */
		op_token = T_OP_CMP_TRUE;

		/*
		 *	Parse the name of the local variable, and use it as the "value" for the CONF_PAIR.
		 */
		if (cf_get_token(parent, &ptr, &value_token, buff[2], stack->bufsize,
				 frame->filename, frame->lineno) < 0) {
			return -1;
		}
		value = buff[2];

		goto alloc_pair;
	}

	/*
	 *	parent single word is done.  Create a CONF_PAIR.
	 */
check_for_eol:
	if (!*ptr || (*ptr == '#') || (*ptr == ',') || (*ptr == ';') || (*ptr == '}')) {
		parent->allow_locals = false;
		value_token = T_INVALID;
		op_token = T_OP_EQ;
		value = NULL;
		goto alloc_pair;
	}

	/*
	 *	A common pattern is: name { ...}
	 *	Check for it and skip ahead.
	 */
	if (*ptr == '{') {
		ptr++;
		name2_token = T_INVALID;
		value = NULL;
		goto alloc_section;
	}

	/*
	 *	We allow certain kinds of strings, attribute
	 *	references (i.e. foreach) and bare names that
	 *	start with a letter.  We also allow UTF-8
	 *	characters.
	 *
	 *	Once we fix the parser to be less generic, we
	 *	can tighten these rules.  Right now, it's
	 *	*technically* possible to define a module with
	 *	&foo or "with spaces" as the second name.
	 *	Which seems bad.  But the old parser allowed
	 *	it, so oh well.
	 */
	if ((*ptr == '"') || (*ptr == '`') || (*ptr == '\'') || ((*ptr == '&') && (ptr[1] != '=')) ||
	    ((*((uint8_t const *) ptr) & 0x80) != 0) || isalpha((uint8_t) *ptr) || isdigit((uint8_t) *ptr)) {
	parse_name2:
		if (cf_get_token(parent, &ptr, &name2_token, buff[2], stack->bufsize,
				 frame->filename, frame->lineno) < 0) {
			return -1;
		}

		if (*ptr != '{') {
			ERROR("%s[%d]: Parse error: expected '{', got text \"%s\"",
			      frame->filename, frame->lineno, ptr);
			return -1;
		}
		ptr++;
		value = buff[2];

	alloc_section:
		css = cf_section_alloc(parent, parent, buff[1], value);
		if (!css) {
			ERROR("%s[%d]: Failed allocating memory for section",
			      frame->filename, frame->lineno);
			return -1;
		}

		cf_filename_set(css, frame->filename);
		cf_lineno_set(css, frame->lineno);
		css->name2_quote = name2_token;

		/*
		 *	Hack for better error messages in
		 *	nested sections.  parent information
		 *	should really be put into a parser
		 *	struct, as with tmpls.
		 */
		if (!frame->special && (strcmp(css->name1, "update") == 0)) {
			frame->special = css;
		}

		/*
		 *	Only a few top-level sections allow "unlang"
		 *	statements.  And for those, "unlang"
		 *	statements are only allowed in child
		 *	subsection.
		 */
		if (!parent->allow_unlang && !parent->item.parent) {
			if (strcmp(css->name1, "server") == 0) css->allow_unlang = 2;
			if (strcmp(css->name1, "policy") == 0) css->allow_unlang = 2;

		} else if ((parent->allow_unlang == 2) && (strcmp(css->name1, "listen") == 0)) { /* hacks for listeners */
			css->allow_unlang = css->allow_locals = false;

		} else {
			/*
			 *	Allow unlang if the parent allows it, but don't allow
			 *	unlang in list assignment sections.
			 */
			css->allow_unlang = css->allow_locals = parent->allow_unlang && !fr_list_assignment_op[name2_token];
		}

	add_section:
		/*
		 *	The current section is now the child section.
		 */
		frame->current = css;
		frame->braces++;
		css = NULL;
		stack->ptr = ptr;
		return 1;
	}

	/*
	 *	The next thing MUST be an operator.  All
	 *	operators start with one of these characters,
	 *	so we check for them first.
	 */
	if ((ptr[0] != '=') && (ptr[0] != '!') && (ptr[0] != '<') && (ptr[0] != '>') &&
	    (ptr[1] != '=') && (ptr[1] != '~')) {
		ERROR("%s[%d]: Parse error at unexpected text: %s",
		      frame->filename, frame->lineno, ptr);
		return -1;
	}

	/*
	 *	If we're not parsing a section, then the next
	 *	token MUST be an operator.
	 */
	name2_token = gettoken(&ptr, buff[2], stack->bufsize, false);
	switch (name2_token) {
	case T_OP_ADD_EQ:
	case T_OP_SUB_EQ:
	case T_OP_AND_EQ:
	case T_OP_OR_EQ:
	case T_OP_NE:
	case T_OP_GE:
	case T_OP_GT:
	case T_OP_LE:
	case T_OP_LT:
	case T_OP_CMP_EQ:
	case T_OP_CMP_FALSE:
		/*
		 *	As a hack, allow any operators when using &foo=bar
		 */
		if (!frame->special && (buff[1][0] != '&')) {
			ERROR("%s[%d]: Invalid operator in assignment for %s ...",
			      frame->filename, frame->lineno, buff[1]);
			return -1;
		}
		FALL_THROUGH;

	case T_OP_EQ:
	case T_OP_SET:
	case T_OP_PREPEND:
		fr_skip_whitespace(ptr);
		op_token = name2_token;
		break;

	default:
		ERROR("%s[%d]: Parse error after \"%s\": unexpected token \"%s\"",
		      frame->filename, frame->lineno, buff[1], fr_table_str_by_value(fr_tokens_table, name2_token, "<INVALID>"));

		return -1;
	}

	/*
	 *	MUST have something after the operator.
	 */
	if (!*ptr || (*ptr == '#') || (*ptr == ',') || (*ptr == ';')) {
		ERROR("%s[%d]: Syntax error: Expected to see a value after the operator '%s': %s",
		      frame->filename, frame->lineno, buff[2], ptr);
		return -1;
	}

	/*
	 *	foo = { ... } for nested groups.
	 *
	 *	As a special case, we allow sub-sections after '=', etc.
	 *
	 *	This syntax is only for inside of "update"
	 *	sections, and for attributes of type "group".
	 *	But the parser isn't (yet) smart enough to
	 *	know about that context.  So we just silently
	 *	allow it everywhere.
	 */
	if (*ptr == '{') {
		if (!parent->allow_unlang && !frame->require_edits) {
			ERROR("%s[%d]: Parse error: Invalid location for grouped attribute",
			      frame->filename, frame->lineno);
			return -1;
		}

		if (*buff[1] != '&') {
			ERROR("%s[%d]: Parse error: Expected '&' before attribute name",
			      frame->filename, frame->lineno);
			return -1;
		}

		if (!fr_list_assignment_op[name2_token]) {
			ERROR("%s[%d]: Parse error: Invalid assignment operator '%s' for list",
			      frame->filename, frame->lineno, buff[2]);
			return -1;
		}

		/*
		 *	Now that we've peeked ahead to
		 *	see the open brace, parse it
		 *	for real.
		 */
		ptr++;

		/*
		 *	Leave name2_token as the
		 *	operator (as a hack).  But
		 *	note that there's no actual
		 *	name2.  We'll deal with that
		 *	situation later.
		 */
		value = NULL;
		frame->require_edits = true;
		goto alloc_section;
	}

	fr_skip_whitespace(ptr);

	/*
	 *	Parse the value for a CONF_PAIR.
	 *
	 *	If it's not an "update" section, and it's an "edit" thing, then try to parse an expression.
	 */
	if (!frame->special && (frame->require_edits || (*buff[1] == '&'))) {
		bool eol;
		ssize_t slen;
		char const *ptr2 = ptr;

		/*
		 *	In most cases, this is just one word.  In some cases it's not.  So we peek ahead to see.
		 */
		if ((*ptr != '(') && (cf_get_token(parent, &ptr2, &value_token, buff[2], stack->bufsize,
						   frame->filename, frame->lineno) == 0)) {
			/*
			 *	The thing after the token is "end of line" in some format, so it's fine.
			 */
			fr_skip_whitespace(ptr2);
			if (terminal_end_line[(uint8_t) *ptr2]) {
				parent->allow_locals = false;
				ptr = ptr2;
				value = buff[2];
				goto alloc_pair;
			}
		} /* else it looks like an expression */

		/*
		 *	Parse the text as an expression.
		 *
		 *	Note that unlike conditions, expressions MUST use \ at the EOL for continuation.
		 *	If we automatically read past EOL, as with:
		 *
		 *		&foo := (bar -
		 *			 baz)
		 *
		 *	That works, mostly.  Until the user forgets to put the trailing ')', and then
		 *	the parse is bad enough that it tries to read to EOF, or to some other random
		 *	parse error.
		 *
		 *	So the simplest way to avoid utter craziness is to simply require a signal which
		 *	says "yes, I intended to put this over multiple lines".
		 */
		slen = fr_skip_condition(ptr, NULL, terminal_end_line, &eol);
		if (slen < 0) {
			ERROR("%s[%d]: Parse error in expression: %s",
			      frame->filename, frame->lineno, fr_strerror());
			return -1;
		}

		/*
		 *	We parsed until the end of the string, but the condition still needs more data.
		 */
		if (eol) {
			ERROR("%s[%d]: Expression is unfinished at end of line",
			      frame->filename, frame->lineno);
			return -1;
		}

		/*
		 *	Keep a copy of the entire RHS.
		 */
		memcpy(buff[2], ptr, slen);
		buff[2][slen] = '\0';

		value = buff[2];

		/*
		 *	Mark it up as an expression
		 *
		 *	@todo - we should really just call cf_data_add() to add a flag, but this is good for
		 *	now.  See map_afrom_cp()
		 */
		value_token = T_HASH;

		/*
		 *	Skip terminal characters
		 */
		ptr += slen;
		if ((*ptr == ',') || (*ptr == ';')) ptr++;

	} else {
		if (cf_get_token(parent, &ptr, &value_token, buff[2], stack->bufsize,
				 frame->filename, frame->lineno) < 0) {
			return -1;
		}
		value = buff[2];
	}

	/*
	 *	Add parent CONF_PAIR to our CONF_SECTION
	 */
	parent->allow_locals = false;

alloc_pair:
	if (add_pair(parent, buff[1], value, name1_token, op_token, value_token, buff[3], frame->filename, frame->lineno) < 0) return -1;

added_pair:
	fr_skip_whitespace(ptr);

	/*
	 *	Skip semicolon if we see it after a
	 *	CONF_PAIR.  Also allow comma for
	 *	backwards compatablity with secret
	 *	things in v3.
	 */
	if ((*ptr == ';') || (*ptr == ',')) {
		ptr++;
		stack->ptr = ptr;
		return 1;
	}

	/*
	 *	Closing brace is allowed after a CONF_PAIR
	 *	definition.
	 */
	if (*ptr == '}') {
		stack->ptr = ptr;
		return 1;
	}

	/*
	 *	Anything OTHER than EOL or comment is a syntax
	 *	error.
	 */
	if (*ptr && (*ptr != '#')) {
		ERROR("%s[%d]: Syntax error: Unexpected text: %s",
		      frame->filename, frame->lineno, ptr);
		return -1;
	}

	/*
	 *	Since we're at EOL or comment, just drop the
	 *	text, and go read another line of text.
	 */
	return 0;
}


static int frame_readdir(cf_stack_t *stack)
{
	cf_stack_frame_t *frame = &stack->frame[stack->depth];
	CONF_SECTION *parent = frame->current;
	cf_file_heap_t *h;

	h = fr_heap_pop(&frame->heap);
	if (!h) {
		/*
		 *	Done reading the directory entry.  Close it, and go
		 *	back up a stack frame.
		 */
		talloc_free(frame->directory);
		stack->depth--;
		return 1;
	}

	/*
	 *	Push the next filename onto the stack.
	 */
	stack->depth++;
	frame = &stack->frame[stack->depth];
	memset(frame, 0, sizeof(*frame));

	frame->type = CF_STACK_FILE;
	frame->fp = NULL;
	frame->parent = parent;
	frame->current = parent;
	frame->filename = h->filename;
	frame->lineno = 0;
	frame->from_dir = true;
	frame->special = NULL; /* can't do includes inside of update / map */
	frame->require_edits = stack->frame[stack->depth - 1].require_edits;
	return 1;
}


static int cf_file_fill(cf_stack_t *stack)
{
	bool at_eof, has_spaces;
	size_t len;
	char const *ptr;
	cf_stack_frame_t *frame = &stack->frame[stack->depth];

read_more:
	has_spaces = false;

read_continuation:
	/*
	 *	Get data, and remember if we are at EOF.
	 */
	at_eof = (fgets(stack->fill, stack->bufsize - (stack->fill - stack->buff[0]), frame->fp) == NULL);
	frame->lineno++;

	/*
	 *	We read the entire 8k worth of data: complain.
	 *	Note that we don't care if the last character
	 *	is \n: it's still forbidden.  This means that
	 *	the maximum allowed length of text is 8k-1, which
	 *	should be plenty.
	 */
	len = strlen(stack->fill);
	if ((stack->fill + len + 1) >= (stack->buff[0] + stack->bufsize)) {
		ERROR("%s[%d]: Line too long", frame->filename, frame->lineno);
		return -1;
	}

	/*
	 *	Suppress leading whitespace after a
	 *	continuation line.
	 */
	if (has_spaces) {
		ptr = stack->fill;
		fr_skip_whitespace(ptr);

		if (ptr > stack->fill) {
			memmove(stack->fill, ptr, len - (ptr - stack->fill));
			len -= (ptr - stack->fill);
		}
	}

	/*
	 *	Skip blank lines when we're at the start of
	 *	the read buffer.
	 */
	if (stack->fill == stack->buff[0]) {
		if (at_eof) return 0;

		ptr = stack->buff[0];
		fr_skip_whitespace(ptr);

		if (!*ptr || (*ptr == '#')) goto read_more;

	} else if (at_eof || (len == 0)) {
		ERROR("%s[%d]: Continuation at EOF is illegal", frame->filename, frame->lineno);
		return -1;
	}

	/*
	 *	See if there's a continuation.
	 */
	while ((len > 0) &&
	       ((stack->fill[len - 1] == '\n') || (stack->fill[len - 1] == '\r'))) {
		len--;
		stack->fill[len] = '\0';
	}

	if ((len > 0) && (stack->fill[len - 1] == '\\')) {
		/*
		 *	Check for "suppress spaces" magic.
		 */
		if (!has_spaces && (len > 2) && (stack->fill[len - 2] == '"')) {
			has_spaces = true;
		}

		stack->fill[len - 1] = '\0';
		stack->fill += len - 1;
		goto read_continuation;
	}

	ptr = stack->fill;

	/*
	 *	We now have one full line of text in the input
	 *	buffer, without continuations.
	 */
	fr_skip_whitespace(ptr);

	/*
	 *	Nothing left, or just a comment.  Go read
	 *	another line of text.
	 */
	if (!*ptr || (*ptr == '#')) goto read_more;

	return 1;
}


/*
 *	Read a configuration file or files.
 */
static int cf_file_include(cf_stack_t *stack)
{
	CONF_SECTION	*parent;
	char const	*ptr;

	cf_stack_frame_t	*frame;
	int		rcode;

do_frame:
	frame = &stack->frame[stack->depth];
	parent = frame->current; /* add items here */

	switch (frame->type) {
#ifdef HAVE_GLOB_H
	case CF_STACK_GLOB:
		if (frame->gl_current == frame->glob.gl_pathc) {
			globfree(&frame->glob);
			goto pop_stack;
		}

		/*
		 *	Process the filename as an include.
		 */
		if (process_include(stack, parent, frame->glob.gl_pathv[frame->gl_current++], frame->required, false) < 0) return -1;

		/*
		 *	Run the correct frame.  If the file is NOT
		 *	required, then the call to process_include()
		 *	may return 0, and we just process the next
		 *	glob.  Otherwise, the call to
		 *	process_include() may return a directory or a
		 *	filename.  Go handle that.
		 */
		goto do_frame;
#endif

#ifdef HAVE_DIRENT_H
	case CF_STACK_DIR:
		rcode = frame_readdir(stack);
		if (rcode == 0) goto do_frame;
		if (rcode < 0) return -1;

		/*
		 *	Reset which frame we're looking at.
		 */
		frame = &stack->frame[stack->depth];
		fr_assert(frame->type == CF_STACK_FILE);
		break;
#endif

	case CF_STACK_FILE:
		break;
	}

#ifndef NDEBUG
	/*
	 *	One last sanity check.
	 */
	if (frame->type != CF_STACK_FILE) {
		cf_log_err(frame->current, "%s: Internal sanity check failed", __FUNCTION__);
		goto pop_stack;
	}
#endif

	/*
	 *	Open the new file if necessary.  It either came from
	 *	the first call to the function, or was pushed onto the
	 *	stack by another function.
	 */
	if (!frame->fp) {
		rcode = cf_file_open(frame->parent, frame->filename, frame->from_dir, &frame->fp);
		if (rcode < 0) return -1;

		/*
		 *	Ignore this file
		 */
		if (rcode == 1) {
			cf_log_warn(frame->current, "Ignoring file %s - it was already read",
				    frame->filename);
			goto pop_stack;
		}
	}

	/*
	 *	Read, checking for line continuations ('\\' at EOL)
	 */
	for (;;) {
		/*
		 *	Fill the buffers with data.
		 */
		stack->fill = stack->buff[0];
		rcode = cf_file_fill(stack);
		if (rcode < 0) return -1;
		if (rcode == 0) break;

		/*
		 *	The text here MUST be at the start of a line,
		 *	OR have only whitespace in front of it.
		 */
		ptr = stack->buff[0];
		fr_skip_whitespace(ptr);

		if (*ptr == '$') {
			/*
			 *	Allow for $INCLUDE files
			 */
			if (strncasecmp(ptr, "$INCLUDE", 8) == 0) {
				ptr += 8;

				if (process_include(stack, parent, ptr, true, true) < 0) return -1;
				goto do_frame;
			}

			if (strncasecmp(ptr, "$-INCLUDE", 9) == 0) {
				ptr += 9;

				rcode = process_include(stack, parent, ptr, false, true);
				if (rcode < 0) return -1;
				if (rcode == 0) continue;
				goto do_frame;
			}

			/*
			 *	Allow for $TEMPLATE things
			 */
			if (strncasecmp(ptr, "$TEMPLATE", 9) == 0) {
				ptr += 9;
				fr_skip_whitespace(ptr);

				stack->ptr = ptr;
				if (process_template(stack) < 0) return -1;
				continue;
			}

			ERROR("%s[%d]: Invalid text starting with '$'", frame->filename, frame->lineno);
			return -1;
		}

		/*
		 *	All of the file handling code is done.  Parse the input.
		 */
		do {
			fr_skip_whitespace(ptr);
			if (!*ptr || (*ptr == '#')) break;

			stack->ptr = ptr;
			rcode = parse_input(stack);
			ptr = stack->ptr;

			if (rcode < 0) return -1;
			parent = frame->current;
		} while (rcode == 1);
	}

	fr_assert(frame->fp != NULL);

	/*
	 *	See if EOF was unexpected.
	 */
	if (feof(frame->fp) && (parent != frame->parent)) {
		ERROR("%s[%d]: EOF reached without closing brace for section %s starting at line %d",
		      frame->filename, frame->lineno, cf_section_name1(parent), cf_lineno(parent));
		return -1;
	}

	fclose(frame->fp);
	frame->fp = NULL;

pop_stack:
	/*
	 *	More things to read, go read them.
	 */
	if (stack->depth > 0) {
		stack->depth--;
		goto do_frame;
	}

	return 0;
}

static void cf_stack_cleanup(cf_stack_t *stack)
{
	cf_stack_frame_t *frame = &stack->frame[stack->depth];

	while (stack->depth >= 0) {
		switch (frame->type) {
		case CF_STACK_FILE:
			if (frame->fp) fclose(frame->fp);
			frame->fp = NULL;
			break;

#ifdef HAVE_DIRENT_H
		case CF_STACK_DIR:
			talloc_free(frame->directory);
			break;
#endif

#ifdef HAVE_GLOB_H
		case CF_STACK_GLOB:
			globfree(&frame->glob);
			break;
#endif
		}

		frame--;
		stack->depth--;
	}

	talloc_free(stack->buff);
}

/*
 *	Bootstrap a config file.
 */
int cf_file_read(CONF_SECTION *cs, char const *filename)
{
	int		i;
	char		*p;
	CONF_PAIR	*cp;
	fr_rb_tree_t	*tree;
	cf_stack_t	stack;
	cf_stack_frame_t	*frame;

	cp = cf_pair_alloc(cs, "confdir", filename, T_OP_EQ, T_BARE_WORD, T_SINGLE_QUOTED_STRING);
	if (!cp) return -1;

	p = strrchr(cp->value, FR_DIR_SEP);
	if (p) *p = '\0';

	MEM(tree = fr_rb_inline_talloc_alloc(cs, cf_file_t, node, _inode_cmp, NULL));

	cf_data_add(cs, tree, "filename", false);

#ifndef NDEBUG
	memset(&stack, 0, sizeof(stack));
#endif

	/*
	 *	Allocate temporary buffers on the heap (so we don't use *all* the stack space)
	 */
	stack.buff = talloc_array(cs, char *, 4);
	for (i = 0; i < 4; i++) MEM(stack.buff[i] = talloc_array(stack.buff, char, 8192));

	stack.depth = 0;
	stack.bufsize = 8192;
	frame = &stack.frame[stack.depth];

	memset(frame, 0, sizeof(*frame));
	frame->parent = frame->current = cs;

	frame->type = CF_STACK_FILE;
	frame->filename = talloc_strdup(frame->parent, filename);
	cs->item.filename = frame->filename;

	if (cf_file_include(&stack) < 0) {
		cf_stack_cleanup(&stack);
		return -1;
	}

	talloc_free(stack.buff);

	/*
	 *	Now that we've read the file, go back through it and
	 *	expand the variables.
	 */
	if (cf_section_pass2(cs) < 0) {
		cf_log_err(cs, "Parsing config items failed");
		return -1;
	}

	return 0;
}

void cf_file_free(CONF_SECTION *cs)
{
	talloc_free(cs);
}

/** Set the euid/egid used when performing file checks
 *
 * Sets the euid, and egid used when cf_file_check is called to check
 * permissions on conf items of type #FR_TYPE_FILE_INPUT.
 *
 * @note This is probably only useful for the freeradius daemon itself.
 *
 * @param uid to set, (uid_t)-1 to use current euid.
 * @param gid to set, (gid_t)-1 to use current egid.
 */
void cf_file_check_user(uid_t uid, gid_t gid)
{
	if (uid != 0) conf_check_uid = uid;
	if (gid != 0) conf_check_gid = gid;
}

static char const parse_tabs[] = "																																																																																																																																																																																																								";

static ssize_t cf_string_write(FILE *fp, char const *string, size_t len, fr_token_t t)
{
	size_t	outlen;
	char	c;
	char	buffer[2048];

	switch (t) {
	default:
		c = '\0';
		break;

	case T_DOUBLE_QUOTED_STRING:
		c = '"';
		break;

	case T_SINGLE_QUOTED_STRING:
		c = '\'';
		break;

	case T_BACK_QUOTED_STRING:
		c = '`';
		break;
	}

	if (c) fprintf(fp, "%c", c);

	outlen = fr_snprint(buffer, sizeof(buffer), string, len, c);
	fwrite(buffer, outlen, 1, fp);

	if (c) fprintf(fp, "%c", c);
	return 1;
}

static int cf_pair_write(FILE *fp, CONF_PAIR *cp)
{
	if (!cp->value) {
		fprintf(fp, "%s\n", cp->attr);
		return 0;
	}

	cf_string_write(fp, cp->attr, strlen(cp->attr), cp->lhs_quote);
	fprintf(fp, " %s ", fr_table_str_by_value(fr_tokens_table, cp->op, "<INVALID>"));
	cf_string_write(fp, cp->value, strlen(cp->value), cp->rhs_quote);
	fprintf(fp, "\n");

	return 1;		/* FIXME */
}


int cf_section_write(FILE *fp, CONF_SECTION *cs, int depth)
{
	if (!fp || !cs) return -1;

	/*
	 *	Print the section name1, etc.
	 */
	fwrite(parse_tabs, depth, 1, fp);
	cf_string_write(fp, cs->name1, strlen(cs->name1), T_BARE_WORD);

	/*
	 *	FIXME: check for "if" or "elsif".  And if so, print
	 *	out the parsed condition, instead of the input text
	 *
	 *	cf_data_find(cs, CF_DATA_TYPE_UNLANG, "if");
	 */
	if (cs->name2) {
		fputs(" ", fp);

#if 0
		c = cf_data_value(cf_data_find(cs, fr_cond_t, NULL));
		if (c) {
			char buffer[1024];

			cond_print(&FR_SBUFF_OUT(buffer, sizeof(buffer)), c);
			fprintf(fp, "(%s)", buffer);
		} else
#endif
			cf_string_write(fp, cs->name2, strlen(cs->name2), cs->name2_quote);
	}

	fputs(" {\n", fp);

	/*
	 *	Loop over the children.  Either recursing, or opening
	 *	a new file.
	 */
	cf_item_foreach(&cs->item, ci) {
		switch (ci->type) {
		case CONF_ITEM_SECTION:
			cf_section_write(fp, cf_item_to_section(ci), depth + 1);
			break;

		case CONF_ITEM_PAIR:
			/*
			 *	Ignore internal things.
			 */
			if (!ci->filename || (ci->filename[0] == '<')) break;

			fwrite(parse_tabs, depth + 1, 1, fp);
			cf_pair_write(fp, cf_item_to_pair(ci));
			break;

		default:
			break;
		}
	}

	fwrite(parse_tabs, depth, 1, fp);
	fputs("}\n\n", fp);

	return 1;
}


CONF_ITEM *cf_reference_item(CONF_SECTION const *parent_cs,
			     CONF_SECTION const *outer_cs,
			     char const *ptr)
{
	CONF_PAIR		*cp;
	CONF_SECTION		*next;
	CONF_SECTION const	*cs = outer_cs;
	char			name[8192];
	char			*p;

	if (!ptr || (!parent_cs && !outer_cs)) return NULL;

	strlcpy(name, ptr, sizeof(name));

	p = name;

	/*
	 *	".foo" means "foo from the current section"
	 */
	if (*p == '.') {
		p++;

		/*
		 *	Just '.' means the current section
		 */
		if (*p == '\0') return cf_section_to_item(cs);

		/*
		 *	..foo means "foo from the section
		 *	enclosing this section" (etc.)
		 */
		while (*p == '.') {
			if (cs->item.parent) cs = cf_item_to_section(cs->item.parent);

			/*
			 *	.. means the section
			 *	enclosing this section
			 */
			if (!*++p) return cf_section_to_item(cs);
		}

		/*
		 *	"foo.bar.baz" means "from the root"
		 */
	} else if (strchr(p, '.') != NULL) {
		if (!parent_cs) return NULL;
		cs = parent_cs;
	}

	while (*p) {
		char *q, *r;

		r = strchr(p, '[');
		q = strchr(p, '.');
		if (!r && !q) break;

		if (r && q > r) q = NULL;
		if (q && q < r) r = NULL;

		/*
		 *	Split off name2.
		 */
		if (r) {
			q = strchr(r + 1, ']');
			if (!q) return NULL; /* parse error */

			/*
			 *	Points to foo[bar]xx: parse error,
			 *	it should be foo[bar] or foo[bar].baz
			 */
			if (q[1] && q[1] != '.') return NULL;

			*r = '\0';
			*q = '\0';
			next = cf_section_find(cs, p, r + 1);
			if (!next && cs->template) next = cf_section_find(cs->template, p, r + 1);
			*r = '[';
			*q = ']';

			/*
			 *	Points to a named instance of a section.
			 */
			if (!q[1]) {
				if (!next) return NULL;
				return &(next->item);
			}

			q++;	/* ensure we skip the ']' and '.' */

		} else {
			*q = '\0';
			next = cf_section_find(cs, p, NULL);
			if (!next && cs->template) next = cf_section_find(cs->template, p, NULL);
			*q = '.';
		}

		if (!next) break; /* it MAY be a pair in this section! */

		cs = next;
		p = q + 1;
	}

	if (!*p) return NULL;

retry:
	/*
	 *	Find it in the current referenced
	 *	section.
	 */
	cp = cf_pair_find(cs, p);
	if (!cp && cs->template) cp = cf_pair_find(cs->template, p);
	if (cp) {
		cp->referenced = true;	/* conf pairs which are referenced count as used */
		return &(cp->item);
	}

	next = cf_section_find(cs, p, NULL);
	if (next) return &(next->item);

	/*
	 *	"foo" is "in the current section, OR in main".
	 */
	if ((p == name) && (parent_cs != NULL) && (cs != parent_cs)) {
		cs = parent_cs;
		goto retry;
	}

	return NULL;
}
