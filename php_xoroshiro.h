#ifndef PHP_XOROSHIRO_H
#define PHP_XOROSHIRO_H

#ifdef  __cplusplus
extern "C" {
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#define PHP_XOROSHIRO_VERSION "1.0.0"
#define PHP_XOROSHIRO_EXTNAME "xoroshiro"

extern zend_module_entry xoroshiro_module_entry;
#define phpext_xoroshiro_ptr &xoroshiro_module_entry

extern zend_class_entry *xoroshiro_ce;
extern zend_class_entry *xoroshiro_splitter_ce;

#ifdef __cplusplus
}
#endif

#endif /* PHP_XOROSHIRO_H */