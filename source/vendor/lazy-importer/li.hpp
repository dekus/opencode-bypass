#ifndef LI_HPP
#define LI_HPP

#include "lazy_importer.hpp"

/* forwarded_safe_cached — normal modules */
#define LI(name) \
	::li::detail::lazy_function<LAZY_IMPORTER_KHASH(#name), decltype(&name)>().forwarded_safe_cached()

/* forwarded_safe — shellcode / PIC (no static cache) */
#define LI_SC(name) \
	::li::detail::lazy_function<LAZY_IMPORTER_KHASH(#name), decltype(&name)>().forwarded_safe()

#endif
