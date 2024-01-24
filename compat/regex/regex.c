/* Extended regular expression matching and search library.
   Copyright (C) 2002-2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Isamu Hasegawa <isamu@yamato.ibm.com>.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

#define __STDC_WANT_IEC_60559_BFP_EXT__

#include <stdint.h>
#include <stdbool.h>

#define __USE_GNU
#define __STRICT_ANSI__
#define assume(x)
#define __glibc_unlikely(x) (x)
#define __glibc_likely(x) (x)
#define libc_hidden_def(name)
#define weak_alias(name, aliasname)
#define __always_inline inline
#define nl_langinfo(x) "UTF8"
#define FALLTHROUGH /* fallthru */
#define uint_fast32_t uint32_t

/* This imitates the `RESULT_MUST_BE_USED` macro in `git-compat-util.h` */
/* The sentinel attribute is valid from gcc version 4.0 */
#if defined(__GNUC__) && (__GNUC__ >= 4)
/* warn_unused_result exists as of gcc 3.4.0, but be lazy and check 4.0 */
#define __attribute_warn_unused_result__ __attribute__ ((warn_unused_result))
#else
#define __attribute_warn_unused_result__
#endif

/* True if the real type T is signed.  */
#define TYPE_SIGNED(t) (! ((t) 0 < (t) -1))

/* Make sure no one compiles this code with a C++ compiler.  */
#if defined __cplusplus && defined _LIBC
# error "This is C code, use a C compiler"
#endif

#ifdef _LIBC
/* We have to keep the namespace clean.  */
# define regfree(preg) __regfree (preg)
# define regexec(pr, st, nm, pm, ef) __regexec (pr, st, nm, pm, ef)
# define regcomp(preg, pattern, cflags) __regcomp (preg, pattern, cflags)
# define regerror(errcode, preg, errbuf, errbuf_size) \
	__regerror(errcode, preg, errbuf, errbuf_size)
# define re_set_registers(bu, re, nu, st, en) \
	__re_set_registers (bu, re, nu, st, en)
# define re_match_2(bufp, string1, size1, string2, size2, pos, regs, stop) \
	__re_match_2 (bufp, string1, size1, string2, size2, pos, regs, stop)
# define re_match(bufp, string, size, pos, regs) \
	__re_match (bufp, string, size, pos, regs)
# define re_search(bufp, string, size, startpos, range, regs) \
	__re_search (bufp, string, size, startpos, range, regs)
# define re_compile_pattern(pattern, length, bufp) \
	__re_compile_pattern (pattern, length, bufp)
# define re_set_syntax(syntax) __re_set_syntax (syntax)
# define re_search_2(bufp, st1, s1, st2, s2, startpos, range, regs, stop) \
	__re_search_2 (bufp, st1, s1, st2, s2, startpos, range, regs, stop)
# define re_compile_fastmap(bufp) __re_compile_fastmap (bufp)

# include "../locale/localeinfo.h"
#endif

/* On some systems, limits.h sets RE_DUP_MAX to a lower value than
   GNU regex allows.  Include it before <regex.h>, which correctly
   #undefs RE_DUP_MAX and sets it to the right value.  */
#include <limits.h>
#include <stdlib.h>

#include <regex.h>
#include "regex_internal.h"

#include "regex_internal.c"
#include "regcomp.c"
#include "regexec.c"

/* Binary backward compatibility.  */
#if _LIBC
# include <shlib-compat.h>
# if SHLIB_COMPAT (libc, GLIBC_2_0, GLIBC_2_3)
link_warning (re_max_failures, "the 're_max_failures' variable is obsolete and will go away.")
int re_max_failures = 2000;
# endif
#endif
