/* fetch.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation. See http://www.gnu.org/ for details.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "apk_applet.h"
#include "apk_database.h"
#include "apk_state.h"
#include "apk_io.h"

#define FETCH_RECURSIVE		1
#define FETCH_STDOUT		2
#define FETCH_LINK		4

struct fetch_ctx {
	unsigned int flags;
	const char *outdir;
};

static int fetch_parse(void *ctx, int optch, int optindex, const char *optarg)
{
	struct fetch_ctx *fctx = (struct fetch_ctx *) ctx;

	switch (optch) {
	case 'R':
		fctx->flags |= FETCH_RECURSIVE;
		break;
	case 's':
		fctx->flags |= FETCH_STDOUT;
		break;
	case 'L':
		fctx->flags |= FETCH_LINK;
		break;
	case 'o':
		fctx->outdir = optarg;
		break;
	default:
		return -1;
	}
	return 0;
}

static int fetch_package(struct fetch_ctx *fctx,
			 struct apk_database *db,
			 struct apk_package *pkg)
{
	struct apk_istream *is;
	char infile[256];
	char outfile[256];
	int i, r, fd;

	if (!(fctx->flags & FETCH_STDOUT)) {
		struct stat st;

		snprintf(outfile, sizeof(outfile), "%s/%s-%s.apk",
			 fctx->outdir ? fctx->outdir : ".",
			 pkg->name->name, pkg->version);

		if (lstat(outfile, &st) == 0 && st.st_size == pkg->size)
			return 0;
	}
	apk_message("Downloading %s-%s", pkg->name->name, pkg->version);

	for (i = 0; i < APK_MAX_REPOS; i++)
		if (pkg->repos & BIT(i))
			break;

	if (i >= APK_MAX_REPOS) {
		apk_error("%s-%s: not present in any repository",
			  pkg->name->name, pkg->version);
		return -1;
	}

	if (apk_flags & APK_SIMULATE)
		return 0;

	snprintf(infile, sizeof(infile), "%s/%s-%s.apk",
		 db->repos[i].url, pkg->name->name, pkg->version);

	if (fctx->flags & FETCH_STDOUT) {
		fd = STDOUT_FILENO;
	} else {
		if ((fctx->flags & FETCH_LINK) && apk_url_local_file(infile)) {
			char real_infile[256];
			int n;
			n = readlink(infile, real_infile, sizeof(real_infile));
			if (n > 0 && n < sizeof(real_infile))
				real_infile[n] = '\0';
			if (link(real_infile, outfile) == 0)
				return 0;
		}
		fd = creat(outfile, 0644);
		if (fd < 0) {
			apk_error("%s: %s", outfile, strerror(errno));
			return -1;
		}
	}

	is = apk_istream_from_url(infile);
	if (is == NULL) {
		apk_error("Unable to download '%s'", infile);
		return -1;
	}

	r = apk_istream_splice(is, fd, pkg->size, NULL, NULL);
	is->close(is);
	if (fd != STDOUT_FILENO)
		close(fd);
	if (r != pkg->size) {
		apk_error("Unable to download '%s'", infile);
		unlink(outfile);
		return -1;
	}

	return 0;
}

static int fetch_main(void *ctx, int argc, char **argv)
{
	struct fetch_ctx *fctx = (struct fetch_ctx *) ctx;
	struct apk_database db;
	int i, j, r;

	r = apk_db_open(&db, apk_root, APK_OPENF_NO_STATE);
	if (r != 0)
		return r;

	for (i = 0; i < argc; i++) {
		struct apk_dependency dep = (struct apk_dependency) {
			.name = apk_db_get_name(&db, APK_BLOB_STR(argv[i])),
			.result_mask = APK_DEPMASK_REQUIRE,
		};

		if (fctx->flags & FETCH_RECURSIVE) {
			struct apk_state *state;
			struct apk_change *change;

			state = apk_state_new(&db);
			r = apk_state_lock_dependency(state, &dep);
			if (r != 0) {
				apk_state_unref(state);
				apk_error("Unable to install '%s'",
					  dep.name->name);
				goto err;
			}

			list_for_each_entry(change, &state->change_list_head, change_list) {
				r = fetch_package(fctx, &db, change->newpkg);
				if (r != 0)
					goto err;
			}

			apk_state_unref(state);
		} else if (dep.name->pkgs != NULL) {
			struct apk_package *pkg = NULL;

			for (j = 0; j < dep.name->pkgs->num; j++)
				if (pkg == NULL ||
				    apk_pkg_version_compare(dep.name->pkgs->item[j],
							    pkg)
				    == APK_VERSION_GREATER)
					pkg = dep.name->pkgs->item[j];

			r = fetch_package(fctx, &db, pkg);
			if (r != 0)
				goto err;
		} else {
			apk_message("Unable to get '%s'", dep.name->name);
			r = -1;
			break;
		}
	}

err:
	apk_db_close(&db);
	return r;
}

static struct apk_option fetch_options[] = {
	{ 'l', "link",		"Create hard links if possible" },
	{ 'R', "recursive",	"Fetch the PACKAGE and all it's dependencies" },
	{ 's', "stdout",
	  "Dump the .apk to stdout (incompatible with -o and -R)" },
	{ 'o', "output",	"Directory to place the PACKAGEs to",
	  required_argument, "DIR" },
};

static struct apk_applet apk_fetch = {
	.name = "fetch",
	.help = "Download PACKAGEs from repositories to a local directory from "
		"which a local mirror repository can be created.",
	.arguments = "PACKAGE...",
	.context_size = sizeof(struct fetch_ctx),
	.num_options = ARRAY_SIZE(fetch_options),
	.options = fetch_options,
	.parse = fetch_parse,
	.main = fetch_main,
};

APK_DEFINE_APPLET(apk_fetch);

