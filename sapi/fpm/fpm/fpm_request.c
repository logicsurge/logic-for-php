
	/* $Id: fpm_request.c,v 1.9.2.1 2008/11/15 00:57:24 anight Exp $ */
	/* (c) 2007,2008 Andrei Nigmatulin */
#ifdef HAVE_TIMES
#include <sys/times.h>
#endif

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_php.h"
#include "fpm_str.h"
#include "fpm_clock.h"
#include "fpm_conf.h"
#include "fpm_trace.h"
#include "fpm_php_trace.h"
#include "fpm_process_ctl.h"
#include "fpm_children.h"
#include "fpm_shm_slots.h"
#include "fpm_status.h"
#include "fpm_request.h"
#include "fpm_log.h"

#include "zlog.h"

void fpm_request_accepting() /* {{{ */
{
	struct fpm_shm_slot_s *slot;

	slot = fpm_shm_slots_acquire(0, 0);
	slot->request_stage = FPM_REQUEST_ACCEPTING;
	fpm_clock_get(&slot->tv);
	memset(slot->request_uri, 0, sizeof(slot->request_uri));
	memset(slot->request_method, 0, sizeof(slot->request_method));
	slot->content_length = 0;
	memset(slot->script_filename, 0, sizeof(slot->script_filename));
	fpm_shm_slots_release(slot);
}
/* }}} */

void fpm_request_reading_headers() /* {{{ */
{
	struct fpm_shm_slot_s *slot;

	slot = fpm_shm_slots_acquire(0, 0);
	slot->request_stage = FPM_REQUEST_READING_HEADERS;
	fpm_clock_get(&slot->tv);
	slot->accepted = slot->tv;
	slot->accepted_epoch = time(NULL);
#ifdef HAVE_TIMES
	times(&slot->cpu_accepted);
#endif
	fpm_shm_slots_release(slot);

	fpm_status_increment_accepted_conn(fpm_status_shm);
}
/* }}} */

void fpm_request_info() /* {{{ */
{
	TSRMLS_FETCH();
	struct fpm_shm_slot_s *slot;
	char *request_uri = fpm_php_request_uri(TSRMLS_C);
	char *request_method = fpm_php_request_method(TSRMLS_C);
	char *script_filename = fpm_php_script_filename(TSRMLS_C);
	char *query_string = fpm_php_query_string(TSRMLS_C);
	char *auth_user = fpm_php_auth_user(TSRMLS_C);

	slot = fpm_shm_slots_acquire(0, 0);
	slot->request_stage = FPM_REQUEST_INFO;
	fpm_clock_get(&slot->tv);

	if (request_uri) {
		strlcpy(slot->request_uri, request_uri, sizeof(slot->request_uri));
	}

	if (request_method) {
		strlcpy(slot->request_method, request_method, sizeof(slot->request_method));
	}

	if (query_string) {
		strlcpy(slot->query_string, query_string, sizeof(slot->query_string));
	}

	if (auth_user) {
		strlcpy(slot->auth_user, auth_user, sizeof(slot->auth_user));
	}

	slot->content_length = fpm_php_content_length(TSRMLS_C);

	/* if cgi.fix_pathinfo is set to "1" and script cannot be found (404)
		the sapi_globals.request_info.path_translated is set to NULL */
	if (script_filename) {
		strlcpy(slot->script_filename, script_filename, sizeof(slot->script_filename));
	}

	fpm_shm_slots_release(slot);
}
/* }}} */

void fpm_request_executing() /* {{{ */
{
	struct fpm_shm_slot_s *slot;

	slot = fpm_shm_slots_acquire(0, 0);
	slot->request_stage = FPM_REQUEST_EXECUTING;
	fpm_clock_get(&slot->tv);
	fpm_shm_slots_release(slot);
}
/* }}} */

void fpm_request_end(TSRMLS_D) /* {{{ */
{
	struct fpm_shm_slot_s *slot;

	slot = fpm_shm_slots_acquire(0, 0);
	slot->request_stage = FPM_REQUEST_FINISHED;
	fpm_clock_get(&slot->tv);
#ifdef HAVE_TIMES
	times(&slot->cpu_finished);
	timersub(&slot->tv, &slot->accepted, &slot->cpu_duration);
#endif
	slot->memory = zend_memory_peak_usage(1 TSRMLS_CC);
	fpm_shm_slots_release(slot);
}
/* }}} */

void fpm_request_finished() /* {{{ */
{
	struct fpm_shm_slot_s *slot;

	slot = fpm_shm_slots_acquire(0, 0);
	slot->request_stage = FPM_REQUEST_FINISHED;
	fpm_clock_get(&slot->tv);
	memset(&slot->accepted, 0, sizeof(slot->accepted));
	slot->accepted_epoch = 0;
	fpm_shm_slots_release(slot);
}
/* }}} */

void fpm_request_check_timed_out(struct fpm_child_s *child, struct timeval *now, int terminate_timeout, int slowlog_timeout) /* {{{ */
{
	struct fpm_shm_slot_s *slot;
	struct fpm_shm_slot_s slot_c;

	slot = fpm_shm_slot(child);
	if (!fpm_shm_slots_acquire(slot, 1)) {
		return;
	}

	slot_c = *slot;
	fpm_shm_slots_release(slot);

#if HAVE_FPM_TRACE
	if (child->slow_logged.tv_sec) {
		if (child->slow_logged.tv_sec != slot_c.accepted.tv_sec || child->slow_logged.tv_usec != slot_c.accepted.tv_usec) {
			child->slow_logged.tv_sec = 0;
			child->slow_logged.tv_usec = 0;
		}
	}
#endif

	if (slot_c.request_stage > FPM_REQUEST_ACCEPTING && slot_c.request_stage < FPM_REQUEST_END) {
		char purified_script_filename[sizeof(slot_c.script_filename)];
		struct timeval tv;

		timersub(now, &slot_c.accepted, &tv);

#if HAVE_FPM_TRACE
		if (child->slow_logged.tv_sec == 0 && slowlog_timeout &&
				slot_c.request_stage == FPM_REQUEST_EXECUTING && tv.tv_sec >= slowlog_timeout) {
			
			str_purify_filename(purified_script_filename, slot_c.script_filename, sizeof(slot_c.script_filename));

			child->slow_logged = slot_c.accepted;
			child->tracer = fpm_php_trace;

			fpm_trace_signal(child->pid);

			zlog(ZLOG_WARNING, "[pool %s] child %d, script '%s' (request: \"%s %s\") executing too slow (%d.%06d sec), logging",
				child->wp->config->name, (int) child->pid, purified_script_filename, slot_c.request_method, slot_c.request_uri,
				(int) tv.tv_sec, (int) tv.tv_usec);
		}
		else
#endif
		if (terminate_timeout && tv.tv_sec >= terminate_timeout) {
			str_purify_filename(purified_script_filename, slot_c.script_filename, sizeof(slot_c.script_filename));
			fpm_pctl_kill(child->pid, FPM_PCTL_TERM);

			zlog(ZLOG_WARNING, "[pool %s] child %d, script '%s' (request: \"%s %s\") execution timed out (%d.%06d sec), terminating",
				child->wp->config->name, (int) child->pid, purified_script_filename, slot_c.request_method, slot_c.request_uri,
				(int) tv.tv_sec, (int) tv.tv_usec);
		}
	}
}
/* }}} */

int fpm_request_is_idle(struct fpm_child_s *child) /* {{{ */
{
	struct fpm_shm_slot_s slot;

	/* no need in atomicity here */
	slot = *fpm_shm_slot(child);

	return slot.request_stage == FPM_REQUEST_ACCEPTING;
}
/* }}} */
