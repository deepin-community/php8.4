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
  | Authors: Sterling Hughes <sterling@php.net>                          |
  |          Marcus Boerger <helly@php.net>                              |
  |          Rob Richards <rrichards@php.net>                            |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#if defined(HAVE_LIBXML) && defined(HAVE_SIMPLEXML)

#include "ext/standard/info.h"
#include "ext/standard/php_string.h" /* For php_trim() */
#include "php_simplexml.h"
#include "php_simplexml_exports.h"
#include "simplexml_arginfo.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "ext/spl/spl_iterators.h"

PHP_SXE_API zend_class_entry *ce_SimpleXMLIterator;
PHP_SXE_API zend_class_entry *ce_SimpleXMLElement;

PHP_SXE_API zend_class_entry *sxe_get_element_class_entry(void) /* {{{ */
{
	return ce_SimpleXMLElement;
}
/* }}} */

static php_sxe_object* php_sxe_object_new(zend_class_entry *ce, zend_function *fptr_count);
static xmlNodePtr php_sxe_reset_iterator(php_sxe_object *sxe);
static xmlNodePtr php_sxe_reset_iterator_no_clear_iter_data(php_sxe_object *sxe, int use_data);
static xmlNodePtr php_sxe_iterator_fetch(php_sxe_object *sxe, xmlNodePtr node, int use_data);
static void php_sxe_iterator_dtor(zend_object_iterator *iter);
static zend_result php_sxe_iterator_valid(zend_object_iterator *iter);
static zval *php_sxe_iterator_current_data(zend_object_iterator *iter);
static void php_sxe_iterator_current_key(zend_object_iterator *iter, zval *key);
static void php_sxe_iterator_move_forward(zend_object_iterator *iter);
static void php_sxe_iterator_rewind(zend_object_iterator *iter);
static zend_result sxe_object_cast_ex(zend_object *readobj, zval *writeobj, int type);

static void sxe_unlink_node(xmlNodePtr node)
{
	xmlUnlinkNode(node);
	/* Only destroy the nodes if we have no objects using them anymore.
	 * Don't assume simplexml owns these. */
	if (!node->_private) {
		php_libxml_node_free_resource(node);
	}
}

/* {{{ _node_as_zval() */
static void node_as_zval(php_sxe_object *sxe, xmlNodePtr node, zval *value, SXE_ITER itertype, zend_string *name, zend_string *nsprefix, int isprefix)
{
	php_sxe_object *subnode;

	subnode = php_sxe_object_new(sxe->zo.ce, sxe->fptr_count);
	subnode->document = sxe->document;
	subnode->document->refcount++;
	subnode->iter.type = itertype;
	if (name) {
		subnode->iter.name = zend_string_copy(name);
	}
	if (nsprefix && *ZSTR_VAL(nsprefix)) {
		subnode->iter.nsprefix = zend_string_copy(nsprefix);
		subnode->iter.isprefix = isprefix;
	}

	php_libxml_increment_node_ptr((php_libxml_node_object *)subnode, node, NULL);

	ZVAL_OBJ(value, &subnode->zo);
}
/* }}} */

static void node_as_zval_str(php_sxe_object *sxe, xmlNodePtr node, zval *value, SXE_ITER itertype, const xmlChar *name, const xmlChar *nsprefix, int isprefix)
{
	zend_string *name_str = zend_string_init((const char *) name, strlen((const char *) name), false);
	zend_string *ns_str = nsprefix ? zend_string_init((const char *) nsprefix, strlen((const char *) nsprefix), false) : NULL;
	node_as_zval(sxe, node, value, itertype, name_str, ns_str, isprefix);
	zend_string_release_ex(name_str, false);
	if (ns_str) {
		zend_string_release_ex(ns_str, false);
	}
}

static xmlNodePtr php_sxe_get_first_node_non_destructive(php_sxe_object *sxe, xmlNodePtr node)
{
	if (sxe && sxe->iter.type != SXE_ITER_NONE) {
		return php_sxe_reset_iterator_no_clear_iter_data(sxe, false);
	} else {
		return node;
	}
}

static inline int match_ns(xmlNodePtr node, const zend_string *name, int prefix) /* {{{ */
{
	if (name == NULL && (node->ns == NULL || node->ns->prefix == NULL)) {
		return 1;
	}

	if (node->ns && xmlStrEqual(prefix ? node->ns->prefix : node->ns->href, name ? BAD_CAST ZSTR_VAL(name) : NULL)) {
		return 1;
	}

	return 0;
}
/* }}} */

static xmlNodePtr sxe_get_element_by_offset(php_sxe_object *sxe, zend_long offset, xmlNodePtr node, zend_long *cnt) /* {{{ */
{
	zend_long nodendx = 0;

	if (sxe->iter.type == SXE_ITER_NONE) {
		if (offset == 0) {
			if (cnt) {
				*cnt = 0;
			}
			return node;
		} else {
			return NULL;
		}
	}
	while (node && nodendx <= offset) {
		if (node->type == XML_ELEMENT_NODE && match_ns(node, sxe->iter.nsprefix, sxe->iter.isprefix)) {
			if (sxe->iter.type == SXE_ITER_CHILD || (
				sxe->iter.type == SXE_ITER_ELEMENT && xmlStrEqual(node->name, BAD_CAST ZSTR_VAL(sxe->iter.name)))) {
				if (nodendx == offset) {
					break;
				}
				nodendx++;
			}
		}
		node = node->next;
	}

	if (cnt) {
		*cnt = nodendx;
	}

	return node;
}
/* }}} */

static xmlNodePtr sxe_find_element_by_name(php_sxe_object *sxe, xmlNodePtr node, const zend_string *name) /* {{{ */
{
	const xmlChar *raw_name = BAD_CAST ZSTR_VAL(name);
	while (node) {
		if (node->type == XML_ELEMENT_NODE && match_ns(node, sxe->iter.nsprefix, sxe->iter.isprefix)) {
			if (xmlStrEqual(node->name, raw_name)) {
				return node;
			}
		}
		node = node->next;
	}
	return NULL;
} /* }}} */

static xmlNodePtr sxe_get_element_by_name(php_sxe_object *sxe, xmlNodePtr node, char *name, SXE_ITER *type) /* {{{ */
{
	int         orgtype;
	xmlNodePtr  orgnode = node;

	if (sxe->iter.type != SXE_ITER_ATTRLIST)
	{
		orgtype = sxe->iter.type;
		if (sxe->iter.type == SXE_ITER_NONE) {
			sxe->iter.type = SXE_ITER_CHILD;
		}
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		sxe->iter.type = orgtype;
	}

	if (sxe->iter.type == SXE_ITER_ELEMENT) {
		orgnode = sxe_find_element_by_name(sxe, node, sxe->iter.name);
		if (!orgnode) {
			return NULL;
		}
		node = orgnode->children;
	}

	while (node) {
		if (node->type == XML_ELEMENT_NODE && match_ns(node, sxe->iter.nsprefix, sxe->iter.isprefix)) {
			if (xmlStrEqual(node->name, (xmlChar *)name)) {
				*type = SXE_ITER_ELEMENT;
				return orgnode;
			}
		}
		node = node->next;
	}

	return NULL;
}
/* }}} */

/* {{{ sxe_prop_dim_read() */
static zval *sxe_prop_dim_read(zend_object *object, zval *member, bool elements, bool attribs, int type, zval *rv)
{
	php_sxe_object *sxe;
	zend_string    *name;
	xmlNodePtr      node;
	xmlAttrPtr      attr = NULL;
	zval            tmp_zv;
	int             nodendx = 0;
	int             test = 0;

	sxe = php_sxe_fetch_object(object);

	if (!member) {
		if (sxe->iter.type == SXE_ITER_ATTRLIST) {
			/* This happens when the user did: $sxe[]->foo = $value */
			zend_throw_error(NULL, "Cannot create unnamed attribute");
			return &EG(uninitialized_zval);
		}
		goto long_dim;
	} else {
		ZVAL_DEREF(member);
		if (Z_TYPE_P(member) == IS_LONG) {
			if (sxe->iter.type != SXE_ITER_ATTRLIST) {
long_dim:
				attribs = 0;
				elements = 1;
			}
			name = NULL;
		} else {
			if (Z_TYPE_P(member) != IS_STRING) {
				zend_string *str = zval_try_get_string_func(member);
				if (UNEXPECTED(!str)) {
					return &EG(uninitialized_zval);
				}
				ZVAL_STR(&tmp_zv, str);
				member = &tmp_zv;
			}
			name = Z_STR_P(member);
		}
	}

	GET_NODE(sxe, node);

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		attribs = 1;
		elements = 0;
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = (xmlAttrPtr)node;
		test = sxe->iter.name != NULL;
	} else if (sxe->iter.type != SXE_ITER_CHILD) {
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = node ? node->properties : NULL;
		test = 0;
		if (!member && node && node->parent &&
		    node->parent->type == XML_DOCUMENT_NODE) {
			/* This happens when the user did: $sxe[]->foo = $value */
			zend_throw_error(NULL, "Cannot create unnamed attribute");
			return &EG(uninitialized_zval);
		}
	}

	ZVAL_UNDEF(rv);

	if (node) {
		if (attribs) {
			if (Z_TYPE_P(member) != IS_LONG || sxe->iter.type == SXE_ITER_ATTRLIST) {
				if (Z_TYPE_P(member) == IS_LONG) {
					while (attr && nodendx <= Z_LVAL_P(member)) {
						if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
							if (nodendx == Z_LVAL_P(member)) {
								node_as_zval(sxe, (xmlNodePtr) attr, rv, SXE_ITER_NONE, NULL, sxe->iter.nsprefix, sxe->iter.isprefix);
								break;
							}
							nodendx++;
						}
						attr = attr->next;
					}
				} else {
					while (attr) {
						if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(name)) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
							node_as_zval(sxe, (xmlNodePtr) attr, rv, SXE_ITER_NONE, NULL, sxe->iter.nsprefix, sxe->iter.isprefix);
							break;
						}
						attr = attr->next;
					}
				}
			}
		}

		if (elements) {
			if (!sxe->node) {
				php_libxml_increment_node_ptr((php_libxml_node_object *)sxe, node, NULL);
			}
			if (!member || Z_TYPE_P(member) == IS_LONG) {
				zend_long cnt = 0;
				xmlNodePtr mynode = node;

				if (sxe->iter.type == SXE_ITER_CHILD) {
					node = php_sxe_get_first_node_non_destructive(sxe, node);
				}
				if (sxe->iter.type == SXE_ITER_NONE) {
					if (member && Z_LVAL_P(member) > 0) {
						php_error_docref(NULL, E_WARNING, "Cannot add element %s number " ZEND_LONG_FMT " when only 0 such elements exist", mynode->name, Z_LVAL_P(member));
					}
				} else if (member) {
					node = sxe_get_element_by_offset(sxe, Z_LVAL_P(member), node, &cnt);
				} else {
					node = NULL;
				}
				if (node) {
					node_as_zval(sxe, node, rv, SXE_ITER_NONE, NULL, sxe->iter.nsprefix, sxe->iter.isprefix);
				} else if (type == BP_VAR_W || type == BP_VAR_RW) {
					if (member && cnt < Z_LVAL_P(member)) {
						php_error_docref(NULL, E_WARNING, "Cannot add element %s number " ZEND_LONG_FMT " when only " ZEND_LONG_FMT " such elements exist", mynode->name, Z_LVAL_P(member), cnt);
					}
					node = xmlNewTextChild(mynode->parent, mynode->ns, mynode->name, NULL);
					node_as_zval(sxe, node, rv, SXE_ITER_NONE, NULL, sxe->iter.nsprefix, sxe->iter.isprefix);
				}
			} else {
				/* In BP_VAR_IS mode only return a proper node if it actually exists. */
				if (type != BP_VAR_IS || sxe_find_element_by_name(sxe, node->children, name)) {
					node_as_zval(sxe, node, rv, SXE_ITER_ELEMENT, name, sxe->iter.nsprefix, sxe->iter.isprefix);
				}
			}
		}
	}

	if (member == &tmp_zv) {
		zval_ptr_dtor_str(&tmp_zv);
	}

	if (Z_ISUNDEF_P(rv)) {
		ZVAL_NULL(rv);
	}

	return rv;
}
/* }}} */

/* {{{ sxe_property_read() */
static zval *sxe_property_read(zend_object *object, zend_string *name, int type, void **cache_slot, zval *rv)
{
	zval member;
	ZVAL_STR(&member, name);
	return sxe_prop_dim_read(object, &member, 1, 0, type, rv);
}
/* }}} */

/* {{{ sxe_dimension_read() */
static zval *sxe_dimension_read(zend_object *object, zval *offset, int type, zval *rv)
{
	return sxe_prop_dim_read(object, offset, 0, 1, type, rv);
}
/* }}} */

/* {{{ change_node_zval() */
static void change_node_zval(xmlNodePtr node, zend_string *value)
{
	xmlChar *buffer = xmlEncodeEntitiesReentrant(node->doc, (xmlChar *)ZSTR_VAL(value));
	/* check for NULL buffer in case of memory error in xmlEncodeEntitiesReentrant */
	if (buffer) {
		xmlNodeSetContent(node, buffer);
		xmlFree(buffer);
	}
}
/* }}} */

/* {{{ sxe_property_write() */
static zval *sxe_prop_dim_write(zend_object *object, zval *member, zval *value, bool elements, bool attribs, xmlNodePtr *pnewnode)
{
	php_sxe_object *sxe;
	xmlNodePtr      node;
	xmlNodePtr      newnode = NULL;
	xmlNodePtr      mynode;
	xmlNodePtr		tempnode;
	xmlAttrPtr      attr = NULL;
	int             counter = 0;
	int             is_attr = 0;
	int				nodendx = 0;
	int             test = 0;
	zend_long            cnt = 0;
	zval            tmp_zv;
	zend_string    *trim_str;
	zend_string    *value_str = NULL;

	sxe = php_sxe_fetch_object(object);

	if (!member) {
		if (sxe->iter.type == SXE_ITER_ATTRLIST) {
			/* This happens when the user did: $sxe[] = $value
			 * and could also be E_PARSE, but we use this only during parsing
			 * and this is during runtime.
			 */
			zend_throw_error(NULL, "Cannot append to an attribute list");
			return &EG(error_zval);
		}
		goto long_dim;
	} else {
		ZVAL_DEREF(member);
		if (Z_TYPE_P(member) == IS_LONG) {
			if (sxe->iter.type != SXE_ITER_ATTRLIST) {
long_dim:
				attribs = 0;
				elements = 1;
			}
		} else {
			if (Z_TYPE_P(member) != IS_STRING) {
				trim_str = zval_try_get_string_func(member);
				if (UNEXPECTED(!trim_str)) {
					return &EG(error_zval);
				}

				ZVAL_STR(&tmp_zv, php_trim(trim_str, NULL, 0, 3));
				zend_string_release_ex(trim_str, 0);
				member = &tmp_zv;
			}

			if (!Z_STRLEN_P(member)) {
				zend_value_error("Cannot create %s with an empty name", attribs ? "attribute" : "element");
				if (member == &tmp_zv) {
					zval_ptr_dtor_str(&tmp_zv);
				}
				return &EG(error_zval);
			}
		}
	}

	GET_NODE(sxe, node);

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		attribs = 1;
		elements = 0;
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = (xmlAttrPtr)node;
		test = sxe->iter.name != NULL;
	} else if (sxe->iter.type != SXE_ITER_CHILD) {
		mynode = node;
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = node ? node->properties : NULL;
		test = 0;
		if (!member && node && node->parent &&
		    node->parent->type == XML_DOCUMENT_NODE) {
			/* This happens when the user did: $sxe[] = $value
			 * and could also be E_PARSE, but we use this only during parsing
			 * and this is during runtime.
			 */
			zend_value_error("Cannot append to an attribute list");
			return &EG(error_zval);
		}
		if (attribs && !node && sxe->iter.type == SXE_ITER_ELEMENT) {
			node = xmlNewChild(mynode, mynode->ns, BAD_CAST ZSTR_VAL(sxe->iter.name), NULL);
			attr = node->properties;
		}
	}

	mynode = node;

	if (value) {
		switch (Z_TYPE_P(value)) {
			case IS_LONG:
			case IS_FALSE:
			case IS_TRUE:
			case IS_DOUBLE:
			case IS_NULL:
			case IS_STRING:
				value_str = zval_get_string(value);
				break;
			case IS_OBJECT:
				if (Z_OBJCE_P(value) == ce_SimpleXMLElement) {
					zval zval_copy;
					zend_result rv = sxe_object_cast_ex(Z_OBJ_P(value), &zval_copy, IS_STRING);
					ZEND_IGNORE_VALUE(rv);
					ZEND_ASSERT(rv == SUCCESS);

					value_str = Z_STR(zval_copy);
					break;
				}
				ZEND_FALLTHROUGH;
			default:
				if (member == &tmp_zv) {
					zval_ptr_dtor_str(&tmp_zv);
				}
				zend_type_error("It's not possible to assign a complex type to %s, %s given", attribs ? "attributes" : "properties", zend_zval_value_name(value));
				return &EG(error_zval);
		}
	}

	if (node) {
		php_libxml_invalidate_node_list_cache_from_doc(node->doc);

		if (attribs) {
			if (Z_TYPE_P(member) == IS_LONG) {
				while (attr && nodendx <= Z_LVAL_P(member)) {
					if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						if (nodendx == Z_LVAL_P(member)) {
							is_attr = 1;
							++counter;
							break;
						}
						nodendx++;
					}
					attr = attr->next;
				}
			} else {
				while (attr) {
					if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && xmlStrEqual(attr->name, (xmlChar *)Z_STRVAL_P(member)) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						is_attr = 1;
						++counter;
						break;
					}
					attr = attr->next;
				}
			}

		}

		if (elements) {
			if (!member || Z_TYPE_P(member) == IS_LONG) {
				if (node->type == XML_ATTRIBUTE_NODE) {
					zend_throw_error(NULL, "Cannot create duplicate attribute");
					if (value_str) {
						zend_string_release(value_str);
					}
					return &EG(error_zval);
				}

				if (sxe->iter.type == SXE_ITER_NONE) {
					newnode = node;
					++counter;
					if (member && Z_LVAL_P(member) > 0) {
						php_error_docref(NULL, E_WARNING, "Cannot add element %s number " ZEND_LONG_FMT " when only 0 such elements exist", mynode->name, Z_LVAL_P(member));
						value = &EG(error_zval);
					}
				} else if (member) {
					newnode = sxe_get_element_by_offset(sxe, Z_LVAL_P(member), node, &cnt);
					if (newnode) {
						++counter;
					}
				}
			} else {
				node = node->children;
				while (node) {
					SKIP_TEXT(node);

					if (xmlStrEqual(node->name, (xmlChar *)Z_STRVAL_P(member)) && match_ns(node, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						newnode = node;
						++counter;
					}

next_iter:
					node = node->next;
				}
			}
		}

		if (counter == 1) {
			if (is_attr) {
				newnode = (xmlNodePtr) attr;
			}
			if (value_str) {
				while ((tempnode = (xmlNodePtr) newnode->children)) {
					sxe_unlink_node(tempnode);
				}
				change_node_zval(newnode, value_str);
			}
		} else if (counter > 1) {
			php_error_docref(NULL, E_WARNING, "Cannot assign to an array of nodes (duplicate subnodes or attr detected)");
			value = &EG(error_zval);
		} else if (elements) {
			if (!node) {
				if (!member || Z_TYPE_P(member) == IS_LONG) {
					newnode = xmlNewTextChild(mynode->parent, mynode->ns, mynode->name, value_str ? (xmlChar *)ZSTR_VAL(value_str) : NULL);
				} else {
					/* Note: we cannot set the namespace here unconditionally because the parent may be a document.
					 * Passing NULL will let libxml decide to either inherit the namespace or not set one at all,
					 * depending on whether the parent is an element. */
					newnode = xmlNewTextChild(mynode, NULL, (xmlChar *)Z_STRVAL_P(member), value_str ? (xmlChar *)ZSTR_VAL(value_str) : NULL);
				}
			} else if (!member || Z_TYPE_P(member) == IS_LONG) {
				if (member && cnt < Z_LVAL_P(member)) {
					php_error_docref(NULL, E_WARNING, "Cannot add element %s number " ZEND_LONG_FMT " when only " ZEND_LONG_FMT " such elements exist", mynode->name, Z_LVAL_P(member), cnt);
				}
				newnode = xmlNewTextChild(mynode->parent, mynode->ns, mynode->name, value_str ? (xmlChar *)ZSTR_VAL(value_str) : NULL);
			}
		} else if (attribs) {
			if (Z_TYPE_P(member) == IS_LONG) {
				php_error_docref(NULL, E_WARNING, "Cannot change attribute number " ZEND_LONG_FMT " when only %d attributes exist", Z_LVAL_P(member), nodendx);
			} else {
				newnode = (xmlNodePtr)xmlNewProp(node, (xmlChar *)Z_STRVAL_P(member), value_str ? (xmlChar *)ZSTR_VAL(value_str) : NULL);
			}
		}
	}

	if (member == &tmp_zv) {
		zval_ptr_dtor_str(&tmp_zv);
	}
	if (pnewnode) {
		*pnewnode = newnode;
	}
	if (value_str) {
		zend_string_release(value_str);
	}
	return value;
}
/* }}} */

/* {{{ sxe_property_write() */
static zval *sxe_property_write(zend_object *object, zend_string *name, zval *value, void **cache_slot)
{
	zval member;
	ZVAL_STR(&member, name);
	zval *retval = sxe_prop_dim_write(object, &member, value, 1, 0, NULL);
	return retval == &EG(error_zval) ? &EG(uninitialized_zval) : retval;
}
/* }}} */

/* {{{ sxe_dimension_write() */
static void sxe_dimension_write(zend_object *object, zval *offset, zval *value)
{
	sxe_prop_dim_write(object, offset, value, 0, 1, NULL);
}
/* }}} */

static zval *sxe_property_get_adr(zend_object *object, zend_string *zname, int fetch_type, void **cache_slot) /* {{{ */
{
	php_sxe_object *sxe;
	xmlNodePtr      node;
	zval            ret;
	char           *name;
	SXE_ITER        type;
	zval            member;

	if (cache_slot) {
		cache_slot[0] = cache_slot[1] = cache_slot[2] = NULL;
	}

	sxe = php_sxe_fetch_object(object);
	GET_NODE(sxe, node);
	if (UNEXPECTED(!node)) {
		return &EG(error_zval);
	}
	name = ZSTR_VAL(zname);
	node = sxe_get_element_by_name(sxe, node, name, &type);
	if (node) {
		return NULL;
	}
	ZVAL_STR(&member, zname);
	if (sxe_prop_dim_write(object, &member, NULL, 1, 0, &node) == &EG(error_zval)) {
		return &EG(error_zval);
	}
	type = SXE_ITER_NONE;

	node_as_zval(sxe, node, &ret, type, NULL, sxe->iter.nsprefix, sxe->iter.isprefix);

	if (!Z_ISUNDEF(sxe->tmp)) {
		zval_ptr_dtor(&sxe->tmp);
	}

	ZVAL_COPY_VALUE(&sxe->tmp, &ret);

	return &sxe->tmp;
}
/* }}} */

/* {{{ sxe_prop_dim_exists() */
static int sxe_prop_dim_exists(zend_object *object, zval *member, int check_empty, bool elements, bool attribs)
{
	php_sxe_object *sxe;
	xmlNodePtr      node;
	xmlAttrPtr      attr = NULL;
	int				exists = 0;
	int             test = 0;
	zval            tmp_zv;

	if (Z_TYPE_P(member) != IS_STRING && Z_TYPE_P(member) != IS_LONG) {
		zend_string *str = zval_try_get_string_func(member);
		if (UNEXPECTED(!str)) {
			return 0;
		}
		ZVAL_STR(&tmp_zv, str);
		member = &tmp_zv;
	}

	sxe = php_sxe_fetch_object(object);

	GET_NODE(sxe, node);

	if (Z_TYPE_P(member) == IS_LONG) {
		if (sxe->iter.type != SXE_ITER_ATTRLIST) {
			attribs = 0;
			elements = 1;
			if (sxe->iter.type == SXE_ITER_CHILD) {
				node = php_sxe_get_first_node_non_destructive(sxe, node);
			}
		}
	}

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		attribs = 1;
		elements = 0;
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = (xmlAttrPtr)node;
		test = sxe->iter.name != NULL;
	} else if (sxe->iter.type != SXE_ITER_CHILD) {
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = node ? node->properties : NULL;
		test = 0;
	}

	if (node) {
		if (attribs) {
			if (Z_TYPE_P(member) == IS_LONG) {
				int	nodendx = 0;

				while (attr && nodendx <= Z_LVAL_P(member)) {
					if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						if (nodendx == Z_LVAL_P(member)) {
							exists = 1;
							break;
						}
						nodendx++;
					}
					attr = attr->next;
				}
			} else {
				while (attr) {
					if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && xmlStrEqual(attr->name, (xmlChar *)Z_STRVAL_P(member)) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						exists = 1;
						break;
					}

					attr = attr->next;
				}
			}
			if (exists && check_empty == 1 &&
				(!attr->children || !attr->children->content || !attr->children->content[0] || xmlStrEqual(attr->children->content, (const xmlChar *) "0")) ) {
				/* Attribute with no content in its text node */
				exists = 0;
			}
		}

		if (elements) {
			if (Z_TYPE_P(member) == IS_LONG) {
				if (sxe->iter.type == SXE_ITER_CHILD) {
					node = php_sxe_get_first_node_non_destructive(sxe, node);
				}
				node = sxe_get_element_by_offset(sxe, Z_LVAL_P(member), node, NULL);
			} else {
				node = sxe_find_element_by_name(sxe, node->children, Z_STR_P(member));
			}
			if (node) {
				exists = 1;
				if (check_empty == 1 &&
					(!node->children || (node->children->type == XML_TEXT_NODE && !node->children->next &&
					 (!node->children->content || !node->children->content[0] || xmlStrEqual(node->children->content, (const xmlChar *) "0")))) ) {
					exists = 0;
				}
			}
		}
	}

	if (member == &tmp_zv) {
		zval_ptr_dtor_str(&tmp_zv);
	}

	return exists;
}
/* }}} */

/* {{{ sxe_property_exists() */
static int sxe_property_exists(zend_object *object, zend_string *name, int check_empty, void **cache_slot)
{
	zval member;
	ZVAL_STR(&member, name);
	return sxe_prop_dim_exists(object, &member, check_empty, 1, 0);
}
/* }}} */

/* {{{ sxe_dimension_exists() */
static int sxe_dimension_exists(zend_object *object, zval *member, int check_empty)
{
	return sxe_prop_dim_exists(object, member, check_empty, 0, 1);
}
/* }}} */

/* {{{ sxe_prop_dim_delete() */
static void sxe_prop_dim_delete(zend_object *object, zval *member, bool elements, bool attribs)
{
	php_sxe_object *sxe;
	xmlNodePtr      node;
	xmlNodePtr      nnext;
	xmlAttrPtr      attr = NULL;
	xmlAttrPtr      anext;
	zval            tmp_zv;
	int             test = 0;

	if (Z_TYPE_P(member) != IS_STRING && Z_TYPE_P(member) != IS_LONG) {
		zend_string *str = zval_try_get_string_func(member);
		if (UNEXPECTED(!str)) {
			return;
		}
		ZVAL_STR(&tmp_zv, str);
		member = &tmp_zv;
	}

	sxe = php_sxe_fetch_object(object);

	GET_NODE(sxe, node);

	if (Z_TYPE_P(member) == IS_LONG) {
		if (sxe->iter.type != SXE_ITER_ATTRLIST) {
			attribs = 0;
			elements = 1;
			if (sxe->iter.type == SXE_ITER_CHILD) {
				node = php_sxe_get_first_node_non_destructive(sxe, node);
			}
		}
	}

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		attribs = 1;
		elements = 0;
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = (xmlAttrPtr)node;
		test = sxe->iter.name != NULL;
	} else if (sxe->iter.type != SXE_ITER_CHILD) {
		node = php_sxe_get_first_node_non_destructive(sxe, node);
		attr = node ? node->properties : NULL;
		test = 0;
	}

	if (node) {
		php_libxml_invalidate_node_list_cache_from_doc(node->doc);

		if (attribs) {
			if (Z_TYPE_P(member) == IS_LONG) {
				int	nodendx = 0;

				while (attr && nodendx <= Z_LVAL_P(member)) {
					if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						if (nodendx == Z_LVAL_P(member)) {
							sxe_unlink_node((xmlNodePtr) attr);
							break;
						}
						nodendx++;
					}
					attr = attr->next;
				}
			} else {
				while (attr) {
					anext = attr->next;
					if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && xmlStrEqual(attr->name, (xmlChar *)Z_STRVAL_P(member)) && match_ns((xmlNodePtr) attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						sxe_unlink_node((xmlNodePtr) attr);
						break;
					}
					attr = anext;
				}
			}
		}

		if (elements) {
			if (Z_TYPE_P(member) == IS_LONG) {
				if (sxe->iter.type == SXE_ITER_CHILD) {
					node = php_sxe_get_first_node_non_destructive(sxe, node);
				}
				node = sxe_get_element_by_offset(sxe, Z_LVAL_P(member), node, NULL);
				if (node) {
					sxe_unlink_node(node);
				}
			} else {
				node = node->children;
				while (node) {
					nnext = node->next;

					SKIP_TEXT(node);

					if (xmlStrEqual(node->name, (xmlChar *)Z_STRVAL_P(member)) && match_ns(node, sxe->iter.nsprefix, sxe->iter.isprefix)) {
						sxe_unlink_node(node);
					}

next_iter:
					node = nnext;
				}
			}
		}
	}

	if (member == &tmp_zv) {
		zval_ptr_dtor_str(&tmp_zv);
	}
}
/* }}} */

/* {{{ sxe_property_delete() */
static void sxe_property_delete(zend_object *object, zend_string *name, void **cache_slot)
{
	zval member;
	ZVAL_STR(&member, name);
	sxe_prop_dim_delete(object, &member, 1, 0);
}
/* }}} */

/* {{{ sxe_dimension_unset() */
static void sxe_dimension_delete(zend_object *object, zval *offset)
{
	sxe_prop_dim_delete(object, offset, 0, 1);
}
/* }}} */

static inline zend_string *sxe_xmlNodeListGetString(xmlDocPtr doc, xmlNodePtr list, int inLine) /* {{{ */
{
	xmlChar *tmp = xmlNodeListGetString(doc, list, inLine);
	zend_string *res;

	if (tmp) {
		res = zend_string_init((char*)tmp, strlen((char *)tmp), 0);
		xmlFree(tmp);
	} else {
		res = ZSTR_EMPTY_ALLOC();
	}

	return res;
}
/* }}} */

/* {{{ get_base_node_value() */
static void get_base_node_value(php_sxe_object *sxe_ref, xmlNodePtr node, zval *value, zend_string *nsprefix, int isprefix)
{
	php_sxe_object *subnode;
	xmlChar        *contents;

	if (node->children && node->children->type == XML_TEXT_NODE && !xmlIsBlankNode(node->children)) {
		contents = xmlNodeListGetString(node->doc, node->children, 1);
		if (contents) {
			ZVAL_STRING(value, (char *)contents);
			xmlFree(contents);
		}
	} else {
		subnode = php_sxe_object_new(sxe_ref->zo.ce, sxe_ref->fptr_count);
		subnode->document = sxe_ref->document;
		subnode->document->refcount++;
		if (nsprefix && *ZSTR_VAL(nsprefix)) {
			subnode->iter.nsprefix = zend_string_copy(nsprefix);
			subnode->iter.isprefix = isprefix;
		}
		php_libxml_increment_node_ptr((php_libxml_node_object *)subnode, node, NULL);

		ZVAL_OBJ(value, &subnode->zo);
		/*zval_add_ref(value);*/
	}
}
/* }}} */

static void sxe_properties_add(HashTable *rv, char *name, int namelen, zval *value) /* {{{ */
{
	zend_string *key;
	zval  *data_ptr;
	zval  newptr;

	key = zend_string_init(name, namelen, 0);
	if ((data_ptr = zend_hash_find(rv, key)) != NULL) {
		if (Z_TYPE_P(data_ptr) == IS_ARRAY) {
			zend_hash_next_index_insert_new(Z_ARRVAL_P(data_ptr), value);
		} else {
			array_init(&newptr);
			zend_hash_next_index_insert_new(Z_ARRVAL(newptr), data_ptr);
			zend_hash_next_index_insert_new(Z_ARRVAL(newptr), value);
			ZVAL_ARR(data_ptr, Z_ARR(newptr));
		}
	} else {
		zend_hash_add_new(rv, key, value);
	}
	zend_string_release_ex(key, 0);
}
/* }}} */

static int sxe_prop_is_empty(zend_object *object) /* {{{ */
{
	php_sxe_object  *sxe;
	xmlNodePtr       node;
	xmlAttrPtr       attr;
	int              test;
	int              is_empty;
	bool             use_iter = false;

	sxe = php_sxe_fetch_object(object);

	GET_NODE(sxe, node);
	if (!node) {
		return 1;
	}

	if (sxe->iter.type == SXE_ITER_ELEMENT) {
		node = php_sxe_get_first_node_non_destructive(sxe, node);
	}
	if (node && node->type != XML_ENTITY_DECL) {
		attr = node->properties;
		test = sxe->iter.name && sxe->iter.type == SXE_ITER_ATTRLIST;
		while (attr) {
			if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && match_ns((xmlNodePtr)attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
				return 0;
			}
			attr = attr->next;
		}
	}

	GET_NODE(sxe, node);
	node = php_sxe_get_first_node_non_destructive(sxe, node);
	is_empty = 1;
	if (node && sxe->iter.type != SXE_ITER_ATTRLIST) {
		if (node->type == XML_ATTRIBUTE_NODE) {
			return 0;
		} else if (sxe->iter.type != SXE_ITER_CHILD) {
			if (sxe->iter.type == SXE_ITER_NONE || !node->children || !node->parent || node->children->next || node->children->children || node->parent->children == node->parent->last) {
				node = node->children;
			} else {
				node = php_sxe_reset_iterator_no_clear_iter_data(sxe, 0);
				use_iter = true;
			}
		}

		while (node) {
			if (node->children != NULL || node->prev != NULL || node->next != NULL) {
				SKIP_TEXT(node);
			} else {
				if (node->type == XML_TEXT_NODE) {
					const xmlChar *cur = node->content;
					if (*cur != 0) {
						is_empty = 0;
						break;
					}
					goto next_iter;
				}
			}

			if (node->type == XML_ELEMENT_NODE && (! match_ns(node, sxe->iter.nsprefix, sxe->iter.isprefix))) {
				goto next_iter;
			}

			if (!node->name) {
				goto next_iter;
			}

			is_empty = 0;
			break;
next_iter:
			if (use_iter) {
				node = php_sxe_iterator_fetch(sxe, node->next, 0);
			} else {
				node = node->next;
			}
		}
	}

	return is_empty;
}
/* }}} */

static HashTable *sxe_get_prop_hash(zend_object *object, int is_debug) /* {{{ */
{
	zval            value;
	zval            zattr;
	HashTable       *rv;
	php_sxe_object  *sxe;
	char            *name;
	xmlNodePtr       node;
	xmlAttrPtr       attr;
	int              namelen;
	int              test;
	bool 		 	 use_iter = false;

	sxe = php_sxe_fetch_object(object);

	if (is_debug) {
		rv = zend_new_array(0);
	} else if (sxe->properties) {
		zend_hash_clean(sxe->properties);
		rv = sxe->properties;
	} else {
		rv = zend_new_array(0);
		sxe->properties = rv;
	}

	GET_NODE(sxe, node);
	if (!node) {
		return rv;
	}
	if (is_debug || sxe->iter.type != SXE_ITER_CHILD) {
		if (sxe->iter.type == SXE_ITER_ELEMENT) {
			node = php_sxe_get_first_node_non_destructive(sxe, node);
		}
		if (node && node->type != XML_ENTITY_DECL) {
			attr = node->properties;
			ZVAL_UNDEF(&zattr);
			test = sxe->iter.name && sxe->iter.type == SXE_ITER_ATTRLIST;
			while (attr) {
				if ((!test || xmlStrEqual(attr->name, BAD_CAST ZSTR_VAL(sxe->iter.name))) && match_ns((xmlNodePtr)attr, sxe->iter.nsprefix, sxe->iter.isprefix)) {
					ZVAL_STR(&value, sxe_xmlNodeListGetString((xmlDocPtr) sxe->document->ptr, attr->children, 1));
					namelen = xmlStrlen(attr->name);
					if (Z_ISUNDEF(zattr)) {
						array_init(&zattr);
						sxe_properties_add(rv, "@attributes", sizeof("@attributes") - 1, &zattr);
					}
					add_assoc_zval_ex(&zattr, (char*)attr->name, namelen, &value);
				}
				attr = attr->next;
			}
		}
	}

	GET_NODE(sxe, node);
	node = php_sxe_get_first_node_non_destructive(sxe, node);

	if (node && sxe->iter.type != SXE_ITER_ATTRLIST) {
		if (node->type == XML_ATTRIBUTE_NODE) {
			ZVAL_STR(&value, sxe_xmlNodeListGetString(node->doc, node->children, 1));
			zend_hash_next_index_insert(rv, &value);
			node = NULL;
		} else if (sxe->iter.type != SXE_ITER_CHILD) {
			if ( sxe->iter.type == SXE_ITER_NONE || !node->children || !node->parent || !node->next || node->children->next || node->children->children || node->parent->children == node->parent->last ) {
				node = node->children;
			} else {
				node = php_sxe_reset_iterator_no_clear_iter_data(sxe, 0);
				use_iter = true;
			}
		}

		while (node) {
			if (node->children != NULL || node->prev != NULL || node->next != NULL || xmlIsBlankNode(node)) {
				SKIP_TEXT(node);
			} else {
				if (node->type == XML_TEXT_NODE) {
					const xmlChar *cur = node->content;

					if (*cur != 0) {
						ZVAL_STR(&value, sxe_xmlNodeListGetString(node->doc, node, 1));
						zend_hash_next_index_insert(rv, &value);
					}
					goto next_iter;
				}
			}

			if (node->type == XML_ELEMENT_NODE && (! match_ns(node, sxe->iter.nsprefix, sxe->iter.isprefix))) {
				goto next_iter;
			}

			name = (char *) node->name;
			if (!name) {
				goto next_iter;
			} else {
				namelen = xmlStrlen(node->name);
			}

			get_base_node_value(sxe, node, &value, sxe->iter.nsprefix, sxe->iter.isprefix);

			if ( use_iter ) {
				zend_hash_next_index_insert(rv, &value);
			} else {
				sxe_properties_add(rv, name, namelen, &value);
			}
next_iter:
			if (UNEXPECTED(node->type == XML_ENTITY_DECL)) {
				/* Entity decls are linked together via the next pointer.
				 * The only way to get to an entity decl is via an entity reference in the document.
				 * If we then continue iterating, we'll end up in the DTD. Even worse, if the entities reference each other we'll infinite loop. */
				break;
			}
			if (use_iter) {
				node = php_sxe_iterator_fetch(sxe, node->next, 0);
			} else {
				node = node->next;
			}
		}
	}

	return rv;
}
/* }}} */

static HashTable *sxe_get_gc(zend_object *object, zval **table, int *n) /* {{{ */ {
	php_sxe_object *sxe;
	sxe = php_sxe_fetch_object(object);

	*table = NULL;
	*n = 0;
	return sxe->properties;
}
/* }}} */

static HashTable *sxe_get_properties(zend_object *object) /* {{{ */
{
	return sxe_get_prop_hash(object, 0);
}
/* }}} */

static HashTable * sxe_get_debug_info(zend_object *object, int *is_temp) /* {{{ */
{
	*is_temp = 1;
	return sxe_get_prop_hash(object, 1);
}
/* }}} */

static int sxe_objects_compare(zval *object1, zval *object2) /* {{{ */
{
	php_sxe_object *sxe1;
	php_sxe_object *sxe2;

	ZEND_COMPARE_OBJECTS_FALLBACK(object1, object2);

	sxe1 = Z_SXEOBJ_P(object1);
	sxe2 = Z_SXEOBJ_P(object2);

	if (sxe1->node != NULL && sxe2->node != NULL) {
		/* Both nodes set: Only support equality comparison between nodes. */
		if (sxe1->node == sxe2->node) {
			return 0;
		}
		return ZEND_UNCOMPARABLE;
	}

	if (sxe1->node == NULL && sxe2->node == NULL) {
		/* Both nodes not set: Only support equality comparison between documents. */
		if (sxe1->document->ptr == sxe2->document->ptr) {
			return 0;
		}
		return ZEND_UNCOMPARABLE;
	}

	/* Only one of the nodes set: Cannot compare. */
	return ZEND_UNCOMPARABLE;
}
/* }}} */

/* {{{ Runs XPath query on the XML data */
PHP_METHOD(SimpleXMLElement, xpath)
{
	php_sxe_object    *sxe;
	zval               value;
	char              *query;
	size_t                query_len;
	int                i;
	int                nsnbr = 0;
	xmlNsPtr          *ns = NULL;
	xmlXPathObjectPtr  retval;
	xmlNodeSetPtr      result;
	xmlNodePtr		   nodeptr;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &query, &query_len) == FAILURE) {
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		return; /* attributes don't have attributes */
	}

	GET_NODE(sxe, nodeptr);
	nodeptr = php_sxe_get_first_node_non_destructive(sxe, nodeptr);
	if (!nodeptr) {
		return;
	}

	if (!sxe->xpath) {
		sxe->xpath = xmlXPathNewContext((xmlDocPtr) sxe->document->ptr);
	}
	sxe->xpath->node = nodeptr;

	ns = xmlGetNsList((xmlDocPtr) sxe->document->ptr, nodeptr);
	if (ns != NULL) {
		while (ns[nsnbr] != NULL) {
			nsnbr++;
		}
	}

	sxe->xpath->namespaces = ns;
	sxe->xpath->nsNr = nsnbr;

	retval = xmlXPathEval((xmlChar *)query, sxe->xpath);
	if (ns != NULL) {
		xmlFree(ns);
		sxe->xpath->namespaces = NULL;
		sxe->xpath->nsNr = 0;
	}

	if (!retval) {
		RETURN_FALSE;
	}

	result = retval->nodesetval;

	if (result != NULL) {
		array_init_size(return_value, result->nodeNr);
		zend_hash_real_init_packed(Z_ARRVAL_P(return_value));

		for (i = 0; i < result->nodeNr; ++i) {
			nodeptr = result->nodeTab[i];
			if (nodeptr->type == XML_TEXT_NODE || nodeptr->type == XML_ELEMENT_NODE || nodeptr->type == XML_ATTRIBUTE_NODE || nodeptr->type == XML_PI_NODE || nodeptr->type == XML_COMMENT_NODE) {
				/**
				 * Detect the case where the last selector is text(), simplexml
				 * always accesses the text() child by default, therefore we assign
				 * to the parent node.
				 */
				if (nodeptr->type == XML_TEXT_NODE) {
					node_as_zval(sxe, nodeptr->parent, &value, SXE_ITER_NONE, NULL, NULL, 0);
				} else if (nodeptr->type == XML_ATTRIBUTE_NODE) {
					node_as_zval_str(sxe, nodeptr->parent, &value, SXE_ITER_ATTRLIST, nodeptr->name, nodeptr->ns ? BAD_CAST nodeptr->ns->href : NULL, 0);
				} else {
					node_as_zval(sxe, nodeptr, &value, SXE_ITER_NONE, NULL, NULL, 0);
				}

				add_next_index_zval(return_value, &value);
			}
		}
	} else {
		RETVAL_EMPTY_ARRAY();
	}

	xmlXPathFreeObject(retval);
}
/* }}} */

/* {{{ Creates a prefix/ns context for the next XPath query */
PHP_METHOD(SimpleXMLElement, registerXPathNamespace)
{
	php_sxe_object    *sxe;
	size_t prefix_len, ns_uri_len;
	char *prefix, *ns_uri;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &prefix, &prefix_len, &ns_uri, &ns_uri_len) == FAILURE) {
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);
	if (!sxe->document) {
		zend_throw_error(NULL, "SimpleXMLElement is not properly initialized");
		RETURN_THROWS();
	}

	if (!sxe->xpath) {
		sxe->xpath = xmlXPathNewContext((xmlDocPtr) sxe->document->ptr);
	}

	if (xmlXPathRegisterNs(sxe->xpath, (xmlChar *)prefix, (xmlChar *)ns_uri) != 0) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}

/* }}} */

/* {{{ Return a well-formed XML string based on SimpleXML element */
PHP_METHOD(SimpleXMLElement, asXML)
{
	php_sxe_object     *sxe;
	xmlNodePtr          node;
	char               *filename = NULL;
	size_t                 filename_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|p!", &filename, &filename_len) == FAILURE) {
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);
	GET_NODE(sxe, node);
	node = php_sxe_get_first_node_non_destructive(sxe, node);

	if (!node) {
		RETURN_FALSE;
	}

	xmlDocPtr doc = sxe->document->ptr;

	if (filename) {
		zend_long bytes;
		if (node->parent && (XML_DOCUMENT_NODE == node->parent->type)) {
			bytes = sxe->document->handlers->dump_doc_to_file(filename, doc, false, (const char *) doc->encoding);
		} else {
			bytes = sxe->document->handlers->dump_node_to_file(filename, doc, node, false, NULL);
		}
		if (bytes == -1) {
			RETURN_FALSE;
		} else {
			RETURN_TRUE;
		}
	}

	zend_string *result;
	if (node->parent && (XML_DOCUMENT_NODE == node->parent->type)) {
		result = sxe->document->handlers->dump_doc_to_str(doc, 0, (const char *) doc->encoding);
	} else {
		result = sxe->document->handlers->dump_node_to_str(doc, node, false, (const char *) doc->encoding);
	}

	if (!result) {
		RETURN_FALSE;
	} else {
		/* Defense-in-depth: don't use the NEW variant in case somehow an empty string gets returned */
		RETURN_STR(result);
	}
}
/* }}} */

#define SXE_NS_PREFIX(ns) (ns->prefix ? (char*)ns->prefix : "")

static inline void sxe_add_namespace_name_raw(zval *return_value, const char *prefix, const char *href)
{
	zend_string *key = zend_string_init(prefix, strlen(prefix), 0);
	zval zv;

	if (!zend_hash_exists(Z_ARRVAL_P(return_value), key)) {
		ZVAL_STRING(&zv, href);
		zend_hash_add_new(Z_ARRVAL_P(return_value), key, &zv);
	}
	zend_string_release_ex(key, 0);
}

static inline void sxe_add_namespace_name(zval *return_value, xmlNsPtr ns) /* {{{ */
{
	char *prefix = SXE_NS_PREFIX(ns);
	sxe_add_namespace_name_raw(return_value, prefix, (const char *) ns->href);
}
/* }}} */

static void sxe_add_namespaces(php_sxe_object *sxe, xmlNodePtr node, bool recursive, zval *return_value) /* {{{ */
{
	xmlAttrPtr  attr;

	if (node->ns) {
		sxe_add_namespace_name(return_value, node->ns);
	}

	attr = node->properties;
	while (attr) {
		if (attr->ns) {
			sxe_add_namespace_name(return_value, attr->ns);
		}
		attr = attr->next;
	}

	if (recursive) {
		node = node->children;
		while (node) {
			if (node->type == XML_ELEMENT_NODE) {
				sxe_add_namespaces(sxe, node, recursive, return_value);
			}
			node = node->next;
		}
	}
} /* }}} */

static inline void sxe_object_free_iterxpath(php_sxe_object *sxe)
{
	if (!Z_ISUNDEF(sxe->iter.data)) {
		zval_ptr_dtor(&sxe->iter.data);
		ZVAL_UNDEF(&sxe->iter.data);
	}

	if (sxe->iter.name) {
		zend_string_release(sxe->iter.name);
		sxe->iter.name = NULL;
	}
	if (sxe->iter.nsprefix) {
		zend_string_release(sxe->iter.nsprefix);
		sxe->iter.nsprefix = NULL;
	}
	if (!Z_ISUNDEF(sxe->tmp)) {
		zval_ptr_dtor(&sxe->tmp);
		ZVAL_UNDEF(&sxe->tmp);
	}

	php_libxml_node_decrement_resource((php_libxml_node_object *)sxe);

	if (sxe->xpath) {
		xmlXPathFreeContext(sxe->xpath);
		sxe->xpath = NULL;
	}
}


/* {{{ Return all namespaces in use */
PHP_METHOD(SimpleXMLElement, getNamespaces)
{
	bool           recursive = 0;
	php_sxe_object     *sxe;
	xmlNodePtr          node;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|b", &recursive) == FAILURE) {
		RETURN_THROWS();
	}

	array_init(return_value);

	sxe = Z_SXEOBJ_P(ZEND_THIS);
	GET_NODE(sxe, node);
	node = php_sxe_get_first_node_non_destructive(sxe, node);

	if (node) {
		if (node->type == XML_ELEMENT_NODE) {
			sxe_add_namespaces(sxe, node, recursive, return_value);
		} else if (node->type == XML_ATTRIBUTE_NODE && node->ns) {
			sxe_add_namespace_name(return_value, node->ns);
		}
	}
}
/* }}} */

static void sxe_add_registered_namespaces(php_sxe_object *sxe, xmlNodePtr node, bool recursive, bool include_xmlns_attributes, zval *return_value) /* {{{ */
{
	xmlNsPtr ns;

	if (node->type == XML_ELEMENT_NODE) {
		ns = node->nsDef;
		while (ns != NULL) {
			sxe_add_namespace_name(return_value, ns);
			ns = ns->next;
		}
		if (include_xmlns_attributes) {
			for (const xmlAttr *attr = node->properties; attr; attr = attr->next) {
				/* Attributes in the xmlns namespace should be treated as namespace declarations too. */
				if (attr->ns && xmlStrEqual(attr->ns->href, (const xmlChar *) "http://www.w3.org/2000/xmlns/")) {
					const char *prefix = attr->ns->prefix ? (const char *) attr->name : "";
					bool free;
					xmlChar *href = php_libxml_attr_value(attr, &free);
					sxe_add_namespace_name_raw(return_value, prefix, (const char *) href);
					if (free) {
						xmlFree(href);
					}
				}
			}
		}
		if (recursive) {
			node = node->children;
			while (node) {
				sxe_add_registered_namespaces(sxe, node, recursive, include_xmlns_attributes, return_value);
				node = node->next;
			}
		}
	}
}
/* }}} */

/* {{{ Return all namespaces registered with document */
PHP_METHOD(SimpleXMLElement, getDocNamespaces)
{
	bool           recursive = 0, from_root = 1;
	php_sxe_object     *sxe;
	xmlNodePtr          node;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|bb", &recursive, &from_root) == FAILURE) {
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);
	if (from_root) {
		if (!sxe->document) {
			zend_throw_error(NULL, "SimpleXMLElement is not properly initialized");
			RETURN_THROWS();
		}

		node = xmlDocGetRootElement((xmlDocPtr)sxe->document->ptr);
	} else {
		GET_NODE(sxe, node);
	}

	if (node == NULL) {
		RETURN_FALSE;
	}

	/* Only do this for modern documents to keep BC. */
	bool include_xmlns_attributes = sxe->document->class_type == PHP_LIBXML_CLASS_MODERN;

	array_init(return_value);
	sxe_add_registered_namespaces(sxe, node, recursive, include_xmlns_attributes, return_value);
}
/* }}} */

/* {{{ Finds children of given node */
PHP_METHOD(SimpleXMLElement, children)
{
	php_sxe_object *sxe;
	zend_string    *nsprefix = NULL;
	xmlNodePtr      node;
	bool       isprefix = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|S!b", &nsprefix, &isprefix) == FAILURE) {
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		return; /* attributes don't have attributes */
	}

	GET_NODE(sxe, node);
	node = php_sxe_get_first_node_non_destructive(sxe, node);
	if (!node) {
		return;
	}

	node_as_zval(sxe, node, return_value, SXE_ITER_CHILD, NULL, nsprefix, isprefix);
}
/* }}} */

/* {{{ Returns name of given node */
PHP_METHOD(SimpleXMLElement, getName)
{
	php_sxe_object *sxe;
	xmlNodePtr      node;
	int             namelen;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);

	GET_NODE(sxe, node);
	node = php_sxe_get_first_node_non_destructive(sxe, node);
	if (node) {
		namelen = xmlStrlen(node->name);
		RETURN_STRINGL((char*)node->name, namelen);
	} else {
		RETURN_EMPTY_STRING();
	}
}
/* }}} */

/* {{{ Identifies an element's attributes */
PHP_METHOD(SimpleXMLElement, attributes)
{
	php_sxe_object *sxe;
	zend_string    *nsprefix = NULL;
	xmlNodePtr      node;
	bool       isprefix = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|S!b", &nsprefix, &isprefix) == FAILURE) {
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);
	GET_NODE(sxe, node);
	node = php_sxe_get_first_node_non_destructive(sxe, node);
	if (!node) {
		return;
	}

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		return; /* attributes don't have attributes */
	}

	node_as_zval(sxe, node, return_value, SXE_ITER_ATTRLIST, NULL, nsprefix, isprefix);
}
/* }}} */

/* {{{ Add Element with optional namespace information */
PHP_METHOD(SimpleXMLElement, addChild)
{
	php_sxe_object *sxe;
	char           *qname, *value = NULL, *nsuri = NULL;
	size_t             qname_len, value_len = 0, nsuri_len = 0;
	xmlNodePtr      node, newnode;
	xmlNsPtr        nsptr = NULL;
	xmlChar        *localname, *prefix = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|s!s!",
		&qname, &qname_len, &value, &value_len, &nsuri, &nsuri_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (qname_len == 0) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);
	GET_NODE(sxe, node);

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		php_error_docref(NULL, E_WARNING, "Cannot add element to attributes");
		return;
	}

	node = php_sxe_get_first_node_non_destructive(sxe, node);

	if (node == NULL) {
		php_error_docref(NULL, E_WARNING, "Cannot add child. Parent is not a permanent member of the XML tree");
		return;
	}

	php_libxml_invalidate_node_list_cache_from_doc(node->doc);

	localname = xmlSplitQName2((xmlChar *)qname, &prefix);
	if (localname == NULL) {
		localname = xmlStrdup((xmlChar *)qname);
	}

	newnode = xmlNewChild(node, NULL, localname, (xmlChar *)value);

	if (nsuri != NULL) {
		if (nsuri_len == 0) {
			newnode->ns = NULL;
			nsptr = xmlNewNs(newnode, (xmlChar *)nsuri, prefix);
		} else {
			nsptr = xmlSearchNsByHref(node->doc, node, (xmlChar *)nsuri);
			if (nsptr == NULL) {
				nsptr = xmlNewNs(newnode, (xmlChar *)nsuri, prefix);
			}
			newnode->ns = nsptr;
		}
	}

	node_as_zval_str(sxe, newnode, return_value, SXE_ITER_NONE, localname, prefix, 0);

	xmlFree(localname);
	if (prefix != NULL) {
		xmlFree(prefix);
	}
}
/* }}} */

/* {{{ Add Attribute with optional namespace information */
PHP_METHOD(SimpleXMLElement, addAttribute)
{
	php_sxe_object *sxe;
	char           *qname, *value = NULL, *nsuri = NULL;
	size_t             qname_len, value_len = 0, nsuri_len = 0;
	xmlNodePtr      node;
	xmlAttrPtr      attrp = NULL;
	xmlNsPtr        nsptr = NULL;
	xmlChar        *localname, *prefix = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss|s!",
		&qname, &qname_len, &value, &value_len, &nsuri, &nsuri_len) == FAILURE) {
		RETURN_THROWS();
	}

	if (qname_len == 0) {
		zend_argument_must_not_be_empty_error(1);
		RETURN_THROWS();
	}

	sxe = Z_SXEOBJ_P(ZEND_THIS);
	GET_NODE(sxe, node);

	node = php_sxe_get_first_node_non_destructive(sxe, node);

	if (node && node->type != XML_ELEMENT_NODE) {
		node = node->parent;
	}

	if (node == NULL) {
		php_error_docref(NULL, E_WARNING, "Unable to locate parent Element");
		return;
	}

	localname = xmlSplitQName2((xmlChar *)qname, &prefix);
	bool free_localname = localname != NULL;
	if (!free_localname) {
		if (nsuri_len > 0) {
			if (prefix != NULL) {
				xmlFree(prefix);
			}
			php_error_docref(NULL, E_WARNING, "Attribute requires prefix for namespace");
			return;
		}
		localname = (xmlChar *) qname;
	}

	attrp = xmlHasNsProp(node, localname, (xmlChar *)nsuri);
	if (attrp != NULL && attrp->type != XML_ATTRIBUTE_DECL) {
		php_error_docref(NULL, E_WARNING, "Attribute already exists");
		goto out;
	}

	if (nsuri != NULL) {
		nsptr = xmlSearchNsByHref(node->doc, node, (xmlChar *)nsuri);
		if (nsptr == NULL) {
			nsptr = xmlNewNs(node, (xmlChar *)nsuri, prefix);
		}
	}

	attrp = xmlNewNsProp(node, nsptr, localname, (xmlChar *)value);

out:
	if (free_localname) {
		xmlFree(localname);
	}
	if (prefix != NULL) {
		xmlFree(prefix);
	}
}
/* }}} */

/* {{{ cast_object() */
static zend_result cast_object(zval *object, int type, char *contents)
{
	if (contents) {
		ZVAL_STRINGL(object, contents, strlen(contents));
	} else {
		ZVAL_NULL(object);
	}

	switch (type) {
		case IS_STRING:
			convert_to_string(object);
			break;
		case IS_LONG:
			convert_to_long(object);
			break;
		case IS_DOUBLE:
			convert_to_double(object);
			break;
		case _IS_NUMBER:
			convert_scalar_to_number(object);
			break;
		default:
			return FAILURE;
	}
	return SUCCESS;
}
/* }}} */

/* {{{ sxe_object_cast() */
static zend_result sxe_object_cast_ex(zend_object *readobj, zval *writeobj, int type)
{
	php_sxe_object *sxe;
	xmlChar        *contents = NULL;
	bool            free_contents = true;
	xmlNodePtr	    node;
	zend_result rv;

	sxe = php_sxe_fetch_object(readobj);

	if (type == _IS_BOOL) {
		node = php_sxe_get_first_node_non_destructive(sxe, NULL);
		if (node) {
			ZVAL_TRUE(writeobj);
		} else {
			ZVAL_BOOL(writeobj, !sxe_prop_is_empty(readobj));
		}
		return SUCCESS;
	}

	if (sxe->iter.type != SXE_ITER_NONE) {
		node = php_sxe_get_first_node_non_destructive(sxe, NULL);
		if (node) {
			contents = xmlNodeListGetString((xmlDocPtr) sxe->document->ptr, node->children, 1);
		}
	} else {
		if (!sxe->node) {
			if (sxe->document) {
				php_libxml_increment_node_ptr((php_libxml_node_object *)sxe, xmlDocGetRootElement((xmlDocPtr) sxe->document->ptr), NULL);
			}
		}

		if (sxe->node && sxe->node->node) {
			if (sxe->node->node->children) {
				contents = xmlNodeListGetString((xmlDocPtr) sxe->document->ptr, sxe->node->node->children, 1);
			} else if (sxe->node->node->type == XML_COMMENT_NODE || sxe->node->node->type == XML_PI_NODE) {
				contents = sxe->node->node->content;
				free_contents = false;
			}
		}
	}

	rv = cast_object(writeobj, type, (char *)contents);

	if (contents && free_contents) {
		xmlFree(contents);
	}

	return rv;
}
/* }}} */

/*  {{{ Variant of sxe_object_cast_ex that handles overwritten __toString() method */
static zend_result sxe_object_cast(zend_object *readobj, zval *writeobj, int type)
{
	if (type == IS_STRING
		&& zend_std_cast_object_tostring(readobj, writeobj, IS_STRING) == SUCCESS
	) {
		return SUCCESS;
	}

	return sxe_object_cast_ex(readobj, writeobj, type);
}
/* }}} */

/* {{{ Returns the string content */
PHP_METHOD(SimpleXMLElement, __toString)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	zend_result rv = sxe_object_cast_ex(Z_OBJ_P(ZEND_THIS), return_value, IS_STRING);
	ZEND_IGNORE_VALUE(rv);
	ZEND_ASSERT(rv == SUCCESS);
}
/* }}} */

static zend_long php_sxe_count_elements_helper(php_sxe_object *sxe) /* {{{ */
{
	zend_long count = 0;
	xmlNodePtr node = php_sxe_reset_iterator_no_clear_iter_data(sxe, 0);

	while (node)
	{
		count++;
		node = php_sxe_iterator_fetch(sxe, node->next, 0);
	}

	return count;
}
/* }}} */

static zend_result sxe_count_elements(zend_object *object, zend_long *count) /* {{{ */
{
	php_sxe_object  *intern;
	intern = php_sxe_fetch_object(object);
	if (intern->fptr_count) {
		zval rv;
		zend_call_method_with_0_params(object, intern->zo.ce, &intern->fptr_count, "count", &rv);
		if (!Z_ISUNDEF(rv)) {
			/* TODO: change this to Z_LVAL_P() once the tentative type on count() is gone. */
			*count = zval_get_long(&rv);
			zval_ptr_dtor(&rv);
			return SUCCESS;
		}
		return FAILURE;
	}
	*count = php_sxe_count_elements_helper(intern);
	return SUCCESS;
}
/* }}} */

/* {{{ Get number of child elements */
PHP_METHOD(SimpleXMLElement, count)
{
	php_sxe_object *sxe = Z_SXEOBJ_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	RETURN_LONG(php_sxe_count_elements_helper(sxe));
}
/* }}} */


/* {{{ Rewind to first element */
PHP_METHOD(SimpleXMLElement, rewind)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	php_sxe_rewind_iterator(Z_SXEOBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ Check whether iteration is valid */
PHP_METHOD(SimpleXMLElement, valid)
{
	php_sxe_object *sxe = Z_SXEOBJ_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	RETURN_BOOL(!Z_ISUNDEF(sxe->iter.data));
}
/* }}} */

/* {{{ Get current element */
PHP_METHOD(SimpleXMLElement, current)
{
	php_sxe_object *sxe = Z_SXEOBJ_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (Z_ISUNDEF(sxe->iter.data)) {
		zend_throw_error(NULL, "Iterator not initialized or already consumed");
		RETURN_THROWS();
	}

	RETURN_COPY_DEREF(&sxe->iter.data);
}
/* }}} */

/* {{{ Get name of current child element */
PHP_METHOD(SimpleXMLElement, key)
{
	xmlNodePtr curnode;
	php_sxe_object *intern;
	php_sxe_object *sxe = Z_SXEOBJ_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (Z_ISUNDEF(sxe->iter.data)) {
		zend_throw_error(NULL, "Iterator not initialized or already consumed");
		RETURN_THROWS();
	}

	intern = Z_SXEOBJ_P(&sxe->iter.data);
	if (intern == NULL || intern->node == NULL) {
		zend_throw_error(NULL, "Iterator not initialized or already consumed");
		RETURN_THROWS();
	}

	curnode = (xmlNodePtr)((php_libxml_node_ptr *)intern->node)->node;
	RETURN_STRINGL((char*)curnode->name, xmlStrlen(curnode->name));
}
/* }}} */

/* {{{ Move to next element */
PHP_METHOD(SimpleXMLElement, next)
{
	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	php_sxe_move_forward_iterator(Z_SXEOBJ_P(ZEND_THIS));
}
/* }}} */

/* {{{ Check whether element has children (elements) */
PHP_METHOD(SimpleXMLElement, hasChildren)
{
	php_sxe_object *sxe = Z_SXEOBJ_P(ZEND_THIS);
	php_sxe_object *child;
	xmlNodePtr      node;

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (Z_ISUNDEF(sxe->iter.data) || sxe->iter.type == SXE_ITER_ATTRLIST) {
		RETURN_FALSE;
	}
	child = Z_SXEOBJ_P(&sxe->iter.data);

	GET_NODE(child, node);
	if (node) {
		node = node->children;
	}
	while (node && node->type != XML_ELEMENT_NODE) {
		node = node->next;
	}
	RETURN_BOOL(node ? 1 : 0);
}
/* }}} */

/* {{{ Get child element iterator */
PHP_METHOD(SimpleXMLElement, getChildren)
{
	php_sxe_object *sxe = Z_SXEOBJ_P(ZEND_THIS);

	if (zend_parse_parameters_none() == FAILURE) {
		RETURN_THROWS();
	}

	if (Z_ISUNDEF(sxe->iter.data) || sxe->iter.type == SXE_ITER_ATTRLIST) {
		return; /* return NULL */
	}

	RETURN_COPY_DEREF(&sxe->iter.data);
}

static zend_object_handlers sxe_object_handlers;

/* {{{ sxe_object_clone() */
static zend_object *
sxe_object_clone(zend_object *object)
{
	php_sxe_object *sxe = php_sxe_fetch_object(object);
	php_sxe_object *clone;
	xmlNodePtr nodep = NULL;
	xmlDocPtr docp = NULL;
	bool is_root_element = sxe->node && sxe->node->node && sxe->node->node->parent
		&& (sxe->node->node->parent->type == XML_DOCUMENT_NODE || sxe->node->node->parent->type == XML_HTML_DOCUMENT_NODE);

	clone = php_sxe_object_new(sxe->zo.ce, sxe->fptr_count);

	if (is_root_element) {
		docp = xmlCopyDoc(sxe->document->ptr, 1);
		php_libxml_increment_doc_ref((php_libxml_node_object *)clone, docp);
	} else {
		clone->document = sxe->document;
		if (clone->document) {
			clone->document->refcount++;
			docp = clone->document->ptr;
		}
	}

	clone->iter.isprefix = sxe->iter.isprefix;
	if (sxe->iter.name != NULL) {
		clone->iter.name = zend_string_copy(sxe->iter.name);
	}
	if (sxe->iter.nsprefix != NULL) {
		clone->iter.nsprefix = zend_string_copy(sxe->iter.nsprefix);
	}
	clone->iter.type = sxe->iter.type;

	if (sxe->node) {
		if (is_root_element) {
			nodep = xmlDocGetRootElement(docp);
		} else {
			nodep = xmlDocCopyNode(sxe->node->node, docp, 1);
		}
	}

	php_libxml_increment_node_ptr((php_libxml_node_object *)clone, nodep, NULL);

	return &clone->zo;
}
/* }}} */

/* {{{ sxe_object_free_storage() */
static void sxe_object_free_storage(zend_object *object)
{
	php_sxe_object *sxe;

	sxe = php_sxe_fetch_object(object);

	zend_object_std_dtor(&sxe->zo);

	sxe_object_free_iterxpath(sxe);

	if (sxe->properties) {
		ZEND_ASSERT(!(GC_FLAGS(sxe->properties) & IS_ARRAY_IMMUTABLE));
		zend_hash_release(sxe->properties);
	}
}
/* }}} */

/* {{{ php_sxe_find_fptr_count() */
static zend_function* php_sxe_find_fptr_count(zend_class_entry *ce)
{
	zend_function *fptr_count = NULL;
	zend_class_entry *parent = ce;
	int inherited = 0;

	while (parent) {
		if (parent == ce_SimpleXMLElement) {
			break;
		}
		parent = parent->parent;
		inherited = 1;
	}

	if (inherited) {
		/* Find count() method */
		fptr_count = zend_hash_find_ptr(&ce->function_table, ZSTR_KNOWN(ZEND_STR_COUNT));
		if (fptr_count->common.scope == parent) {
			fptr_count = NULL;
		}
	}

	return fptr_count;
}
/* }}} */

/* {{{ php_sxe_object_new() */
static php_sxe_object* php_sxe_object_new(zend_class_entry *ce, zend_function *fptr_count)
{
	php_sxe_object *intern;

	intern = zend_object_alloc(sizeof(php_sxe_object), ce);

	intern->iter.type = SXE_ITER_NONE;
	intern->iter.nsprefix = NULL;
	intern->iter.name = NULL;
	intern->fptr_count = fptr_count;

	zend_object_std_init(&intern->zo, ce);
	object_properties_init(&intern->zo, ce);

	return intern;
}
/* }}} */

/* {{{ sxe_object_new() */
PHP_SXE_API zend_object *
sxe_object_new(zend_class_entry *ce)
{
	php_sxe_object    *intern;

	intern = php_sxe_object_new(ce, php_sxe_find_fptr_count(ce));
	return &intern->zo;
}
/* }}} */

/* {{{ Load a filename and return a simplexml_element object to allow for processing */
PHP_FUNCTION(simplexml_load_file)
{
	php_sxe_object *sxe;
	char           *filename;
	size_t             filename_len;
	xmlDocPtr       docp;
	zend_string      *ns = zend_empty_string;
	zend_long            options = 0;
	zend_class_entry *ce= ce_SimpleXMLElement;
	zend_function    *fptr_count;
	bool       isprefix = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "p|C!lSb", &filename, &filename_len, &ce, &options, &ns, &isprefix) == FAILURE) {
		RETURN_THROWS();
	}

	if (ZEND_LONG_EXCEEDS_INT(options)) {
		zend_argument_value_error(3, "is too large");
		RETURN_THROWS();
	}

	PHP_LIBXML_SANITIZE_GLOBALS(read_file);
	docp = xmlReadFile(filename, NULL, (int)options);
	PHP_LIBXML_RESTORE_GLOBALS(read_file);

	if (!docp) {
		RETURN_FALSE;
	}

	if (!ce) {
		ce = ce_SimpleXMLElement;
		fptr_count = NULL;
	} else {
		fptr_count = php_sxe_find_fptr_count(ce);
	}
	sxe = php_sxe_object_new(ce, fptr_count);
	sxe->iter.nsprefix = ZSTR_LEN(ns) ? zend_string_copy(ns) : NULL;
	sxe->iter.isprefix = isprefix;
	php_libxml_increment_doc_ref((php_libxml_node_object *)sxe, docp);
	php_libxml_increment_node_ptr((php_libxml_node_object *)sxe, xmlDocGetRootElement(docp), NULL);

	RETURN_OBJ(&sxe->zo);
}
/* }}} */

/* {{{ Load a string and return a simplexml_element object to allow for processing */
PHP_FUNCTION(simplexml_load_string)
{
	php_sxe_object *sxe;
	char           *data;
	size_t             data_len;
	xmlDocPtr       docp;
	zend_string      *ns = zend_empty_string;
	zend_long            options = 0;
	zend_class_entry *ce= ce_SimpleXMLElement;
	zend_function    *fptr_count;
	bool       isprefix = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|C!lSb", &data, &data_len, &ce, &options, &ns, &isprefix) == FAILURE) {
		RETURN_THROWS();
	}

	if (ZEND_SIZE_T_INT_OVFL(data_len)) {
		zend_argument_value_error(1, "is too long");
		RETURN_THROWS();
	}
	if (ZEND_SIZE_T_INT_OVFL(ZSTR_LEN(ns))) {
		zend_argument_value_error(4, "is too long");
		RETURN_THROWS();
	}
	if (ZEND_LONG_EXCEEDS_INT(options)) {
		zend_argument_value_error(3, "is too large");
		RETURN_THROWS();
	}

	PHP_LIBXML_SANITIZE_GLOBALS(read_memory);
	docp = xmlReadMemory(data, (int)data_len, NULL, NULL, (int)options);
	PHP_LIBXML_RESTORE_GLOBALS(read_memory);

	if (!docp) {
		RETURN_FALSE;
	}

	if (!ce) {
		ce = ce_SimpleXMLElement;
		fptr_count = NULL;
	} else {
		fptr_count = php_sxe_find_fptr_count(ce);
	}
	sxe = php_sxe_object_new(ce, fptr_count);
	sxe->iter.nsprefix = ZSTR_LEN(ns) ? zend_string_copy(ns) : NULL;
	sxe->iter.isprefix = isprefix;
	php_libxml_increment_doc_ref((php_libxml_node_object *)sxe, docp);
	php_libxml_increment_node_ptr((php_libxml_node_object *)sxe, xmlDocGetRootElement(docp), NULL);

	RETURN_OBJ(&sxe->zo);
}
/* }}} */

/* {{{ SimpleXMLElement constructor */
PHP_METHOD(SimpleXMLElement, __construct)
{
	php_sxe_object *sxe = Z_SXEOBJ_P(ZEND_THIS);
	char           *data;
	zend_string    *ns = zend_empty_string;
	size_t             data_len;
	xmlDocPtr       docp;
	zend_long            options = 0;
	bool       is_url = 0, isprefix = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|lbSb", &data, &data_len, &options, &is_url, &ns, &isprefix) == FAILURE) {
		RETURN_THROWS();
	}

	if (ZEND_SIZE_T_INT_OVFL(data_len)) {
		zend_argument_error(zend_ce_exception, 1, "is too long");
		RETURN_THROWS();
	}
	if (ZEND_SIZE_T_INT_OVFL(ZSTR_LEN(ns))) {
		zend_argument_error(zend_ce_exception, 4, "is too long");
		RETURN_THROWS();
	}
	if (ZEND_LONG_EXCEEDS_INT(options)) {
		zend_argument_error(zend_ce_exception, 2, "is invalid");
		RETURN_THROWS();
	}

	PHP_LIBXML_SANITIZE_GLOBALS(read_file_or_memory);
	docp = is_url ? xmlReadFile(data, NULL, (int)options) : xmlReadMemory(data, (int)data_len, NULL, NULL, (int)options);
	PHP_LIBXML_RESTORE_GLOBALS(read_file_or_memory);

	if (!docp) {
		zend_throw_exception(zend_ce_exception, "String could not be parsed as XML", 0);
		RETURN_THROWS();
	}

	sxe_object_free_iterxpath(sxe);

	sxe->iter.nsprefix = ZSTR_LEN(ns) ? zend_string_copy(ns) : NULL;
	sxe->iter.isprefix = isprefix;
	php_libxml_increment_doc_ref((php_libxml_node_object *)sxe, docp);
	php_libxml_increment_node_ptr((php_libxml_node_object *)sxe, xmlDocGetRootElement(docp), NULL);
}
/* }}} */

static const zend_object_iterator_funcs php_sxe_iterator_funcs = { /* {{{ */
	php_sxe_iterator_dtor,
	php_sxe_iterator_valid,
	php_sxe_iterator_current_data,
	php_sxe_iterator_current_key,
	php_sxe_iterator_move_forward,
	php_sxe_iterator_rewind,
	NULL,
	NULL, /* get_gc */
};
/* }}} */

static xmlNodePtr php_sxe_iterator_fetch(php_sxe_object *sxe, xmlNodePtr node, int use_data) /* {{{ */
{
	zend_string *prefix  = sxe->iter.nsprefix;
	int isprefix  = sxe->iter.isprefix;

	if (sxe->iter.type == SXE_ITER_ATTRLIST) {
		if (sxe->iter.name) {
			while (node) {
				if (node->type == XML_ATTRIBUTE_NODE) {
					if (xmlStrEqual(node->name, BAD_CAST ZSTR_VAL(sxe->iter.name)) && match_ns(node, prefix, isprefix)) {
						break;
					}
				}
				node = node->next;
			}
		} else {
			while (node) {
				if (node->type == XML_ATTRIBUTE_NODE) {
					if (match_ns(node, prefix, isprefix)) {
						break;
					}
				}
				node = node->next;
			}
		}
	} else if (sxe->iter.type == SXE_ITER_ELEMENT && sxe->iter.name) {
		while (node) {
			if (node->type == XML_ELEMENT_NODE) {
				if (xmlStrEqual(node->name, BAD_CAST ZSTR_VAL(sxe->iter.name)) && match_ns(node, prefix, isprefix)) {
					break;
				}
			}
			node = node->next;
		}
	} else {
		while (node) {
			if (node->type == XML_ELEMENT_NODE) {
				if (match_ns(node, prefix, isprefix)) {
					break;
				}
			}
			node = node->next;
		}
	}

	if (node && use_data) {
		node_as_zval(sxe, node, &sxe->iter.data, SXE_ITER_NONE, NULL, prefix, isprefix);
	}

	return node;
}
/* }}} */

static xmlNodePtr php_sxe_reset_iterator_no_clear_iter_data(php_sxe_object *sxe, int use_data)
{
	xmlNodePtr node;
	GET_NODE(sxe, node)

	if (node) {
		switch (sxe->iter.type) {
			case SXE_ITER_ELEMENT:
			case SXE_ITER_CHILD:
			case SXE_ITER_NONE:
				node = node->children;
				break;
			case SXE_ITER_ATTRLIST:
				node = (xmlNodePtr) node->properties;
		}
		if (use_data) {
			ZEND_ASSERT(Z_ISUNDEF(sxe->iter.data));
		}
		return php_sxe_iterator_fetch(sxe, node, use_data);
	}
	return NULL;
}

static xmlNodePtr php_sxe_reset_iterator(php_sxe_object *sxe) /* {{{ */
{
	if (!Z_ISUNDEF(sxe->iter.data)) {
		zval_ptr_dtor(&sxe->iter.data);
		ZVAL_UNDEF(&sxe->iter.data);
	}

	return php_sxe_reset_iterator_no_clear_iter_data(sxe, 1);
}
/* }}} */

zend_object_iterator *php_sxe_get_iterator(zend_class_entry *ce, zval *object, int by_ref) /* {{{ */
{
	php_sxe_iterator *iterator;

	if (by_ref) {
		zend_throw_error(NULL, "An iterator cannot be used with foreach by reference");
		return NULL;
	}
	iterator = emalloc(sizeof(php_sxe_iterator));
	zend_iterator_init(&iterator->intern);

	ZVAL_OBJ_COPY(&iterator->intern.data, Z_OBJ_P(object));
	iterator->intern.funcs = &php_sxe_iterator_funcs;
	iterator->sxe = Z_SXEOBJ_P(object);

	return (zend_object_iterator*)iterator;
}
/* }}} */

static void php_sxe_iterator_dtor(zend_object_iterator *iter) /* {{{ */
{
	php_sxe_iterator *iterator = (php_sxe_iterator *)iter;

	/* cleanup handled in sxe_object_dtor as we don't always have an iterator wrapper */
	if (!Z_ISUNDEF(iterator->intern.data)) {
		zval_ptr_dtor(&iterator->intern.data);
	}
}
/* }}} */

static zend_result php_sxe_iterator_valid(zend_object_iterator *iter) /* {{{ */
{
	php_sxe_iterator *iterator = (php_sxe_iterator *)iter;

	return Z_ISUNDEF(iterator->sxe->iter.data) ? FAILURE : SUCCESS;
}
/* }}} */

static zval *php_sxe_iterator_current_data(zend_object_iterator *iter) /* {{{ */
{
	php_sxe_iterator *iterator = (php_sxe_iterator *)iter;

	zval *data = &iterator->sxe->iter.data;
	if (Z_ISUNDEF_P(data)) {
		return NULL;
	}
	return data;
}
/* }}} */

static void php_sxe_iterator_current_key(zend_object_iterator *iter, zval *key) /* {{{ */
{
	php_sxe_iterator *iterator = (php_sxe_iterator *)iter;
	zval *curobj = &iterator->sxe->iter.data;
	if (Z_ISUNDEF_P(curobj)) {
		ZVAL_NULL(key);
		return;
	}

	php_sxe_object *intern = Z_SXEOBJ_P(curobj);

	xmlNodePtr curnode = NULL;
	if (intern != NULL && intern->node != NULL) {
		curnode = (xmlNodePtr)((php_libxml_node_ptr *)intern->node)->node;
	}

	if (curnode) {
		ZVAL_STRINGL(key, (char *) curnode->name, xmlStrlen(curnode->name));
	} else {
		ZVAL_NULL(key);
	}
}
/* }}} */

PHP_SXE_API void php_sxe_move_forward_iterator(php_sxe_object *sxe) /* {{{ */
{
	xmlNodePtr      node = NULL;
	php_sxe_object  *intern;

	if (!Z_ISUNDEF(sxe->iter.data)) {
		intern = Z_SXEOBJ_P(&sxe->iter.data);
		GET_NODE(intern, node)
		zval_ptr_dtor(&sxe->iter.data);
		ZVAL_UNDEF(&sxe->iter.data);
	}

	if (node) {
		php_sxe_iterator_fetch(sxe, node->next, 1);
	}
}
/* }}} */

static void php_sxe_iterator_move_forward(zend_object_iterator *iter) /* {{{ */
{
	php_sxe_iterator *iterator = (php_sxe_iterator *)iter;
	php_sxe_move_forward_iterator(iterator->sxe);
}
/* }}} */

PHP_SXE_API void php_sxe_rewind_iterator(php_sxe_object *sxe) /* {{{ */
{
	php_sxe_reset_iterator(sxe);
}
/* }}} */

static void php_sxe_iterator_rewind(zend_object_iterator *iter) /* {{{ */
{
	php_sxe_object	*sxe;

	php_sxe_iterator *iterator = (php_sxe_iterator *)iter;
	sxe = iterator->sxe;

	php_sxe_reset_iterator(sxe);
}
/* }}} */

void *simplexml_export_node(zval *object) /* {{{ */
{
	php_sxe_object *sxe;
	xmlNodePtr node;

	sxe = Z_SXEOBJ_P(object);
	GET_NODE(sxe, node);
	return php_sxe_get_first_node_non_destructive(sxe, node);
}
/* }}} */

/* {{{ Get a simplexml_element object from dom to allow for processing */
PHP_FUNCTION(simplexml_import_dom)
{
	php_sxe_object *sxe;
	zval *node;
	php_libxml_node_object *object;
	xmlNodePtr		nodep = NULL;
	zend_class_entry *ce = ce_SimpleXMLElement;
	zend_function    *fptr_count;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "o|C!", &node, &ce) == FAILURE) {
		RETURN_THROWS();
	}

	nodep = php_libxml_import_node(node);

	if (!nodep) {
		zend_argument_type_error(1, "must be a valid XML node");
		RETURN_THROWS();
	}

	if (nodep->doc == NULL) {
		php_error_docref(NULL, E_WARNING, "Imported Node must have associated Document");
		RETURN_NULL();
	}

	if (nodep->type == XML_DOCUMENT_NODE || nodep->type == XML_HTML_DOCUMENT_NODE) {
		nodep = xmlDocGetRootElement((xmlDocPtr) nodep);
	}

	if (nodep && nodep->type == XML_ELEMENT_NODE) {
		if (!ce) {
			ce = ce_SimpleXMLElement;
			fptr_count = NULL;
		} else {
			fptr_count = php_sxe_find_fptr_count(ce);
		}

		object = Z_LIBXML_NODE_P(node);

		sxe = php_sxe_object_new(ce, fptr_count);
		sxe->document = object->document;
		php_libxml_increment_doc_ref((php_libxml_node_object *)sxe, nodep->doc);
		php_libxml_increment_node_ptr((php_libxml_node_object *)sxe, nodep, NULL);

		RETURN_OBJ(&sxe->zo);
	} else {
		php_error_docref(NULL, E_WARNING, "Invalid Nodetype to import");
		RETVAL_NULL();
	}
}
/* }}} */

static const zend_module_dep simplexml_deps[] = { /* {{{ */
	ZEND_MOD_REQUIRED("libxml")
	ZEND_MOD_REQUIRED("spl")
	ZEND_MOD_END
};
/* }}} */

zend_module_entry simplexml_module_entry = { /* {{{ */
	STANDARD_MODULE_HEADER_EX, NULL,
	simplexml_deps,
	"SimpleXML",
	ext_functions,
	PHP_MINIT(simplexml),
	PHP_MSHUTDOWN(simplexml),
	NULL,
	NULL,
	PHP_MINFO(simplexml),
	PHP_SIMPLEXML_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_SIMPLEXML
ZEND_GET_MODULE(simplexml)
#endif

/* {{{ PHP_MINIT_FUNCTION(simplexml) */
PHP_MINIT_FUNCTION(simplexml)
{
	ce_SimpleXMLElement = register_class_SimpleXMLElement(zend_ce_stringable, zend_ce_countable, spl_ce_RecursiveIterator);
	ce_SimpleXMLElement->create_object = sxe_object_new;
	ce_SimpleXMLElement->default_object_handlers = &sxe_object_handlers;
	ce_SimpleXMLElement->get_iterator = php_sxe_get_iterator;

	memcpy(&sxe_object_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	sxe_object_handlers.offset = XtOffsetOf(php_sxe_object, zo);
	sxe_object_handlers.free_obj = sxe_object_free_storage;
	sxe_object_handlers.clone_obj = sxe_object_clone;
	sxe_object_handlers.read_property = sxe_property_read;
	sxe_object_handlers.write_property = sxe_property_write;
	sxe_object_handlers.read_dimension = sxe_dimension_read;
	sxe_object_handlers.write_dimension = sxe_dimension_write;
	sxe_object_handlers.get_property_ptr_ptr = sxe_property_get_adr;
	sxe_object_handlers.has_property = sxe_property_exists;
	sxe_object_handlers.unset_property = sxe_property_delete;
	sxe_object_handlers.has_dimension = sxe_dimension_exists;
	sxe_object_handlers.unset_dimension = sxe_dimension_delete;
	sxe_object_handlers.get_properties = sxe_get_properties;
	sxe_object_handlers.compare = sxe_objects_compare;
	sxe_object_handlers.cast_object = sxe_object_cast;
	sxe_object_handlers.count_elements = sxe_count_elements;
	sxe_object_handlers.get_debug_info = sxe_get_debug_info;
	sxe_object_handlers.get_closure = NULL;
	sxe_object_handlers.get_gc = sxe_get_gc;

	ce_SimpleXMLIterator = register_class_SimpleXMLIterator(ce_SimpleXMLElement);

	php_libxml_register_export(ce_SimpleXMLElement, simplexml_export_node);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION(simplexml) */
PHP_MSHUTDOWN_FUNCTION(simplexml)
{
	ce_SimpleXMLElement = NULL;
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION(simplexml) */
PHP_MINFO_FUNCTION(simplexml)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "SimpleXML support", "enabled");
	php_info_print_table_row(2, "Schema support",
#ifdef LIBXML_SCHEMAS_ENABLED
		"enabled");
#else
		"not available");
#endif
	php_info_print_table_end();
}
/* }}} */

#endif
