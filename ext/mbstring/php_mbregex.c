/*
   +----------------------------------------------------------------------+
   | PHP Version 4                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2003 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.0 of the PHP license,       |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_0.txt.                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Tsukada Takuya <tsukada@fminn.nagano.nagano.jp>              |
   +----------------------------------------------------------------------+
 */

/* $Id$ */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"

#if HAVE_MBREGEX

#include "ext/standard/php_smart_str.h"
#include "php_mbregex.h"
#include "mbstring.h"

ZEND_EXTERN_MODULE_GLOBALS(mbstring)

/* {{{ static void php_mb_regex_free_cache() */
static void php_mb_regex_free_cache(php_mb_regex_t **pre) 
{
	php_mb_regex_free(*pre);
}
/* }}} */

/* {{{ _php_mb_regex_globals_ctor */
void _php_mb_regex_globals_ctor(zend_mbstring_globals *pglobals TSRMLS_DC)
{
	MBSTRG(default_mbctype) = REGCODE_EUCJP;
	MBSTRG(current_mbctype) = REGCODE_EUCJP;
	zend_hash_init(&(MBSTRG(ht_rc)), 0, NULL, (void (*)(void *)) php_mb_regex_free_cache, 1);
	MBSTRG(search_str) = (zval*) NULL;
	MBSTRG(search_re) = (php_mb_regex_t*)NULL;
	MBSTRG(search_pos) = 0;
	MBSTRG(search_regs) = (php_mb_reg_region*)NULL;
	MBSTRG(regex_default_options) = RE_OPTION_POSIXLINE;
	MBSTRG(regex_default_syntax) = REG_SYNTAX_RUBY;
}
/* }}} */

/* {{{ _php_mb_regex_globals_dtor */
void _php_mb_regex_globals_dtor(zend_mbstring_globals *pglobals TSRMLS_DC) 
{
	zend_hash_destroy(&MBSTRG(ht_rc));
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION(mb_regex) */
PHP_MINIT_FUNCTION(mb_regex)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION(mb_regex) */
PHP_MSHUTDOWN_FUNCTION(mb_regex)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION(mb_regex) */
PHP_RINIT_FUNCTION(mb_regex)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION(mb_regex) */
PHP_RSHUTDOWN_FUNCTION(mb_regex)
{
	MBSTRG(current_mbctype) = MBSTRG(default_mbctype);

	if (MBSTRG(search_str) != NULL) {
		zval_ptr_dtor(&MBSTRG(search_str));
		MBSTRG(search_str) = (zval *)NULL;
	}
	MBSTRG(search_pos) = 0;

	if (MBSTRG(search_regs) != NULL) {
		php_mb_regex_region_free(MBSTRG(search_regs), 1);
		MBSTRG(search_regs) = (php_mb_reg_region *)NULL;
	}
	zend_hash_clean(&MBSTRG(ht_rc));

	return SUCCESS;
}
/* }}} */

/*
 * encoding name resolver
 */
/* {{{ php_mb_regex_name2mbctype */
php_mb_reg_char_encoding php_mb_regex_name2mbctype(const char *pname)
{
	php_mb_reg_char_encoding mbctype;

	mbctype = REGCODE_UNDEF;
	if (pname != NULL) {
		if (strcasecmp("EUC-JP", pname) == 0
		    || strcasecmp("X-EUC-JP", pname) == 0
		    || strcasecmp("UJIS", pname) == 0
		    || strcasecmp("EUCJP", pname) == 0
		    || strcasecmp("EUC_JP", pname) == 0
		    || strcasecmp("EUCJP-WIN", pname) == 0) {
			mbctype = REGCODE_EUCJP;
		} else if (strcasecmp("UTF-8", pname) == 0
		           || strcasecmp("UTF8", pname) == 0) {
			mbctype = REGCODE_UTF8;
		} else if (strcasecmp("SJIS", pname) == 0
		           || strcasecmp("CP932", pname) == 0
		           || strcasecmp("MS932", pname) == 0
		           || strcasecmp("SHIFT_JIS", pname) == 0
		           || strcasecmp("SJIS-WIN", pname) == 0) {
			mbctype = REGCODE_SJIS;
		} else if (strcasecmp("ASCII", pname) == 0) {
			mbctype = REGCODE_ASCII;
		}
	}

	return mbctype;
}
/* }}} */

/* {{{ php_mbregex_mbctype2name */
const char *php_mb_regex_mbctype2name(php_mb_reg_char_encoding mbctype)
{
	const char *p = NULL;

	if (mbctype == REGCODE_EUCJP) {
		p = "EUC-JP";
	} else if(mbctype == REGCODE_UTF8) {
		p = "UTF-8";
	} else if(mbctype == REGCODE_SJIS) {
		p = "SJIS";
	} else if(mbctype == REGCODE_ASCII) {
		p = "ascii";
	}
	return p;
}
/* }}} */

/*
 * regex cache
 */
/* {{{ php_mbregex_compile_pattern */
static php_mb_regex_t *php_mbregex_compile_pattern(const char *pattern, int patlen, php_mb_reg_option_type options, php_mb_reg_char_encoding enc, php_mb_reg_syntax_type *syntax TSRMLS_DC)
{
	int err_code = 0;
	int found = 0;
	php_mb_regex_t *retval = NULL, **rc = NULL;
	php_mb_reg_error_info err_info;
	UChar err_str[REG_MAX_ERROR_MESSAGE_LEN];

	found = zend_hash_find(&MBSTRG(ht_rc), (char *)pattern, patlen+1, (void **) &rc);
	if (found == FAILURE || (*rc)->options != options || (*rc)->enc != enc || (*rc)->syntax != syntax) {
		if ((err_code = php_mb_regex_new(&retval, (UChar *)pattern, (UChar *)(pattern + patlen), options, enc, syntax, &err_info)) != REG_NORMAL) {
			php_mb_regex_error_code_to_str(err_str, err_code, err_info);
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "mbregex compile err: %s", err_str);
			retval = NULL;
			goto out;
		}
		zend_hash_update(&MBSTRG(ht_rc), (char *) pattern, patlen + 1, (void *) &retval, sizeof(retval), NULL);
	} else if (found == SUCCESS) {
		retval = *rc;
	}
out:
	return retval; 
}
/* }}} */

/* {{{ _php_mb_regex_get_option_string */
static size_t _php_mb_regex_get_option_string(char *str, size_t len, php_mb_reg_option_type option, php_mb_reg_syntax_type *syntax)
{
	size_t len_left = len;
	size_t len_req = 0;
	char *p = str;
	char c;

	if ((option & RE_OPTION_IGNORECASE) != 0) {
		if (len_left > 0) {
			--len_left;
			*(p++) = 'i';
		}
		++len_req;	
	}

	if ((option & RE_OPTION_EXTENDED) != 0) {
		if (len_left > 0) {
			--len_left;
			*(p++) = 'x';
		}
		++len_req;	
	}

	if ((option & RE_OPTION_POSIXLINE) == RE_OPTION_POSIXLINE) {
		if (len_left > 0) {
			--len_left;
			*(p++) = 'p';
		}
		++len_req;	
	} else {
		if ((option & RE_OPTION_MULTILINE) != 0) {
			if (len_left > 0) {
				--len_left;
				*(p++) = 'm';
			}
			++len_req;	
		}

		if ((option & RE_OPTION_SINGLELINE) != 0) {
			if (len_left > 0) {
				--len_left;
				*(p++) = 's';
			}
			++len_req;	
		}
	}	
	if ((option & RE_OPTION_LONGEST) != 0) {
		if (len_left > 0) {
			--len_left;
			*(p++) = 'l';
		}
		++len_req;	
	}
	if ((option & REG_OPTION_FIND_NOT_EMPTY) != 0) {
		if (len_left > 0) {
			--len_left;
			*(p++) = 'n';
		}
		++len_req;	
	}

	c = 0;

	if (syntax == REG_SYNTAX_JAVA) {
		c = 'j';
	} else if (syntax == REG_SYNTAX_GNU_REGEX) {
		c = 'u';
	} else if (syntax == REG_SYNTAX_GREP) {
		c = 'g';
	} else if (syntax == REG_SYNTAX_EMACS) {
		c = 'c';
	} else if (syntax == REG_SYNTAX_RUBY) {
		c = 'r';
	} else if (syntax == REG_SYNTAX_PERL) {
		c = 'z';
	} else if (syntax == REG_SYNTAX_POSIX_BASIC) {
		c = 'b';
	} else if (syntax == REG_SYNTAX_POSIX_EXTENDED) {
		c = 'd';
	}

	if (c != 0) {
		if (len_left > 0) {
			--len_left;
			*(p++) = c;
		}
		++len_req;
	}


	if (len_left > 0) {
		--len_left;
		*(p++) = '\0';
	}
	++len_req;	
	if (len < len_req) {
		return len_req;
	}

	return 0;
}
/* }}} */

/* {{{ _php_mb_regex_init_options */
static void
_php_mb_regex_init_options(const char *parg, int narg, php_mb_reg_option_type *option, php_mb_reg_syntax_type **syntax, int *eval) 
{
	int n;
	char c;
	int optm = 0; 

	*syntax = REG_SYNTAX_RUBY;

	if (parg != NULL) {
		n = 0;
		while(n < narg) {
			c = parg[n++];
			switch (c) {
				case 'i':
					optm |= RE_OPTION_IGNORECASE;
					break;
				case 'x':
					optm |= RE_OPTION_EXTENDED;
					break;
				case 'm':
					optm |= RE_OPTION_MULTILINE;
					break;
				case 's':
					optm |= RE_OPTION_SINGLELINE;
					break;
				case 'p':
					optm |= RE_OPTION_POSIXLINE;
					break;
				case 'l':
					optm |= RE_OPTION_LONGEST;
					break;
				case 'n':
					optm |= REG_OPTION_FIND_NOT_EMPTY;
					break;
				case 'j':
					*syntax = REG_SYNTAX_JAVA;
					break;
				case 'u':
					*syntax = REG_SYNTAX_GNU_REGEX;
					break;
				case 'g':
					*syntax = REG_SYNTAX_GREP;
					break;
				case 'c':
					*syntax = REG_SYNTAX_EMACS;
					break;
				case 'r':
					*syntax = REG_SYNTAX_RUBY;
					break;
				case 'z':
					*syntax = REG_SYNTAX_PERL;
					break;
				case 'b':
					*syntax = REG_SYNTAX_POSIX_BASIC;
					break;
				case 'd':
					*syntax = REG_SYNTAX_POSIX_EXTENDED;
					break;
				case 'e':
					if (eval != NULL) *eval = 1; 
					break;
				default:
					break;
			}
		}
		if (option != NULL) *option|=optm; 
	}
}
/* }}} */

/*
 * php funcions
 */

/* {{{ proto string mb_regex_encoding([string encoding])
   Returns the current encoding for regex as a string. */
PHP_FUNCTION(mb_regex_encoding)
{
	zval **arg1;
	php_mb_reg_char_encoding mbctype;

	if (ZEND_NUM_ARGS() == 0) {
		const char *retval = php_mb_regex_mbctype2name(MBSTRG(current_mbctype));
		if ( retval != NULL ) {
			RETVAL_STRING((char *)retval, 1);
		} else {
			RETVAL_FALSE;
		}
	} else if (ZEND_NUM_ARGS() == 1 &&
	           zend_get_parameters_ex(1, &arg1) != FAILURE) {
		convert_to_string_ex(arg1);
		mbctype = php_mb_regex_name2mbctype(Z_STRVAL_PP(arg1));
		if (mbctype < 0) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unknown encoding \"%s\"", Z_STRVAL_PP(arg1));
			RETVAL_FALSE;
		} else {
			MBSTRG(current_mbctype) = mbctype;
			RETVAL_TRUE;
		}
	} else {
		WRONG_PARAM_COUNT;
	}
}
/* }}} */

/* {{{ _php_mb_regex_ereg_exec */
static void _php_mb_regex_ereg_exec(INTERNAL_FUNCTION_PARAMETERS, int icase)
{
	zval tmp;
	zval *arg_pattern, *array;
	char *string;
	int string_len;
	php_mb_regex_t *re;
	php_mb_reg_region *regs = NULL;
	int i, match_len, option, beg, end;
	char *str;

	array = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zs|z", &arg_pattern, &string, &string_len, &array) == FAILURE) {
		RETURN_FALSE;
	}

	option = MBSTRG(regex_default_options);
	if (icase) {
		option |= RE_OPTION_IGNORECASE;
	}

	/* compile the regular expression from the supplied regex */
	if (Z_TYPE_P(arg_pattern) != IS_STRING) {
		/* we convert numbers to integers and treat them as a string */
		tmp = *arg_pattern;
		zval_copy_ctor(&tmp);
		if (Z_TYPE_P(&tmp) == IS_DOUBLE) {
			convert_to_long(&tmp);	/* get rid of decimal places */
		}
		convert_to_string(&tmp);
		arg_pattern = &tmp;
		/* don't bother doing an extended regex with just a number */
	}
	re = php_mbregex_compile_pattern(Z_STRVAL_P(arg_pattern), Z_STRLEN_P(arg_pattern), option, MBSTRG(current_mbctype), MBSTRG(regex_default_syntax) TSRMLS_CC);
	if (re == NULL) {
		RETVAL_FALSE;
		goto out;
	}

	regs = php_mb_regex_region_new();

	/* actually execute the regular expression */
	if (php_mb_regex_search(re, (UChar *)string, (UChar *)(string + string_len), string, (UChar *)(string + string_len), regs, 0) < 0) {
		RETVAL_FALSE;
		goto out;
	}

	match_len = 1;
	str = string;
	if (array != NULL) {
		zval ret_array;
		match_len = regs->end[0] - regs->beg[0];
		array_init(&ret_array);
		for (i = 0; i < regs->num_regs; i++) {
			beg = regs->beg[i];
			end = regs->end[i];
			if (beg >= 0 && beg < end && end <= string_len) {
				add_index_stringl(&ret_array, i, (char *)&str[beg], end - beg, 1);
			} else {
				add_index_bool(&ret_array, i, 0);
			}
		}
		REPLACE_ZVAL_VALUE(&array, &ret_array, 0);
	}

	if (match_len == 0) {
		match_len = 1;
	}
	RETVAL_LONG(match_len);
out:
	if (regs != NULL) {
		php_mb_regex_region_free(regs, 1);
	}
	if (arg_pattern == &tmp) {
		zval_dtor(&tmp);
	}
}
/* }}} */

/* {{{ proto int mb_ereg(string pattern, string string [, array registers])
   Regular expression match for multibyte string */
PHP_FUNCTION(mb_ereg)
{
	_php_mb_regex_ereg_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ proto int mb_eregi(string pattern, string string [, array registers])
   Case-insensitive regular expression match for multibyte string */
PHP_FUNCTION(mb_eregi)
{
	_php_mb_regex_ereg_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ _php_mb_regex_ereg_replace_exec */
static void _php_mb_regex_ereg_replace_exec(INTERNAL_FUNCTION_PARAMETERS, int option)
{
	zval *arg_pattern_zval;

	char *arg_pattern;
	int arg_pattern_len;

	char *replace;
	int replace_len;

	char *string;
	int string_len;

	char *p;
	php_mb_regex_t *re;
	php_mb_reg_syntax_type *syntax;
	php_mb_reg_region *regs = NULL;
	smart_str out_buf = { 0 };
	smart_str eval_buf = { 0 };
	smart_str *pbuf;
	int i, err, eval, n;
	UChar *pos;
	UChar *string_lim;
	char *description = NULL;
	char pat_buf[2];

	const mbfl_encoding *enc;

	{
		const char *current_enc_name;
		current_enc_name = php_mb_regex_mbctype2name(MBSTRG(current_mbctype));
		if (current_enc_name == NULL ||
			(enc = mbfl_name2encoding(current_enc_name)) == NULL) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unknown error");
			RETURN_FALSE;
		}
	}
	eval = 0;
	{
		char *option_str = NULL;
		int option_str_len = 0;

		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zss|s",
									&arg_pattern_zval,
									&replace, &replace_len,
									&string, &string_len,
									&option_str, &option_str_len) == FAILURE) {
			RETURN_FALSE;
		}

		if (option_str != NULL) {
			_php_mb_regex_init_options(option_str, option_str_len, &option, &syntax, &eval);
		} else {
			option |= MBSTRG(regex_default_options);
			syntax = MBSTRG(regex_default_syntax);
		}
	}
	if (Z_TYPE_P(arg_pattern_zval) == IS_STRING) {
		arg_pattern = Z_STRVAL_P(arg_pattern_zval);
		arg_pattern_len = Z_STRLEN_P(arg_pattern_zval);
	} else {
		/* FIXME: this code is not multibyte aware! */
		convert_to_long_ex(&arg_pattern_zval);
		pat_buf[0] = (char)Z_LVAL_P(arg_pattern_zval);	
		pat_buf[1] = '\0';

		arg_pattern = pat_buf;
		arg_pattern_len = 1;	
	}
	/* create regex pattern buffer */
	re = php_mbregex_compile_pattern(arg_pattern, arg_pattern_len, option, MBSTRG(current_mbctype), syntax TSRMLS_CC);
	if (re == NULL) {
		RETURN_FALSE;
	}

	if (eval) {
		pbuf = &eval_buf;
		description = zend_make_compiled_string_description("mbregex replace" TSRMLS_CC);
	} else {
		pbuf = &out_buf;
		description = NULL;
	}

	/* do the actual work */
	err = 0;
	pos = string;
	string_lim = (UChar*)(string + string_len);
	regs = php_mb_regex_region_new();
	while (err >= 0) {
		err = php_mb_regex_search(re, (UChar *)string, (UChar *)string_lim, pos, (UChar *)string_lim, regs, 0);
		if (err <= -2) {
			UChar err_str[REG_MAX_ERROR_MESSAGE_LEN];
			php_mb_regex_error_code_to_str(err_str, err);
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "mbregex search failure in php_mbereg_replace_exec(): %s", err_str);
			break;
		}
		if (err >= 0) {
#if moriyoshi_0
			if (regs->beg[0] == regs->end[0]) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Empty regular expression");
				break;
			}
#endif
			/* copy the part of the string before the match */
			smart_str_appendl(&out_buf, pos, (size_t)((UChar *)(string + regs->beg[0]) - pos));
			/* copy replacement and backrefs */
			i = 0;
			p = replace;
			while (i < replace_len) {
				int fwd = (int) php_mb_mbchar_bytes_ex(p, enc);
				n = -1;
				if ((replace_len - i) >= 2 && fwd == 1 &&
					p[0] == '\\' && p[1] >= '0' && p[1] <= '9') {
					n = p[1] - '0';
				}
				if (n >= 0 && n < regs->num_regs) {
					if (regs->beg[n] >= 0 && regs->beg[n] < regs->end[n] && regs->end[n] <= string_len) {
						smart_str_appendl(pbuf, string + regs->beg[n], regs->end[n] - regs->beg[n]);
					}
					p += 2;
					i += 2;
				} else {
					smart_str_appendl(pbuf, p, fwd);
					p += fwd;
					i += fwd;
				}
			}
			if (eval) {
				zval v;
				/* null terminate buffer */
				smart_str_appendc(&eval_buf, '\0');
				/* do eval */
				zend_eval_string(eval_buf.c, &v, description TSRMLS_CC);
				/* result of eval */
				convert_to_string(&v);
				smart_str_appendl(&out_buf, Z_STRVAL(v), Z_STRLEN(v));
				/* Clean up */
				eval_buf.len = 0;
				zval_dtor(&v);
			}
			n = regs->end[0];
			if ((size_t)(pos - (UChar *)string) < n) {
				pos = string + n;
			} else {
				if (pos < string_lim) {
					smart_str_appendl(&out_buf, pos, 1); 
				}
				pos++;
			}
		} else { /* nomatch */
			/* stick that last bit of string on our output */
			if (string_lim - pos > 0) {
				smart_str_appendl(&out_buf, pos, string_lim - pos);
			}
		}
		php_mb_regex_region_free(regs, 0);
	}

	if (description) {
		efree(description);
	}
	if (regs != NULL) {
		php_mb_regex_region_free(regs, 1);
	}
	smart_str_free(&eval_buf);

	if (err <= -2) {
		smart_str_free(&out_buf);	
		RETVAL_FALSE;
	} else {
		smart_str_appendc(&out_buf, '\0');
		RETVAL_STRINGL((char *)out_buf.c, out_buf.len - 1, 0);
	}
}
/* }}} */

/* {{{ proto string mb_ereg_replace(string pattern, string replacement, string string [, string option])
   Replace regular expression for multibyte string */
PHP_FUNCTION(mb_ereg_replace)
{
	_php_mb_regex_ereg_replace_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ proto string mb_eregi_replace(string pattern, string replacement, string string)
   Case insensitive replace regular expression for multibyte string */
PHP_FUNCTION(mb_eregi_replace)
{
	_php_mb_regex_ereg_replace_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU, RE_OPTION_IGNORECASE);
}
/* }}} */

/* {{{ proto array mb_split(string pattern, string string [, int limit])
   split multibyte string into array by regular expression */
PHP_FUNCTION(mb_split)
{
	char *arg_pattern;
	int arg_pattern_len;
	php_mb_regex_t *re;
	php_mb_reg_region *regs = NULL;
	char *string;
	UChar *pos;
	int string_len;

	int n, err;
	long count = -1;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|l", &arg_pattern, &arg_pattern_len, &string, &string_len, &count) == FAILURE) {
		RETURN_FALSE;
	} 

	if (count == 0) {
		count = 1;
	}

	/* create regex pattern buffer */
	if ((re = php_mbregex_compile_pattern(arg_pattern, arg_pattern_len, MBSTRG(regex_default_options), MBSTRG(current_mbctype), MBSTRG(regex_default_syntax) TSRMLS_CC)) == NULL) {
		RETURN_FALSE;
	}

	array_init(return_value);

	pos = (UChar *)string;
	err = 0;
	regs = php_mb_regex_region_new();
	/* churn through str, generating array entries as we go */
	while ((--count != 0) &&
		   (err = php_mb_regex_search(re, (UChar *)string, (UChar *)(string + string_len), pos, (UChar *)(string + string_len), regs, 0)) >= 0) {
		if (regs->beg[0] == regs->end[0]) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Empty regular expression");
			break;
		}

		/* add it to the array */
		if (regs->beg[0] < string_len && regs->beg[0] >= (size_t)(pos - (UChar *)string)) {
			add_next_index_stringl(return_value, pos, ((UChar *)(string + regs->beg[0]) - pos), 1);
		} else {
			err = -2;
			break;
		}
		/* point at our new starting point */
		n = regs->end[0];
		if ((pos - (UChar *)string) < n) {
			pos = (UChar *)string + n;
		}
		if (count < 0) {
			count = 0;
		}
		php_mb_regex_region_free(regs, 0);
	}

	php_mb_regex_region_free(regs, 1);

	/* see if we encountered an error */
	if (err <= -2) {
		UChar err_str[REG_MAX_ERROR_MESSAGE_LEN];
		php_mb_regex_error_code_to_str(err_str, err);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "mbregex search failure in mbsplit(): %s", err_str);
		zval_dtor(return_value);
		RETURN_FALSE;
	}

	/* otherwise we just have one last element to add to the array */
	n = ((UChar *)(string + string_len) - pos);
	if (n > 0) {
		add_next_index_stringl(return_value, pos, n, 1);
	} else {
		add_next_index_stringl(return_value, empty_string, 0, 1);
	}
}
/* }}} */

/* {{{ proto bool mb_ereg_match(string pattern, string string [,string option])
   Regular expression match for multibyte string */
PHP_FUNCTION(mb_ereg_match)
{
	char *arg_pattern;
	int arg_pattern_len;

	char *string;
	int string_len;

	php_mb_regex_t *re;
	php_mb_reg_syntax_type *syntax;
	int option = 0, err;

	{
		char *option_str = NULL;
		int option_str_len = 0;

		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|s",
		                          &arg_pattern, &arg_pattern_len, &string, &string_len,
		                          &option_str, &option_str_len)==FAILURE) {
			RETURN_FALSE;
		}

		if (option_str != NULL) {
			_php_mb_regex_init_options(option_str, option_str_len, &option, &syntax, NULL);
		} else {
			option |= MBSTRG(regex_default_options);
			syntax = MBSTRG(regex_default_syntax);
		}
	}

	if ((re = php_mbregex_compile_pattern(arg_pattern, arg_pattern_len, option, MBSTRG(current_mbctype), syntax TSRMLS_CC)) == NULL) {
		RETURN_FALSE;
	}

	/* match */
	err = php_mb_regex_match(re, (UChar *)string, (UChar *)(string + string_len), (UChar *)string, NULL, 0);
	if (err >= 0) {
		RETVAL_TRUE;
	} else {
		RETVAL_FALSE;
	}
}
/* }}} */

/* regex search */
/* {{{ _php_mb_regex_ereg_search_exec */
static void
_php_mb_regex_ereg_search_exec(INTERNAL_FUNCTION_PARAMETERS, int mode)
{
	zval **arg_pattern, **arg_options;
	int n, i, err, pos, len, beg, end, option;
	UChar *str;
	php_mb_reg_syntax_type *syntax;

	option = MBSTRG(regex_default_options);
	switch (ZEND_NUM_ARGS()) {
	case 0:
		break;
	case 1:
		if (zend_get_parameters_ex(1, &arg_pattern) == FAILURE) {
			WRONG_PARAM_COUNT;
		}
		break;
	case 2:
		if (zend_get_parameters_ex(2, &arg_pattern, &arg_options) == FAILURE) {
			WRONG_PARAM_COUNT;
		}
		convert_to_string_ex(arg_options);
		option = 0;
		_php_mb_regex_init_options(Z_STRVAL_PP(arg_options), Z_STRLEN_PP(arg_options), &option, &syntax, NULL);
		break;
	default:
		WRONG_PARAM_COUNT;
		break;
	}
	if (ZEND_NUM_ARGS() > 0) {
		/* create regex pattern buffer */
		convert_to_string_ex(arg_pattern);

		if ((MBSTRG(search_re) = php_mbregex_compile_pattern(Z_STRVAL_PP(arg_pattern), Z_STRLEN_PP(arg_pattern), option, MBSTRG(current_mbctype), MBSTRG(regex_default_syntax) TSRMLS_CC)) == NULL) {
			RETURN_FALSE;
		}
	}

	pos = MBSTRG(search_pos);
	str = NULL;
	len = 0;
	if (MBSTRG(search_str) != NULL && Z_TYPE_P(MBSTRG(search_str)) == IS_STRING){
		str = (UChar *)Z_STRVAL_P(MBSTRG(search_str));
		len = Z_STRLEN_P(MBSTRG(search_str));
	}

	if (MBSTRG(search_re) == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "No regex given");
		RETURN_FALSE;
	}

	if (str == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "No string given");
		RETURN_FALSE;
	}

	if (MBSTRG(search_regs)) {
		php_mb_regex_region_free(MBSTRG(search_regs), 1);
	}
	MBSTRG(search_regs) = php_mb_regex_region_new();

	err = php_mb_regex_search(MBSTRG(search_re), str, str + len, str + pos, str  + len, MBSTRG(search_regs), 0);
	if (err == REG_MISMATCH) {
		MBSTRG(search_pos) = len;
		RETVAL_FALSE;
	} else if (err <= -2) {
		UChar err_str[REG_MAX_ERROR_MESSAGE_LEN];
		php_mb_regex_error_code_to_str(err_str, err);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "mbregex search failure in mbregex_search(): %s", err_str);
		RETVAL_FALSE;
	} else {
		if (MBSTRG(search_regs)->beg[0] == MBSTRG(search_regs)->end[0]) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Empty regular expression");
		}
		switch (mode) {
		case 1:
			array_init(return_value);
			beg = MBSTRG(search_regs)->beg[0];
			end = MBSTRG(search_regs)->end[0];
			add_next_index_long(return_value, beg);
			add_next_index_long(return_value, end - beg);
			break;
		case 2:
			array_init(return_value);
			n = MBSTRG(search_regs)->num_regs;
			for (i = 0; i < n; i++) {
				beg = MBSTRG(search_regs)->beg[i];
				end = MBSTRG(search_regs)->end[i];
				if (beg >= 0 && beg <= end && end <= len) {
					add_index_stringl(return_value, i, (char *)&str[beg], end - beg, 1);
				} else {
					add_index_bool(return_value, i, 0);
				}
			}
			break;
		default:
			RETVAL_TRUE;
			break;
		}
		end = MBSTRG(search_regs)->end[0];
		if (pos < end) {
			MBSTRG(search_pos) = end;
		} else {
			MBSTRG(search_pos) = pos + 1;
		}
	}

	if (err < 0) {
		php_mb_regex_region_free(MBSTRG(search_regs), 1);
		MBSTRG(search_regs) = (php_mb_reg_region *)NULL;
	}
}
/* }}} */

/* {{{ proto bool mb_ereg_search([string pattern[, string option]])
   Regular expression search for multibyte string */
PHP_FUNCTION(mb_ereg_search)
{
	_php_mb_regex_ereg_search_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ proto array mb_ereg_search_pos([string pattern[, string option]])
   Regular expression search for multibyte string */
PHP_FUNCTION(mb_ereg_search_pos)
{
	_php_mb_regex_ereg_search_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ proto array mb_ereg_search_regs([string pattern[, string option]])
   Regular expression search for multibyte string */
PHP_FUNCTION(mb_ereg_search_regs)
{
	_php_mb_regex_ereg_search_exec(INTERNAL_FUNCTION_PARAM_PASSTHRU, 2);
}
/* }}} */

/* {{{ proto bool mb_ereg_search_init(string string [, string pattern[, string option]])
   Initialize string and regular expression for search. */
PHP_FUNCTION(mb_ereg_search_init)
{
	zval **arg_str, **arg_pattern, **arg_options;
	php_mb_reg_syntax_type *syntax = NULL;
	int option;

	option = MBSTRG(regex_default_options);
	syntax = MBSTRG(regex_default_syntax);
	switch (ZEND_NUM_ARGS()) {
	case 1:
		if (zend_get_parameters_ex(1, &arg_str) == FAILURE) {
			WRONG_PARAM_COUNT;
		}
		break;
	case 2:
		if (zend_get_parameters_ex(2, &arg_str, &arg_pattern) == FAILURE) {
			WRONG_PARAM_COUNT;
		}
		break;
	case 3:
		if (zend_get_parameters_ex(3, &arg_str, &arg_pattern, &arg_options) == FAILURE) {
			WRONG_PARAM_COUNT;
		}
		convert_to_string_ex(arg_options);
		option = 0;
		_php_mb_regex_init_options(Z_STRVAL_PP(arg_options), Z_STRLEN_PP(arg_options), &option, &syntax, NULL);
		break;
	default:
		WRONG_PARAM_COUNT;
		break;
	}
	if (ZEND_NUM_ARGS() > 1) {
		/* create regex pattern buffer */
		convert_to_string_ex(arg_pattern);

		if ((MBSTRG(search_re) = php_mbregex_compile_pattern(Z_STRVAL_PP(arg_pattern), Z_STRLEN_PP(arg_pattern), option, MBSTRG(current_mbctype), syntax TSRMLS_CC)) == NULL) {
			RETURN_FALSE;
		}
	}

	if (MBSTRG(search_str) != NULL) {
		zval_ptr_dtor(&MBSTRG(search_str));
		MBSTRG(search_str) = (zval *)NULL;
	}

	MBSTRG(search_str) = *arg_str;
	ZVAL_ADDREF(MBSTRG(search_str));
	SEPARATE_ZVAL_IF_NOT_REF(&MBSTRG(search_str));

	MBSTRG(search_pos) = 0;

	if (MBSTRG(search_regs) != NULL) {
		php_mb_regex_region_free(MBSTRG(search_regs), 1);
		MBSTRG(search_regs) = (php_mb_reg_region *) NULL;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array mb_ereg_search_getregs(void)
   Get matched substring of the last time */
PHP_FUNCTION(mb_ereg_search_getregs)
{
	int n, i, len, beg, end;
	UChar *str;

	if (MBSTRG(search_regs) != NULL && Z_TYPE_P(MBSTRG(search_str)) == IS_STRING && Z_STRVAL_P(MBSTRG(search_str)) != NULL) {
		array_init(return_value);

		str = (UChar *)Z_STRVAL_P(MBSTRG(search_str));
		len = Z_STRLEN_P(MBSTRG(search_str));
		n = MBSTRG(search_regs)->num_regs;
		for (i = 0; i < n; i++) {
			beg = MBSTRG(search_regs)->beg[i];
			end = MBSTRG(search_regs)->end[i];
			if (beg >= 0 && beg <= end && end <= len) {
				add_index_stringl(return_value, i, (char *)&str[beg], end - beg, 1);
			} else {
				add_index_bool(return_value, i, 0);
			}
		}
	} else {
		RETVAL_FALSE;
	}
}
/* }}} */

/* {{{ proto int mb_ereg_search_getpos(void)
   Get search start position */
PHP_FUNCTION(mb_ereg_search_getpos)
{
	RETVAL_LONG(MBSTRG(search_pos));
}
/* }}} */

/* {{{ proto bool mb_ereg_search_setpos(int position)
   Set search start position */
PHP_FUNCTION(mb_ereg_search_setpos)
{
	zval **arg_pos;
	int n;

	if (ZEND_NUM_ARGS() != 1 || zend_get_parameters_ex(1, &arg_pos) == FAILURE) {
		WRONG_PARAM_COUNT;
	}
	convert_to_long_ex(arg_pos);
	n = Z_LVAL_PP(arg_pos);
	if (n < 0 || (MBSTRG(search_str) != NULL && Z_TYPE_P(MBSTRG(search_str)) == IS_STRING && n >= Z_STRLEN_P(MBSTRG(search_str)))) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Position is out of range");
		MBSTRG(search_pos) = 0;
		RETVAL_FALSE;
	} else {
		MBSTRG(search_pos) = n;
		RETVAL_TRUE;
	}
}
/* }}} */

/* {{{ php_mb_regex_set_options */
void php_mb_regex_set_options(php_mb_reg_option_type options, php_mb_reg_syntax_type *syntax, php_mb_reg_option_type *prev_options, php_mb_reg_syntax_type **prev_syntax TSRMLS_DC) 
{
	if (prev_options != NULL) {
		*prev_options = MBSTRG(regex_default_options);
	}
	if (prev_syntax != NULL) {
		*prev_syntax = MBSTRG(regex_default_syntax);
	}
	MBSTRG(regex_default_options) = options;
	MBSTRG(regex_default_syntax) = syntax;
}
/* }}} */

/* {{{ proto string mb_regex_set_options([string options])
   Set or get the default options for mbregex functions */
PHP_FUNCTION(mb_regex_set_options)
{
	php_mb_reg_option_type opt;
	php_mb_reg_syntax_type *syntax;
	char *string = NULL;
	int string_len;
	char buf[16];

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s",
	                          &string, &string_len) == FAILURE) {
		RETURN_FALSE;
	}
	if (string != NULL) {
		opt = 0;
		syntax = NULL;
		_php_mb_regex_init_options(string, string_len, &opt, &syntax, NULL);
		php_mb_regex_set_options(opt, syntax, NULL, NULL TSRMLS_CC);
	} else {
		opt = MBSTRG(regex_default_options);
		syntax = MBSTRG(regex_default_syntax);
	}
	_php_mb_regex_get_option_string(buf, sizeof(buf), opt, syntax);

	RETVAL_STRING(buf, 1);
}
/* }}} */

#endif	/* HAVE_MBREGEX */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: fdm=marker
 * vim: noet sw=4 ts=4
 */
