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
  | Author: Rob Richards <rrichards@php.net>                             |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_observer.h"
#include "php_xmlreader.h"
#ifdef HAVE_DOM
#include "ext/dom/xml_common.h"
#include "ext/dom/dom_ce.h"
#endif
#include <libxml/xmlreader.h>
#include <libxml/uri.h>
#include "php_xmlreader_arginfo.h"

zend_class_entry *xmlreader_class_entry;

static zend_object_handlers xmlreader_object_handlers;

static HashTable xmlreader_prop_handlers;

static zend_internal_function xmlreader_open_fn;
static zend_internal_function xmlreader_xml_fn;

typedef int (*xmlreader_read_int_t)(xmlTextReaderPtr reader);
typedef unsigned char *(*xmlreader_read_char_t)(xmlTextReaderPtr reader);
typedef const unsigned char *(*xmlreader_read_const_char_t)(xmlTextReaderPtr reader);

typedef unsigned char *(*xmlreader_read_one_char_t)(xmlTextReaderPtr reader, const unsigned char *);

typedef struct _xmlreader_prop_handler {
	xmlreader_read_int_t read_int_func;
	xmlreader_read_const_char_t read_char_func;
	int type;
} xmlreader_prop_handler;

#define XMLREADER_LOAD_STRING 0
#define XMLREADER_LOAD_FILE 1

static void xmlreader_register_prop_handler(HashTable *prop_handler, const char *name, size_t name_len, const xmlreader_prop_handler *hnd)
{
	zend_string *str = zend_string_init_interned(name, name_len, true);
	zend_hash_add_new_ptr(prop_handler, str, (void *) hnd);
	zend_string_release_ex(str, true);
}

#define XMLREADER_REGISTER_PROP_HANDLER(prop_handler, name, prop_read_int_func, prop_read_char_func, prop_type) do { \
		static const xmlreader_prop_handler hnd = {.read_int_func = prop_read_int_func, .read_char_func = prop_read_char_func, .type = prop_type}; \
		xmlreader_register_prop_handler(prop_handler, "" name, sizeof("" name) - 1, &hnd); \
	} while (0)

/* {{{ xmlreader_property_reader */
static int xmlreader_property_reader(xmlreader_object *obj, xmlreader_prop_handler *hnd, zval *rv)
{
	const xmlChar *retchar = NULL;
	int retint = 0;

	if (obj->ptr != NULL) {
		if (hnd->read_char_func) {
			retchar = hnd->read_char_func(obj->ptr);
		} else {
			if (hnd->read_int_func) {
				retint = hnd->read_int_func(obj->ptr);
				if (retint == -1) {
					zend_throw_error(NULL, "Failed to read property because no XML data has been read yet");
					return FAILURE;
				}
			}
		}
	}

	switch (hnd->type) {
		case IS_STRING:
			if (retchar) {
				ZVAL_STRING(rv, (char *) retchar);
			} else {
				ZVAL_EMPTY_STRING(rv);
			}
			break;
		case _IS_BOOL:
			ZVAL_BOOL(rv, retint);
			break;
		case IS_LONG:
			ZVAL_LONG(rv, retint);
			break;
		EMPTY_SWITCH_DEFAULT_CASE()
	}

	return SUCCESS;
}
/* }}} */

/* {{{ xmlreader_get_property_ptr_ptr */
zval *xmlreader_get_property_ptr_ptr(zend_object *object, zend_string *name, int type, void **cache_slot)
{
	zval *retval = NULL;

	xmlreader_prop_handler *hnd = zend_hash_find_ptr(&xmlreader_prop_handlers, name);
	if (hnd == NULL) {
		retval = zend_std_get_property_ptr_ptr(object, name, type, cache_slot);
	} else if (cache_slot) {
		cache_slot[0] = cache_slot[1] = cache_slot[2] = NULL;
	}

	return retval;
}
/* }}} */

static int xmlreader_has_property(zend_object *object, zend_string *name, int type, void **cache_slot)
{
	xmlreader_object *obj = php_xmlreader_fetch_object(object);
	xmlreader_prop_handler *hnd = zend_hash_find_ptr(&xmlreader_prop_handlers, name);

	if (hnd != NULL) {
		if (type == ZEND_PROPERTY_EXISTS) {
			return 1;
		}

		zval rv;
		if (xmlreader_property_reader(obj, hnd, &rv) == FAILURE) {
			return 0;
		}

		bool result;

		if (type == ZEND_PROPERTY_NOT_EMPTY) {
			result = zend_is_true(&rv);
		} else if (type == ZEND_PROPERTY_ISSET) {
			result = (Z_TYPE(rv) != IS_NULL);
		} else {
			ZEND_UNREACHABLE();
		}

		zval_ptr_dtor(&rv);

		return result;
	}

	return zend_std_has_property(object, name, type, cache_slot);
}


/* {{{ xmlreader_read_property */
zval *xmlreader_read_property(zend_object *object, zend_string *name, int type, void **cache_slot, zval *rv)
{
	zval *retval = NULL;
	xmlreader_object *obj = php_xmlreader_fetch_object(object);
	xmlreader_prop_handler *hnd = zend_hash_find_ptr(&xmlreader_prop_handlers, name);

	if (hnd != NULL) {
		if (xmlreader_property_reader(obj, hnd, rv) == FAILURE) {
			retval = &EG(uninitialized_zval);
		} else {
			retval = rv;
		}
	} else {
		retval = zend_std_read_property(object, name, type, cache_slot, rv);
	}

	return retval;
}
/* }}} */

/* {{{ xmlreader_write_property */
zval *xmlreader_write_property(zend_object *object, zend_string *name, zval *value, void **cache_slot)
{
	xmlreader_prop_handler *hnd = zend_hash_find_ptr(&xmlreader_prop_handlers, name);

	if (hnd != NULL) {
		zend_readonly_property_modification_error_ex(ZSTR_VAL(object->ce->name), ZSTR_VAL(name));
	} else {
		value = zend_std_write_property(object, name, value, cache_slot);
	}

	return value;
}
/* }}} */

void xmlreader_unset_property(zend_object *object, zend_string *name, void **cache_slot)
{
	xmlreader_prop_handler *hnd = zend_hash_find_ptr(&xmlreader_prop_handlers, name);

	if (hnd != NULL) {
		zend_throw_error(NULL, "Cannot unset %s::$%s", ZSTR_VAL(object->ce->name), ZSTR_VAL(name));
		return;
	}

	zend_std_unset_property(object, name, cache_slot);
}

/* {{{ */
static zend_function *xmlreader_get_method(zend_object **obj, zend_string *name, const zval *key)
{
	zend_function *method = zend_std_get_method(obj, name, key);
	if (method && (method->common.fn_flags & ZEND_ACC_STATIC) && method->common.type == ZEND_INTERNAL_FUNCTION) {
		/* There are only two static internal methods and they both have overrides. */
		if (ZSTR_LEN(name) == sizeof("xml") - 1) {
			return (zend_function *) &xmlreader_xml_fn;
		} else if (ZSTR_LEN(name) == sizeof("open") - 1) {
			return (zend_function *) &xmlreader_open_fn;
		}
	}
	return method;
}
/* }}} */

static HashTable* xmlreader_get_debug_info(zend_object *object, int *is_temp)
{
	*is_temp = 1;

	xmlreader_object *obj = php_xmlreader_fetch_object(object);
	HashTable *std_props = zend_std_get_properties(object);
	HashTable *debug_info = zend_array_dup(std_props);

	zend_string *string_key;
	xmlreader_prop_handler *entry;
	ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(&xmlreader_prop_handlers, string_key, entry) {
		ZEND_ASSERT(string_key != NULL);

		zval value;
		if (xmlreader_property_reader(obj, entry, &value) == SUCCESS) {
			zend_hash_update(debug_info, string_key, &value);
		}
	} ZEND_HASH_FOREACH_END();

	return debug_info;
}

/* {{{ _xmlreader_get_valid_file_path */
/* _xmlreader_get_valid_file_path and _xmlreader_get_relaxNG should be made a
	common function in libxml extension as code is common to a few xml extensions */
char *_xmlreader_get_valid_file_path(char *source, char *resolved_path, int resolved_path_len ) {
	xmlURI *uri;
	xmlChar *escsource;
	char *file_dest;
	int isFileUri = 0;

	uri = xmlCreateURI();
	if (uri == NULL) {
		return NULL;
	}
	escsource = xmlURIEscapeStr((xmlChar *)source, (xmlChar *)":");
	xmlParseURIReference(uri, (const char *)escsource);
	xmlFree(escsource);

	if (uri->scheme != NULL) {
		/* absolute file uris - libxml only supports localhost or empty host */
		if (strncasecmp(source, "file:///",8) == 0) {
			isFileUri = 1;
#ifdef PHP_WIN32
			source += 8;
#else
			source += 7;
#endif
		} else if (strncasecmp(source, "file://localhost/",17) == 0) {
			isFileUri = 1;
#ifdef PHP_WIN32
			source += 17;
#else
			source += 16;
#endif
		}
	}

	file_dest = source;

	if ((uri->scheme == NULL || isFileUri)) {
		if (!VCWD_REALPATH(source, resolved_path) && !expand_filepath(source, resolved_path)) {
			xmlFreeURI(uri);
			return NULL;
		}
		file_dest = resolved_path;
	}

	xmlFreeURI(uri);

	return file_dest;
}
/* }}} */

#ifdef LIBXML_SCHEMAS_ENABLED
/* {{{ _xmlreader_get_relaxNG */
static xmlRelaxNGPtr _xmlreader_get_relaxNG(char *source, size_t source_len, size_t type,
											xmlRelaxNGValidityErrorFunc error_func,
											xmlRelaxNGValidityWarningFunc warn_func)
{
	char *valid_file = NULL;
	xmlRelaxNGParserCtxtPtr parser = NULL;
	xmlRelaxNGPtr           sptr;
	char resolved_path[MAXPATHLEN + 1];

	switch (type) {
	case XMLREADER_LOAD_FILE:
		valid_file = _xmlreader_get_valid_file_path(source, resolved_path, MAXPATHLEN );
		if (!valid_file) {
			return NULL;
		}
		parser = xmlRelaxNGNewParserCtxt(valid_file);
		break;
	case XMLREADER_LOAD_STRING:
		parser = xmlRelaxNGNewMemParserCtxt(source, source_len);
		/* If loading from memory, we need to set the base directory for the document
		   but it is not apparent how to do that for schema's */
		break;
	default:
		return NULL;
	}

	if (parser == NULL) {
		return NULL;
	}

	PHP_LIBXML_SANITIZE_GLOBALS(parse);
	if (error_func || warn_func) {
		xmlRelaxNGSetParserErrors(parser,
			(xmlRelaxNGValidityErrorFunc) error_func,
			(xmlRelaxNGValidityWarningFunc) warn_func,
			parser);
	}
	sptr = xmlRelaxNGParse(parser);
	xmlRelaxNGFreeParserCtxt(parser);
	PHP_LIBXML_RESTORE_GLOBALS(parse);

	return sptr;
}
/* }}} */
#endif

static const zend_module_dep xmlreader_deps[] = {
#ifdef HAVE_DOM
	ZEND_MOD_REQUIRED("dom")
#endif
	ZEND_MOD_REQUIRED("libxml")
	ZEND_MOD_END
};

/* {{{ xmlreader_module_entry */
zend_module_entry xmlreader_module_entry = {
	STANDARD_MODULE_HEADER_EX, NULL,
	xmlreader_deps,
	"xmlreader",
	NULL,
	PHP_MINIT(xmlreader),
	PHP_MSHUTDOWN(xmlreader),
	NULL,
	NULL,
	PHP_MINFO(xmlreader),
	PHP_XMLREADER_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_XMLREADER
ZEND_GET_MODULE(xmlreader)
#endif

/* {{{ xmlreader_free_resources */
static void xmlreader_free_resources(xmlreader_object *intern) {
	if (intern->input) {
		xmlFreeParserInputBuffer(intern->input);
		intern->input = NULL;
	}

	if (intern->ptr) {
		xmlFreeTextReader(intern->ptr);
		intern->ptr = NULL;
	}
#ifdef LIBXML_SCHEMAS_ENABLED
	if (intern->schema) {
		xmlRelaxNGFree((xmlRelaxNGPtr) intern->schema);
		intern->schema = NULL;
	}
#endif
}
/* }}} */

/* {{{ xmlreader_objects_free_storage */
void xmlreader_objects_free_storage(zend_object *object)
{
	xmlreader_object *intern = php_xmlreader_fetch_object(object);

	zend_object_std_dtor(&intern->std);

	xmlreader_free_resources(intern);
}
/* }}} */

/* {{{ xmlreader_objects_new */
zend_object *xmlreader_objects_new(zend_class_entry *class_type)
{
	xmlreader_object *intern;

	intern = zend_object_alloc(sizeof(xmlreader_object), class_type);
	zend_object_std_init(&intern->std, class_type);
	object_properties_init(&intern->std, class_type);

	return &intern->std;
}
/* }}} */

/* {{{ php_xmlreader_string_arg */
static void php_xmlreader_string_arg(INTERNAL_FUNCTION_PARAMETERS, xmlreader_read_one_char_t internal_function) {
	zval *id;
	size_t name_len = 0;
	char *retchar = NULL;
	xmlreader_object *intern;
	char *name;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &name, &name_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (!name_len) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retchar = (char *)internal_function(intern->ptr, (const unsigned char *)name);
	}
	if (retchar) {
		RETVAL_STRING(retchar);
		xmlFree(retchar);
		return;
	} else {
		RETVAL_NULL();
	}
}
/* }}} */

/* {{{ php_xmlreader_no_arg */
static void php_xmlreader_no_arg(INTERNAL_FUNCTION_PARAMETERS, xmlreader_read_int_t internal_function) {
	zval *id;
	int retval;
	xmlreader_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retval = internal_function(intern->ptr);
		if (retval == 1) {
			RETURN_TRUE;
		}
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ php_xmlreader_no_arg_string */
static void php_xmlreader_no_arg_string(INTERNAL_FUNCTION_PARAMETERS, xmlreader_read_char_t internal_function) {
	zval *id;
	char *retchar = NULL;
	xmlreader_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retchar = (char *)internal_function(intern->ptr);
	}
	if (retchar) {
		RETVAL_STRING(retchar);
		xmlFree(retchar);
		return;
	} else {
		RETVAL_EMPTY_STRING();
	}
}
/* }}} */

/* {{{ php_xmlreader_set_relaxng_schema */
static void php_xmlreader_set_relaxng_schema(INTERNAL_FUNCTION_PARAMETERS, int type) {
#ifdef LIBXML_SCHEMAS_ENABLED
	zval *id;
	size_t source_len = 0;
	int retval = -1;
	xmlreader_object *intern;
	xmlRelaxNGPtr schema = NULL;
	char *source;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "p!", &source, &source_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (source != NULL && !source_len) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		if (source) {
			schema =  _xmlreader_get_relaxNG(source, source_len, type, NULL, NULL);
			if (schema) {
				retval = xmlTextReaderRelaxNGSetSchema(intern->ptr, schema);
			}
		} else {
			/* unset the associated relaxNG context and schema if one exists */
			retval = xmlTextReaderRelaxNGSetSchema(intern->ptr, NULL);
		}

		if (retval == 0) {
			if (intern->schema) {
				xmlRelaxNGFree((xmlRelaxNGPtr) intern->schema);
			}

			intern->schema = schema;

			RETURN_TRUE;
		} else {
			php_error_docref(NULL, E_WARNING, "Schema contains errors");
			RETURN_FALSE;
		}
	} else {
		zend_throw_error(NULL, "Schema must be set prior to reading");
		RETURN_THROWS();
	}
#else
	php_error_docref(NULL, E_WARNING, "No schema support built into libxml");
	RETURN_FALSE;
#endif
}
/* }}} */

/* {{{ Closes xmlreader - current frees resources until xmlTextReaderClose is fixed in libxml */
PHP_METHOD(XMLReader, close)
{
	zval *id;
	xmlreader_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;
	intern = Z_XMLREADER_P(id);
	/* libxml is segfaulting in versions up to 2.6.8 using xmlTextReaderClose so for
	now we will free the whole reader when close is called as it would get rebuilt on
	a new load anyways */
	xmlreader_free_resources(intern);

	RETURN_TRUE;
}
/* }}} */

/* {{{ Get value of an attribute from current element */
PHP_METHOD(XMLReader, getAttribute)
{
	php_xmlreader_string_arg(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderGetAttribute);
}
/* }}} */

/* {{{ Get value of an attribute at index from current element */
PHP_METHOD(XMLReader, getAttributeNo)
{
	zval *id;
	zend_long attr_pos;
	char *retchar = NULL;
	xmlreader_object *intern;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &attr_pos) == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retchar = (char *)xmlTextReaderGetAttributeNo(intern->ptr, attr_pos);
	}
	if (retchar) {
		RETVAL_STRING(retchar);
		xmlFree(retchar);
	}
}
/* }}} */

/* {{{ Get value of a attribute via name and namespace from current element */
PHP_METHOD(XMLReader, getAttributeNs)
{
	zval *id;
	size_t name_len = 0, ns_uri_len = 0;
	xmlreader_object *intern;
	char *name, *ns_uri, *retchar = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &name, &name_len, &ns_uri, &ns_uri_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (name_len == 0) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	if (ns_uri_len == 0) {
		zend_argument_must_not_be_empty_error(2);
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retchar = (char *)xmlTextReaderGetAttributeNs(intern->ptr, (xmlChar *)name, (xmlChar *)ns_uri);
	}
	if (retchar) {
		RETVAL_STRING(retchar);
		xmlFree(retchar);
	}
}
/* }}} */

/* {{{ Indicates whether given property (one of the parser option constants) is set or not on parser */
PHP_METHOD(XMLReader, getParserProperty)
{
	zval *id;
	zend_long property;
	xmlreader_object *intern;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &property) == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (!intern || !intern->ptr) {
		zend_throw_error(NULL, "Cannot access parser properties before loading data");
		RETURN_THROWS();
	}

	int retval = xmlTextReaderGetParserProp(intern->ptr,property);
	if (retval == -1) {
		zend_argument_value_error(1, "must be a valid parser property");
		RETURN_THROWS();
	}

	RETURN_BOOL(retval);
}
/* }}} */

/* {{{ Returns boolean indicating if parsed document is valid or not.
Must set XMLREADER_LOADDTD or XMLREADER_VALIDATE parser option prior to the first call to read
or this method will always return FALSE */
PHP_METHOD(XMLReader, isValid)
{
	php_xmlreader_no_arg(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderIsValid);
}
/* }}} */

/* {{{ Return namespaceURI for associated prefix on current node */
PHP_METHOD(XMLReader, lookupNamespace)
{
	php_xmlreader_string_arg(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderLookupNamespace);
}
/* }}} */

/* {{{ Positions reader at specified attribute - Returns TRUE on success and FALSE on failure */
PHP_METHOD(XMLReader, moveToAttribute)
{
	zval *id;
	size_t name_len = 0;
	int retval;
	xmlreader_object *intern;
	char *name;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &name, &name_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (name_len == 0) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retval = xmlTextReaderMoveToAttribute(intern->ptr, (xmlChar *)name);
		if (retval == 1) {
			RETURN_TRUE;
		}
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ Positions reader at attribute at specified index.
Returns TRUE on success and FALSE on failure */
PHP_METHOD(XMLReader, moveToAttributeNo)
{
	zval *id;
	zend_long attr_pos;
	int retval;
	xmlreader_object *intern;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &attr_pos) == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retval = xmlTextReaderMoveToAttributeNo(intern->ptr, attr_pos);
		if (retval == 1) {
			RETURN_TRUE;
		}
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ Positions reader at attribute spcified by name and namespaceURI.
Returns TRUE on success and FALSE on failure */
PHP_METHOD(XMLReader, moveToAttributeNs)
{
	zval *id;
	size_t name_len=0, ns_uri_len=0;
	int retval;
	xmlreader_object *intern;
	char *name, *ns_uri;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &name, &name_len, &ns_uri, &ns_uri_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (name_len == 0) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	if (ns_uri_len == 0) {
		zend_argument_must_not_be_empty_error(2);
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retval = xmlTextReaderMoveToAttributeNs(intern->ptr, (xmlChar *)name, (xmlChar *)ns_uri);
		if (retval == 1) {
			RETURN_TRUE;
		}
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ Moves the position of the current instance to the node that contains the current Attribute node. */
PHP_METHOD(XMLReader, moveToElement)
{
	php_xmlreader_no_arg(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderMoveToElement);
}
/* }}} */

/* {{{ Moves the position of the current instance to the first attribute associated with the current node. */
PHP_METHOD(XMLReader, moveToFirstAttribute)
{
	php_xmlreader_no_arg(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderMoveToFirstAttribute);
}
/* }}} */

/* {{{ Moves the position of the current instance to the next attribute associated with the current node. */
PHP_METHOD(XMLReader, moveToNextAttribute)
{
	php_xmlreader_no_arg(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderMoveToNextAttribute);
}
/* }}} */

/* {{{ Moves the position of the current instance to the next node in the stream. */
PHP_METHOD(XMLReader, read)
{
	zval *id;
	int retval;
	xmlreader_object *intern;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;
	intern = Z_XMLREADER_P(id);
	if (!intern->ptr) {
		zend_throw_error(NULL, "Data must be loaded before reading");
		RETURN_THROWS();
	}

	retval = xmlTextReaderRead(intern->ptr);
	if (retval == -1) {
		RETURN_FALSE;
	} else {
		RETURN_BOOL(retval);
	}
}
/* }}} */

/* {{{ Moves the position of the current instance to the next node in the stream. */
PHP_METHOD(XMLReader, next)
{
	zval *id;
	int retval;
	size_t name_len=0;
	xmlreader_object *intern;
	char *name = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|s!", &name, &name_len) == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;
	intern = Z_XMLREADER_P(id);
	if (intern->ptr) {
		retval = xmlTextReaderNext(intern->ptr);
		while (name != NULL && retval == 1) {
			if (xmlStrEqual(xmlTextReaderConstLocalName(intern->ptr), (xmlChar *)name)) {
				RETURN_TRUE;
			}
			retval = xmlTextReaderNext(intern->ptr);
		}
		if (retval == -1) {
			RETURN_FALSE;
		} else {
			RETURN_BOOL(retval);
		}
	}

	zend_throw_error(NULL, "Data must be loaded before reading");
}
/* }}} */

static bool xmlreader_valid_encoding(const char *encoding)
{
	if (!encoding) {
		return true;
	}

	/* Normally we could use xmlTextReaderConstEncoding() afterwards but libxml2 < 2.12.0 has a bug of course
	 * where it returns NULL for some valid encodings instead. */
	xmlCharEncodingHandlerPtr handler = xmlFindCharEncodingHandler(encoding);
	if (!handler) {
		return false;
	}
	xmlCharEncCloseFunc(handler);
	return true;
}

/* {{{ Sets the URI that the XMLReader will parse. */
static void xml_reader_from_uri(INTERNAL_FUNCTION_PARAMETERS, zend_class_entry *instance_ce, bool use_exceptions)
{
	zval *id;
	size_t source_len = 0, encoding_len = 0;
	zend_long options = 0;
	xmlreader_object *intern = NULL;
	char *source, *valid_file = NULL;
	char *encoding = NULL;
	char resolved_path[MAXPATHLEN + 1];
	xmlTextReaderPtr reader = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "p|p!l", &source, &source_len, &encoding, &encoding_len, &options) == FAILURE) {
		RETURN_THROWS();
	}

	id = getThis();
	if (id != NULL) {
		ZEND_ASSERT(instanceof_function(Z_OBJCE_P(id), xmlreader_class_entry));
		intern = Z_XMLREADER_P(id);
		xmlreader_free_resources(intern);
	}

	if (!source_len) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	if (!xmlreader_valid_encoding(encoding)) {
		zend_argument_value_error(2, "must be a valid character encoding");
		RETURN_THROWS();
	}

	valid_file = _xmlreader_get_valid_file_path(source, resolved_path, MAXPATHLEN );

	if (valid_file) {
		PHP_LIBXML_SANITIZE_GLOBALS(reader_for_file);
		reader = xmlReaderForFile(valid_file, encoding, options);
		PHP_LIBXML_RESTORE_GLOBALS(reader_for_file);
	}

	if (reader == NULL) {
		if (use_exceptions) {
			zend_throw_error(NULL, "Unable to open source data");
			RETURN_THROWS();
		} else {
			php_error_docref(NULL, E_WARNING, "Unable to open source data");
			RETURN_FALSE;
		}
	}

	if (id == NULL) {
		if (UNEXPECTED(object_init_with_constructor(return_value, instance_ce, 0, NULL, NULL) != SUCCESS)) {
			xmlFreeTextReader(reader);
			RETURN_THROWS();
		}
		intern = Z_XMLREADER_P(return_value);
		intern->ptr = reader;
		return;
	}

	intern->ptr = reader;

	RETURN_TRUE;

}

PHP_METHOD(XMLReader, open)
{
	xml_reader_from_uri(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlreader_class_entry, false);
}

PHP_METHOD(XMLReader, fromUri)
{
	xml_reader_from_uri(INTERNAL_FUNCTION_PARAM_PASSTHRU, Z_CE_P(ZEND_THIS), true);
}
/* }}} */

static int xml_reader_stream_read(void *context, char *buffer, int len)
{
	zend_resource *resource = context;
	if (EXPECTED(resource->ptr)) {
		php_stream *stream = resource->ptr;
		return php_stream_read(stream, buffer, len);
	}
	return -1;
}

static int xml_reader_stream_close(void *context)
{
	zend_resource *resource = context;
	/* Don't close it as others may still use it! We don't own the resource!
	 * Just delete our reference (and clean up if we're the last one). */
	zend_list_delete(resource);
	return 0;
}

PHP_METHOD(XMLReader, fromStream)
{
	zval *stream_zv;
	php_stream *stream;
	char *document_uri = NULL;
	char *encoding_name = NULL;
	size_t document_uri_len, encoding_name_len;
	zend_long flags = 0;

	ZEND_PARSE_PARAMETERS_START(1, 4)
		Z_PARAM_RESOURCE(stream_zv);
		Z_PARAM_OPTIONAL
		Z_PARAM_PATH_OR_NULL(encoding_name, encoding_name_len)
		Z_PARAM_LONG(flags)
		Z_PARAM_PATH_OR_NULL(document_uri, document_uri_len)
	ZEND_PARSE_PARAMETERS_END();

	php_stream_from_res(stream, Z_RES_P(stream_zv));

	if (!xmlreader_valid_encoding(encoding_name)) {
		zend_argument_value_error(2, "must be a valid character encoding");
		RETURN_THROWS();
	}

	PHP_LIBXML_SANITIZE_GLOBALS(reader_for_stream);
	xmlTextReaderPtr reader = xmlReaderForIO(
		xml_reader_stream_read,
		xml_reader_stream_close,
		stream->res,
		document_uri,
		encoding_name,
		flags
	);
	PHP_LIBXML_RESTORE_GLOBALS(reader_for_stream);

	if (UNEXPECTED(reader == NULL)) {
		zend_throw_error(NULL, "Could not construct libxml reader");
		RETURN_THROWS();
	}

	/* When the reader is closed (even in error paths) the reference is destroyed. */
	Z_ADDREF_P(stream_zv);

	if (object_init_with_constructor(return_value, Z_CE_P(ZEND_THIS), 0, NULL, NULL) == SUCCESS) {
		xmlreader_object *intern = Z_XMLREADER_P(return_value);
		intern->ptr = reader;
	} else {
		xmlFreeTextReader(reader);
	}
}

/* Not Yet Implemented in libxml - functions exist just not coded
PHP_METHOD(XMLReader, resetState)
{

}
*/

/* {{{ Reads the contents of the current node, including child nodes and markup. */
PHP_METHOD(XMLReader, readInnerXml)
{
	php_xmlreader_no_arg_string(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderReadInnerXml);
}
/* }}} */

/* {{{ Reads the contents of the current node, including child nodes and markup. */
PHP_METHOD(XMLReader, readOuterXml)
{
	php_xmlreader_no_arg_string(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderReadOuterXml);
}
/* }}} */

/* {{{ Reads the contents of an element or a text node as a string. */
PHP_METHOD(XMLReader, readString)
{
	php_xmlreader_no_arg_string(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlTextReaderReadString);
}
/* }}} */

/* {{{ Use W3C XSD schema to validate the document as it is processed. Activation is only possible before the first Read(). */
PHP_METHOD(XMLReader, setSchema)
{
#ifdef LIBXML_SCHEMAS_ENABLED
	zval *id;
	size_t source_len = 0;
	int retval = -1;
	xmlreader_object *intern;
	char *source;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "p!", &source, &source_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (source != NULL && !source_len) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (intern && intern->ptr) {
		PHP_LIBXML_SANITIZE_GLOBALS(schema);
		retval = xmlTextReaderSchemaValidate(intern->ptr, source);
		PHP_LIBXML_RESTORE_GLOBALS(schema);

		if (retval == 0) {
			RETURN_TRUE;
		} else {
			php_error_docref(NULL, E_WARNING, "Schema contains errors");
			RETURN_FALSE;
		}
	} else {
		zend_throw_error(NULL, "Schema must be set prior to reading");
		RETURN_THROWS();
	}
#else
	php_error_docref(NULL, E_WARNING, "No schema support built into libxml");
	RETURN_FALSE;
#endif
}
/* }}} */

/* {{{ Sets parser property (one of the parser option constants).
Properties must be set after open() or XML() and before the first read() is called */
PHP_METHOD(XMLReader, setParserProperty)
{
	zval *id;
	zend_long property;
	bool value;
	xmlreader_object *intern;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "lb", &property, &value) == FAILURE) {
		RETURN_THROWS();
	}

	id = ZEND_THIS;

	intern = Z_XMLREADER_P(id);
	if (!intern || !intern->ptr) {
		zend_throw_error(NULL, "Cannot access parser properties before loading data");
		RETURN_THROWS();
	}

	int retval = xmlTextReaderSetParserProp(intern->ptr,property, value);
	if (retval == -1) {
		zend_argument_value_error(1, "must be a valid parser property");
		RETURN_THROWS();
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Sets the string that the XMLReader will parse. */
PHP_METHOD(XMLReader, setRelaxNGSchema)
{
	php_xmlreader_set_relaxng_schema(INTERNAL_FUNCTION_PARAM_PASSTHRU, XMLREADER_LOAD_FILE);
}
/* }}} */

/* {{{ Sets the string that the XMLReader will parse. */
PHP_METHOD(XMLReader, setRelaxNGSchemaSource)
{
	php_xmlreader_set_relaxng_schema(INTERNAL_FUNCTION_PARAM_PASSTHRU, XMLREADER_LOAD_STRING);
}
/* }}} */

/* TODO
XMLPUBFUN int XMLCALL
		    xmlTextReaderSetSchema	(xmlTextReaderPtr reader,
		    				 xmlSchemaPtr schema);
*/

/* {{{ Sets the string that the XMLReader will parse. */
static void xml_reader_from_string(INTERNAL_FUNCTION_PARAMETERS, zend_class_entry *instance_ce, bool throw)
{
	zval *id;
	size_t source_len = 0, encoding_len = 0;
	zend_long options = 0;
	xmlreader_object *intern = NULL;
	char *source, *uri = NULL, *encoding = NULL;
	int resolved_path_len, ret = 0;
	char *directory=NULL, resolved_path[MAXPATHLEN + 1];
	xmlParserInputBufferPtr inputbfr;
	xmlTextReaderPtr reader;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|p!l", &source, &source_len, &encoding, &encoding_len, &options) == FAILURE) {
		RETURN_THROWS();
	}

	id = getThis();
	if (id != NULL) {
		ZEND_ASSERT(instanceof_function(Z_OBJCE_P(id), xmlreader_class_entry));
		intern = Z_XMLREADER_P(id);
		xmlreader_free_resources(intern);
	}

	if (!source_len) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	if (!xmlreader_valid_encoding(encoding)) {
		zend_argument_value_error(2, "must be a valid character encoding");
		RETURN_THROWS();
	}

	inputbfr = xmlParserInputBufferCreateMem(source, source_len, XML_CHAR_ENCODING_NONE);

	if (inputbfr != NULL) {
/* Get the URI of the current script so that we can set the base directory in libxml */
#ifdef HAVE_GETCWD
		directory = VCWD_GETCWD(resolved_path, MAXPATHLEN);
#elif defined(HAVE_GETWD)
		directory = VCWD_GETWD(resolved_path);
#endif
		if (directory) {
			resolved_path_len = strlen(resolved_path);
			if (resolved_path[resolved_path_len - 1] != DEFAULT_SLASH) {
				resolved_path[resolved_path_len] = DEFAULT_SLASH;
				resolved_path[++resolved_path_len] = '\0';
			}
			uri = (char *) xmlCanonicPath((const xmlChar *) resolved_path);
		}
		PHP_LIBXML_SANITIZE_GLOBALS(text_reader);
		reader = xmlNewTextReader(inputbfr, uri);

		if (reader != NULL) {
			ret = xmlTextReaderSetup(reader, NULL, uri, encoding, options);
			if (ret == 0) {
				if (id == NULL) {
					if (UNEXPECTED(object_init_with_constructor(return_value, instance_ce, 0, NULL, NULL) != SUCCESS)) {
						xmlFree(uri);
						xmlFreeParserInputBuffer(inputbfr);
						xmlFreeTextReader(reader);
						RETURN_THROWS();
					}
					intern = Z_XMLREADER_P(return_value);
				} else {
					RETVAL_TRUE;
				}
				intern->input = inputbfr;
				intern->ptr = reader;

				if (uri) {
					xmlFree(uri);
				}

				PHP_LIBXML_RESTORE_GLOBALS(text_reader);
				return;
			}
		}
		PHP_LIBXML_RESTORE_GLOBALS(text_reader);
	}

	if (uri) {
		xmlFree(uri);
	}

	if (inputbfr) {
		xmlFreeParserInputBuffer(inputbfr);
	}

	if (throw) {
		zend_throw_error(NULL, "Unable to load source data");
		RETURN_THROWS();
	} else {
		php_error_docref(NULL, E_WARNING, "Unable to load source data");
		RETURN_FALSE;
	}
}
/* }}} */

PHP_METHOD(XMLReader, XML)
{
	xml_reader_from_string(INTERNAL_FUNCTION_PARAM_PASSTHRU, xmlreader_class_entry, false);
}

PHP_METHOD(XMLReader, fromString)
{
	xml_reader_from_string(INTERNAL_FUNCTION_PARAM_PASSTHRU, Z_CE_P(ZEND_THIS), true);
}

/* {{{ Moves the position of the current instance to the next node in the stream. */
PHP_METHOD(XMLReader, expand)
{
#ifdef HAVE_DOM
	zval *id, *basenode = NULL;
	xmlreader_object *intern;
	xmlNode *node, *nodec;
	xmlDocPtr docp = NULL;
	php_libxml_node_object *domobj = NULL;

	id = ZEND_THIS;
	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|O!", &basenode, dom_node_class_entry) == FAILURE) {
		RETURN_THROWS();
	}

	if (basenode != NULL) {
		/* Note: cannot use NODE_GET_OBJ here because of the wrong return type */
		domobj = Z_LIBXML_NODE_P(basenode);
		if (UNEXPECTED(domobj->node == NULL)) {
			php_error_docref(NULL, E_WARNING, "Couldn't fetch %s", ZSTR_VAL(Z_OBJCE_P(basenode)->name));
			RETURN_FALSE;
		}
		node = domobj->node->node;
		docp = node->doc;
	}

	intern = Z_XMLREADER_P(id);

	if (intern->ptr) {
		node = xmlTextReaderExpand(intern->ptr);

		if (node == NULL) {
			php_error_docref(NULL, E_WARNING, "An Error Occurred while expanding");
			RETURN_FALSE;
		} else {
			nodec = xmlDocCopyNode(node, docp, 1);
			if (nodec == NULL) {
				php_error_docref(NULL, E_NOTICE, "Cannot expand this node type");
				RETURN_FALSE;
			} else {
				DOM_RET_OBJ(nodec, (dom_object *)domobj);
			}
		}
	} else {
		zend_throw_error(NULL, "Data must be loaded before expanding");
		RETURN_THROWS();
	}
#else
	zval *dummy;
	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|z!", &dummy) == FAILURE) {
		RETURN_THROWS();
	}

	zend_throw_error(NULL, "XMLReader::expand() requires the DOM extension to be enabled");
	RETURN_THROWS();
#endif
}
/* }}} */

static zend_result (*prev_zend_post_startup_cb)(void);
static zend_result xmlreader_fixup_temporaries(void) {
	if (ZEND_OBSERVER_ENABLED) {
		++xmlreader_open_fn.T;
		++xmlreader_xml_fn.T;
	}
#ifndef ZTS
	ZEND_MAP_PTR(xmlreader_open_fn.run_time_cache) = ZEND_MAP_PTR(((zend_internal_function *)zend_hash_str_find_ptr(&xmlreader_class_entry->function_table, "open", sizeof("open")-1))->run_time_cache);
	ZEND_MAP_PTR(xmlreader_xml_fn.run_time_cache) = ZEND_MAP_PTR(((zend_internal_function *)zend_hash_str_find_ptr(&xmlreader_class_entry->function_table, "xml", sizeof("xml")-1))->run_time_cache);
#endif
	if (prev_zend_post_startup_cb) {
		return prev_zend_post_startup_cb();
	}
	return SUCCESS;
}

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(xmlreader)
{

	memcpy(&xmlreader_object_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	xmlreader_object_handlers.offset = XtOffsetOf(xmlreader_object, std);
	xmlreader_object_handlers.free_obj = xmlreader_objects_free_storage;
	xmlreader_object_handlers.has_property = xmlreader_has_property;
	xmlreader_object_handlers.read_property = xmlreader_read_property;
	xmlreader_object_handlers.write_property = xmlreader_write_property;
	xmlreader_object_handlers.unset_property = xmlreader_unset_property;
	xmlreader_object_handlers.get_property_ptr_ptr = xmlreader_get_property_ptr_ptr;
	xmlreader_object_handlers.get_method = xmlreader_get_method;
	xmlreader_object_handlers.clone_obj = NULL;
	xmlreader_object_handlers.get_debug_info = xmlreader_get_debug_info;

	xmlreader_class_entry = register_class_XMLReader();
	xmlreader_class_entry->create_object = xmlreader_objects_new;
	xmlreader_class_entry->default_object_handlers = &xmlreader_object_handlers;

	memcpy(&xmlreader_open_fn, zend_hash_str_find_ptr(&xmlreader_class_entry->function_table, "open", sizeof("open")-1), sizeof(zend_internal_function));
	xmlreader_open_fn.fn_flags &= ~ZEND_ACC_STATIC;
	memcpy(&xmlreader_xml_fn, zend_hash_str_find_ptr(&xmlreader_class_entry->function_table, "xml", sizeof("xml")-1), sizeof(zend_internal_function));
	xmlreader_xml_fn.fn_flags &= ~ZEND_ACC_STATIC;

	prev_zend_post_startup_cb = zend_post_startup_cb;
	zend_post_startup_cb = xmlreader_fixup_temporaries;

	/* Note: update the size upon adding properties. */
	zend_hash_init(&xmlreader_prop_handlers, 14, NULL, NULL, true);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "attributeCount", xmlTextReaderAttributeCount, NULL, IS_LONG);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "baseURI", NULL, xmlTextReaderConstBaseUri, IS_STRING);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "depth", xmlTextReaderDepth, NULL, IS_LONG);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "hasAttributes", xmlTextReaderHasAttributes, NULL, _IS_BOOL);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "hasValue", xmlTextReaderHasValue, NULL, _IS_BOOL);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "isDefault", xmlTextReaderIsDefault, NULL, _IS_BOOL);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "isEmptyElement", xmlTextReaderIsEmptyElement, NULL, _IS_BOOL);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "localName", NULL, xmlTextReaderConstLocalName, IS_STRING);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "name", NULL, xmlTextReaderConstName, IS_STRING);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "namespaceURI", NULL, xmlTextReaderConstNamespaceUri, IS_STRING);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "nodeType", xmlTextReaderNodeType, NULL, IS_LONG);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "prefix", NULL, xmlTextReaderConstPrefix, IS_STRING);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "value", NULL, xmlTextReaderConstValue, IS_STRING);
	XMLREADER_REGISTER_PROP_HANDLER(&xmlreader_prop_handlers, "xmlLang", NULL, xmlTextReaderConstXmlLang, IS_STRING);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(xmlreader)
{
	zend_hash_destroy(&xmlreader_prop_handlers);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(xmlreader)
{
	php_info_print_table_start();
	{
		php_info_print_table_row(2, "XMLReader", "enabled");
	}
	php_info_print_table_end();
}
/* }}} */
