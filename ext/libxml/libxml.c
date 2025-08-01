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
   | Authors: Shane Caraveo <shane@php.net>                               |
   |          Wez Furlong <wez@thebrainroom.com>                          |
   +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "SAPI.h"

#include "zend_attributes.h"
#include "zend_variables.h"
#include "ext/standard/info.h"
#include "ext/standard/file.h"

#ifdef HAVE_LIBXML

#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlsave.h>
#include <libxml/xmlerror.h>
#include <libxml/entities.h>
#ifdef LIBXML_SCHEMAS_ENABLED
#include <libxml/relaxng.h>
#include <libxml/xmlschemas.h>
#endif

#include "php_libxml.h"

#define PHP_LIBXML_LOADED_VERSION ((char *)xmlParserVersion)

#include "libxml_arginfo.h"

/* a true global for initialization */
static int _php_libxml_initialized = 0;
static int _php_libxml_per_request_initialization = 1;
static xmlExternalEntityLoader _php_libxml_default_entity_loader;

typedef struct _php_libxml_func_handler {
	php_libxml_export_node export_func;
} php_libxml_func_handler;

static HashTable php_libxml_exports;

static ZEND_DECLARE_MODULE_GLOBALS(libxml)
static PHP_GINIT_FUNCTION(libxml);

static zend_class_entry *libxmlerror_class_entry;

/* {{{ dynamically loadable module stuff */
#ifdef COMPILE_DL_LIBXML
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(libxml)
#endif /* COMPILE_DL_LIBXML */
/* }}} */

/* {{{ function prototypes */
static PHP_MINIT_FUNCTION(libxml);
static PHP_RINIT_FUNCTION(libxml);
static PHP_RSHUTDOWN_FUNCTION(libxml);
static PHP_MSHUTDOWN_FUNCTION(libxml);
static PHP_MINFO_FUNCTION(libxml);
static zend_result php_libxml_post_deactivate(void);

static zend_string *php_libxml_default_dump_node_to_str(xmlDocPtr doc, xmlNodePtr node, bool format, const char *encoding);
static zend_string *php_libxml_default_dump_doc_to_str(xmlDocPtr doc, int options, const char *encoding);
static zend_long php_libxml_dump_node_to_file(const char *filename, xmlDocPtr doc, xmlNodePtr node, bool format, const char *encoding);
static zend_long php_libxml_default_dump_doc_to_file(const char *filename, xmlDocPtr doc, bool format, const char *encoding);

/* }}} */

zend_module_entry libxml_module_entry = {
	STANDARD_MODULE_HEADER,
	"libxml",                /* extension name */
	ext_functions,           /* extension function list */
	PHP_MINIT(libxml),       /* extension-wide startup function */
	PHP_MSHUTDOWN(libxml),   /* extension-wide shutdown function */
	PHP_RINIT(libxml),       /* per-request startup function */
	PHP_RSHUTDOWN(libxml),   /* per-request shutdown function */
	PHP_MINFO(libxml),       /* information function */
	PHP_LIBXML_VERSION,
	PHP_MODULE_GLOBALS(libxml), /* globals descriptor */
	PHP_GINIT(libxml),          /* globals ctor */
	NULL,                       /* globals dtor */
	php_libxml_post_deactivate, /* post deactivate */
	STANDARD_MODULE_PROPERTIES_EX
};

/* }}} */

static const php_libxml_document_handlers php_libxml_default_document_handlers = {
	.dump_node_to_str = php_libxml_default_dump_node_to_str,
	.dump_doc_to_str = php_libxml_default_dump_doc_to_str,
	.dump_node_to_file = php_libxml_dump_node_to_file,
	.dump_doc_to_file = php_libxml_default_dump_doc_to_file,
};

static void php_libxml_set_old_ns_list(xmlDocPtr doc, xmlNsPtr first, xmlNsPtr last)
{
	if (UNEXPECTED(doc == NULL)) {
		return;
	}

	ZEND_ASSERT(last->next == NULL);

	/* Note: we'll use a prepend strategy instead of append to
	 * make sure we don't lose performance when the list is long.
	 * As libxml2 could assume the xml node is the first one, we'll place our
	 * new entries after the first one. */

	if (UNEXPECTED(doc->oldNs == NULL)) {
		doc->oldNs = (xmlNsPtr) xmlMalloc(sizeof(xmlNs));
		if (doc->oldNs == NULL) {
			return;
		}
		memset(doc->oldNs, 0, sizeof(xmlNs));
		doc->oldNs->type = XML_LOCAL_NAMESPACE;
		doc->oldNs->href = xmlStrdup(XML_XML_NAMESPACE);
		doc->oldNs->prefix = xmlStrdup((const xmlChar *)"xml");
	} else {
		last->next = doc->oldNs->next;
	}
	doc->oldNs->next = first;
}

PHP_LIBXML_API void php_libxml_set_old_ns(xmlDocPtr doc, xmlNsPtr ns)
{
	php_libxml_set_old_ns_list(doc, ns, ns);
}

/* Function pointer typedef changed in 2.9.8, see https://github.com/GNOME/libxml2/commit/e03f0a199a67017b2f8052354cf732b2b4cae787 */
#if LIBXML_VERSION >= 20908
static void php_libxml_unlink_entity(void *data, void *table, const xmlChar *name)
#else
static void php_libxml_unlink_entity(void *data, void *table, xmlChar *name)
#endif
{
	xmlEntityPtr entity = data;
	if (entity->_private != NULL) {
		xmlHashRemoveEntry(table, name, NULL);
	}
}

/* {{{ internal functions for interoperability */
static void php_libxml_unregister_node(xmlNodePtr nodep)
{
	php_libxml_node_ptr *nodeptr = nodep->_private;

	if (nodeptr != NULL) {
		php_libxml_node_object *wrapper = nodeptr->_private;
		if (wrapper) {
			php_libxml_decrement_node_ptr(wrapper);
			php_libxml_decrement_doc_ref(wrapper);
		} else {
			if (nodep->type != XML_DOCUMENT_NODE) {
				nodep->_private = NULL;
			}
			nodeptr->node = NULL;
		}
	}
}

/* Workaround for libxml2 peculiarity */
static void php_libxml_unlink_entity_decl(xmlEntityPtr entity)
{
	xmlDtdPtr dtd = entity->parent;
	if (dtd != NULL) {
		if (xmlHashLookup(dtd->entities, entity->name) == entity) {
			xmlHashRemoveEntry(dtd->entities, entity->name, NULL);
		}
		if (xmlHashLookup(dtd->pentities, entity->name) == entity) {
			xmlHashRemoveEntry(dtd->pentities, entity->name, NULL);
		}
	}
}

static void php_libxml_node_free(xmlNodePtr node)
{
	if (node->_private != NULL) {
		((php_libxml_node_ptr *) node->_private)->node = NULL;
	}
	switch (node->type) {
		case XML_ATTRIBUTE_NODE:
			xmlFreeProp((xmlAttrPtr) node);
			break;
		/* libxml2 has a peculiarity where if you unlink an entity it'll only unlink it from the dtd if the
		 * dtd is attached to the document. This works around the issue by inspecting the parent directly. */
		case XML_ENTITY_DECL: {
			xmlEntityPtr entity = (xmlEntityPtr) node;
			if (entity->etype != XML_INTERNAL_PREDEFINED_ENTITY) {
				php_libxml_unlink_entity_decl(entity);
#if LIBXML_VERSION >= 21200
				xmlFreeEntity(entity);
#else
				if (entity->children != NULL && entity->owner && entity == (xmlEntityPtr) entity->children->parent) {
					xmlFreeNodeList(entity->children);
				}
				xmlDictPtr dict = entity->doc != NULL ? entity->doc->dict : NULL;
				if (dict == NULL || !xmlDictOwns(dict, entity->name)) {
					xmlFree((xmlChar *) entity->name);
				}
				if (dict == NULL || !xmlDictOwns(dict, entity->ExternalID)) {
					xmlFree((xmlChar *) entity->ExternalID);
				}
				if (dict == NULL || !xmlDictOwns(dict, entity->SystemID)) {
					xmlFree((xmlChar *) entity->SystemID);
				}
				if (dict == NULL || !xmlDictOwns(dict, entity->URI)) {
					xmlFree((xmlChar *) entity->URI);
				}
				if (dict == NULL || !xmlDictOwns(dict, entity->content)) {
					xmlFree(entity->content);
				}
				if (dict == NULL || !xmlDictOwns(dict, entity->orig)) {
					xmlFree(entity->orig);
				}
				xmlFree(entity);
#endif
			}
			break;
		}
		case XML_NOTATION_NODE: {
			/* See create_notation(), these aren't regular XML_NOTATION_NODE, but entities in disguise... */
			xmlEntityPtr entity = (xmlEntityPtr) node;
			if (node->name != NULL) {
				xmlFree((char *) node->name);
			}
			if (entity->ExternalID != NULL) {
				xmlFree((char *) entity->ExternalID);
			}
			if (entity->SystemID != NULL) {
				xmlFree((char *) entity->SystemID);
			}
			xmlFree(node);
			break;
		}
		case XML_ELEMENT_DECL:
		case XML_ATTRIBUTE_DECL:
			break;
		case XML_NAMESPACE_DECL:
			if (node->ns) {
				xmlFreeNs(node->ns);
				node->ns = NULL;
			}
			node->type = XML_ELEMENT_NODE;
			xmlFreeNode(node);
			break;
		case XML_DTD_NODE: {
			xmlDtdPtr dtd = (xmlDtdPtr) node;
			if (dtd->_private == NULL) {
				/* There's no userland reference to the dtd,
				 * but there might be entities referenced from userland. Unlink those. */
				xmlHashScan(dtd->entities, php_libxml_unlink_entity, dtd->entities);
				xmlHashScan(dtd->pentities, php_libxml_unlink_entity, dtd->pentities);
				/* No unlinking of notations, see remark above at case XML_NOTATION_NODE. */
			}
			xmlFreeDtd(dtd);
			break;
		}
		case XML_ELEMENT_NODE: {
			if (node->ns && (((uintptr_t) node->ns->_private) & 1) == LIBXML_NS_TAG_HOOK) {
				/* Special destruction routine hook should be called because it belongs to a "special" namespace. */
				php_libxml_private_data_header *header = (php_libxml_private_data_header *) (((uintptr_t) node->ns->_private) & ~1);
				header->ns_hook(header, node);
			}
			if (node->nsDef && node->doc) {
				/* Make the namespace declaration survive the destruction of the holding element.
				 * This prevents a use-after-free on the namespace declaration.
				 *
				 * The main problem is that libxml2 doesn't have a reference count on the namespace declaration.
				 * We don't actually need to save the namespace declaration if we know the subtree it belongs to
				 * has no references from userland. However, we can't know that without traversing the whole subtree
				 * (=> slow), or without adding some subtree metadata (=> also slow).
				 * So we have to assume we need to save everything.
				 *
				 * However, namespace declarations are quite rare in comparison to other node types.
				 * Most node types are either elements, text or attributes.
				 * And you only need one namespace declaration per namespace (in principle).
				 * So I expect the number of namespace declarations to be low for an average XML document.
				 *
				 * In the worst possible case we have to save all namespace declarations when we for example remove
				 * the whole document. But given the above reasoning this likely won't be a lot of declarations even
				 * in the worst case.
				 * A single declaration only takes about 48 bytes of memory, and I don't expect the worst case to occur
				 * very often (why would you remove the whole document?).
				 */
				xmlNsPtr ns = node->nsDef;
				xmlNsPtr last = ns;
				while (last->next) {
					last = last->next;
				}
				php_libxml_set_old_ns_list(node->doc, ns, last);
				node->nsDef = NULL;
			}
			xmlFreeNode(node);
			break;
		}
		default:
			xmlFreeNode(node);
			break;
	}
}

PHP_LIBXML_API void php_libxml_node_free_list(xmlNodePtr node)
{
	xmlNodePtr curnode;

	if (node != NULL) {
		curnode = node;
		while (curnode != NULL) {
			/* If the _private field is set, there's still a userland reference somewhere. We'll delay freeing in this case. */
			if (curnode->_private) {
				xmlNodePtr next = curnode->next;
				/* Must unlink such that freeing of the parent doesn't free this child. */
				xmlUnlinkNode(curnode);
				if (curnode->type == XML_ELEMENT_NODE) {
					/* This ensures that namespace references in this subtree are defined within this subtree,
					 * otherwise a use-after-free would be possible when the original namespace holder gets freed. */
					php_libxml_node_ptr *ptr = curnode->_private;

					/* Checking in case it runs out of reference */
					if (ptr->_private) {
						php_libxml_node_object *obj = ptr->_private;
						if (!obj->document || obj->document->class_type < PHP_LIBXML_CLASS_MODERN) {
							xmlReconciliateNs(curnode->doc, curnode);
						}
					}
				}
				/* Skip freeing */
				curnode = next;
				continue;
			}

			node = curnode;
			switch (node->type) {
				/* Skip property freeing for the following types */
				case XML_ENTITY_REF_NODE:
				case XML_NOTATION_NODE:
					break;
				case XML_ENTITY_DECL:
					php_libxml_unlink_entity_decl((xmlEntityPtr) node);
					break;
				case XML_ATTRIBUTE_NODE:
					if ((node->doc != NULL) && (((xmlAttrPtr) node)->atype == XML_ATTRIBUTE_ID)) {
						xmlRemoveID(node->doc, (xmlAttrPtr) node);
					}
					ZEND_FALLTHROUGH;
				case XML_ATTRIBUTE_DECL:
				case XML_DTD_NODE:
				case XML_DOCUMENT_TYPE_NODE:
				case XML_NAMESPACE_DECL:
				case XML_TEXT_NODE:
					php_libxml_node_free_list(node->children);
					break;
				default:
					php_libxml_node_free_list(node->children);
					php_libxml_node_free_list((xmlNodePtr) node->properties);
			}

			curnode = node->next;
			xmlUnlinkNode(node);
			php_libxml_unregister_node(node);
			php_libxml_node_free(node);
		}
	}
}

/* }}} */

/* {{{ startup, shutdown and info functions */
static PHP_GINIT_FUNCTION(libxml)
{
#if defined(COMPILE_DL_LIBXML) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	ZVAL_UNDEF(&libxml_globals->stream_context);
	libxml_globals->error_buffer.s = NULL;
	libxml_globals->error_list = NULL;
	libxml_globals->entity_loader_callback = empty_fcall_info_cache;
}

PHP_LIBXML_API php_stream_context *php_libxml_get_stream_context(void)
{
	return php_stream_context_from_zval(Z_ISUNDEF(LIBXML(stream_context)) ? NULL : &LIBXML(stream_context), false);
}

/* Channel libxml file io layer through the PHP streams subsystem.
 * This allows use of ftps:// and https:// urls */

static void *php_libxml_streams_IO_open_wrapper(const char *filename, const char *mode, const int read_only)
{
	php_stream_statbuf ssbuf;
	char *resolved_path;
	const char *path_to_open = NULL;
	bool isescaped = false;

	if (strstr(filename, "%00")) {
		php_error_docref(NULL, E_WARNING, "URI must not contain percent-encoded NUL bytes");
		return NULL;
	}

	xmlURI *uri = xmlParseURI(filename);
	if (uri && (uri->scheme == NULL ||
			(xmlStrncmp(BAD_CAST uri->scheme, BAD_CAST "file", 4) == 0))) {
		resolved_path = xmlURIUnescapeString(filename, 0, NULL);
		isescaped = 1;
#if LIBXML_VERSION >= 20902 && LIBXML_VERSION < 21300 && defined(PHP_WIN32)
		/* Libxml 2.9.2 prefixes local paths with file:/ instead of file://,
			thus the php stream wrapper will fail on a valid case. For this
			reason the prefix is rather better cut off.
			As of libxml 2.13.0 this issue is resolved. */
		{
			size_t pre_len = sizeof("file:/") - 1;

			if (strncasecmp(resolved_path, "file:/", pre_len) == 0
				&& '/' != resolved_path[pre_len]) {
				xmlChar *tmp = xmlStrdup(resolved_path + pre_len);
				xmlFree(resolved_path);
				resolved_path = tmp;
			}
		}
#endif
	} else {
		resolved_path = (char *)filename;
	}

	if (uri) {
		xmlFreeURI(uri);
	}

	if (resolved_path == NULL) {
		return NULL;
	}

	/* logic copied from _php_stream_stat, but we only want to fail
	   if the wrapper supports stat, otherwise, figure it out from
	   the open.  This logic is only to support hiding warnings
	   that the streams layer puts out at times, but for libxml we
	   may try to open files that don't exist, but it is not a failure
	   in xml processing (eg. DTD files)  */
	php_stream_wrapper *wrapper = php_stream_locate_url_wrapper(resolved_path, &path_to_open, 0);
	if (wrapper && read_only && wrapper->wops->url_stat) {
		if (wrapper->wops->url_stat(wrapper, path_to_open, PHP_STREAM_URL_STAT_QUIET, &ssbuf, NULL) == -1) {
			if (isescaped) {
				xmlFree(resolved_path);
			}
			return NULL;
		}
	}

	php_stream_context *context = php_libxml_get_stream_context();

	php_stream *ret_val = php_stream_open_wrapper_ex(path_to_open, mode, REPORT_ERRORS, NULL, context);
	if (ret_val) {
		/* Prevent from closing this by fclose() */
		ret_val->flags |= PHP_STREAM_FLAG_NO_FCLOSE;
	}
	if (isescaped) {
		xmlFree(resolved_path);
	}
	return ret_val;
}

static void *php_libxml_streams_IO_open_read_wrapper(const char *filename)
{
	return php_libxml_streams_IO_open_wrapper(filename, "rb", 1);
}

static void *php_libxml_streams_IO_open_write_wrapper(const char *filename)
{
	return php_libxml_streams_IO_open_wrapper(filename, "wb", 0);
}

static int php_libxml_streams_IO_read(void *context, char *buffer, int len)
{
	return php_stream_read((php_stream*)context, buffer, len);
}

static int php_libxml_streams_IO_write(void *context, const char *buffer, int len)
{
	return php_stream_write((php_stream*)context, buffer, len);
}

static int php_libxml_streams_IO_close(void *context)
{
	return php_stream_close((php_stream*)context);
}

static xmlParserInputBufferPtr
php_libxml_input_buffer_create_filename(const char *URI, xmlCharEncoding enc)
{
	xmlParserInputBufferPtr ret;
	void *context = NULL;

	if (LIBXML(entity_loader_disabled)) {
		return NULL;
	}

	if (URI == NULL)
		return(NULL);

	context = php_libxml_streams_IO_open_read_wrapper(URI);

	if (context == NULL) {
		return(NULL);
	}

	/* Check if there's been an external transport protocol with an encoding information */
	if (enc == XML_CHAR_ENCODING_NONE) {
		php_stream *s  = (php_stream *) context;
		zend_string *charset = php_libxml_sniff_charset_from_stream(s);
		if (charset != NULL) {
			enc = xmlParseCharEncoding(ZSTR_VAL(charset));
			if (enc <= XML_CHAR_ENCODING_NONE) {
				enc = XML_CHAR_ENCODING_NONE;
			}
			zend_string_release_ex(charset, false);
		}
	}

	/* Allocate the Input buffer front-end. */
	ret = xmlAllocParserInputBuffer(enc);
	if (ret != NULL) {
		ret->context = context;
		ret->readcallback = php_libxml_streams_IO_read;
		ret->closecallback = php_libxml_streams_IO_close;
	} else
		php_libxml_streams_IO_close(context);

	return(ret);
}

static xmlOutputBufferPtr
php_libxml_output_buffer_create_filename(const char *URI,
                              xmlCharEncodingHandlerPtr encoder,
                              int compression)
{
	ZEND_IGNORE_VALUE(compression);

	xmlOutputBufferPtr ret;
	xmlURIPtr puri;
	void *context = NULL;
	char *unescaped = NULL;

	if (URI == NULL)
		goto err;

	if (strstr(URI, "%00")) {
		php_error_docref(NULL, E_WARNING, "URI must not contain percent-encoded NUL bytes");
		goto err;
	}

	puri = xmlParseURI(URI);
	if (puri != NULL) {
		if (puri->scheme != NULL)
			unescaped = xmlURIUnescapeString(URI, 0, NULL);
		xmlFreeURI(puri);
	}

	if (unescaped != NULL) {
		context = php_libxml_streams_IO_open_write_wrapper(unescaped);
		xmlFree(unescaped);
	}

	/* try with a non-escaped URI this may be a strange filename */
	if (context == NULL) {
		context = php_libxml_streams_IO_open_write_wrapper(URI);
	}

	if (context == NULL) {
		goto err;
	}

	/* Allocate the Output buffer front-end. */
	ret = xmlAllocOutputBuffer(encoder);
	if (ret != NULL) {
		ret->context = context;
		ret->writecallback = php_libxml_streams_IO_write;
		ret->closecallback = php_libxml_streams_IO_close;
	}

	return(ret);

err:
	/* Similarly to __xmlOutputBufferCreateFilename we should also close the encoder on failure. */
	xmlCharEncCloseFunc(encoder);
	return NULL;
}

static void _php_libxml_free_error(void *ptr)
{
	/* This will free the libxml alloc'd memory */
	xmlResetError((xmlErrorPtr) ptr);
}

#if LIBXML_VERSION >= 21200
static void _php_list_set_error_structure(const xmlError *error, const char *msg, int line, int column)
#else
static void _php_list_set_error_structure(xmlError *error, const char *msg, int line, int column)
#endif
{
	xmlError error_copy;
	int ret;


	memset(&error_copy, 0, sizeof(xmlError));

	if (error) {
		ret = xmlCopyError(error, &error_copy);
	} else {
		error_copy.code = XML_ERR_INTERNAL_ERROR;
		error_copy.level = XML_ERR_ERROR;
		error_copy.line = line;
		error_copy.int2 = column;
		error_copy.message = (char*)xmlStrdup((const xmlChar*)msg);
		ret = 0;
	}

	if (ret == 0) {
		zend_llist_add_element(LIBXML(error_list), &error_copy);
	}
}

static void php_libxml_ctx_error_level(int level, void *ctx, const char *msg, int line)
{
	xmlParserCtxtPtr parser;

	parser = (xmlParserCtxtPtr) ctx;

	if (parser != NULL && parser->input != NULL) {
		if (parser->input->filename) {
			php_error_docref(NULL, level, "%s in %s, line: %d", msg, parser->input->filename, line);
		} else {
			php_error_docref(NULL, level, "%s in Entity, line: %d", msg, line);
		}
	} else {
		php_error_docref(NULL, E_WARNING, "%s", msg);
	}
}

void php_libxml_issue_error(int level, const char *msg)
{
	if (LIBXML(error_list)) {
		_php_list_set_error_structure(NULL, msg, 0, 0);
	} else {
		php_error_docref(NULL, level, "%s", msg);
	}
}

static void php_libxml_internal_error_handler_ex(php_libxml_error_level error_type, void *ctx, const char *msg, va_list ap, int line, int column)
{
	char *buf;
	bool output = false;

	size_t len = vspprintf(&buf, 0, msg, ap);
	size_t len_iter = len;

	/* remove any trailing \n */
	while (len_iter && buf[--len_iter] == '\n') {
		buf[len_iter] = '\0';
		output = true;
	}

	smart_str_appendl(&LIBXML(error_buffer), buf, len);

	efree(buf);

	if (output) {
		if (LIBXML(error_list)) {
			_php_list_set_error_structure(NULL, ZSTR_VAL(LIBXML(error_buffer).s), line, column);
		} else if (!EG(exception)) {
			/* Don't throw additional notices/warnings if an exception has already been thrown. */
			switch (error_type) {
				case PHP_LIBXML_CTX_ERROR:
					php_libxml_ctx_error_level(E_WARNING, ctx, ZSTR_VAL(LIBXML(error_buffer).s), line);
					break;
				case PHP_LIBXML_CTX_WARNING:
					php_libxml_ctx_error_level(E_NOTICE, ctx, ZSTR_VAL(LIBXML(error_buffer).s), line);
					break;
				default:
					php_error_docref(NULL, E_WARNING, "%s", ZSTR_VAL(LIBXML(error_buffer).s));
			}
		}
		smart_str_free(&LIBXML(error_buffer));
	}
}

PHP_LIBXML_API void php_libxml_error_handler_va(php_libxml_error_level error_type, void *ctx, const char *msg, va_list ap)
{
	int line = 0;
	int column = 0;
	xmlParserCtxtPtr parser = (xmlParserCtxtPtr) ctx;
	/* Context is not valid for PHP_LIBXML_ERROR, don't dereference it in that case */
	if (error_type != PHP_LIBXML_ERROR && parser != NULL && parser->input != NULL) {
		line = parser->input->line;
		column = parser->input->col;
	}
	php_libxml_internal_error_handler_ex(error_type, ctx, msg, ap, line, column);
}

static xmlParserInputPtr _php_libxml_external_entity_loader(const char *URL,
		const char *ID, xmlParserCtxtPtr context)
{
	xmlParserInputPtr	ret			= NULL;
	const char			*resource	= NULL;
	zval 				*ctxzv, retval;
	zval				params[3];

	/* no custom user-land callback set up; delegate to original loader */
	if (!ZEND_FCC_INITIALIZED(LIBXML(entity_loader_callback))) {
		return _php_libxml_default_entity_loader(URL, ID, context);
	}

	if (ID != NULL) {
		ZVAL_STRING(&params[0], ID);
	} else {
		ZVAL_NULL(&params[0]);
	}
	if (URL != NULL) {
		ZVAL_STRING(&params[1], URL);
	} else {
		ZVAL_NULL(&params[1]);
	}
	ctxzv = &params[2];
	array_init_size(ctxzv, 4);

#define ADD_NULL_OR_STRING_KEY(memb) \
	if (context->memb == NULL) { \
		add_assoc_null_ex(ctxzv, #memb, sizeof(#memb) - 1); \
	} else { \
		add_assoc_string_ex(ctxzv, #memb, sizeof(#memb) - 1, \
				(char *)context->memb); \
	}

	ADD_NULL_OR_STRING_KEY(directory)
	ADD_NULL_OR_STRING_KEY(intSubName)
	ADD_NULL_OR_STRING_KEY(extSubURI)
	ADD_NULL_OR_STRING_KEY(extSubSystem)

#undef ADD_NULL_OR_STRING_KEY

	zend_call_known_fcc(&LIBXML(entity_loader_callback), &retval, 3, params, /* named_params */ NULL);

	if (Z_ISUNDEF(retval)) {
		php_libxml_ctx_error(context,
				"Call to user entity loader callback '%s' has failed",
				ZSTR_VAL(LIBXML(entity_loader_callback).function_handler->common.function_name));
	} else {
		if (Z_TYPE(retval) == IS_STRING) {
is_string:
			resource = Z_STRVAL(retval);
		} else if (Z_TYPE(retval) == IS_RESOURCE) {
			php_stream *stream = (php_stream*)zend_fetch_resource2_ex(&retval, NULL, php_file_le_stream(), php_file_le_pstream());
			if (UNEXPECTED(stream == NULL)) {
				zval callable;
				zend_get_callable_zval_from_fcc(&LIBXML(entity_loader_callback), &callable);
				zend_string *callable_name = zend_get_callable_name(&callable);
				zend_string *func_name = get_active_function_or_method_name();
				zend_type_error(
					"%s(): The user entity loader callback \"%s\" has returned a resource, but it is not a stream",
					ZSTR_VAL(func_name), ZSTR_VAL(callable_name));
				zend_string_release(func_name);
				zend_string_release(callable_name);
				zval_ptr_dtor(&callable);
			} else {
				/* TODO: allow storing the encoding in the stream context? */
				xmlCharEncoding enc = XML_CHAR_ENCODING_NONE;
				xmlParserInputBufferPtr pib = xmlAllocParserInputBuffer(enc);
				if (pib == NULL) {
					php_libxml_ctx_error(context, "Could not allocate parser "
							"input buffer");
				} else {
					/* make stream not being closed when the zval is freed */
					GC_ADDREF(stream->res);
					pib->context = stream;
					pib->readcallback = php_libxml_streams_IO_read;
					pib->closecallback = php_libxml_streams_IO_close;

					ret = xmlNewIOInputStream(context, pib, enc);
					if (ret == NULL) {
						xmlFreeParserInputBuffer(pib);
					}
				}
			}
		} else if (Z_TYPE(retval) != IS_NULL) {
			/* retval not string nor resource nor null; convert to string */
			if (try_convert_to_string(&retval)) {
				goto is_string;
			}
		} /* else is null; don't try anything */
	}

	if (ret == NULL) {
		if (resource == NULL) {
			if (ID == NULL) {
				php_libxml_ctx_error(context,
						"Failed to load external entity because the resolver function returned null\n");
			} else {
				php_libxml_ctx_error(context,
						"Failed to load external entity \"%s\"\n", ID);
			}
		} else {
			/* we got the resource in the form of a string; open it */
			ret = xmlNewInputFromFile(context, resource);
		}
	}

	zval_ptr_dtor(&params[0]);
	zval_ptr_dtor(&params[1]);
	zval_ptr_dtor(&params[2]);
	zval_ptr_dtor(&retval);
	return ret;
}

static xmlParserInputPtr _php_libxml_pre_ext_ent_loader(const char *URL,
		const char *ID, xmlParserCtxtPtr context)
{

	/* Check whether we're running in a PHP context, since the entity loader
	 * we've defined is an application level (true global) setting.
	 * If we are, we also want to check whether we've finished activating
	 * the modules (RINIT phase). Using our external entity loader during a
	 * RINIT should not be problem per se (though during MINIT it is, because
	 * we don't even have a resource list by then), but then whether one
	 * extension would be using the custom external entity loader or not
	 * could depend on extension loading order
	 * (if _php_libxml_per_request_initialization */
	if (xmlGenericError == php_libxml_error_handler && PG(modules_activated)) {
		return _php_libxml_external_entity_loader(URL, ID, context);
	} else {
		return _php_libxml_default_entity_loader(URL, ID, context);
	}
}

PHP_LIBXML_API void php_libxml_pretend_ctx_error_ex(const char *file, int line, int column, const char *msg,...)
{
	va_list args;
	va_start(args, msg);
	php_libxml_internal_error_handler_ex(PHP_LIBXML_CTX_ERROR, NULL, msg, args, line, column);
	va_end(args);

	/* Propagate back into libxml */
	if (LIBXML(error_list)) {
		xmlErrorPtr last = zend_llist_get_last(LIBXML(error_list));
		if (last && !last->file) {
			last->file = strdup(file);
		}
	}
}

PHP_LIBXML_API void php_libxml_ctx_error(void *ctx, const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	php_libxml_error_handler_va(PHP_LIBXML_CTX_ERROR, ctx, msg, args);
	va_end(args);
}

PHP_LIBXML_API void php_libxml_ctx_warning(void *ctx, const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	php_libxml_error_handler_va(PHP_LIBXML_CTX_WARNING, ctx, msg, args);
	va_end(args);
}

#if LIBXML_VERSION >= 21200
static void php_libxml_structured_error_handler(void *userData, const xmlError *error)
#else
static void php_libxml_structured_error_handler(void *userData, xmlErrorPtr error)
#endif
{
	_php_list_set_error_structure(error, NULL, 0, 0);
}

PHP_LIBXML_API void php_libxml_error_handler(void *ctx, const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	php_libxml_error_handler_va(PHP_LIBXML_ERROR, ctx, msg, args);
	va_end(args);
}

static void php_libxml_exports_dtor(zval *zv)
{
	free(Z_PTR_P(zv));
}

PHP_LIBXML_API void php_libxml_initialize(void)
{
	if (!_php_libxml_initialized) {
		/* we should be the only one's to ever init!! */
		ZEND_IGNORE_LEAKS_BEGIN();
		xmlInitParser();
		ZEND_IGNORE_LEAKS_END();

		_php_libxml_default_entity_loader = xmlGetExternalEntityLoader();
		xmlSetExternalEntityLoader(_php_libxml_pre_ext_ent_loader);

		zend_hash_init(&php_libxml_exports, 0, NULL, php_libxml_exports_dtor, 1);

		_php_libxml_initialized = 1;
	}
}

PHP_LIBXML_API void php_libxml_shutdown(void)
{
	if (_php_libxml_initialized) {
#if defined(LIBXML_SCHEMAS_ENABLED) && LIBXML_VERSION < 21000
		xmlRelaxNGCleanupTypes();
#endif
		/* xmlCleanupParser(); */
		zend_hash_destroy(&php_libxml_exports);

		xmlSetExternalEntityLoader(_php_libxml_default_entity_loader);
		_php_libxml_initialized = 0;
	}
}

PHP_LIBXML_API void php_libxml_switch_context(zval *context, zval *oldcontext)
{
	if (oldcontext) {
		ZVAL_COPY_VALUE(oldcontext, &LIBXML(stream_context));
	}
	if (context) {
		ZVAL_COPY_VALUE(&LIBXML(stream_context), context);
	}
}

static PHP_MINIT_FUNCTION(libxml)
{
	php_libxml_initialize();

	register_libxml_symbols(module_number);

	libxmlerror_class_entry = register_class_LibXMLError();

	if (sapi_module.name) {
		static const char * const supported_sapis[] = {
			"cgi-fcgi",
			"litespeed",
			NULL
		};
		const char * const *sapi_name;

		for (sapi_name = supported_sapis; *sapi_name; sapi_name++) {
			if (strcmp(sapi_module.name, *sapi_name) == 0) {
				_php_libxml_per_request_initialization = 0;
				break;
			}
		}
	}

	if (!_php_libxml_per_request_initialization) {
		/* report errors via handler rather than stderr */
		xmlSetGenericErrorFunc(NULL, php_libxml_error_handler);
		xmlParserInputBufferCreateFilenameDefault(php_libxml_input_buffer_create_filename);
		xmlOutputBufferCreateFilenameDefault(php_libxml_output_buffer_create_filename);
	}

	return SUCCESS;
}


static PHP_RINIT_FUNCTION(libxml)
{
	if (_php_libxml_per_request_initialization) {
		/* report errors via handler rather than stderr */
		xmlSetGenericErrorFunc(NULL, php_libxml_error_handler);
		xmlParserInputBufferCreateFilenameDefault(php_libxml_input_buffer_create_filename);
		xmlOutputBufferCreateFilenameDefault(php_libxml_output_buffer_create_filename);
	}

	/* Enable the entity loader by default. This ensures that
	 * other threads/requests that might have disabled the loader
	 * do not affect the current request.
	 */
	LIBXML(entity_loader_disabled) = 0;

	return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(libxml)
{
	if (ZEND_FCC_INITIALIZED(LIBXML(entity_loader_callback))) {
		zend_fcc_dtor(&LIBXML(entity_loader_callback));
	}

	return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(libxml)
{
	if (!_php_libxml_per_request_initialization) {
		xmlSetGenericErrorFunc(NULL, NULL);

		xmlParserInputBufferCreateFilenameDefault(NULL);
		xmlOutputBufferCreateFilenameDefault(NULL);
	}
	php_libxml_shutdown();

	return SUCCESS;
}

static zend_result php_libxml_post_deactivate(void)
{
	/* reset libxml generic error handling */
	if (_php_libxml_per_request_initialization) {
		xmlSetGenericErrorFunc(NULL, NULL);

		xmlParserInputBufferCreateFilenameDefault(NULL);
		xmlOutputBufferCreateFilenameDefault(NULL);
	}
	xmlSetStructuredErrorFunc(NULL, NULL);

	/* the steam_context resource will be released by resource list destructor */
	ZVAL_UNDEF(&LIBXML(stream_context));
	smart_str_free(&LIBXML(error_buffer));
	if (LIBXML(error_list)) {
		zend_llist_destroy(LIBXML(error_list));
		efree(LIBXML(error_list));
		LIBXML(error_list) = NULL;
	}
	xmlResetLastError();

	return SUCCESS;
}


static PHP_MINFO_FUNCTION(libxml)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "libXML support", "active");
	php_info_print_table_row(2, "libXML Compiled Version", LIBXML_DOTTED_VERSION);
	php_info_print_table_row(2, "libXML Loaded Version", (char *)xmlParserVersion);
	php_info_print_table_row(2, "libXML streams", "enabled");
	php_info_print_table_end();
}
/* }}} */

/* {{{ Set the streams context for the next libxml document load or write */
PHP_FUNCTION(libxml_set_streams_context)
{
	zval *arg;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_RESOURCE(arg)
	ZEND_PARSE_PARAMETERS_END();

	if (php_stream_context_from_zval(arg, true) != NULL) {
		if (!Z_ISUNDEF(LIBXML(stream_context))) {
			zval_ptr_dtor(&LIBXML(stream_context));
		}
		ZVAL_COPY(&LIBXML(stream_context), arg);
	}
}
/* }}} */

PHP_LIBXML_API bool php_libxml_uses_internal_errors(void)
{
	return xmlStructuredError == php_libxml_structured_error_handler;
}

/* {{{ Disable libxml errors and allow user to fetch error information as needed */
PHP_FUNCTION(libxml_use_internal_errors)
{
	bool use_errors, use_errors_is_null = true;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL_OR_NULL(use_errors, use_errors_is_null)
	ZEND_PARSE_PARAMETERS_END();

	bool retval = php_libxml_uses_internal_errors();

	if (use_errors_is_null) {
		RETURN_BOOL(retval);
	}

	if (use_errors == 0) {
		xmlSetStructuredErrorFunc(NULL, NULL);
		if (LIBXML(error_list)) {
			zend_llist_destroy(LIBXML(error_list));
			efree(LIBXML(error_list));
			LIBXML(error_list) = NULL;
		}
	} else {
		xmlSetStructuredErrorFunc(NULL, php_libxml_structured_error_handler);
		if (LIBXML(error_list) == NULL) {
			LIBXML(error_list) = (zend_llist *) emalloc(sizeof(zend_llist));
			zend_llist_init(LIBXML(error_list), sizeof(xmlError), _php_libxml_free_error, 0);
		}
	}
	RETURN_BOOL(retval);
}
/* }}} */

static void php_libxml_create_error_object(zval *return_value, const xmlError *error)
{
	object_init_ex(return_value, libxmlerror_class_entry);
	add_property_long(return_value, "level", error->level);
	add_property_long(return_value, "code", error->code);
	add_property_long(return_value, "column", error->int2);
	if (error->message) {
		add_property_string(return_value, "message", error->message);
	} else {
		add_property_str(return_value, "message", zend_empty_string);
	}
	if (error->file) {
		add_property_string(return_value, "file", error->file);
	} else {
		add_property_str(return_value, "file", zend_empty_string);
	}
	add_property_long(return_value, "line", error->line);
}

/* {{{ Retrieve last error from libxml */
PHP_FUNCTION(libxml_get_last_error)
{
	ZEND_PARSE_PARAMETERS_NONE();

	const xmlError *error;

	if (LIBXML(error_list)) {
		error = zend_llist_get_last(LIBXML(error_list));
	} else {
		error = xmlGetLastError();
	}

	if (error) {
		php_libxml_create_error_object(return_value, error);
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ Retrieve array of errors */
PHP_FUNCTION(libxml_get_errors)
{
	xmlErrorPtr error;

	ZEND_PARSE_PARAMETERS_NONE();

	if (LIBXML(error_list)) {
		array_init(return_value);
		error = zend_llist_get_first(LIBXML(error_list));

		while (error != NULL) {
			zval z_error;
			php_libxml_create_error_object(&z_error, error);
			add_next_index_zval(return_value, &z_error);
			error = zend_llist_get_next(LIBXML(error_list));
		}
	} else {
		RETURN_EMPTY_ARRAY();
	}
}
/* }}} */

/* {{{ Clear last error from libxml */
PHP_FUNCTION(libxml_clear_errors)
{
	ZEND_PARSE_PARAMETERS_NONE();

	xmlResetLastError();
	if (LIBXML(error_list)) {
		zend_llist_clean(LIBXML(error_list));
	}
}
/* }}} */

PHP_LIBXML_API bool php_libxml_disable_entity_loader(bool disable) /* {{{ */
{
	bool old = LIBXML(entity_loader_disabled);

	LIBXML(entity_loader_disabled) = disable;
	return old;
} /* }}} */

/* {{{ Disable/Enable ability to load external entities */
PHP_FUNCTION(libxml_disable_entity_loader)
{
	bool disable = 1;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(disable)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_BOOL(php_libxml_disable_entity_loader(disable));
}
/* }}} */

/* {{{ Changes the default external entity loader */
PHP_FUNCTION(libxml_set_external_entity_loader)
{
	zend_fcall_info			fci;
	zend_fcall_info_cache	fcc;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_FUNC_NO_TRAMPOLINE_FREE_OR_NULL(fci, fcc)
	ZEND_PARSE_PARAMETERS_END();

	/* Unset old callback if it's defined */
	if (ZEND_FCC_INITIALIZED(LIBXML(entity_loader_callback))) {
		zend_fcc_dtor(&LIBXML(entity_loader_callback));
	}
	if (ZEND_FCI_INITIALIZED(fci)) { /* argument not null */
		zend_fcc_dup(&LIBXML(entity_loader_callback), &fcc);
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ Get the current external entity loader, or null if the default loader is installer. */
PHP_FUNCTION(libxml_get_external_entity_loader)
{
	ZEND_PARSE_PARAMETERS_NONE();

	if (ZEND_FCC_INITIALIZED(LIBXML(entity_loader_callback))) {
		zend_get_callable_zval_from_fcc(&LIBXML(entity_loader_callback), return_value);
		return;
	}
	RETURN_NULL();
}
/* }}} */

/* {{{ Common functions shared by extensions */
int php_libxml_xmlCheckUTF8(const unsigned char *s)
{
	size_t i;
	unsigned char c;

	for (i = 0; (c = s[i++]);) {
		if ((c & 0x80) == 0) {
		} else if ((c & 0xe0) == 0xc0) {
			if ((s[i++] & 0xc0) != 0x80) {
				return 0;
			}
		} else if ((c & 0xf0) == 0xe0) {
			if ((s[i++] & 0xc0) != 0x80 || (s[i++] & 0xc0) != 0x80) {
				return 0;
			}
		} else if ((c & 0xf8) == 0xf0) {
			if ((s[i++] & 0xc0) != 0x80 || (s[i++] & 0xc0) != 0x80 || (s[i++] & 0xc0) != 0x80) {
				return 0;
			}
		} else {
			return 0;
		}
	}
	return 1;
}

zval *php_libxml_register_export(zend_class_entry *ce, php_libxml_export_node export_function)
{
	php_libxml_func_handler export_hnd;

	/* Initialize in case this module hasn't been loaded yet */
	php_libxml_initialize();
	export_hnd.export_func = export_function;

	return zend_hash_add_mem(&php_libxml_exports, ce->name, &export_hnd, sizeof(export_hnd));
}

PHP_LIBXML_API xmlNodePtr php_libxml_import_node(zval *object)
{
	zend_class_entry *ce = NULL;
	xmlNodePtr node = NULL;
	php_libxml_func_handler *export_hnd;

	if (Z_TYPE_P(object) == IS_OBJECT) {
		ce = Z_OBJCE_P(object);
		while (ce->parent != NULL) {
			ce = ce->parent;
		}
		if ((export_hnd = zend_hash_find_ptr(&php_libxml_exports, ce->name))) {
			node = export_hnd->export_func(object);
		}
	}
	return node;
}

PHP_LIBXML_API int php_libxml_increment_node_ptr(php_libxml_node_object *object, xmlNodePtr node, void *private_data)
{
	int ret_refcount = -1;

	if (object != NULL && node != NULL) {
		if (object->node != NULL) {
			if (object->node->node == node) {
				return object->node->refcount;
			} else {
				php_libxml_decrement_node_ptr(object);
			}
		}
		if (node->_private != NULL) {
			object->node = node->_private;
			ret_refcount = ++object->node->refcount;
			/* Only dom uses _private */
			if (object->node->_private == NULL) {
				object->node->_private = private_data;
			}
		} else {
			object->node = emalloc(sizeof(php_libxml_node_ptr));
			ret_refcount = 1;
			object->node->node = node;
			object->node->refcount = 1;
			object->node->_private = private_data;
			node->_private = object->node;
		}
	}

	return ret_refcount;
}

PHP_LIBXML_API int php_libxml_decrement_node_ptr_ref(php_libxml_node_ptr *ptr)
{
	ZEND_ASSERT(ptr != NULL);

	int ret_refcount = --ptr->refcount;
	if (ret_refcount == 0) {
		if (ptr->node != NULL) {
			ptr->node->_private = NULL;
		}
		if (ptr->_private) {
			php_libxml_node_object *object = (php_libxml_node_object *) ptr->_private;
			object->node = NULL;
		}
		efree(ptr);
	}
	return ret_refcount;
}

PHP_LIBXML_API int php_libxml_decrement_node_ptr(php_libxml_node_object *object)
{
	if (object != NULL && object->node != NULL) {
		return php_libxml_decrement_node_ptr_ref(object->node);
	}
	return -1;
}

PHP_LIBXML_API int php_libxml_increment_doc_ref(php_libxml_node_object *object, xmlDocPtr docp)
{
	int ret_refcount = -1;

	if (object->document != NULL) {
		object->document->refcount++;
		ret_refcount = object->document->refcount;
	} else if (docp != NULL) {
		ret_refcount = 1;
		object->document = emalloc(sizeof(php_libxml_ref_obj));
		object->document->ptr = docp;
		object->document->refcount = ret_refcount;
		object->document->doc_props = NULL;
		object->document->cache_tag.modification_nr = 1; /* iterators start at 0, such that they will start in an uninitialised state */
		object->document->private_data = NULL;
		object->document->class_type = PHP_LIBXML_CLASS_UNSET;
		object->document->handlers = &php_libxml_default_document_handlers;
		object->document->quirks_mode = PHP_LIBXML_NO_QUIRKS;
	}

	return ret_refcount;
}

PHP_LIBXML_API int php_libxml_decrement_doc_ref_directly(php_libxml_ref_obj *document)
{
	int ret_refcount = --document->refcount;
	if (ret_refcount == 0) {
		if (document->private_data != NULL) {
			document->private_data->dtor(document->private_data);
		}
		if (document->ptr != NULL) {
			xmlFreeDoc((xmlDoc *) document->ptr);
		}
		if (document->doc_props != NULL) {
			if (document->doc_props->classmap) {
				zend_hash_destroy(document->doc_props->classmap);
				FREE_HASHTABLE(document->doc_props->classmap);
			}
			efree(document->doc_props);
		}
		efree(document);
	}

	return ret_refcount;
}

PHP_LIBXML_API int php_libxml_decrement_doc_ref(php_libxml_node_object *object)
{
	int ret_refcount = -1;

	if (object != NULL && object->document != NULL) {
		ret_refcount = php_libxml_decrement_doc_ref_directly(object->document);
		object->document = NULL;
	}

	return ret_refcount;
}

PHP_LIBXML_API void php_libxml_node_free_resource(xmlNodePtr node)
{
	if (!node) {
		return;
	}

	switch (node->type) {
		case XML_DOCUMENT_NODE:
		case XML_HTML_DOCUMENT_NODE:
			break;
		case XML_ENTITY_REF_NODE:
			/* Entity reference nodes are special: their children point to entity declarations,
			 * but they don't own the declarations and therefore shouldn't free the children.
			 * Moreover, there can be more than one reference node for a single entity declarations. */
			php_libxml_unregister_node(node);
			if (node->parent == NULL) {
				php_libxml_node_free(node);
			}
			break;
		default:
			if (node->parent == NULL || node->type == XML_NAMESPACE_DECL) {
				php_libxml_node_free_list((xmlNodePtr) node->children);
				if (node->type == XML_ELEMENT_NODE) {
					php_libxml_node_free_list((xmlNodePtr) node->properties);
				}
				php_libxml_unregister_node(node);
				php_libxml_node_free(node);
			} else {
				php_libxml_unregister_node(node);
			}
	}
}

PHP_LIBXML_API void php_libxml_node_decrement_resource(php_libxml_node_object *object)
{
	if (object != NULL && object->node != NULL) {
		php_libxml_node_ptr *obj_node = (php_libxml_node_ptr *) object->node;
		xmlNodePtr nodep = obj_node->node;
		int ret_refcount = php_libxml_decrement_node_ptr(object);
		if (ret_refcount == 0) {
			php_libxml_node_free_resource(nodep);
		} else {
			if (object == obj_node->_private) {
				obj_node->_private = NULL;
			}
		}
	}
	if (object != NULL && object->document != NULL) {
		/* Safe to call as if the resource were freed then doc pointer is NULL */
		php_libxml_decrement_doc_ref(object);
	}
}
/* }}} */

PHP_LIBXML_API xmlChar *php_libxml_attr_value(const xmlAttr *attr, bool *free)
{
	/* For attributes we can have an optimized fast-path.
	 * This fast-path is only possible in the (common) case where the attribute
	 * has a single text child. Note that if the child or the content is NULL, this
	 * is equivalent to not having content (i.e. the attribute has the empty string as value). */

	*free = false;

	if (attr->children == NULL) {
		return BAD_CAST "";
	}

	if (attr->children->type == XML_TEXT_NODE && attr->children->next == NULL) {
		if (attr->children->content == NULL) {
			return BAD_CAST "";
		} else {
			return attr->children->content;
		}
	}

	xmlChar *value = xmlNodeGetContent((const xmlNode *) attr);
	if (UNEXPECTED(value == NULL)) {
		return BAD_CAST "";
	}

	*free = true;
	return value;
}

static int php_libxml_write_smart_str(void *context, const char *buffer, int len)
{
	smart_str *str = context;
	smart_str_appendl(str, buffer, len);
	return len;
}

static zend_string *php_libxml_default_dump_doc_to_str(xmlDocPtr doc, int options, const char *encoding)
{
	smart_str str = {0};

	/* Encoding is handled from the encoding property set on the document */
	xmlSaveCtxtPtr ctxt = xmlSaveToIO(php_libxml_write_smart_str, NULL, &str, encoding, options);
	if (!ctxt) {
		return NULL;
	}

	long status = xmlSaveDoc(ctxt, doc);
	status |= xmlSaveClose(ctxt);
	if (status < 0) {
		smart_str_free_ex(&str, false);
		return NULL;
	}

	return smart_str_extract(&str);
}

static zend_string *php_libxml_default_dump_node_to_str(xmlDocPtr doc, xmlNodePtr node, bool format, const char *encoding)
{
	smart_str str = {0};
	// TODO: should this buffer take an encoding? For now keep it NULL for BC.
	xmlOutputBufferPtr buf = xmlOutputBufferCreateIO(php_libxml_write_smart_str, NULL, &str, NULL);
	if (!buf) {
		return NULL;
	}

	xmlNodeDumpOutput(buf, doc, node, 0, format, encoding);

	if (xmlOutputBufferFlush(buf) < 0) {
		smart_str_free_ex(&str, false);
		xmlOutputBufferClose(buf);
		return NULL;
	}

	xmlOutputBufferClose(buf);

	return smart_str_extract(&str);
}

static zend_long php_libxml_default_dump_doc_to_file(const char *filename, xmlDocPtr doc, bool format, const char *encoding)
{
	return xmlSaveFormatFileEnc(filename, doc, encoding, format);
}

static zend_long php_libxml_dump_node_to_file(const char *filename, xmlDocPtr doc, xmlNodePtr node, bool format, const char *encoding)
{
	xmlOutputBufferPtr outbuf = xmlOutputBufferCreateFilename(filename, NULL, 0);
	if (!outbuf) {
		return -1;
	}

	xmlNodeDumpOutput(outbuf, doc, node, 0, format, encoding);
	return xmlOutputBufferClose(outbuf);
}

#if defined(PHP_WIN32) && defined(COMPILE_DL_LIBXML)
PHP_LIBXML_API BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	return xmlDllMain(hinstDLL, fdwReason, lpvReserved);
}
#endif

#endif
