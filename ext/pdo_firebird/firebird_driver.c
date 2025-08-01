/*
  +----------------------------------------------------------------------+
  | Copyright (c) The PHP Group                                          |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | https://www.php.net/license/3_01.txt                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Ard Biesheuvel <abies@php.net>                               |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pdo/php_pdo.h"
#include "ext/pdo/php_pdo_driver.h"
#include "php_pdo_firebird.h"
#include "php_pdo_firebird_int.h"
#include "pdo_firebird_utils.h"

static int php_firebird_alloc_prepare_stmt(pdo_dbh_t*, const zend_string*, XSQLDA*, isc_stmt_handle*,
	HashTable*);
static bool php_firebird_rollback_transaction(pdo_dbh_t *dbh);

const char CHR_LETTER = 1;
const char CHR_DIGIT = 2;
const char CHR_IDENT = 4;
const char CHR_QUOTE = 8;
const char CHR_WHITE = 16;
const char CHR_HEX = 32;
const char CHR_INTRODUCER = 64;

static const char classes_array[] = {
	/* 000     */ 0,
	/* 001     */ 0,
	/* 002     */ 0,
	/* 003     */ 0,
	/* 004     */ 0,
	/* 005     */ 0,
	/* 006     */ 0,
	/* 007     */ 0,
	/* 008     */ 0,
	/* 009     */ 16, /* CHR_WHITE */
	/* 010     */ 16, /* CHR_WHITE */
	/* 011     */ 0,
	/* 012     */ 0,
	/* 013     */ 16, /* CHR_WHITE */
	/* 014     */ 0,
	/* 015     */ 0,
	/* 016     */ 0,
	/* 017     */ 0,
	/* 018     */ 0,
	/* 019     */ 0,
	/* 020     */ 0,
	/* 021     */ 0,
	/* 022     */ 0,
	/* 023     */ 0,
	/* 024     */ 0,
	/* 025     */ 0,
	/* 026     */ 0,
	/* 027     */ 0,
	/* 028     */ 0,
	/* 029     */ 0,
	/* 030     */ 0,
	/* 031     */ 0,
	/* 032     */ 16, /* CHR_WHITE */
	/* 033  !  */ 0,
	/* 034  "  */ 8, /* CHR_QUOTE */
	/* 035  #  */ 0,
	/* 036  $  */ 4, /* CHR_IDENT */
	/* 037  %  */ 0,
	/* 038  &  */ 0,
	/* 039  '  */ 8, /* CHR_QUOTE */
	/* 040  (  */ 0,
	/* 041  )  */ 0,
	/* 042  *  */ 0,
	/* 043  +  */ 0,
	/* 044  ,  */ 0,
	/* 045  -  */ 0,
	/* 046  .  */ 0,
	/* 047  /  */ 0,
	/* 048  0  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 049  1  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 050  2  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 051  3  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 052  4  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 053  5  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 054  6  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 055  7  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 056  8  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 057  9  */ 38, /* CHR_DIGIT | CHR_IDENT | CHR_HEX */
	/* 058  :  */ 0,
	/* 059  ;  */ 0,
	/* 060  <  */ 0,
	/* 061  =  */ 0,
	/* 062  >  */ 0,
	/* 063  ?  */ 0,
	/* 064  @  */ 0,
	/* 065  A  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 066  B  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 067  C  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 068  D  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 069  E  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 070  F  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 071  G  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 072  H  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 073  I  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 074  J  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 075  K  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 076  L  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 077  M  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 078  N  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 079  O  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 080  P  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 081  Q  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 082  R  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 083  S  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 084  T  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 085  U  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 086  V  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 087  W  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 088  X  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 089  Y  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 090  Z  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 091  [  */ 0,
	/* 092  \  */ 0,
	/* 093  ]  */ 0,
	/* 094  ^  */ 0,
	/* 095  _  */ 68, /* CHR_IDENT | CHR_INTRODUCER */
	/* 096  `  */ 0,
	/* 097  a  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 098  b  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 099  c  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 100  d  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 101  e  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 102  f  */ 37, /* CHR_LETTER | CHR_IDENT | CHR_HEX */
	/* 103  g  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 104  h  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 105  i  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 106  j  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 107  k  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 108  l  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 109  m  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 110  n  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 111  o  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 112  p  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 113  q  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 114  r  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 115  s  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 116  t  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 117  u  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 118  v  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 119  w  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 120  x  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 121  y  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 122  z  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 123  {  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 124  |  */ 0,
	/* 125  }  */ 5, /* CHR_LETTER | CHR_IDENT */
	/* 126  ~  */ 0,
	/* 127     */ 0
};

static inline char php_firebird_classes(char idx)
{
	unsigned char uidx = (unsigned char) idx;
	if (uidx > 127) return 0;
	return classes_array[uidx];
}

typedef enum {
	ttNone,
	ttWhite,
	ttComment,
	ttBrokenComment,
	ttString,
	ttParamMark,
	ttIdent,
	ttOther
} FbTokenType;

static FbTokenType php_firebird_get_token(const char** begin, const char* end)
{
	FbTokenType ret = ttNone;
	const char* p = *begin;

	char c = *p++;
	switch (c)
	{
	case ':':
	case '?':
		ret = ttParamMark;
		break;

	case '\'':
	case '"':
		while (p < end)
		{
			if (*p++ == c)
			{
				ret = ttString;
				break;
			}
		}
		break;

	case '/':
		if (p < end && *p == '*')
		{
			ret = ttBrokenComment;
			p++;
			while (p < end)
			{
				if (*p++ == '*' && p < end && *p == '/')
				{
					p++;
					ret = ttComment;
					break;
				}
			}
		}
		else {
			ret = ttOther;
		}
		break;

	case '-':
		if (p < end && *p == '-')
		{
			while (++p < end)
			{
				if (*p == '\r')
				{
					p++;
					if (p < end && *p == '\n')
						p++;
					break;
				}
				else if (*p == '\n')
					break;
			}

			ret = ttComment;
		}
		else
			ret = ttOther;
		break;

	default:
		if (php_firebird_classes(c) & CHR_DIGIT)
		{
			while (p < end && (php_firebird_classes(*p) & CHR_DIGIT))
				p++;
			ret = ttOther;
		}
		else if (php_firebird_classes(c) & CHR_IDENT)
		{
			while (p < end && (php_firebird_classes(*p) & CHR_IDENT))
				p++;
			ret = ttIdent;
		}
		else if (php_firebird_classes(c) & CHR_WHITE)
		{
			while (p < end && (php_firebird_classes(*p) & CHR_WHITE))
				p++;
			ret = ttWhite;
		}
		else
		{
			while (p < end && !(php_firebird_classes(*p) & (CHR_DIGIT | CHR_IDENT | CHR_WHITE)) &&
				(*p != '/') && (*p != '-') && (*p != ':') && (*p != '?') &&
				(*p != '\'') && (*p != '"'))
			{
				p++;
			}
			ret = ttOther;
		}
	}

	*begin = p;
	return ret;
}

static int php_firebird_preprocess(const zend_string* sql, char* sql_out, HashTable* named_params)
{
	bool passAsIs = 1, execBlock = 0;
	zend_long pindex = -1;
	char pname[254], ident[253], ident2[253];
	unsigned int l;
	const char* p = ZSTR_VAL(sql), * end = ZSTR_VAL(sql) + ZSTR_LEN(sql);
	const char* start = p;
	FbTokenType tok = php_firebird_get_token(&p, end);

	const char* i = start;
	while (p < end && (tok == ttComment || tok == ttWhite))
	{
		i = p;
		tok = php_firebird_get_token(&p, end);
	}

	if (p >= end || tok != ttIdent)
	{
		/* Execute statement preprocess SQL error */
		/* Statement expected */
		return 0;
	}
	/* skip leading comments ?? */
	start = i;
	l = p - i;
	/* check the length of the identifier */
	/* in Firebird 4.0 it is 63 characters, in previous versions 31 bytes */
	if (l > 252) {
		return 0;
	}
	strncpy(ident, i, l);
	ident[l] = '\0';
	if (!strcasecmp(ident, "EXECUTE"))
	{
		/* For EXECUTE PROCEDURE and EXECUTE BLOCK statements, named parameters must be processed. */
		/* However, in EXECUTE BLOCK this is done in a special way. */
		const char* i2 = p;
		tok = php_firebird_get_token(&p, end);
		while (p < end && (tok == ttComment || tok == ttWhite))
		{
			i2 = p;
			tok = php_firebird_get_token(&p, end);
		}
		if (p >= end || tok != ttIdent)
		{
			/* Execute statement preprocess SQL error */
			/* Statement expected */
			return 0;
		}
		l = p - i2;
		/* check the length of the identifier */
		/* in Firebird 4.0 it is 63 characters, in previous versions 31 bytes */
		if (l > 252) {
			return 0;
		}
		strncpy(ident2, i2, l);
		ident2[l] = '\0';
		execBlock = !strcasecmp(ident2, "BLOCK");
		passAsIs = 0;
	}
	else
	{
		/* Named parameters must be processed in the INSERT, UPDATE, DELETE, MERGE statements. */
		/* If CTEs are present in the query, they begin with the WITH keyword. */
		passAsIs = strcasecmp(ident, "INSERT") && strcasecmp(ident, "UPDATE") &&
			strcasecmp(ident, "DELETE") && strcasecmp(ident, "MERGE") &&
			strcasecmp(ident, "SELECT") && strcasecmp(ident, "WITH");
	}

	if (passAsIs)
	{
		strcpy(sql_out, ZSTR_VAL(sql));
		return 1;
	}

	strncat(sql_out, start, p - start);

	while (p < end)
	{
		start = p;
		tok = php_firebird_get_token(&p, end);
		switch (tok)
		{
		case ttParamMark:
			tok = php_firebird_get_token(&p, end);
			if (tok == ttIdent /*|| tok == ttString*/)
			{
				++pindex;
				l = p - start;
				/* check the length of the identifier */
				/* in Firebird 4.0 it is 63 characters, in previous versions 31 bytes */
				/* + symbol ":" */
				if (l > 253) {
					return 0;
				}
				strncpy(pname, start, l);
				pname[l] = '\0';

				if (named_params) {
					zval tmp;
					ZVAL_LONG(&tmp, pindex);
					zend_hash_str_update(named_params, pname, l, &tmp);
				}

				strcat(sql_out, "?");
			}
			else
			{
				if (strncmp(start, "?", 1)) {
					/* Execute statement preprocess SQL error */
					/* Parameter name expected */
					return 0;
				}
				++pindex;
				strncat(sql_out, start, p - start);
			}
			break;

		case ttIdent:
			if (execBlock)
			{
				/* In the EXECUTE BLOCK statement, processing must be */
				/* carried out up to the keyword AS. */
				l = p - start;
				/* check the length of the identifier */
				/* in Firebird 4.0 it is 63 characters, in previous versions 31 bytes */
				if (l > 252) {
					return 0;
				}
				strncpy(ident, start, l);
				ident[l] = '\0';
				if (!strcasecmp(ident, "AS"))
				{
					strncat(sql_out, start, end - start);
					return 1;
				}
			}
			/* TODO Check this is correct? */
			ZEND_FALLTHROUGH;

		case ttWhite:
		case ttComment:
		case ttString:
		case ttOther:
			strncat(sql_out, start, p - start);
			break;

		case ttBrokenComment:
		{
			/* Execute statement preprocess SQL error */
			/* Unclosed comment found near ''@1'' */
			return 0;
		}
		break;


		case ttNone:
			/* Execute statement preprocess SQL error */
			return 0;
			break;
		}
	}
	return 1;
}

#if FB_API_VER >= 40
/* set coercing a data type */
static void set_coercing_output_data_types(XSQLDA* sqlda)
{
	/* Data types introduced in Firebird 4.0 are difficult to process using the Firebird Legacy API. */
	/* These data types include DECFLOAT(16), DECFLOAT(34), INT128 (NUMERIC/DECIMAL(38, x)). */
	/* In any case, at this data types can only be mapped to strings. */
	/* This function allows you to ensure minimal performance of queries if they contain columns of the above types. */
	unsigned int i;
	short dtype;
	short nullable;
	XSQLVAR* var;
	unsigned fb_client_version = fb_get_client_version();
	unsigned fb_client_major_version = (fb_client_version >> 8) & 0xFF;
	for (i=0, var = sqlda->sqlvar; i < sqlda->sqld; i++, var++) {
		dtype = (var->sqltype & ~1); /* drop flag bit  */
		nullable = (var->sqltype & 1);
		switch(dtype) {
			case SQL_INT128:
				var->sqltype = SQL_VARYING + nullable;
				var->sqllen = 46;
				var->sqlscale = 0;
				break;

			case SQL_DEC16:
				var->sqltype = SQL_VARYING + nullable;
				var->sqllen = 24;
				break;

			case SQL_DEC34:
				var->sqltype = SQL_VARYING + nullable;
				var->sqllen = 43;
				break;

			case SQL_TIMESTAMP_TZ:
			    if (fb_client_major_version < 4) {
					/* If the client version is below 4.0, then it is impossible to handle time zones natively, */
					/* so we convert these types to a string. */
					var->sqltype = SQL_VARYING + nullable;
					var->sqllen = 58;
				}
				break;

			case SQL_TIME_TZ:
				if (fb_client_major_version < 4) {
					/* If the client version is below 4.0, then it is impossible to handle time zones natively, */
					/* so we convert these types to a string. */
					var->sqltype = SQL_VARYING + nullable;
					var->sqllen = 46;
				}
				break;

			default:
				break;
		}
	}
}
#endif

/* map driver specific error message to PDO error */
void php_firebird_set_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *state, const size_t state_len,
	const char *msg, const size_t msg_len) /* {{{ */
{
	pdo_error_type *const error_code = stmt ? &stmt->error_code : &dbh->error_code;
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	pdo_firebird_error_info *einfo = &H->einfo;
	int sqlcode = -999;

	if (einfo->errmsg) {
		pefree(einfo->errmsg, dbh->is_persistent);
		einfo->errmsg = NULL;
		einfo->errmsg_length = 0;
	}

	if (H->isc_status && (H->isc_status[0] == 1 && H->isc_status[1] > 0)) {
		char buf[512];
		size_t buf_size = sizeof(buf), read_len = 0;
		ssize_t tmp_len;
		const ISC_STATUS *s = H->isc_status;
		sqlcode = isc_sqlcode(s);

		while ((buf_size > (read_len + 1)) && (tmp_len = fb_interpret(&buf[read_len], (buf_size - read_len - 1), &s)) && tmp_len > 0) {
			read_len += tmp_len;
			buf[read_len++] = ' ';
		}

		/* remove last space */
		if (read_len) {
			buf[read_len--] = '\0';
		}

		einfo->errmsg_length = read_len;
		einfo->errmsg = pestrndup(buf, read_len, dbh->is_persistent);

		char sqlstate[sizeof(pdo_error_type)];
		fb_sqlstate(sqlstate, H->isc_status);
		if (sqlstate != NULL && strlen(sqlstate) < sizeof(pdo_error_type)) {
			strcpy(*error_code, sqlstate);
			goto end;
		}
	} else if (msg && msg_len) {
		einfo->errmsg_length = msg_len;
		einfo->errmsg = pestrndup(msg, einfo->errmsg_length, dbh->is_persistent);
	}

	if (state && state_len && state_len < sizeof(pdo_error_type)) {
		memcpy(*error_code, state, state_len + 1);
	} else {
		memcpy(*error_code, "HY000", sizeof("HY000"));
	}

end:
	einfo->sqlcode = sqlcode;
	if (!dbh->methods) {
		pdo_throw_exception(0, einfo->errmsg, error_code);
	}
}
/* }}} */

/* called by PDO to close a db handle */
static void firebird_handle_closer(pdo_dbh_t *dbh) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	if (H->tr) {
		if (dbh->auto_commit) {
			php_firebird_commit_transaction(dbh, /* retain */ false);
		} else {
			php_firebird_rollback_transaction(dbh);
		}
	}
	H->in_manually_txn = 0;

	/* isc_detach_database returns 0 on success, 1 on failure. */
	if (H->db && isc_detach_database(H->isc_status, &H->db)) {
		php_firebird_error(dbh);
	}

	if (H->date_format) {
		pefree(H->date_format, dbh->is_persistent);
	}
	if (H->time_format) {
		pefree(H->time_format, dbh->is_persistent);
	}
	if (H->timestamp_format) {
		pefree(H->timestamp_format, dbh->is_persistent);
	}

	if (H->einfo.errmsg) {
		pefree(H->einfo.errmsg, dbh->is_persistent);
		H->einfo.errmsg = NULL;
	}

	pefree(H, dbh->is_persistent);
}
/* }}} */

/* called by PDO to prepare an SQL query */
static bool firebird_handle_preparer(pdo_dbh_t *dbh, zend_string *sql, /* {{{ */
	pdo_stmt_t *stmt, zval *driver_options)
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	pdo_firebird_stmt *S = NULL;
	HashTable *np;

	do {
		isc_stmt_handle s = PDO_FIREBIRD_HANDLE_INITIALIZER;
		XSQLDA num_sqlda;
		static char const info[] = { isc_info_sql_stmt_type };
		char result[8];

		num_sqlda.version = PDO_FB_SQLDA_VERSION;
		num_sqlda.sqln = 1;

		ALLOC_HASHTABLE(np);
		zend_hash_init(np, 8, NULL, NULL, 0);

		/* allocate and prepare statement */
		if (!php_firebird_alloc_prepare_stmt(dbh, sql, &num_sqlda, &s, np)) {
			break;
		}

		/* allocate a statement handle struct of the right size (struct out_sqlda is inlined) */
		S = ecalloc(1, sizeof(*S)-sizeof(XSQLDA) + XSQLDA_LENGTH(num_sqlda.sqld));
		S->H = H;
		S->stmt = s;
		S->out_sqlda.version = PDO_FB_SQLDA_VERSION;
		S->out_sqlda.sqln = stmt->column_count = num_sqlda.sqld;
		S->named_params = np;

		/* determine the statement type */
		if (isc_dsql_sql_info(H->isc_status, &s, sizeof(info), const_cast(info), sizeof(result),
				result)) {
			break;
		}
		S->statement_type = result[3];

		/* fill the output sqlda with information about the prepared query */
		if (isc_dsql_describe(H->isc_status, &s, PDO_FB_SQLDA_VERSION, &S->out_sqlda)) {
			php_firebird_error(dbh);
			break;
		}

#if FB_API_VER >= 40
		/* set coercing a data type */
		set_coercing_output_data_types(&S->out_sqlda);
#endif

		/* allocate the input descriptors */
		if (isc_dsql_describe_bind(H->isc_status, &s, PDO_FB_SQLDA_VERSION, &num_sqlda)) {
			break;
		}

		if (num_sqlda.sqld) {
			S->in_sqlda = ecalloc(1,XSQLDA_LENGTH(num_sqlda.sqld));
			S->in_sqlda->version = PDO_FB_SQLDA_VERSION;
			S->in_sqlda->sqln = num_sqlda.sqld;

			if (isc_dsql_describe_bind(H->isc_status, &s, PDO_FB_SQLDA_VERSION, S->in_sqlda)) {
				break;
			}

			/* make all parameters nullable */
			unsigned int i;
			XSQLVAR* var;
			for (i = 0, var = S->in_sqlda->sqlvar; i < S->in_sqlda->sqld; i++, var++) {
				/* The low bit of sqltype indicates that the parameter can take a NULL value */
				var->sqltype |= 1;
			}
		}

		stmt->driver_data = S;
		stmt->methods = &firebird_stmt_methods;
		stmt->supports_placeholders = PDO_PLACEHOLDER_POSITIONAL;

		return true;

	} while (0);

	php_firebird_error(dbh);

	zend_hash_destroy(np);
	FREE_HASHTABLE(np);

	if (S) {
		if (S->in_sqlda) {
			efree(S->in_sqlda);
		}
		efree(S);
	}

	return false;
}
/* }}} */

/* called by PDO to execute a statement that doesn't produce a result set */
static zend_long firebird_handle_doer(pdo_dbh_t *dbh, const zend_string *sql) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	isc_stmt_handle stmt = PDO_FIREBIRD_HANDLE_INITIALIZER;
	static char const info_count[] = { isc_info_sql_records };
	char result[64];
	int ret = 0;
	XSQLDA in_sqlda, out_sqlda;

	/* TODO no placeholders in exec() for now */
	in_sqlda.version = out_sqlda.version = PDO_FB_SQLDA_VERSION;
	in_sqlda.sqld = out_sqlda.sqld = 0;
	out_sqlda.sqln = 1;

	/* allocate and prepare statement */
	if (!php_firebird_alloc_prepare_stmt(dbh, sql, &out_sqlda, &stmt, 0)) {
		return -1;
	}

	/* execute the statement */
	if (isc_dsql_execute2(H->isc_status, &H->tr, &stmt, PDO_FB_SQLDA_VERSION, &in_sqlda, &out_sqlda)) {
		php_firebird_error(dbh);
		ret = -1;
		goto free_statement;
	}

	/* find out how many rows were affected */
	if (isc_dsql_sql_info(H->isc_status, &stmt, sizeof(info_count), const_cast(info_count),
			sizeof(result),	result)) {
		php_firebird_error(dbh);
		ret = -1;
		goto free_statement;
	}

	if (result[0] == isc_info_sql_records) {
		unsigned i = 3, result_size = isc_vax_integer(&result[1],2);

		if (result_size > sizeof(result)) {
			ret = -1;
			goto free_statement;
		}
		while (result[i] != isc_info_end && i < result_size) {
			short len = (short)isc_vax_integer(&result[i+1],2);
			/* bail out on bad len */
			if (len != 1 && len != 2 && len != 4) {
				ret = -1;
				goto free_statement;
			}
			if (result[i] != isc_info_req_select_count) {
				ret += isc_vax_integer(&result[i+3],len);
			}
			i += len+3;
		}
	}

	if (dbh->auto_commit && !H->in_manually_txn) {
		if (!php_firebird_commit_transaction(dbh, /* retain */ true)) {
			ret = -1;
		}
	}

free_statement:

	if (isc_dsql_free_statement(H->isc_status, &stmt, DSQL_drop)) {
		php_firebird_error(dbh);
	}

	return ret;
}
/* }}} */

/* called by the PDO SQL parser to add quotes to values that are copied into SQL */
static zend_string* firebird_handle_quoter(pdo_dbh_t *dbh, const zend_string *unquoted, enum pdo_param_type paramtype)
{
	size_t qcount = 0;
	char const *co, *l, *r;
	char *c;
	size_t quotedlen;
	zend_string *quoted_str;

	if (ZSTR_LEN(unquoted) == 0) {
		return ZSTR_INIT_LITERAL("''", 0);
	}

	/* Firebird only requires single quotes to be doubled if string lengths are used */
	/* count the number of ' characters */
	for (co = ZSTR_VAL(unquoted); (co = strchr(co,'\'')); qcount++, co++);

	if (UNEXPECTED(ZSTR_LEN(unquoted) + 2 > ZSTR_MAX_LEN - qcount)) {
		return NULL;
	}

	quotedlen = ZSTR_LEN(unquoted) + qcount + 2;
	quoted_str = zend_string_alloc(quotedlen, 0);
	c = ZSTR_VAL(quoted_str);
	*c++ = '\'';

	/* foreach (chunk that ends in a quote) */
	for (l = ZSTR_VAL(unquoted); (r = strchr(l,'\'')); l = r+1) {
		strncpy(c, l, r-l+1);
		c += (r-l+1);
		/* add the second quote */
		*c++ = '\'';
	}

	/* copy the remainder */
	strncpy(c, l, quotedlen-(c-ZSTR_VAL(quoted_str))-1);
	ZSTR_VAL(quoted_str)[quotedlen-1] = '\'';
	ZSTR_VAL(quoted_str)[quotedlen]   = '\0';

	return quoted_str;
}
/* }}} */

/* php_firebird_begin_transaction */
static bool php_firebird_begin_transaction(pdo_dbh_t *dbh, bool is_auto_commit_txn) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	/* isc_xxx are all 1 byte. */
	char tpb[4] = { isc_tpb_version3 };
	size_t tpb_size;

	/* access mode. writable or readonly */
	tpb[1] = H->is_writable_txn ? isc_tpb_write : isc_tpb_read;

	if (is_auto_commit_txn) {
		/*
		 * In autocommit mode, we need to always read the latest information, so we set `read committed`.
		 */
		tpb[2] = isc_tpb_read_committed;
		/* Ignore indeterminate data from other transactions. This option only required with `read committed`. */
		tpb[3] = isc_tpb_rec_version;
		tpb_size = 4;
	} else {
		switch (H->txn_isolation_level) {
			/*
			* firebird's `read committed` has the option to wait until other transactions
			* commit or rollback if there is indeterminate data.
			* Introducing too many configuration values at once can cause confusion, so
			* we don't support in PDO that feature yet.
			*/
			case PDO_FB_READ_COMMITTED:
				tpb[2] = isc_tpb_read_committed;
				/* Ignore indeterminate data from other transactions. This option only required with `read committed`. */
				tpb[3] = isc_tpb_rec_version;
				tpb_size = 4;
				break;

			case PDO_FB_SERIALIZABLE:
				tpb[2] = isc_tpb_consistency;
				tpb_size = 3;
				break;

			case PDO_FB_REPEATABLE_READ:
			default:
				tpb[2] = isc_tpb_concurrency;
				tpb_size = 3;
				break;
		}
	}

	if (isc_start_transaction(H->isc_status, &H->tr, 1, &H->db, tpb_size, tpb)) {
		php_firebird_error(dbh);
		return false;
	}
	return true;
}
/* }}} */

/* called by PDO to start a transaction */
static bool firebird_handle_manually_begin(pdo_dbh_t *dbh) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	/**
	 * If in autocommit mode and in transaction, we will need to close the transaction once.
	 */
	if (dbh->auto_commit && H->tr) {
		if (!php_firebird_commit_transaction(dbh, /* retain */ false)) {
			return false;
		}
	}

	if (!php_firebird_begin_transaction(dbh, /* auto commit mode */ false)) {
		return false;
	}
	H->in_manually_txn = 1;
	return true;
}
/* }}} */

/* php_firebird_commit_transaction */
bool php_firebird_commit_transaction(pdo_dbh_t *dbh, bool retain) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	/**
	 * `retaining` keeps the transaction open without closing it.
	 *
	 * firebird needs to always have a transaction open to emulate autocommit mode,
	 * and in autocommit mode it keeps the transaction open.
	 *
	 * Same as close and then begin again, but use retain to save overhead.
	 */
	if (retain) {
		if (isc_commit_retaining(H->isc_status, &H->tr)) {
			php_firebird_error(dbh);
			return false;
		}
	} else {
		if (isc_commit_transaction(H->isc_status, &H->tr)) {
			php_firebird_error(dbh);
			return false;
		}
	}
	return true;
}
/* }}} */

/* called by PDO to commit a transaction */
static bool firebird_handle_manually_commit(pdo_dbh_t *dbh) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	if (!php_firebird_commit_transaction(dbh, /*release*/ false)) {
		return false;
	}

	/**
	 * If in autocommit mode, begin the transaction again
	 * Reopen instead of retain because isolation level may change
	 */
	if (dbh->auto_commit) {
		if (!php_firebird_begin_transaction(dbh, /* auto commit mode */ true)) {
			return false;
		}
	}
	H->in_manually_txn = 0;
	return true;
}
/* }}} */

/* php_firebird_rollback_transaction */
static bool php_firebird_rollback_transaction(pdo_dbh_t *dbh) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	if (isc_rollback_transaction(H->isc_status, &H->tr)) {
		php_firebird_error(dbh);
		return false;
	}
	return true;
}
/* }}} */

/* called by PDO to rollback a transaction */
static bool firebird_handle_manually_rollback(pdo_dbh_t *dbh) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	if (!php_firebird_rollback_transaction(dbh)) {
		return false;
	}

	/**
	 * If in autocommit mode, begin the transaction again
	 * Reopen instead of retain because isolation level may change
	 */
	if (dbh->auto_commit) {
		if (!php_firebird_begin_transaction(dbh, /* auto commit mode */ true)) {
			return false;
		}
	}
	H->in_manually_txn = 0;
	return true;
}
/* }}} */

/* used by prepare and exec to allocate a statement handle and prepare the SQL */
static int php_firebird_alloc_prepare_stmt(pdo_dbh_t *dbh, const zend_string *sql,
	XSQLDA *out_sqlda, isc_stmt_handle *s, HashTable *named_params)
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	char *new_sql;

	/* allocate the statement */
	if (isc_dsql_allocate_statement(H->isc_status, &H->db, s)) {
		php_firebird_error(dbh);
		return 0;
	}

	/* in order to support named params, which Firebird itself doesn't,
	   we need to replace :foo by ?, and store the name we just replaced */
	new_sql = emalloc(ZSTR_LEN(sql)+1);
	new_sql[0] = '\0';
	if (!php_firebird_preprocess(sql, new_sql, named_params)) {
		php_firebird_error_with_info(dbh, "07000", strlen("07000"), NULL, 0);
		efree(new_sql);
		return 0;
	}

	/* prepare the statement */
	if (isc_dsql_prepare(H->isc_status, &H->tr, s, 0, new_sql, H->sql_dialect, out_sqlda)) {
		php_firebird_error(dbh);
		efree(new_sql);
		return 0;
	}

	efree(new_sql);
	return 1;
}

/* called by PDO to set a driver-specific dbh attribute */
static bool pdo_firebird_set_attribute(pdo_dbh_t *dbh, zend_long attr, zval *val) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	bool bval;
	zend_long lval;

	switch (attr) {
		case PDO_ATTR_AUTOCOMMIT:
			{
				if (!pdo_get_bool_param(&bval, val)) {
					return false;
				}

				if (H->in_manually_txn) {
					/* change auto commit mode with an open transaction is illegal, because
						we won't know what to do with it */
					pdo_raise_impl_error(dbh, NULL, "HY000", "Cannot change autocommit mode while a transaction is already open");
					return false;
				}

				/* ignore if the new value equals the old one */
				if (dbh->auto_commit ^ bval) {
					if (bval) {
						/*
						 * change to auto commit mode.
						 * If the transaction is not started, start it.
						 */
						if (!H->tr) {
							if (!php_firebird_begin_transaction(dbh, /* auto commit mode */ true)) {
								return false;
							}
						}
					} else {
						/*
						 * change to not auto commit mode.
						 * close the transaction if exists.
						 */
						if (H->tr) {
							if (!php_firebird_commit_transaction(dbh, /* retain */ false)) {
								return false;
							}
						}
					}
					dbh->auto_commit = bval;
				}
			}
			return true;

		case PDO_ATTR_FETCH_TABLE_NAMES:
			if (!pdo_get_bool_param(&bval, val)) {
				return false;
			}
			H->fetch_table_names = bval;
			return true;

		case PDO_FB_ATTR_DATE_FORMAT:
			{
				zend_string *str = zval_try_get_string(val);
				if (UNEXPECTED(!str)) {
					return false;
				}
				if (H->date_format) {
					pefree(H->date_format, dbh->is_persistent);
					H->date_format = NULL;
				}
				H->date_format = pestrndup(ZSTR_VAL(str), ZSTR_LEN(str),dbh->is_persistent);
				zend_string_release_ex(str, 0);
			}
			return true;

		case PDO_FB_ATTR_TIME_FORMAT:
			{
				zend_string *str = zval_try_get_string(val);
				if (UNEXPECTED(!str)) {
					return false;
				}
				if (H->time_format) {
					pefree(H->time_format, dbh->is_persistent);
					H->time_format = NULL;
				}
				H->time_format = pestrndup(ZSTR_VAL(str), ZSTR_LEN(str),dbh->is_persistent);
				zend_string_release_ex(str, 0);
			}
			return true;

		case PDO_FB_ATTR_TIMESTAMP_FORMAT:
			{
				zend_string *str = zval_try_get_string(val);
				if (UNEXPECTED(!str)) {
					return false;
				}
				if (H->timestamp_format) {
					pefree(H->timestamp_format, dbh->is_persistent);
					H->timestamp_format = NULL;
				}
				H->timestamp_format = pestrndup(ZSTR_VAL(str), ZSTR_LEN(str),dbh->is_persistent);
				zend_string_release_ex(str, 0);
			}
			return true;

		case PDO_FB_TRANSACTION_ISOLATION_LEVEL:
			{
				if (!pdo_get_long_param(&lval, val)) {
					return false;
				}

				if (H->in_manually_txn) {
					pdo_raise_impl_error(dbh, NULL, "HY000", "Cannot change transaction isolation level while a transaction is already open");
					return false;
				}

				/* ignore if the new value equals the old one */
				if (H->txn_isolation_level != lval) {
					if (lval == PDO_FB_READ_COMMITTED ||
						lval == PDO_FB_REPEATABLE_READ ||
						lval == PDO_FB_SERIALIZABLE
					) {
						/*
						 * Autocommit mode is always read-committed, so this setting is used the next time
						 * a manual transaction starts. Therefore, there is no need to immediately reopen the transaction.
						 */
						H->txn_isolation_level = lval;
					} else {
						zend_value_error("Pdo\\Firebird::TRANSACTION_ISOLATION_LEVEL must be a valid transaction isolation level "
							"(Pdo\\Firebird::READ_COMMITTED, Pdo\\Firebird::REPEATABLE_READ, or Pdo\\Firebird::SERIALIZABLE)");
						return false;
					}
				}
			}
			return true;

		case PDO_FB_WRITABLE_TRANSACTION:
			{
				if (!pdo_get_bool_param(&bval, val)) {
					return false;
				}

				if (H->in_manually_txn) {
					pdo_raise_impl_error(dbh, NULL, "HY000", "Cannot change access mode while a transaction is already open");
					return false;
				}

				/* ignore if the new value equals the old one */
				if (H->is_writable_txn != bval) {
					H->is_writable_txn = bval;
					if (dbh->auto_commit) {
						if (H->tr) {
							if (!php_firebird_commit_transaction(dbh, /* retain */ false)) {
								/* In case of error, revert the setting */
								H->is_writable_txn = !bval;
								return false;
							}
						}
						if (!php_firebird_begin_transaction(dbh, /* auto commit mode */ true)) {
							/* In case of error, revert the setting */
							H->is_writable_txn = !bval;
							return false;
						}
					}
				}
			}
			return true;
	}
	return false;
}
/* }}} */

#define INFO_BUF_LEN 512

/* callback to used to report database server info */
static void php_firebird_info_cb(void *arg, char const *s) /* {{{ */
{
	if (arg) {
		if (*(char*)arg) { /* second call */
			strlcat(arg, " ", INFO_BUF_LEN);
		}
		strlcat(arg, s, INFO_BUF_LEN);
	}
}
/* }}} */

/* called by PDO to get a driver-specific dbh attribute */
static int pdo_firebird_get_attribute(pdo_dbh_t *dbh, zend_long attr, zval *val) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	switch (attr) {
		char tmp[INFO_BUF_LEN];

		case PDO_ATTR_AUTOCOMMIT:
			ZVAL_BOOL(val,dbh->auto_commit);
			return 1;

		case PDO_ATTR_CONNECTION_STATUS:
			ZVAL_BOOL(val, !isc_version(&H->db, php_firebird_info_cb, NULL));
			return 1;

		case PDO_ATTR_CLIENT_VERSION: {
#if defined(__GNUC__) || defined(PHP_WIN32)
			info_func_t info_func = NULL;
#ifdef __GNUC__
			info_func = (info_func_t)dlsym(RTLD_DEFAULT, "isc_get_client_version");
#else
			HMODULE l = GetModuleHandle("fbclient");

			if (!l) {
				break;
			}
			info_func = (info_func_t)GetProcAddress(l, "isc_get_client_version");
#endif
			if (info_func) {
				info_func(tmp);
				ZVAL_STRING(val, tmp);
			}
#else
			ZVAL_NULL(val);
#endif
			}
			return 1;

		case PDO_ATTR_SERVER_VERSION:
		case PDO_ATTR_SERVER_INFO:
			*tmp = 0;

			if (!isc_version(&H->db, php_firebird_info_cb, (void*)tmp)) {
				ZVAL_STRING(val, tmp);
				return 1;
			}
			return -1;

		case PDO_ATTR_FETCH_TABLE_NAMES:
			ZVAL_BOOL(val, H->fetch_table_names);
			return 1;

		case PDO_FB_ATTR_DATE_FORMAT:
			ZVAL_STRING(val, H->date_format ? H->date_format : PDO_FB_DEF_DATE_FMT);
			return 1;

		case PDO_FB_ATTR_TIME_FORMAT:
			ZVAL_STRING(val, H->time_format ? H->time_format : PDO_FB_DEF_TIME_FMT);
			return 1;

		case PDO_FB_ATTR_TIMESTAMP_FORMAT:
			ZVAL_STRING(val, H->timestamp_format ? H->timestamp_format : PDO_FB_DEF_TIMESTAMP_FMT);
			return 1;

		case PDO_FB_TRANSACTION_ISOLATION_LEVEL:
			ZVAL_LONG(val, H->txn_isolation_level);
			return 1;

		case PDO_FB_WRITABLE_TRANSACTION:
			ZVAL_BOOL(val, H->is_writable_txn);
			return 1;
	}
	return 0;
}
/* }}} */

/* called by PDO to check liveness */
static zend_result pdo_firebird_check_liveness(pdo_dbh_t *dbh) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;

	/* fb_ping return 0 if the connection is alive */
	return fb_ping(H->isc_status, &H->db) ? FAILURE : SUCCESS;
}
/* }}} */

/* called by PDO to retrieve driver-specific information about an error that has occurred */
static void pdo_firebird_fetch_error_func(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info) /* {{{ */
{
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	if (H->einfo.sqlcode != IS_NULL) {
		add_next_index_long(info, H->einfo.sqlcode);
	}
	if (H->einfo.errmsg && H->einfo.errmsg_length) {
		add_next_index_stringl(info, H->einfo.errmsg, H->einfo.errmsg_length);
	}
}
/* }}} */

/* {{{ firebird_in_manually_transaction */
static bool pdo_firebird_in_manually_transaction(pdo_dbh_t *dbh)
{
	/**
	 * we can tell if a transaction exists now by checking H->tr,
	 * but which will always be true in autocommit mode.
	 * So this function checks if there is currently a "manually begun transaction".
	 */
	pdo_firebird_db_handle *H = (pdo_firebird_db_handle *)dbh->driver_data;
	return H->in_manually_txn;
}
/* }}} */

static const struct pdo_dbh_methods firebird_methods = { /* {{{ */
	firebird_handle_closer,
	firebird_handle_preparer,
	firebird_handle_doer,
	firebird_handle_quoter,
	firebird_handle_manually_begin,
	firebird_handle_manually_commit,
	firebird_handle_manually_rollback,
	pdo_firebird_set_attribute,
	NULL, /* last_id not supported */
	pdo_firebird_fetch_error_func,
	pdo_firebird_get_attribute,
	pdo_firebird_check_liveness,
	NULL, /* get driver methods */
	NULL, /* request shutdown */
	pdo_firebird_in_manually_transaction,
	NULL, /* get gc */
	NULL /* scanner */
};
/* }}} */

/* the driver-specific PDO handle constructor */
static int pdo_firebird_handle_factory(pdo_dbh_t *dbh, zval *driver_options) /* {{{ */
{
	struct pdo_data_src_parser vars[] = {
		{ "dbname", NULL, 0 },
		{ "charset",  NULL,	0 },
		{ "role", NULL,	0 },
		{ "dialect", "3", 0 },
		{ "user", NULL, 0 },
		{ "password", NULL, 0 }
	};
	int i, ret = 0;
	short buf_len = 256, dpb_len;

	pdo_firebird_db_handle *H = dbh->driver_data = pecalloc(1,sizeof(*H),dbh->is_persistent);

	php_pdo_parse_data_source(dbh->data_source, dbh->data_source_len, vars, 6);

	if (!dbh->username && vars[4].optval) {
		dbh->username = pestrdup(vars[4].optval, dbh->is_persistent);
	}

	if (!dbh->password && vars[5].optval) {
		dbh->password = pestrdup(vars[5].optval, dbh->is_persistent);
	}

	H->in_manually_txn = 0;
	H->is_writable_txn = pdo_attr_lval(driver_options, PDO_FB_WRITABLE_TRANSACTION, 1);
	zend_long txn_isolation_level = pdo_attr_lval(driver_options, PDO_FB_TRANSACTION_ISOLATION_LEVEL, PDO_FB_REPEATABLE_READ);
	if (txn_isolation_level == PDO_FB_READ_COMMITTED ||
		txn_isolation_level == PDO_FB_REPEATABLE_READ ||
		txn_isolation_level == PDO_FB_SERIALIZABLE
	) {
		H->txn_isolation_level = txn_isolation_level;
	} else {
		zend_value_error("Pdo\\Firebird::TRANSACTION_ISOLATION_LEVEL must be a valid transaction isolation level "
			"(Pdo\\Firebird::READ_COMMITTED, Pdo\\Firebird::REPEATABLE_READ, or Pdo\\Firebird::SERIALIZABLE)");
		ret = 0;
	}

	do {
		static char const dpb_flags[] = {
			isc_dpb_user_name, isc_dpb_password, isc_dpb_lc_ctype, isc_dpb_sql_role_name };
		char const *dpb_values[] = { dbh->username, dbh->password, vars[1].optval, vars[2].optval };
		char dpb_buffer[256] = { isc_dpb_version1 }, *dpb;

		dpb = dpb_buffer + 1;

		/* loop through all the provided arguments and set dpb fields accordingly */
		for (i = 0; i < sizeof(dpb_flags); ++i) {
			if (dpb_values[i] && buf_len > 0) {
				dpb_len = slprintf(dpb, buf_len, "%c%c%s", dpb_flags[i], (unsigned char)strlen(dpb_values[i]),
					dpb_values[i]);
				dpb += dpb_len;
				buf_len -= dpb_len;
			}
		}

		H->sql_dialect = PDO_FB_DIALECT;
		if (vars[3].optval) {
			H->sql_dialect = atoi(vars[3].optval);
		}

		/* fire it up baby! */
		if (isc_attach_database(H->isc_status, 0, vars[0].optval, &H->db,(short)(dpb-dpb_buffer), dpb_buffer)) {
			break;
		}

		dbh->methods = &firebird_methods;
		dbh->native_case = PDO_CASE_UPPER;
		dbh->alloc_own_columns = 1;

		ret = 1;

	} while (0);

	for (i = 0; i < sizeof(vars)/sizeof(vars[0]); ++i) {
		if (vars[i].freeme) {
			efree(vars[i].optval);
		}
	}

	if (!dbh->methods) {
		char errmsg[512];
		const ISC_STATUS *s = H->isc_status;
		fb_interpret(errmsg, sizeof(errmsg),&s);
		zend_throw_exception_ex(php_pdo_get_exception(), H->isc_status[1], "SQLSTATE[%s] [%ld] %s",
				"HY000", H->isc_status[1], errmsg);
	}

	if (ret && dbh->auto_commit && !H->tr) {
		ret = php_firebird_begin_transaction(dbh, /* auto commit mode */ true);
	}

	if (!ret) {
		firebird_handle_closer(dbh);
	}

	return ret;
}
/* }}} */


const pdo_driver_t pdo_firebird_driver = { /* {{{ */
	PDO_DRIVER_HEADER(firebird),
	pdo_firebird_handle_factory
};
/* }}} */
