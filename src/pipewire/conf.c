/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <regex.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#if defined(__FreeBSD__) || defined(__MidnightBSD__) || defined(__GNU__)
#ifndef O_PATH
#define O_PATH 0
#endif
#endif

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/cleanup.h>
#include <pipewire/impl.h>
#include <pipewire/private.h>

PW_LOG_TOPIC_EXTERN(log_conf);
#define PW_LOG_TOPIC_DEFAULT log_conf

static int make_path(char *path, size_t size, const char *paths[])
{
	int i, len;
	char *p = path;
	for (i = 0; paths[i] != NULL; i++) {
		len = snprintf(p, size, "%s%s", i == 0 ? "" : "/", paths[i]);
		if (len < 0)
			return -errno;
		if ((size_t)len >= size)
			return -ENOSPC;
		p += len;
		size -= len;
	}
	return 0;
}

static int get_abs_path(char *path, size_t size, const char *prefix, const char *name)
{
	if (prefix[0] == '/') {
		const char *paths[] = { prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
		return -ENOENT;
	}
	return 0;
}

static int get_envconf_path(char *path, size_t size, const char *prefix, const char *name)
{
	const char *dir;

	dir = getenv("PIPEWIRE_CONFIG_DIR");
	if (dir != NULL) {
		const char *paths[] = { dir, prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
		return -ENOENT;
	}
	return 0;
}

static int get_homeconf_path(char *path, size_t size, const char *prefix, const char *name)
{
	char buffer[4096];
	const char *dir;

	dir = getenv("XDG_CONFIG_HOME");
	if (dir != NULL) {
		const char *paths[] = { dir, "pipewire", prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
	}
	dir = getenv("HOME");
	if (dir == NULL) {
		struct passwd pwd, *result = NULL;
		if (getpwuid_r(getuid(), &pwd, buffer, sizeof(buffer), &result) == 0)
			dir = result ? result->pw_dir : NULL;
	}
	if (dir != NULL) {
		const char *paths[] = { dir, ".config", "pipewire", prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
	}
	return 0;
}

static int get_configdir_path(char *path, size_t size, const char *prefix, const char *name)
{
	const char *dir;
	dir = PIPEWIRE_CONFIG_DIR;
	if (dir != NULL) {
		const char *paths[] = { dir, prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
	}
	return 0;
}

static int get_confdata_path(char *path, size_t size, const char *prefix, const char *name)
{
	const char *dir;
	dir = PIPEWIRE_CONFDATADIR;
	if (dir != NULL) {
		const char *paths[] = { dir, prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
	}
	return 0;
}

static int get_config_path(char *path, size_t size, const char *prefix, const char *name)
{
	int res;

	if (prefix == NULL) {
		prefix = name;
		name = NULL;
	}
	if ((res = get_abs_path(path, size, prefix, name)) != 0)
		return res;

	if (pw_check_option("no-config", "true"))
		goto no_config;

	if ((res = get_envconf_path(path, size, prefix, name)) != 0)
		return res;

	if ((res = get_homeconf_path(path, size, prefix, name)) != 0)
		return res;

	if ((res = get_configdir_path(path, size, prefix, name)) != 0)
		return res;
no_config:
	if ((res = get_confdata_path(path, size, prefix, name)) != 0)
		return res;
	return 0;
}

static int get_config_dir(char *path, size_t size, const char *prefix, const char *name, int *level)
{
	int res;
	bool no_config;

	if (prefix == NULL) {
		prefix = name;
		name = NULL;
	}
	if ((res = get_abs_path(path, size, prefix, name)) != 0) {
		if ((*level)++ == 0)
			return res;
		return -ENOENT;
	}

	no_config = pw_check_option("no-config", "true");
	if (no_config)
		goto no_config;

	if ((res = get_envconf_path(path, size, prefix, name)) != 0) {
		if ((*level)++ == 0)
			return res;
		return -ENOENT;
	}

	if (*level == 0) {
no_config:
		(*level)++;
		if ((res = get_confdata_path(path, size, prefix, name)) != 0)
			return res;
		if (no_config)
			return 0;
	}
	if (*level == 1) {
		(*level)++;
		if ((res = get_configdir_path(path, size, prefix, name)) != 0)
			return res;
	}
	if (*level == 2) {
		(*level)++;
		if ((res = get_homeconf_path(path, size, prefix, name)) != 0)
			return res;
	}
	return 0;
}

static int get_envstate_path(char *path, size_t size, const char *prefix, const char *name)
{
	const char *dir;
	dir = getenv("PIPEWIRE_STATE_DIR");
	if (dir != NULL) {
		const char *paths[] = { dir, prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
		return -ENOENT;
	}
	return 0;
}

static int get_homestate_path(char *path, size_t size, const char *prefix, const char *name)
{
	const char *dir;
	char buffer[4096];

	dir = getenv("XDG_STATE_HOME");
	if (dir != NULL) {
		const char *paths[] = { dir, "pipewire", prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
	}
	dir = getenv("HOME");
	if (dir == NULL) {
		struct passwd pwd, *result = NULL;
		if (getpwuid_r(getuid(), &pwd, buffer, sizeof(buffer), &result) == 0)
			dir = result ? result->pw_dir : NULL;
	}
	if (dir != NULL) {
		const char *paths[] = { dir, ".local", "state", "pipewire", prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
	}
	if (dir != NULL) {
		/* fallback for old XDG_CONFIG_HOME */
		const char *paths[] = { dir, ".config", "pipewire", prefix, name, NULL };
		if (make_path(path, size, paths) == 0 &&
		    access(path, R_OK) == 0)
			return 1;
	}
	return 0;
}

static int get_state_path(char *path, size_t size, const char *prefix, const char *name)
{
	int res;

	if (prefix == NULL) {
		prefix = name;
		name = NULL;
	}
	if ((res = get_abs_path(path, size, prefix, name)) != 0)
		return res;

	if ((res = get_envstate_path(path, size, prefix, name)) != 0)
		return res;

	if ((res = get_homestate_path(path, size, prefix, name)) != 0)
		return res;

	return 0;
}

static int ensure_path(char *path, int size, const char *paths[])
{
	int i, len, mode;
	char *p = path;

	for (i = 0; paths[i] != NULL; i++) {
		len = snprintf(p, size, "%s/", paths[i]);
		if (len < 0)
			return -errno;
		if (len >= size)
			return -ENOSPC;

		p += len;
		size -= len;

		mode = X_OK;
		if (paths[i+1] == NULL)
			mode |= R_OK | W_OK;

		if (access(path, mode) < 0) {
			if (errno != ENOENT)
				return -errno;
			if (mkdir(path, 0700) < 0) {
				pw_log_info("Can't create directory %s: %m", path);
                                return -errno;
			}
			if (access(path, mode) < 0)
				return -errno;

			pw_log_info("created directory %s", path);
		}
	}
	return 0;
}

static int open_write_dir(char *path, int size, const char *prefix)
{
	const char *dir;
	char buffer[4096];
	int res;

	if (prefix != NULL && prefix[0] == '/') {
		const char *paths[] = { prefix, NULL };
		if (ensure_path(path, size, paths) == 0)
			goto found;
	}
	dir = getenv("XDG_STATE_HOME");
	if (dir != NULL) {
		const char *paths[] = { dir, "pipewire", prefix, NULL };
		if (ensure_path(path, size, paths) == 0)
			goto found;
	}
	dir = getenv("HOME");
	if (dir == NULL) {
		struct passwd pwd, *result = NULL;
		if (getpwuid_r(getuid(), &pwd, buffer, sizeof(buffer), &result) == 0)
			dir = result ? result->pw_dir : NULL;
	}
	if (dir != NULL) {
		const char *paths[] = { dir, ".local", "state", "pipewire", prefix, NULL };
		if (ensure_path(path, size, paths) == 0)
			goto found;
	}
	return -ENOENT;
found:
	if ((res = open(path, O_CLOEXEC | O_DIRECTORY | O_PATH)) < 0) {
		pw_log_error("Can't open state directory %s: %m", path);
		return -errno;
	}
        return res;
}

SPA_EXPORT
int pw_conf_save_state(const char *prefix, const char *name, const struct pw_properties *conf)
{
	char path[PATH_MAX];
	char *tmp_name;
	spa_autoclose int sfd = -1;
	int res, fd, count = 0;
	FILE *f;

	if ((sfd = open_write_dir(path, sizeof(path), prefix)) < 0)
		return sfd;

	tmp_name = alloca(strlen(name)+5);
	sprintf(tmp_name, "%s.tmp", name);
	if ((fd = openat(sfd, tmp_name,  O_CLOEXEC | O_CREAT | O_WRONLY | O_TRUNC, 0600)) < 0) {
		res = -errno;
		pw_log_error("can't open file '%s': %m", tmp_name);
		return res;
	}

	f = fdopen(fd, "w");
	fprintf(f, "{");
	count += pw_properties_serialize_dict(f, &conf->dict, PW_PROPERTIES_FLAG_NL);
	fprintf(f, "%s}", count == 0 ? " " : "\n");
	fclose(f);

	if (renameat(sfd, tmp_name, sfd, name) < 0) {
		res = -errno;
		pw_log_error("can't rename temp file '%s': %m", tmp_name);
		return res;
	}

	pw_log_info("%p: saved state '%s%s'", conf, path, name);

	return 0;
}

static int conf_load(const char *path, struct pw_properties *conf)
{
	char *data;
	struct stat sbuf;
	int count;

	spa_autoclose int fd = open(path,  O_CLOEXEC | O_RDONLY);
	if (fd < 0)
		goto error;

	if (fstat(fd, &sbuf) < 0)
		goto error;

	if (sbuf.st_size > 0) {
		if ((data = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
			goto error;

		count = pw_properties_update_string(conf, data, sbuf.st_size);
		munmap(data, sbuf.st_size);
	} else {
		count = 0;
	}

	pw_log_info("%p: loaded config '%s' with %d items", conf, path, count);

	return 0;

error:
	pw_log_warn("%p: error loading config '%s': %m", conf, path);
	return -errno;
}

static bool check_override(struct pw_properties *conf, const char *name, int level)
{
	const struct spa_dict_item *it;

	spa_dict_for_each(it, &conf->dict) {
		int lev, idx;

		if (!spa_streq(name, it->value))
			continue;
		if (sscanf(it->key, "override.%d.%d.config.name", &lev, &idx) != 2)
			continue;
		if (lev < level)
			return false;
	}
	return true;
}

static void add_override(struct pw_properties *conf, struct pw_properties *override,
		const char *path, const char *name, int level, int index)
{
	const struct spa_dict_item *it;
	char key[1024];

	snprintf(key, sizeof(key), "override.%d.%d.config.path", level, index);
	pw_properties_set(conf, key, path);
	snprintf(key, sizeof(key), "override.%d.%d.config.name", level, index);
	pw_properties_set(conf, key, name);
	spa_dict_for_each(it, &override->dict) {
		snprintf(key, sizeof(key), "override.%d.%d.%s", level, index, it->key);
		pw_properties_set(conf, key, it->value);
	}
}

static int conf_filter(const struct dirent *entry)
{
	return spa_strendswith(entry->d_name, ".conf");
}

SPA_EXPORT
int pw_conf_load_conf(const char *prefix, const char *name, struct pw_properties *conf)
{
	char path[PATH_MAX];
	char fname[PATH_MAX + 256];
	int i, res, level = 0;
	spa_autoptr(pw_properties) override = NULL;
	const char *dname;

	if (name == NULL) {
		pw_log_debug("%p: config name must not be NULL", conf);
		return -EINVAL;
	}

	if (get_config_path(path, sizeof(path), prefix, name) == 0) {
		pw_log_debug("%p: can't load config '%s': %m", conf, path);
		return -ENOENT;
	}
	pw_properties_set(conf, "config.prefix", prefix);
	pw_properties_set(conf, "config.name", name);
	pw_properties_set(conf, "config.path", path);

	if ((res = conf_load(path, conf)) < 0)
		return res;

	pw_properties_setf(conf, "config.name.d", "%s.d", name);
	dname = pw_properties_get(conf, "config.name.d");

	while (true) {
		struct dirent **entries = NULL;
		int n;

		if (get_config_dir(path, sizeof(path), prefix, dname, &level) <= 0)
			break;

		n = scandir(path, &entries, conf_filter, alphasort);
		if (n == 0)
			continue;
		if (n < 0) {
			pw_log_warn("scandir %s failed: %m", path);
			continue;
		}
		if (override == NULL &&
		    (override = pw_properties_new(NULL, NULL)) == NULL)
			return -errno;

		for (i = 0; i < n; i++) {
			const char *name = entries[i]->d_name;

			snprintf(fname, sizeof(fname), "%s/%s", path, name);
			if (check_override(conf, name, level)) {
				if (conf_load(fname, override) >= 0)
					add_override(conf, override, fname, name, level, i);
				pw_properties_clear(override);
			} else {
				pw_log_info("skip override %s with lower priority", fname);
			}
			free(entries[i]);
		}
		free(entries);
	}

	return 0;
}

SPA_EXPORT
int pw_conf_load_state(const char *prefix, const char *name, struct pw_properties *conf)
{
	char path[PATH_MAX];

	if (name == NULL) {
		pw_log_debug("%p: config name must not be NULL", conf);
		return -EINVAL;
	}

	if (get_state_path(path, sizeof(path), prefix, name) == 0) {
		pw_log_debug("%p: can't load config '%s': %m", conf, path);
		return -ENOENT;
	}
	return conf_load(path, conf);
}

struct data {
	struct pw_context *context;
	struct pw_properties *props;
	int count;
};

/* context.spa-libs = {
 *  <factory-name regex> = <library-name>
 * }
 */
static int parse_spa_libs(void *user_data, const char *location,
		const char *section, const char *str, size_t len)
{
	struct data *d = user_data;
	struct pw_context *context = d->context;
	struct spa_json it[2];
	char key[512], value[512];

	spa_json_init(&it[0], str, len);
	if (spa_json_enter_object(&it[0], &it[1]) < 0) {
		pw_log_error("config file error: context.spa-libs is not an object");
		return -EINVAL;
	}

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_json_get_string(&it[1], value, sizeof(value)) > 0) {
			pw_context_add_spa_lib(context, key, value);
			d->count++;
		}
	}
	return 0;
}

static int load_module(struct pw_context *context, const char *key, const char *args, const char *flags)
{
	if (pw_context_load_module(context, key, args, NULL) == NULL) {
		if (errno == ENOENT && flags && strstr(flags, "ifexists") != NULL) {
			pw_log_info("%p: skipping unavailable module %s",
					context, key);
		} else if (flags == NULL || strstr(flags, "nofail") == NULL) {
			pw_log_error("%p: could not load mandatory module \"%s\": %m",
					context, key);
			return -errno;
		} else {
			pw_log_info("%p: could not load optional module \"%s\": %m",
					context, key);
		}
	} else {
		pw_log_info("%p: loaded module %s", context, key);
	}
	return 0;
}

/*
 * {
 *     # all keys must match the value. ~ in value starts regex.
 *     # ! as the first char of the value negates the match
 *     <key> = <value>
 *     ...
 * }
 */
static bool find_match(struct spa_json *arr, const struct spa_dict *props)
{
	struct spa_json it[1];

	while (spa_json_enter_object(arr, &it[0]) > 0) {
		char key[256], val[1024];
		const char *str, *value;
		int match = 0, fail = 0;
		int len;

		while (spa_json_get_string(&it[0], key, sizeof(key)) > 0) {
			bool success = false;
			int skip = 0;

			if ((len = spa_json_next(&it[0], &value)) <= 0)
				break;

			str = spa_dict_lookup(props, key);

			if (spa_json_is_null(value, len)) {
				success = str == NULL;
			} else {
				if (spa_json_parse_stringn(value, len, val, sizeof(val)) < 0)
					continue;
				value = val;
				len = strlen(val);
				if (len > 0 && value[0] == '!') {
					success = !success;
					skip++;
				}
			}
			if (str != NULL) {
				if (value[skip] == '~') {
					regex_t preg;
					int res;
					skip++;
					if ((res = regcomp(&preg, value+skip, REG_EXTENDED | REG_NOSUB)) != 0) {
						char errbuf[1024];
						regerror(res, &preg, errbuf, sizeof(errbuf));
						pw_log_warn("invalid regex %s: %s", value+skip, errbuf);
					} else {
						if (regexec(&preg, str, 0, NULL, 0) == 0)
							success = !success;
						regfree(&preg);
					}
				} else if (strncmp(str, value+skip, len-skip) == 0 &&
				    strlen(str) == (size_t)(len-skip)) {
					success = !success;
				}
			}
			if (success) {
				match++;
				pw_log_debug("'%s' match '%s' < > '%.*s'", key, str, len, value);
			}
			else {
				pw_log_debug("'%s' fail '%s' < > '%.*s'", key, str, len, value);
				fail++;
				break;
			}
		}
		if (match > 0 && fail == 0)
			return true;
	}
	return false;
}

/*
 * context.modules = [
 *   {   name = <module-name>
 *       ( args = { <key> = <value> ... } )
 *       ( flags = [ ( ifexists ) ( nofail ) ]
 *       ( condition = [ { key = value, .. } .. ] )
 *   }
 * ]
 */
static int parse_modules(void *user_data, const char *location,
		const char *section, const char *str, size_t len)
{
	struct data *d = user_data;
	struct pw_context *context = d->context;
	struct spa_json it[4];
	char key[512];
	int res = 0;

	spa_autofree char *s = strndup(str, len);
	spa_json_init(&it[0], s, len);
	if (spa_json_enter_array(&it[0], &it[1]) < 0) {
		pw_log_error("config file error: context.modules is not an array");
		return -EINVAL;
	}

	while (spa_json_enter_object(&it[1], &it[2]) > 0) {
		char *name = NULL, *args = NULL, *flags = NULL;
		bool have_match = true;

		while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
			const char *val;
			int len;

			if ((len = spa_json_next(&it[2], &val)) <= 0)
				break;

			if (spa_streq(key, "name")) {
				name = (char*)val;
				spa_json_parse_stringn(val, len, name, len+1);
			} else if (spa_streq(key, "args")) {
				if (spa_json_is_container(val, len))
					len = spa_json_container_len(&it[2], val, len);

				args = (char*)val;
				spa_json_parse_stringn(val, len, args, len+1);
			} else if (spa_streq(key, "flags")) {
				if (spa_json_is_container(val, len))
					len = spa_json_container_len(&it[2], val, len);
				flags = (char*)val;
				spa_json_parse_stringn(val, len, flags, len+1);
			} else if (spa_streq(key, "condition")) {
				if (!spa_json_is_array(val, len))
					break;
				spa_json_enter(&it[2], &it[3]);
				have_match = find_match(&it[3], &context->properties->dict);
			}
		}
		if (!have_match)
			continue;

		if (name != NULL)
			res = load_module(context, name, args, flags);

		if (res < 0)
			break;

		d->count++;
	}

	return res;
}

static int create_object(struct pw_context *context, const char *key, const char *args, const char *flags)
{
	struct pw_impl_factory *factory;
	void *obj;

	pw_log_debug("find factory %s", key);
	factory = pw_context_find_factory(context, key);
	if (factory == NULL) {
		if (flags && strstr(flags, "nofail") != NULL)
			return 0;
		pw_log_error("can't find factory %s", key);
		return -ENOENT;
	}
	pw_log_debug("create object with args %s", args);
	obj = pw_impl_factory_create_object(factory,
			NULL, NULL, 0,
			args ? pw_properties_new_string(args) : NULL,
			SPA_ID_INVALID);
	if (obj == NULL) {
		if (flags && strstr(flags, "nofail") != NULL)
			return 0;
		pw_log_error("can't create object from factory %s: %m", key);
		return -errno;
	}
	return 0;
}

/*
 * context.objects = [
 *   {   factory = <factory-name>
 *       ( args  = { <key> = <value> ... } )
 *       ( flags = [ ( nofail ) ] )
 *       ( condition = [ { key = value, .. } .. ] )
 *   }
 * ]
 */
static int parse_objects(void *user_data, const char *location,
		const char *section, const char *str, size_t len)
{
	struct data *d = user_data;
	struct pw_context *context = d->context;
	struct spa_json it[4];
	char key[512];
	int res = 0;

	spa_autofree char *s = strndup(str, len);
	spa_json_init(&it[0], s, len);
	if (spa_json_enter_array(&it[0], &it[1]) < 0) {
		pw_log_error("config file error: context.objects is not an array");
		return -EINVAL;
	}

	while (spa_json_enter_object(&it[1], &it[2]) > 0) {
		char *factory = NULL, *args = NULL, *flags = NULL;
		bool have_match = true;

		while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
			const char *val;
			int len;

			if ((len = spa_json_next(&it[2], &val)) <= 0)
				break;

			if (spa_streq(key, "factory")) {
				factory = (char*)val;
				spa_json_parse_stringn(val, len, factory, len+1);
			} else if (spa_streq(key, "args")) {
				if (spa_json_is_container(val, len))
					len = spa_json_container_len(&it[2], val, len);

				args = (char*)val;
				spa_json_parse_stringn(val, len, args, len+1);
			} else if (spa_streq(key, "flags")) {
				if (spa_json_is_container(val, len))
					len = spa_json_container_len(&it[2], val, len);

				flags = (char*)val;
				spa_json_parse_stringn(val, len, flags, len+1);
			} else if (spa_streq(key, "condition")) {
				if (!spa_json_is_array(val, len))
					break;
				spa_json_enter(&it[2], &it[3]);
				have_match = find_match(&it[3], &context->properties->dict);
			}
		}
		if (!have_match)
			continue;

		if (factory != NULL)
			res = create_object(context, factory, args, flags);

		if (res < 0)
			break;
		d->count++;
	}

	return res;
}

static int do_exec(struct pw_context *context, const char *key, const char *args)
{
	int pid, res, n_args;

	pid = fork();

	if (pid == 0) {
		char *cmd, **argv;

		/* Double fork to avoid zombies; we don't want to set SIGCHLD handler */
		pid = fork();

		if (pid < 0) {
			pw_log_error("fork error: %m");
			exit(1);
		} else if (pid != 0) {
			exit(0);
		}

		cmd = spa_aprintf("%s %s", key, args ? args : "");
		argv = pw_split_strv(cmd, " \t", INT_MAX, &n_args);
		free(cmd);

		pw_log_info("exec %s '%s'", key, args);
		res = execvp(key, argv);
		pw_free_strv(argv);

		if (res == -1) {
			res = -errno;
			pw_log_error("execvp error '%s': %m", key);
		}

		exit(1);
	} else if (pid < 0) {
		pw_log_error("fork error: %m");
	} else {
		int status = 0;
		do {
			errno = 0;
			res = waitpid(pid, &status, 0);
		} while (res < 0 && errno == EINTR);
		pw_log_debug("exec got pid %d res:%d status:%d", (int)pid, res, status);
	}
	return 0;
}

/*
 * context.exec = [
 *   {   path = <program-name>
 *       ( args = "<arguments>" )
 *       ( condition = [ { key = value, .. } .. ] )
 *   }
 * ]
 */
static int parse_exec(void *user_data, const char *location,
		const char *section, const char *str, size_t len)
{
	struct data *d = user_data;
	struct pw_context *context = d->context;
	struct spa_json it[4];
	char key[512];
	int res = 0;

	spa_autofree char *s = strndup(str, len);
	spa_json_init(&it[0], s, len);
	if (spa_json_enter_array(&it[0], &it[1]) < 0) {
		pw_log_error("config file error: context.exec is not an array");
		return -EINVAL;
	}

	while (spa_json_enter_object(&it[1], &it[2]) > 0) {
		char *path = NULL, *args = NULL;
		bool have_match = true;

		while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
			const char *val;
			int len;

			if ((len = spa_json_next(&it[2], &val)) <= 0)
				break;

			if (spa_streq(key, "path")) {
				path = (char*)val;
				spa_json_parse_stringn(val, len, path, len+1);
			} else if (spa_streq(key, "args")) {
				args = (char*)val;
				spa_json_parse_stringn(val, len, args, len+1);
			} else if (spa_streq(key, "condition")) {
				if (!spa_json_is_array(val, len))
					break;
				spa_json_enter(&it[2], &it[3]);
				have_match = find_match(&it[3], &context->properties->dict);
			}
		}
		if (!have_match)
			continue;

		if (path != NULL)
			res = do_exec(context, path, args);

		if (res < 0)
			break;

		d->count++;
	}

	return res;
}


SPA_EXPORT
int pw_conf_section_for_each(const struct spa_dict *conf, const char *section,
		int (*callback) (void *data, const char *location, const char *section,
			const char *str, size_t len),
		void *data)
{
	const char *path = NULL;
	const struct spa_dict_item *it;
	int res = 0;

	spa_dict_for_each(it, conf) {
		if (spa_strendswith(it->key, "config.path")) {
			path = it->value;
			continue;

		} else if (spa_streq(it->key, section)) {
			pw_log_info("handle config '%s' section '%s'", path, section);
		} else if (spa_strstartswith(it->key, "override.") &&
		    spa_strendswith(it->key, section)) {
			pw_log_info("handle override '%s' section '%s'", path, section);
		} else
			continue;

		res = callback(data, path, section, it->value, strlen(it->value));
		if (res != 0)
			break;
	}
	return res;
}

static int update_props(void *user_data, const char *location, const char *key,
			const char *val, size_t len)
{
	struct data *data = user_data;
	data->count += pw_properties_update_string(data->props, val, len);
	return 0;
}

SPA_EXPORT
int pw_conf_section_update_props(const struct spa_dict *conf,
		const char *section, struct pw_properties *props)
{
	struct data data = { .props = props };
	int res;
	const char *str;

	res = pw_conf_section_for_each(conf, section,
			update_props, &data);

	str = pw_properties_get(props, "config.ext");
	if (res == 0 && str != NULL) {
		char key[128];
		snprintf(key, sizeof(key), "%s.%s", section, str);
		res = pw_conf_section_for_each(conf, key,
				update_props, &data);
	}
	return res == 0 ? data.count : res;
}

static bool valid_conf_name(const char *str)
{
	return spa_streq(str, "null") || spa_strendswith(str, ".conf");
}

static int try_load_conf(const char *conf_prefix, const char *conf_name,
			 struct pw_properties *conf)
{
	int res;

	if (conf_name == NULL)
		return -EINVAL;
	if (spa_streq(conf_name, "null"))
		return 0;
	if ((res = pw_conf_load_conf(conf_prefix, conf_name, conf)) < 0) {
		bool skip_prefix = conf_prefix == NULL || conf_name[0] == '/';
		pw_log_warn("can't load config %s%s%s: %s",
				skip_prefix ? "" : conf_prefix,
				skip_prefix ? "" : "/",
				conf_name, spa_strerror(res));
	}
	return res;
}

SPA_EXPORT
int pw_conf_load_conf_for_context(struct pw_properties *props, struct pw_properties *conf)
{
	const char *conf_prefix, *conf_name;
	int res;

	conf_prefix = getenv("PIPEWIRE_CONFIG_PREFIX");
	if (conf_prefix == NULL)
		conf_prefix = pw_properties_get(props, PW_KEY_CONFIG_PREFIX);

	conf_name = getenv("PIPEWIRE_CONFIG_NAME");
	if ((res = try_load_conf(conf_prefix, conf_name, conf)) < 0) {
		conf_name = pw_properties_get(props, PW_KEY_CONFIG_NAME);
		if (conf_name == NULL)
			conf_name = "client.conf";
		else if (!valid_conf_name(conf_name)) {
			pw_log_error("%s '%s' does not end with .conf",
				PW_KEY_CONFIG_NAME, conf_name);
			return -EINVAL;
		}
		if ((res = try_load_conf(conf_prefix, conf_name, conf)) < 0) {
			pw_log_error("can't load config %s: %s",
				conf_name, spa_strerror(res));
			return res;
		}
	}

	conf_name = pw_properties_get(props, PW_KEY_CONFIG_OVERRIDE_NAME);
	if (conf_name != NULL) {
		struct pw_properties *override;
		const char *path, *name;

		if (!valid_conf_name(conf_name)) {
			pw_log_error("%s '%s' does not end with .conf",
				PW_KEY_CONFIG_OVERRIDE_NAME, conf_name);
			return -EINVAL;
		}

		override = pw_properties_new(NULL, NULL);
		if (override == NULL) {
			res = -errno;
			return res;
		}

		conf_prefix = pw_properties_get(props, PW_KEY_CONFIG_OVERRIDE_PREFIX);
		if ((res = try_load_conf(conf_prefix, conf_name, override)) < 0) {
			pw_log_error("can't load default override config %s: %s",
				conf_name, spa_strerror(res));
			pw_properties_free (override);
			return res;
		}
		path = pw_properties_get(override, "config.path");
		name = pw_properties_get(override, "config.name");
		add_override(conf, override, path, name, 0, 1);
		pw_properties_free(override);
	}

	return res;
}

/**
 * [
 *     {
 *         matches = [
 *             # any of the items in matches needs to match, if one does,
 *             # actions are emited.
 *             {
 *                 # all keys must match the value. ! negates. ~ starts regex.
 *                 <key> = <value>
 *                 ...
 *             }
 *             ...
 *         ]
 *         actions = {
 *             <action> = <value>
 *             ...
 *         }
 *     }
 * ]
 */
SPA_EXPORT
int pw_conf_match_rules(const char *str, size_t len, const char *location,
		const struct spa_dict *props,
		int (*callback) (void *data, const char *location, const char *action,
			const char *str, size_t len),
		void *data)
{
	const char *val;
	struct spa_json it[4], actions;

	spa_json_init(&it[0], str, len);
	if (spa_json_enter_array(&it[0], &it[1]) < 0)
		return 0;

	while (spa_json_enter_object(&it[1], &it[2]) > 0) {
		char key[64];
		bool have_match = false, have_actions = false;

		while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
			if (spa_streq(key, "matches")) {
				if (spa_json_enter_array(&it[2], &it[3]) < 0)
					break;

				have_match = find_match(&it[3], props);
			}
			else if (spa_streq(key, "actions")) {
				if (spa_json_enter_object(&it[2], &actions) > 0)
					have_actions = true;
			}
			else if (spa_json_next(&it[2], &val) <= 0)
                                break;
		}
		if (!have_match || !have_actions)
			continue;

		while (spa_json_get_string(&actions, key, sizeof(key)) > 0) {
			int res, len;
			pw_log_debug("action %s", key);

			if ((len = spa_json_next(&actions, &val)) <= 0)
				break;

			if (spa_json_is_container(val, len))
				len = spa_json_container_len(&actions, val, len);

			if ((res = callback(data, location, key, val, len)) < 0)
				return res;
		}
	}
	return 0;
}

struct match {
	const struct spa_dict *props;
	int (*matched) (void *data, const char *location, const char *action,
			const char *val, size_t len);
	void *data;
};

static int match_rules(void *data, const char *location, const char *section,
		const char *str, size_t len)
{
	struct match *match = data;
	return pw_conf_match_rules(str, len, location,
		match->props, match->matched, match->data);
}

SPA_EXPORT
int pw_conf_section_match_rules(const struct spa_dict *conf, const char *section,
		const struct spa_dict *props,
		int (*callback) (void *data, const char *location, const char *action,
			const char *str, size_t len),
		void *data)
{
	struct match match = {
		.props = props,
		.matched = callback,
		.data = data };
	int res;
	const char *str;

	res = pw_conf_section_for_each(conf, section,
			match_rules, &match);

	str = spa_dict_lookup(props, "config.ext");
	if (res == 0 && str != NULL) {
		char key[128];
		snprintf(key, sizeof(key), "%s.%s", section, str);
		res = pw_conf_section_for_each(conf, key,
				match_rules, &match);
	}
	return res;
}

SPA_EXPORT
int pw_context_conf_update_props(struct pw_context *context,
		const char *section, struct pw_properties *props)
{
	return pw_conf_section_update_props(&context->conf->dict,
			section, props);
}

SPA_EXPORT
int pw_context_conf_section_for_each(struct pw_context *context, const char *section,
		int (*callback) (void *data, const char *location, const char *section,
			const char *str, size_t len),
		void *data)
{
	return pw_conf_section_for_each(&context->conf->dict, section, callback, data);
}


SPA_EXPORT
int pw_context_parse_conf_section(struct pw_context *context,
		struct pw_properties *conf, const char *section)
{
	struct data data = { .context = context };
	int res;

	if (spa_streq(section, "context.spa-libs"))
		res = pw_conf_section_for_each(&conf->dict, section,
				parse_spa_libs, &data);
	else if (spa_streq(section, "context.modules"))
		res = pw_conf_section_for_each(&conf->dict, section,
				parse_modules, &data);
	else if (spa_streq(section, "context.objects"))
		res = pw_conf_section_for_each(&conf->dict, section,
				parse_objects, &data);
	else if (spa_streq(section, "context.exec"))
		res = pw_conf_section_for_each(&conf->dict, section,
				parse_exec, &data);
	else
		res = -EINVAL;

	return res == 0 ? data.count : res;
}

SPA_EXPORT
int pw_context_conf_section_match_rules(struct pw_context *context, const char *section,
		const struct spa_dict *props,
		int (*callback) (void *data, const char *location, const char *action,
			const char *str, size_t len),
		void *data)
{
	return pw_conf_section_match_rules(&context->conf->dict, section,
			props, callback, data);
}
