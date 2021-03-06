
	/* $Id: fpm_conf.c,v 1.33.2.3 2008/12/13 03:50:29 anight Exp $ */
	/* (c) 2007,2008 Andrei Nigmatulin */

#include "fpm_config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#if HAVE_INTTYPES_H
# include <inttypes.h>
#else
# include <stdint.h>
#endif
#ifdef HAVE_GLOB
# ifndef PHP_WIN32
#  include <glob.h>
# else
#  include "win32/glob.h"
# endif
#endif

#include <stdio.h>
#include <unistd.h>

#include "php.h"
#include "zend_ini_scanner.h"
#include "zend_globals.h"
#include "zend_stream.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_stdio.h"
#include "fpm_worker_pool.h"
#include "fpm_cleanup.h"
#include "fpm_php.h"
#include "fpm_sockets.h"
#include "fpm_shm.h"
#include "fpm_status.h"
#include "fpm_log.h"
#include "zlog.h"

static int fpm_conf_load_ini_file(char *filename TSRMLS_DC);
static char *fpm_conf_set_integer(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_time(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_boolean(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_string(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_log_level(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_rlimit_core(zval *value, void **config, intptr_t offset);
static char *fpm_conf_set_pm(zval *value, void **config, intptr_t offset);

struct fpm_global_config_s fpm_global_config = { .daemonize = 1 };
static struct fpm_worker_pool_s *current_wp = NULL;
static int ini_recursion = 0;
static char *ini_filename = NULL;
static int ini_lineno = 0;
static char *ini_include = NULL;

#define GO(field) offsetof(struct fpm_global_config_s, field)
#define WPO(field) offsetof(struct fpm_worker_pool_config_s, field)

static struct ini_value_parser_s ini_fpm_global_options[] = {
	{ "emergency_restart_threshold", 	&fpm_conf_set_integer, 		GO(emergency_restart_threshold) },
	{ "emergency_restart_interval",		&fpm_conf_set_time,				GO(emergency_restart_interval) },
	{ "process_control_timeout",			&fpm_conf_set_time,				GO(process_control_timeout) },
	{ "daemonize",										&fpm_conf_set_boolean,		GO(daemonize) },
	{ "pid",													&fpm_conf_set_string,			GO(pid_file) },
	{ "error_log",										&fpm_conf_set_string,			GO(error_log) },
	{ "log_level",										&fpm_conf_set_log_level,	0 },
	{ "rlimit_files",									&fpm_conf_set_integer,		GO(rlimit_files) },
	{ "rlimit_core",									&fpm_conf_set_rlimit_core,GO(rlimit_core) },
	{ 0, 0, 0 }
};

static struct ini_value_parser_s ini_fpm_pool_options[] = {
	{ "prefix", &fpm_conf_set_string, WPO(prefix) },
	{ "user", &fpm_conf_set_string, WPO(user) },
	{ "group", &fpm_conf_set_string, WPO(group) },
	{ "chroot", &fpm_conf_set_string, WPO(chroot) },
	{ "chdir", &fpm_conf_set_string, WPO(chdir) },
	{ "request_terminate_timeout", &fpm_conf_set_time, WPO(request_terminate_timeout) },
	{ "request_slowlog_timeout", &fpm_conf_set_time, WPO(request_slowlog_timeout) },
	{ "slowlog", &fpm_conf_set_string, WPO(slowlog) },
	{ "rlimit_files", &fpm_conf_set_integer, WPO(rlimit_files) },
	{ "rlimit_core", &fpm_conf_set_rlimit_core, WPO(rlimit_core) },
	{ "catch_workers_output", &fpm_conf_set_boolean, WPO(catch_workers_output) },
	{ "listen", &fpm_conf_set_string, WPO(listen_address) },
	{ "listen.owner", &fpm_conf_set_string, WPO(listen_owner) },
	{ "listen.group", &fpm_conf_set_string, WPO(listen_group) },
	{ "listen.mode", &fpm_conf_set_string, WPO(listen_mode) },
	{ "listen.backlog", &fpm_conf_set_integer, WPO(listen_backlog) },
	{ "listen.allowed_clients", &fpm_conf_set_string, WPO(listen_allowed_clients) },
	{ "pm", &fpm_conf_set_pm, WPO(pm) },
	{ "pm.max_requests", &fpm_conf_set_integer, WPO(pm_max_requests) },
	{ "pm.max_children", &fpm_conf_set_integer, WPO(pm_max_children) },
	{ "pm.start_servers", &fpm_conf_set_integer, WPO(pm_start_servers) },
	{ "pm.min_spare_servers", &fpm_conf_set_integer, WPO(pm_min_spare_servers) },
	{ "pm.max_spare_servers", &fpm_conf_set_integer, WPO(pm_max_spare_servers) },
	{ "pm.status_path", &fpm_conf_set_string, WPO(pm_status_path) },
	{ "ping.path", &fpm_conf_set_string, WPO(ping_path) },
	{ "ping.response", &fpm_conf_set_string, WPO(ping_response) },
	{ "access.log", &fpm_conf_set_string, WPO(access_log) },
	{ "access.format", &fpm_conf_set_string, WPO(access_format) },
	{ 0, 0, 0 }
};

static int fpm_conf_is_dir(char *path) /* {{{ */
{
	struct stat sb;

	if (stat(path, &sb) != 0) {
		return 0;
	}

	return (sb.st_mode & S_IFMT) == S_IFDIR;
}
/* }}} */

static int fpm_conf_expand_pool_name(char **value) {
	char *token;

	if (!value || !*value) {
		return 0;
	}

	while ((token = strstr(*value, "$pool"))) {
		char *buf;
		char *p1 = *value;
		char *p2 = token + strlen("$pool");
		if (!current_wp || !current_wp->config  || !current_wp->config->name) {
			return -1;
		}
		token[0] = '\0';
		spprintf(&buf, 0, "%s%s%s", p1, current_wp->config->name, p2);
		*value = strdup(buf);
		efree(buf);
	}

	return 0;
}

static char *fpm_conf_set_boolean(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	long value_y = !strcasecmp(val, "1");
	long value_n = !strcasecmp(val, "");

	if (!value_y && !value_n) {
		return "invalid boolean value";
	}

	* (int *) ((char *) *config + offset) = value_y ? 1 : 0;
	return NULL;
}
/* }}} */

static char *fpm_conf_set_string(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *new;
	char **old = (char **) ((char *) *config + offset);
	if (*old) {
		return "it's already been defined. Can't do that twice.";
	}

	new = strdup(Z_STRVAL_P(value));
	if (!new) {
		return "fpm_conf_set_string(): strdup() failed";
	}
	if (fpm_conf_expand_pool_name(&new) == -1) {
		return "Can't use '$pool' when the pool is not defined";
	}

	*old = new;
	return NULL;
}
/* }}} */

static char *fpm_conf_set_integer(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	char *p;

	for(p=val; *p; p++) {
		if ( p == val && *p == '-' ) continue;
		if (*p < '0' || *p > '9') {
			return "is not a valid number (greater or equal than zero)";
		}
	}
	* (int *) ((char *) *config + offset) = atoi(val);
	return NULL;
}
/* }}} */

static char *fpm_conf_set_time(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	int len = strlen(val);
	char suffix;
	int seconds;
	if (!len) {
		return "invalid time value";
	}

	suffix = val[len-1];
	switch (suffix) {
		case 'm' :
			val[len-1] = '\0';
			seconds = 60 * atoi(val);
			break;
		case 'h' :
			val[len-1] = '\0';
			seconds = 60 * 60 * atoi(val);
			break;
		case 'd' :
			val[len-1] = '\0';
			seconds = 24 * 60 * 60 * atoi(val);
			break;
		case 's' : /* s is the default suffix */
			val[len-1] = '\0';
			suffix = '0';
		default :
			if (suffix < '0' || suffix > '9') {
				return "unknown suffix used in time value";
			}
			seconds = atoi(val);
			break;
	}

	* (int *) ((char *) *config + offset) = seconds;
	return NULL;
}
/* }}} */

static char *fpm_conf_set_log_level(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);

	if (!strcasecmp(val, "debug")) {
		fpm_globals.log_level = ZLOG_DEBUG;
	} else if (!strcasecmp(val, "notice")) {
		fpm_globals.log_level = ZLOG_NOTICE;
	} else if (!strcasecmp(val, "warning") || !strcasecmp(val, "warn")) {
		fpm_globals.log_level = ZLOG_WARNING;
	} else if (!strcasecmp(val, "error")) {
		fpm_globals.log_level = ZLOG_ERROR;
	} else if (!strcasecmp(val, "alert")) {
		fpm_globals.log_level = ZLOG_ALERT;
	} else {
		return "invalid value for 'log_level'";
	}

	return NULL;
}
/* }}} */

static char *fpm_conf_set_rlimit_core(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	int *ptr = (int *) ((char *) *config + offset);

	if (!strcasecmp(val, "unlimited")) {
		*ptr = -1;
	} else {
		int int_value;
		void *subconf = &int_value;
		char *error;

		error = fpm_conf_set_integer(value, &subconf, 0);

		if (error) { 
			return error;
		}

		if (int_value < 0) {
			return "must be greater than zero or 'unlimited'";
		}

		*ptr = int_value;
	}

	return NULL;
}
/* }}} */

static char *fpm_conf_set_pm(zval *value, void **config, intptr_t offset) /* {{{ */
{
	char *val = Z_STRVAL_P(value);
	struct fpm_worker_pool_config_s  *c = *config;
	if (!strcasecmp(val, "static")) {
		c->pm = PM_STYLE_STATIC;
	} else if (!strcasecmp(val, "dynamic")) {
		c->pm = PM_STYLE_DYNAMIC;
	} else {
		return "invalid process manager (static or dynamic)";
	}
	return NULL;
}
/* }}} */

static char *fpm_conf_set_array(zval *key, zval *value, void **config, int convert_to_bool) /* {{{ */
{
	struct key_value_s *kv;
	struct key_value_s ***parent = (struct key_value_s ***) config;
	int b;
	void *subconf = &b;

	kv = malloc(sizeof(*kv));

	if (!kv) {
		return "malloc() failed";
	}

	memset(kv, 0, sizeof(*kv));
	kv->key = strdup(Z_STRVAL_P(key));

	if (!kv->key) {
		return "fpm_conf_set_array: strdup(key) failed";
	}

	if (convert_to_bool) {
		char *err = fpm_conf_set_boolean(value, &subconf, 0);
		if (err) return err;
		kv->value = strdup(b ? "On" : "Off");
	} else {
		kv->value = strdup(Z_STRVAL_P(value));
		if (fpm_conf_expand_pool_name(&kv->value) == -1) {
			return "Can't use '$pool' when the pool is not defined";
		}
	}

	if (!kv->value) {
		free(kv->key);
		return "fpm_conf_set_array: strdup(value) failed";
	}

	kv->next = **parent;
	**parent = kv;
	return NULL;
}
/* }}} */

static void *fpm_worker_pool_config_alloc() /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	wp = fpm_worker_pool_alloc();

	if (!wp) {
		return 0;
	}

	wp->config = malloc(sizeof(struct fpm_worker_pool_config_s));

	if (!wp->config) { 
		return 0;
	}

	memset(wp->config, 0, sizeof(struct fpm_worker_pool_config_s));
	wp->config->listen_backlog = FPM_BACKLOG_DEFAULT;

	if (!fpm_worker_all_pools) {
		fpm_worker_all_pools = wp;
	} else {
		struct fpm_worker_pool_s *tmp = fpm_worker_all_pools;
		while (tmp) {
			if (!tmp->next) {
				tmp->next = wp;
				break;
			}
			tmp = tmp->next;
		}
	}

	current_wp = wp;
	return wp->config;
}
/* }}} */

int fpm_worker_pool_config_free(struct fpm_worker_pool_config_s *wpc) /* {{{ */
{
	struct key_value_s *kv, *kv_next;

	free(wpc->name);
	free(wpc->pm_status_path);
	free(wpc->ping_path);
	free(wpc->ping_response);
	free(wpc->listen_address);
	free(wpc->listen_owner);
	free(wpc->listen_group);
	free(wpc->listen_mode);
	for (kv = wpc->php_values; kv; kv = kv_next) {
		kv_next = kv->next;
		free(kv->key);
		free(kv->value);
		free(kv);
	}
	for (kv = wpc->php_admin_values; kv; kv = kv_next) {
		kv_next = kv->next;
		free(kv->key);
		free(kv->value);
		free(kv);
	}
	for (kv = wpc->env; kv; kv = kv_next) {
		kv_next = kv->next;
		free(kv->key);
		free(kv->value);
		free(kv);
	}
	free(wpc->listen_allowed_clients);
	free(wpc->user);
	free(wpc->group);
	free(wpc->chroot);
	free(wpc->chdir);
	free(wpc->slowlog);
	free(wpc->prefix);
	free(wpc->access_log);
	free(wpc->access_format);

	return 0;
}
/* }}} */

static int fpm_evaluate_full_path(char **path, struct fpm_worker_pool_s *wp, char *default_prefix, int expand) /* {{{ */
{
	char *prefix = NULL;
	char *full_path;

	if (!path || !*path || **path == '/') {
		return 0;
	}

	if (wp && wp->config) {
		prefix = wp->config->prefix;
	}

	/* if the wp prefix is not set */
	if (prefix == NULL) {
		prefix = fpm_globals.prefix;
	}

	/* if the global prefix is not set */
	if (prefix == NULL) {
		prefix = default_prefix ? default_prefix : PHP_PREFIX;
	}

	if (expand) {
		char *tmp;
		tmp = strstr(*path, "$prefix");
		if (tmp != NULL) {

			if (tmp != *path) {
				zlog(ZLOG_ERROR, "'$prefix' must be use at the begining of the value");
				return -1;
			}

			if (strlen(*path) > strlen("$prefix")) {
				free(*path);
				tmp = strdup((*path) + strlen("$prefix"));
				*path = tmp;
			} else {
				free(*path);
				*path = NULL;
			}
		}
	}

	if (*path) {
		spprintf(&full_path, 0, "%s/%s", prefix, *path);
		free(*path);
		*path = strdup(full_path);
		efree(full_path);
	} else {
		*path = strdup(prefix);
	}

	if (**path != '/' && wp != NULL && wp->config) {
		return fpm_evaluate_full_path(path, NULL, default_prefix, expand);
	}
	return 0;
}
/* }}} */

static int fpm_conf_process_all_pools() /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	if (!fpm_worker_all_pools) {
		zlog(ZLOG_ERROR, "at least one pool section must be specified in config file");
		return -1;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {

		if (wp->config->prefix && *wp->config->prefix) {
			fpm_evaluate_full_path(&wp->config->prefix, NULL, NULL, 0);

			if (!fpm_conf_is_dir(wp->config->prefix)) {
				zlog(ZLOG_ERROR, "[pool %s] the prefix '%s' does not exist or is not a directory", wp->config->name, wp->config->prefix);
				return -1;
			}
		}

		if (wp->config->listen_address && *wp->config->listen_address) {
			wp->listen_address_domain = fpm_sockets_domain_from_address(wp->config->listen_address);

			if (wp->listen_address_domain == FPM_AF_UNIX && *wp->config->listen_address != '/') {
				fpm_evaluate_full_path(&wp->config->listen_address, wp, NULL, 0);
			}
		} else {
			zlog(ZLOG_ALERT, "[pool %s] no listen address have been defined!", wp->config->name);
			return -1;
		}

		if (!wp->config->user) {
			zlog(ZLOG_ALERT, "[pool %s] user has not been defined", wp->config->name);
			return -1;
		}

		if (wp->config->pm != PM_STYLE_STATIC && wp->config->pm != PM_STYLE_DYNAMIC) {
			zlog(ZLOG_ALERT, "[pool %s] the process manager is missing (static or dynamic)", wp->config->name);
			return -1;
		}

		if (wp->config->pm_max_children < 1) {
			zlog(ZLOG_ALERT, "[pool %s] pm.max_children must be a positive value", wp->config->name);
			return -1;
		}

		if (wp->config->pm == PM_STYLE_DYNAMIC) {
			struct fpm_worker_pool_config_s *config = wp->config;

			if (config->pm_min_spare_servers <= 0) {
				zlog(ZLOG_ALERT, "[pool %s] pm.min_spare_servers(%d) must be a positive value", wp->config->name, config->pm_min_spare_servers);
				return -1;
			}

			if (config->pm_max_spare_servers <= 0) {
				zlog(ZLOG_ALERT, "[pool %s] pm.max_spare_servers(%d) must be a positive value", wp->config->name, config->pm_max_spare_servers);
				return -1;
			}

			if (config->pm_min_spare_servers > config->pm_max_children ||
					config->pm_max_spare_servers > config->pm_max_children) {
				zlog(ZLOG_ALERT, "[pool %s] pm.min_spare_servers(%d) and pm.max_spare_servers(%d) cannot be greater than pm.max_children(%d)",
						wp->config->name, config->pm_min_spare_servers, config->pm_max_spare_servers, config->pm_max_children);
				return -1;
			}

			if (config->pm_max_spare_servers < config->pm_min_spare_servers) {
				zlog(ZLOG_ALERT, "[pool %s] pm.max_spare_servers(%d) must not be less than pm.min_spare_servers(%d)", wp->config->name, config->pm_max_spare_servers, config->pm_min_spare_servers);
				return -1;
			}

			if (config->pm_start_servers <= 0) {
				config->pm_start_servers = config->pm_min_spare_servers + ((config->pm_max_spare_servers - config->pm_min_spare_servers) / 2);
				zlog(ZLOG_WARNING, "[pool %s] pm.start_servers is not set. It's been set to %d.", wp->config->name, config->pm_start_servers);
			} else if (config->pm_start_servers < config->pm_min_spare_servers || config->pm_start_servers > config->pm_max_spare_servers) {
				zlog(ZLOG_ALERT, "[pool %s] pm.start_servers(%d) must not be less than pm.min_spare_servers(%d) and not greater than pm.max_spare_servers(%d)", wp->config->name, config->pm_start_servers, config->pm_min_spare_servers, config->pm_max_spare_servers);
				return -1;
			}

		}

		if (wp->config->slowlog && *wp->config->slowlog) {
			fpm_evaluate_full_path(&wp->config->slowlog, wp, NULL, 0);
		}

		if (wp->config->request_slowlog_timeout) {
#if HAVE_FPM_TRACE
			if (! (wp->config->slowlog && *wp->config->slowlog)) {
				zlog(ZLOG_ERROR, "[pool %s] 'slowlog' must be specified for use with 'request_slowlog_timeout'", wp->config->name);
				return -1;
			}
#else
			static int warned = 0;

			if (!warned) {
				zlog(ZLOG_WARNING, "[pool %s] 'request_slowlog_timeout' is not supported on your system",	wp->config->name);
				warned = 1;
			}

			wp->config->request_slowlog_timeout = 0;
#endif

			if (wp->config->slowlog && *wp->config->slowlog) {
				int fd;

				fd = open(wp->config->slowlog, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);

				if (0 > fd) {
					zlog(ZLOG_SYSERROR, "open(%s) failed", wp->config->slowlog);
					return -1;
				}
				close(fd);
			}
		}

		if (wp->config->ping_path && *wp->config->ping_path) {
			char *ping = wp->config->ping_path;
			int i;

			if (*ping != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the ping path '%s' must start with a '/'", wp->config->name, ping);
				return -1;
			}

			if (strlen(ping) < 2) {
				zlog(ZLOG_ERROR, "[pool %s] the ping path '%s' is not long enough", wp->config->name, ping);
				return -1;
			}

			for (i=0; i<strlen(ping); i++) {
				if (!isalnum(ping[i]) && ping[i] != '/' && ping[i] != '-' && ping[i] != '_' && ping[i] != '.') {
					zlog(ZLOG_ERROR, "[pool %s] the ping path '%s' must containt only the following characters '[alphanum]/_-.'", wp->config->name, ping);
					return -1;
				}
			}

			if (!wp->config->ping_response) {
				wp->config->ping_response = strdup("pong");
			} else {
				if (strlen(wp->config->ping_response) < 1) {
					zlog(ZLOG_ERROR, "[pool %s] the ping response page '%s' is not long enough", wp->config->name, wp->config->ping_response);
					return -1;
				}
			}
		} else {
			if (wp->config->ping_response) {
				free(wp->config->ping_response);
				wp->config->ping_response = NULL;
			}
		}

		if (wp->config->pm_status_path && *wp->config->pm_status_path) {
			int i;
			char *status = wp->config->pm_status_path;
			/* struct fpm_status_s fpm_status; */

			if (*status != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the status path '%s' must start with a '/'", wp->config->name, status);
				return -1;
			}

			if (strlen(status) < 2) {
				zlog(ZLOG_ERROR, "[pool %s] the status path '%s' is not long enough", wp->config->name, status);
				return -1;
			}

			for (i=0; i<strlen(status); i++) {
				if (!isalnum(status[i]) && status[i] != '/' && status[i] != '-' && status[i] != '_' && status[i] != '.') {
					zlog(ZLOG_ERROR, "[pool %s] the status path '%s' must contain only the following characters '[alphanum]/_-.'", wp->config->name, status);
					return -1;
				}
			}
			wp->shm_status = fpm_shm_alloc(sizeof(struct fpm_status_s));
			if (!wp->shm_status) {
				zlog(ZLOG_ERROR, "[pool %s] unable to allocate shared memory for status page '%s'", wp->config->name, status);
				return -1;
			}
			fpm_status_update_accepted_conn(wp->shm_status, 0);
			fpm_status_update_activity(wp->shm_status, -1, -1, -1, 0, -1, 1);
			fpm_status_update_max_children_reached(wp->shm_status, 0);
			fpm_status_set_pm(wp->shm_status, wp->config->pm);
			/* memset(&fpm_status.last_update, 0, sizeof(fpm_status.last_update)); */
		}

		if (wp->config->access_log && *wp->config->access_log) {
			fpm_evaluate_full_path(&wp->config->access_log, wp, NULL, 0);
			if (!wp->config->access_format) {
				wp->config->access_format = strdup("%R - %u %t \"%m %r\" %s");
			}
		}

		if (wp->config->chroot && *wp->config->chroot) {

			fpm_evaluate_full_path(&wp->config->chroot, wp, NULL, 1);

			if (*wp->config->chroot != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the chroot path '%s' must start with a '/'", wp->config->name, wp->config->chroot);
				return -1;
			}
			if (!fpm_conf_is_dir(wp->config->chroot)) {
				zlog(ZLOG_ERROR, "[pool %s] the chroot path '%s' does not exist or is not a directory", wp->config->name, wp->config->chroot);
				return -1;
			}
		}

		if (wp->config->chdir && *wp->config->chdir) {

			fpm_evaluate_full_path(&wp->config->chdir, wp, NULL, 0);

			if (*wp->config->chdir != '/') {
				zlog(ZLOG_ERROR, "[pool %s] the chdir path '%s' must start with a '/'", wp->config->name, wp->config->chdir);
				return -1;
			}

			if (wp->config->chroot) {
				char *buf;

				spprintf(&buf, 0, "%s/%s", wp->config->chroot, wp->config->chdir);

				if (!fpm_conf_is_dir(buf)) {
					zlog(ZLOG_ERROR, "[pool %s] the chdir path '%s' within the chroot path '%s' ('%s') does not exist or is not a directory", wp->config->name, wp->config->chdir, wp->config->chroot, buf);
					efree(buf);
					return -1;
				}

				efree(buf);
			} else {
				if (!fpm_conf_is_dir(wp->config->chdir)) {
					zlog(ZLOG_ERROR, "[pool %s] the chdir path '%s' does not exist or is not a directory", wp->config->name, wp->config->chdir);
					return -1;
				}
			}
		}
		if (!wp->config->chroot) {
			struct key_value_s *kv;
			char *options[] = FPM_PHP_INI_TO_EXPAND;
			char **p;

			for (kv = wp->config->php_values; kv; kv = kv->next) {
				for (p=options; *p; p++) {
					if (!strcasecmp(kv->key, *p)) {
						fpm_evaluate_full_path(&kv->value, wp, NULL, 0);
					}
				}
			}
			for (kv = wp->config->php_admin_values; kv; kv = kv->next) {
				for (p=options; *p; p++) {
					if (!strcasecmp(kv->key, *p)) {
						fpm_evaluate_full_path(&kv->value, wp, NULL, 0);
					}
				}
			}
		}
	}
	return 0;
}
/* }}} */

int fpm_conf_unlink_pid() /* {{{ */
{
	if (fpm_global_config.pid_file) {
		if (0 > unlink(fpm_global_config.pid_file)) {
			zlog(ZLOG_SYSERROR, "unlink(\"%s\") failed", fpm_global_config.pid_file);
			return -1;
		}
	}
	return 0;
}
/* }}} */

int fpm_conf_write_pid() /* {{{ */
{
	int fd;

	if (fpm_global_config.pid_file) {
		char buf[64];
		int len;

		unlink(fpm_global_config.pid_file);
		fd = creat(fpm_global_config.pid_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

		if (fd < 0) {
			zlog(ZLOG_SYSERROR, "creat(\"%s\") failed", fpm_global_config.pid_file);
			return -1;
		}

		len = sprintf(buf, "%d", (int) fpm_globals.parent_pid);

		if (len != write(fd, buf, len)) {
			zlog(ZLOG_SYSERROR, "write() failed");
			return -1;
		}
		close(fd);
	}
	return 0;
}
/* }}} */

static int fpm_conf_post_process(TSRMLS_D) /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	if (fpm_global_config.pid_file) {
		fpm_evaluate_full_path(&fpm_global_config.pid_file, NULL, PHP_LOCALSTATEDIR, 0);
	}

	if (!fpm_global_config.error_log) {
		fpm_global_config.error_log = strdup("log/php-fpm.log");
	}

	fpm_evaluate_full_path(&fpm_global_config.error_log, NULL, PHP_LOCALSTATEDIR, 0);

	if (0 > fpm_stdio_open_error_log(0)) {
		return -1;
	}

	if (0 > fpm_log_open(0)) {
		return -1;
	}

	if (0 > fpm_conf_process_all_pools()) {
		return -1;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (!wp->config->access_log || !*wp->config->access_log) {
			continue;
		}
		if (0 > fpm_log_write(wp->config->access_format TSRMLS_CC)) {
			zlog(ZLOG_ERROR, "[pool %s] wrong format for access.format '%s'", wp->config->name, wp->config->access_format);
			return -1;
		}
	}

	return 0;
}
/* }}} */

static void fpm_conf_cleanup(int which, void *arg) /* {{{ */
{
	free(fpm_global_config.pid_file);
	free(fpm_global_config.error_log);
	fpm_global_config.pid_file = 0;
	fpm_global_config.error_log = 0;
	free(fpm_globals.config);
}
/* }}} */

static void fpm_conf_ini_parser_include(char *inc, void *arg TSRMLS_DC) /* {{{ */
{
	char *filename;
	int *error = (int *)arg;;
	glob_t g;
	int i;

	if (!inc || !arg) return;
	if (*error) return; /* We got already an error. Switch to the end. */
	spprintf(&filename, 0, "%s", ini_filename); 

#ifdef HAVE_GLOB
	{
		g.gl_offs = 0;
		if ((i = glob(inc, GLOB_ERR | GLOB_MARK | GLOB_NOSORT, NULL, &g)) != 0) {
#ifdef GLOB_NOMATCH
			if (i == GLOB_NOMATCH) {
				zlog(ZLOG_WARNING, "Nothing matches the include pattern '%s' from %s at line %d.", inc, filename, ini_lineno);
				efree(filename);
				return;
			} 
#endif /* GLOB_NOMATCH */
			zlog(ZLOG_ERROR, "Unable to globalize '%s' (ret=%d) from %s at line %d.", inc, i, filename, ini_lineno);
			*error = 1;
			efree(filename);
			return;
		}

		for(i=0; i<g.gl_pathc; i++) {
			int len = strlen(g.gl_pathv[i]);
			if (len < 1) continue;
			if (g.gl_pathv[i][len - 1] == '/') continue; /* don't parse directories */
			if (0 > fpm_conf_load_ini_file(g.gl_pathv[i] TSRMLS_CC)) {
				zlog(ZLOG_ERROR, "Unable to include %s from %s at line %d", g.gl_pathv[i], filename, ini_lineno);
				*error = 1;
				efree(filename);
				return;
			}
		}
		globfree(&g);
	}
#else /* HAVE_GLOB */
	if (0 > fpm_conf_load_ini_file(inc TSRMLS_CC)) {
		zlog(ZLOG_ERROR, "Unable to include %s from %s at line %d", inc, filename, ini_lineno);
		*error = 1;
		efree(filename);
		return;
	}
#endif /* HAVE_GLOB */

	efree(filename);
}
/* }}} */

static void fpm_conf_ini_parser_section(zval *section, void *arg TSRMLS_DC) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	struct fpm_worker_pool_config_s *config;
	int *error = (int *)arg;

	/* switch to global conf */
	if (!strcasecmp(Z_STRVAL_P(section), "global")) {
		current_wp = NULL;
		return;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (!wp->config) continue;
		if (!wp->config->name) continue;
		if (!strcasecmp(wp->config->name, Z_STRVAL_P(section))) {
			/* Found a wp with the same name. Bring it back */
			current_wp = wp;
			return;
		}
	}

	/* it's a new pool */
	config = (struct fpm_worker_pool_config_s *)fpm_worker_pool_config_alloc();
	if (!current_wp || !config) {
		zlog(ZLOG_ERROR, "[%s:%d] Unable to alloc a new WorkerPool for worker '%s'", ini_filename, ini_lineno, Z_STRVAL_P(section));
		*error = 1;
		return;
	}
	config->name = strdup(Z_STRVAL_P(section));
	if (!config->name) {
		zlog(ZLOG_ERROR, "[%s:%d] Unable to alloc memory for configuration name for worker '%s'", ini_filename, ini_lineno, Z_STRVAL_P(section));
		*error = 1;
		return;
	}
}
/* }}} */

static void fpm_conf_ini_parser_entry(zval *name, zval *value, void *arg TSRMLS_DC) /* {{{ */
{
	struct ini_value_parser_s *parser;
	void *config = NULL;

	int *error = (int *)arg;
	if (!value) {
		zlog(ZLOG_ERROR, "[%s:%d] value is NULL for a ZEND_INI_PARSER_ENTRY", ini_filename, ini_lineno);
		*error = 1;
		return;
	}

	if (!strcmp(Z_STRVAL_P(name), "include")) {
		if (ini_include) {
			zlog(ZLOG_ERROR, "[%s:%d] two includes at the same time !", ini_filename, ini_lineno);
			*error = 1;
			return;
		}
		ini_include = strdup(Z_STRVAL_P(value));
		return;
	}

	if (!current_wp) { /* we are in the global section */
		parser = ini_fpm_global_options;
		config = &fpm_global_config;
	} else {
		parser = ini_fpm_pool_options;
		config = current_wp->config;
	}

	for (;parser->name; parser++) {
		if (!strcasecmp(parser->name, Z_STRVAL_P(name))) {
			char *ret;
			if (!parser->parser) {
				zlog(ZLOG_ERROR, "[%s:%d] the parser for entry '%s' is not defined", ini_filename, ini_lineno, parser->name);
				*error = 1;
				return;
			}

			ret = parser->parser(value, &config, parser->offset);
			if (ret) {
				zlog(ZLOG_ERROR, "[%s:%d] unable to parse value for entry '%s': %s", ini_filename, ini_lineno, parser->name, ret);
				*error = 1;
				return;
			}

			/* all is good ! */
			return;
		}
	}

	/* nothing has been found if we got here */
	zlog(ZLOG_ERROR, "[%s:%d] unknown entry '%s'", ini_filename, ini_lineno, Z_STRVAL_P(name));
	*error = 1;
}
/* }}} */

static void fpm_conf_ini_parser_array(zval *name, zval *key, zval *value, void *arg TSRMLS_DC) /* {{{ */
{
	int *error = (int *)arg;
	char *err = NULL;
	void *config;

	if (!Z_STRVAL_P(key) || !Z_STRVAL_P(value) || !*Z_STRVAL_P(key)) {
		zlog(ZLOG_ERROR, "[%s:%d] Misspelled  array ?", ini_filename, ini_lineno);
		*error = 1;
		return;
	}
	if (!current_wp || !current_wp->config) {
		zlog(ZLOG_ERROR, "[%s:%d] Array are not allowed in the global section", ini_filename, ini_lineno);
		*error = 1;
		return;
	}

	if (!strcmp("env", Z_STRVAL_P(name))) {
		if (!*Z_STRVAL_P(value)) {
			zlog(ZLOG_ERROR, "[%s:%d] empty value", ini_filename, ini_lineno);
			*error = 1;
			return;
		}
		config = (char *)current_wp->config + WPO(env);
		err = fpm_conf_set_array(key, value, &config, 0);

	} else if (!strcmp("php_value", Z_STRVAL_P(name))) {
		if (!*Z_STRVAL_P(value)) {
			zlog(ZLOG_ERROR, "[%s:%d] empty value", ini_filename, ini_lineno);
			*error = 1;
			return;
		}
		config = (char *)current_wp->config + WPO(php_values);
		err = fpm_conf_set_array(key, value, &config, 0);

	} else if (!strcmp("php_admin_value", Z_STRVAL_P(name))) {
		if (!*Z_STRVAL_P(value)) {
			zlog(ZLOG_ERROR, "[%s:%d] empty value", ini_filename, ini_lineno);
			*error = 1;
			return;
		}
		config = (char *)current_wp->config + WPO(php_admin_values);
		err = fpm_conf_set_array(key, value, &config, 0);

	} else if (!strcmp("php_flag", Z_STRVAL_P(name))) {
		config = (char *)current_wp->config + WPO(php_values);
		err = fpm_conf_set_array(key, value, &config, 1);

	} else if (!strcmp("php_admin_flag", Z_STRVAL_P(name))) {
		config = (char *)current_wp->config + WPO(php_admin_values);
		err = fpm_conf_set_array(key, value, &config, 1);

	} else {
		zlog(ZLOG_ERROR, "[%s:%d] unknown directive '%s'", ini_filename, ini_lineno, Z_STRVAL_P(name));
		*error = 1;
		return;
	}

	if (err) {
		zlog(ZLOG_ERROR, "[%s:%d] error while parsing '%s[%s]' : %s", ini_filename, ini_lineno, Z_STRVAL_P(name), Z_STRVAL_P(key), err);
		*error = 1;
		return;
	}
}
/* }}} */

static void fpm_conf_ini_parser(zval *arg1, zval *arg2, zval *arg3, int callback_type, void *arg TSRMLS_DC) /* {{{ */
{
	int *error;

	if (!arg1 || !arg) return;
	error = (int *)arg;
	if (*error) return; /* We got already an error. Switch to the end. */

	switch(callback_type) {
		case ZEND_INI_PARSER_ENTRY:
			fpm_conf_ini_parser_entry(arg1, arg2, error TSRMLS_CC);
			break;;
		case ZEND_INI_PARSER_SECTION:
			fpm_conf_ini_parser_section(arg1, error TSRMLS_CC);
			break;;
		case ZEND_INI_PARSER_POP_ENTRY:
			fpm_conf_ini_parser_array(arg1, arg3, arg2, error TSRMLS_CC);
			break;;
		default:
			zlog(ZLOG_ERROR, "[%s:%d] Unknown INI syntax", ini_filename, ini_lineno);
			*error = 1;
			break;;
	}
}
/* }}} */

int fpm_conf_load_ini_file(char *filename TSRMLS_DC) /* {{{ */
{
	int error = 0;
	char buf[1024+1];
	int fd, n;
	int nb_read = 1;
	char c = '*';

	int ret = 1;

	if (!filename || !filename[0]) {
		zlog(ZLOG_ERROR, "Configuration file is empty");
		return -1;
	}

	fd = open(filename, O_RDONLY, 0);
	if (fd < 0) {
		zlog(ZLOG_ERROR, "Unable to open file '%s', errno=%d", filename, errno);
		return -1;
	}

	if (ini_recursion++ > 4) {
		zlog(ZLOG_ERROR, "You can include more than 5 files recusively");
		return -1;
	}

	ini_lineno = 0;
	while (nb_read > 0) {
		int tmp;
		memset(buf, 0, sizeof(char) * (1024 + 1));
		for (n=0; n<1024 && (nb_read = read(fd, &c, sizeof(char))) == sizeof(char) && c != '\n'; n++) {
			buf[n] = c;
		}
		buf[n++] = '\n';
		ini_lineno++;
		ini_filename = filename;
		tmp = zend_parse_ini_string(buf, 1, ZEND_INI_SCANNER_NORMAL, (zend_ini_parser_cb_t)fpm_conf_ini_parser, &error TSRMLS_CC);
		ini_filename = filename;
		if (error || tmp == FAILURE) {
			if (ini_include) free(ini_include);
			ini_recursion--;
			close(fd);
			return -1;
		}
		if (ini_include) {
			char *tmp = ini_include;
			ini_include = NULL;
			fpm_evaluate_full_path(&tmp, NULL, NULL, 0);
			fpm_conf_ini_parser_include(tmp, &error TSRMLS_CC);
			if (error) {
				free(tmp);
				ini_recursion--;
				close(fd);
				return -1;
			}
			free(tmp);
		}
	}

	ini_recursion--;
	close(fd);
	return ret;

}
/* }}} */

static void fpm_conf_dump() /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	zlog(ZLOG_NOTICE, "[General]");
	zlog(ZLOG_NOTICE, "\tpid = %s", STR2STR(fpm_global_config.pid_file));
	zlog(ZLOG_NOTICE, "\tdaemonize = %s", BOOL2STR(fpm_global_config.daemonize));
	zlog(ZLOG_NOTICE, "\terror_log = %s", STR2STR(fpm_global_config.error_log));
	zlog(ZLOG_NOTICE, "\tlog_level = %s", zlog_get_level_name());
	zlog(ZLOG_NOTICE, "\tprocess_control_timeout = %ds", fpm_global_config.process_control_timeout);
	zlog(ZLOG_NOTICE, "\temergency_restart_interval = %ds", fpm_global_config.emergency_restart_interval);
	zlog(ZLOG_NOTICE, "\temergency_restart_threshold = %d", fpm_global_config.emergency_restart_threshold);
	zlog(ZLOG_NOTICE, "\trlimit_files = %d", fpm_global_config.rlimit_files);
	zlog(ZLOG_NOTICE, "\trlimit_core = %d", fpm_global_config.rlimit_core);
	zlog(ZLOG_NOTICE, " ");

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		struct key_value_s *kv;
		if (!wp->config) continue;
		zlog(ZLOG_NOTICE, "[%s]", STR2STR(wp->config->name));
		zlog(ZLOG_NOTICE, "\tprefix = %s", STR2STR(wp->config->prefix));
		zlog(ZLOG_NOTICE, "\tuser = %s", STR2STR(wp->config->user));
		zlog(ZLOG_NOTICE, "\tgroup = %s", STR2STR(wp->config->group));
		zlog(ZLOG_NOTICE, "\tchroot = %s", STR2STR(wp->config->chroot));
		zlog(ZLOG_NOTICE, "\tchdir = %s", STR2STR(wp->config->chdir));
		zlog(ZLOG_NOTICE, "\tlisten = %s", STR2STR(wp->config->listen_address));
		zlog(ZLOG_NOTICE, "\tlisten.backlog = %d", wp->config->listen_backlog);
		zlog(ZLOG_NOTICE, "\tlisten.owner = %s", STR2STR(wp->config->listen_owner));
		zlog(ZLOG_NOTICE, "\tlisten.group = %s", STR2STR(wp->config->listen_group));
		zlog(ZLOG_NOTICE, "\tlisten.mode = %s", STR2STR(wp->config->listen_mode));
		zlog(ZLOG_NOTICE, "\tlisten.allowed_clients = %s", STR2STR(wp->config->listen_allowed_clients));
		zlog(ZLOG_NOTICE, "\tpm = %s", PM2STR(wp->config->pm));
		zlog(ZLOG_NOTICE, "\tpm.max_children = %d", wp->config->pm_max_children);
		zlog(ZLOG_NOTICE, "\tpm.max_requests = %d", wp->config->pm_max_requests);
		zlog(ZLOG_NOTICE, "\tpm.start_servers = %d", wp->config->pm_start_servers);
		zlog(ZLOG_NOTICE, "\tpm.min_spare_servers = %d", wp->config->pm_min_spare_servers);
		zlog(ZLOG_NOTICE, "\tpm.max_spare_servers = %d", wp->config->pm_max_spare_servers);
		zlog(ZLOG_NOTICE, "\tpm.status_path = %s", STR2STR(wp->config->pm_status_path));
		zlog(ZLOG_NOTICE, "\tping.path = %s", STR2STR(wp->config->ping_path));
		zlog(ZLOG_NOTICE, "\tping.response = %s", STR2STR(wp->config->ping_response));
		zlog(ZLOG_NOTICE, "\taccess.log = %s", STR2STR(wp->config->access_log));
		zlog(ZLOG_NOTICE, "\taccess.format = %s", STR2STR(wp->config->access_format));
		zlog(ZLOG_NOTICE, "\tcatch_workers_output = %s", BOOL2STR(wp->config->catch_workers_output));
		zlog(ZLOG_NOTICE, "\trequest_terminate_timeout = %ds", wp->config->request_terminate_timeout);
		zlog(ZLOG_NOTICE, "\trequest_slowlog_timeout = %ds", wp->config->request_slowlog_timeout);
		zlog(ZLOG_NOTICE, "\tslowlog = %s", STR2STR(wp->config->slowlog));
		zlog(ZLOG_NOTICE, "\trlimit_files = %d", wp->config->rlimit_files);
		zlog(ZLOG_NOTICE, "\trlimit_core = %d", wp->config->rlimit_core);

		for (kv = wp->config->env; kv; kv = kv->next) {
			zlog(ZLOG_NOTICE, "\tenv[%s] = %s", kv->key, kv->value);
		}

		for (kv = wp->config->php_values; kv; kv = kv->next) {
			zlog(ZLOG_NOTICE, "\tphp_value[%s] = %s", kv->key, kv->value);
		}

		for (kv = wp->config->php_admin_values; kv; kv = kv->next) {
			zlog(ZLOG_NOTICE, "\tphp_admin_value[%s] = %s", kv->key, kv->value);
		}
		zlog(ZLOG_NOTICE, " ");
	}
}
/* }}} */

int fpm_conf_init_main(int test_conf) /* {{{ */
{
	int ret;
	TSRMLS_FETCH();

	if (fpm_globals.prefix && *fpm_globals.prefix) {
		if (!fpm_conf_is_dir(fpm_globals.prefix)) {
			zlog(ZLOG_ERROR, "the global prefix '%s' does not exist or is not a directory", fpm_globals.prefix);
			return -1;
		}
	}

	if (fpm_globals.config == NULL) {
		char *tmp;

		if (fpm_globals.prefix == NULL) {
			spprintf(&tmp, 0, "%s/php-fpm.conf", PHP_SYSCONFDIR);
		} else {
			spprintf(&tmp, 0, "%s/etc/php-fpm.conf", fpm_globals.prefix);
		}

		if (!tmp) {
			zlog(ZLOG_SYSERROR, "spprintf() failed (tmp for fpm_globals.config)");
			return -1;
		}

		fpm_globals.config = strdup(tmp);
		efree(tmp);

		if (!fpm_globals.config) {
			zlog(ZLOG_SYSERROR, "spprintf() failed (fpm_globals.config)");
			return -1;
		}
	}

	ret = fpm_conf_load_ini_file(fpm_globals.config TSRMLS_CC);

	if (0 > ret) {
		zlog(ZLOG_ERROR, "failed to load configuration file '%s'", fpm_globals.config);
		return -1;
	}

	if (0 > fpm_conf_post_process(TSRMLS_C)) {
		zlog(ZLOG_ERROR, "failed to post process the configuration");
		return -1;
	}

	if (test_conf) {
		if (test_conf > 1) {
			fpm_conf_dump();
		}
		zlog(ZLOG_NOTICE, "configuration file %s test is successful\n", fpm_globals.config);
		fpm_globals.test_successful = 1;
		return -1;
	}

	if (0 > fpm_cleanup_add(FPM_CLEANUP_ALL, fpm_conf_cleanup, 0)) {
		return -1;
	}

	return 0;
}
/* }}} */
