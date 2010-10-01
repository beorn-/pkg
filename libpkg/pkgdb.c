#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <err.h>
#include <fnmatch.h>
#include <regex.h>

#include "pkgdb.h"
#include "pkgdb_cache.h"


const char *
pkgdb_get_dir(void)
{
	const char *pkg_dbdir;

	if ((pkg_dbdir = getenv("PKG_DBDIR")) == NULL)
		pkg_dbdir = PKG_DBDIR;

	return pkg_dbdir;
}

int
pkgdb_open(struct pkgdb *db)
{
	char path[MAXPATHLEN];
	int fd;

	snprintf(path, sizeof(path), "%s/pkgdb.cache", pkgdb_get_dir());

	if ((fd = open(path, O_RDONLY)) != -1)
		fd = cdb_init(&db->db, fd);

	return (fd);
}

/* query formated using string key */
const void *
pkgdb_query(struct pkgdb *db, const char *fmt, ...)
{
	va_list args;
	char key[BUFSIZ];
	size_t len;
	const void *val;

	va_start(args, fmt);
	len = vsnprintf(key, sizeof(key), fmt, args);

	if (len != strlen(key)) {
		warnx("key too long:");
		vwarnx(fmt, args);
		va_end(args);
		return NULL;
	}

	va_end(args);

	if (cdb_find(&db->db, key, len) < 0)
		return NULL;

	db_get(val, &db->db);
	return (val);
}

/* query a pkg from db using index */
struct pkg *
pkgdb_pkg_query(struct pkgdb *db, size_t idx)
{
	struct pkg *pkg;

	if (db == NULL)
		return NULL;

	if (cdb_find(&db->db, &idx, sizeof(idx)) <= 0)
		return NULL;

	pkg = malloc(sizeof(*pkg));
	pkg->idx = idx;
	db_get(pkg->name_version, &db->db);

	pkg->name = pkgdb_query(db, PKGDB_NAME, idx);
	pkg->version = pkgdb_query(db, PKGDB_VERSION, idx);
	pkg->comment = pkgdb_query(db, PKGDB_COMMENT, idx);
	pkg->desc = pkgdb_query(db, PKGDB_DESC, idx);
	pkg->origin = pkgdb_query(db, PKGDB_ORIGIN, idx);

	return (pkg);
}

/* populate deps on pkg */
void
pkgdb_deps_query(struct pkgdb *db, struct pkg *pkg)
{
	struct cdb_find cdbf;
	size_t count = 0, j, klen;
	char key[BUFSIZ];
	struct pkg *dep;

	if (db == NULL || pkg == NULL)
		return;

	snprintf(key, BUFSIZ, PKGDB_DEPS, pkg->idx);
	klen = strlen(key);

	cdb_findinit(&cdbf, &db->db, key, klen);

	while (cdb_findnext(&cdbf) > 0)
		count++;

	pkg->deps = calloc(count+1, sizeof(*pkg->deps));
	pkg->deps[count] = NULL;

	cdb_findinit(&cdbf, &db->db, key, klen);

	j = 0;
	while (cdb_findnext(&cdbf) > 0) {
		dep = calloc(1, sizeof(*dep));
		db_get(dep->name_version, &db->db);
		pkg->deps[j++] = dep;
	}
}

/* populate rdeps on package */
static void
pkgdb_rdeps_query(struct pkgdb *db, struct pkg *pkg, size_t count)
{
	size_t i, j;
	struct pkg *p, **deps;

	if (db == NULL || pkg == NULL)
		return;

	pkg->rdeps = calloc(count+1, sizeof(struct pkg *));

	for (i = 0, j = 0; i < count; i++) {

		if ((p = pkgdb_pkg_query(db, i)) == NULL)
			continue;

		pkgdb_deps_query(db, p);

		for (deps = p->deps; *deps != NULL; deps++) {
			if (!((*deps)->errors & PKGERR_NOT_INSTALLED)
					&& strcmp((*deps)->name_version, pkg->name_version) == 0) {
				pkg->rdeps[j] = p;
				break;
			}
		}

		/* free deps */
		for (deps = p->deps; *deps != NULL; deps++)
			free(*deps);
		free(p->deps);

		if (pkg->rdeps[j] == p)
			j++;
		else
			free(p);
	}
	pkg->rdeps = realloc(pkg->rdeps, (j+1) * sizeof(struct pkg *));
	pkg->rdeps[j] = NULL;
	return;
}

static int
pkg_cmp(void const *a, void const *b)
{
	struct pkg * const *pa = a;
	struct pkg * const *pb = b;
	return (strcmp((*pa)->name, (*pb)->name));
}

static int
pkg_match(struct pkg *pkg, const regex_t *re, const char *pattern, match_t match)
{
	int matched = 1;
	switch (match) {
		case MATCH_ALL:
			matched = 0;
			break;
		case MATCH_EXACT:
			matched = strcmp(pkg->name_version, pattern);
			break;
		case MATCH_GLOB:
			matched = fnmatch(pattern, pkg->name_version, 0);
			break;
		case MATCH_REGEX:
		case MATCH_EREGEX:
			matched = regexec(re, pkg->name_version, 0, NULL, 0);
			break;
	}
	return (matched);
}

/*
 * Acquire a lock to access the database.
 * If `writer' is set to 1, an exclusive lock is requested so it wont mess up
 * with other writers or readers.
 */
void
pkgdb_lock(struct pkgdb *db, int writer)
{
	char fname[FILENAME_MAX];
	int flags;

	snprintf(fname, sizeof(fname), "%s/%s", pkgdb_get_dir(), PKGDB_LOCK);
	if ((db->lock_fd = open(fname, O_RDONLY | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH)) < 0)
		err(EXIT_FAILURE, "open(%s)", fname);

	if (writer == 1)
		flags = LOCK_EX;
	else
		flags = LOCK_SH;

	if (flock(db->lock_fd, flags) < 0)
		errx(EXIT_FAILURE, "unable to acquire a lock to the database");
}

void
pkgdb_unlock(struct pkgdb *db)
{
	flock(db->lock_fd, LOCK_UN);
	close(db->lock_fd);
	db->lock_fd = -1;
}

void
pkgdb_init(struct pkgdb *db, const char *pattern, match_t match, unsigned char flags) {
	size_t count, i;
	struct pkg *pkg;
	regex_t re;

	pkgdb_cache_update(db);

	if (pkgdb_open(db) == -1)
		return;

	pkgdb_lock(db, 0);

	db->count = 0;
	db->flags = flags;

	if (match != MATCH_ALL && pattern == NULL) {
		warnx("a pattern is required");
		return;
	}

	if (cdb_find(&db->db, PKGDB_COUNT, strlen(PKGDB_COUNT)) <= 0) {
		warnx("corrupted database");
		return;
	}

	cdb_read(&db->db, &count, sizeof(count), cdb_datapos(&db->db));
	db->pkgs = calloc(count+1, sizeof(struct pkg *));

	/* Regex initialisation */
	if (match == MATCH_REGEX) {
		if (regcomp(&re, pattern, REG_BASIC | REG_NOSUB) != 0) {
			warnx("'%s' is not a valid regular expression", pattern);
			return;
		}
	} else if (match == MATCH_EREGEX) {
		if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
			warnx("'%s' is not a valid extended regular expression", pattern);
			return;
		}
	}

	for (i = 0; i < count; i++) {
		/* get package */
		if ((pkg = pkgdb_pkg_query(db, i)) == NULL)
			continue;

		if (pkg_match(pkg, &re, pattern, match) == 0) {
			if (db->flags & PKGDB_INIT_DEPS)
				pkgdb_deps_query(db, pkg);
			if (db->flags & PKGDB_INIT_RDEPS)
				pkgdb_rdeps_query(db, pkg, count);
			db->pkgs[db->count++] = pkg;
		}
		else
			free(pkg);
	}

	if (match == MATCH_REGEX || match == MATCH_EREGEX)
		regfree(&re);

	/* sort packages */
	db->pkgs = realloc(db->pkgs, (db->count+1) * sizeof(struct pkg *));
	db->pkgs[db->count] = NULL;
	qsort(db->pkgs, db->count, sizeof(struct pkg *), pkg_cmp);

	pkgdb_unlock(db);
	return;
}

static void
pkg_free(struct pkgdb *db, struct pkg *pkg)
{
	struct pkg **deps;
	if (db->flags & PKGDB_INIT_DEPS) {
		if (!(pkg->errors & PKGERR_NOT_INSTALLED)) {
			for (deps = pkg->deps; *deps != NULL; deps++) {
				pkg_free(db, *deps);
			}
		}
	}
	free(pkg);
}

void
pkgdb_free(struct pkgdb *db)
{
	int fd;
	struct pkg *pkg, **deps;

	fd = cdb_fileno(&db->db);
	cdb_free(&db->db);
	close(fd);

	PKGDB_FOREACH(pkg, db) {
		if (db->flags & PKGDB_INIT_DEPS) {
			for (deps = pkg->deps; *deps != NULL; deps++)
				free(*deps);
			free(pkg->deps);
		}
		if (db->flags & PKGDB_INIT_RDEPS) {
			for (deps = pkg->rdeps; *deps != NULL; deps++)
				free(*deps);
			free(pkg->rdeps);
		}
		free(pkg);
	}

	free(db->pkgs);
}

size_t
pkgdb_count(struct pkgdb *db)
{
	return (db->count);
}


