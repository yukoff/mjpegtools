/*
    $Id$

    Copyright (C) 2000 Herbert Valerio Riedel <hvr@gnu.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __MJPEG_TYPES_H__
#define __MJPEG_TYPES_H__

#include <sys/types.h> /* FreeBSD, others - ssize_t */

/*
 * modern CYGWIN releases have stdint/inttypes so there's no need
 * to check for it or provide typedefs/defines
*/

#if defined(HAVE_STDINT_H)
# include <stdint.h>
#elif defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#elif  defined(_WIN32)
#include <win32defs.h>
#else
/* warning ISO/IEC 9899:1999 <stdint.h> was missing and even <inttypes.h> */
/* fixme */
/* (Ronald) we'll just give an error now...Better solutions might come later */
#error You don't seem to have sys/types.h, inttypes.h or stdint.h! \
This might mean two things: \
Either you really don't have them, in which case you should \
install the system headers and/or C-library headers. \
You might also have forgotten to define whether you have them. \
You can do this by either defining their presence before including \
mjpegtools' header files (e.g. "#define HAVE_STDINT_H"), or you can check \
for their presence in a configure script. mjpegtools' configure \
script is a good example of how to do this. You need to check for \
PRId64, stdbool.h, inttypes.h, stdint.h and sys/types.h
#endif /* HAVE_STDINT_H */

#ifndef PRId64
#define PRId64 PRID64_STRING_FORMAT
#endif

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 4)
#define GNUC_PRINTF( format_idx, arg_idx )    \
  __attribute__((format (printf, format_idx, arg_idx)))
#else   /* !__GNUC__ */
#define GNUC_PRINTF( format_idx, arg_idx )
#endif  /* !__GNUC__ */

#endif /* __MJPEG_TYPES_H__ */


/* 
 * Local variables:
 *  c-file-style: "gnu"
 *  tab-width: 8
 *  indent-tabs-mode: nil
 * End:
 */
