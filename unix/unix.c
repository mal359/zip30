/*
  unix/unix.c - Zip 3

  Copyright (c) 1990-2008 Info-ZIP.  All rights reserved.

  See the accompanying file LICENSE, version 2007-Mar-4 or later
  (the contents of which are also included in zip.h) for terms of use.
  If, for some reason, all these files are missing, the Info-ZIP license
  also may be found at:  ftp://ftp.info-zip.org/pub/infozip/license.html
*/
#include "zip.h"

#ifndef UTIL    /* the companion #endif is a bit of ways down ... */

#include <time.h>

#if defined(MINIX) || defined(__mpexl)
#  ifdef S_IWRITE
#    undef S_IWRITE
#  endif /* S_IWRITE */
#  define S_IWRITE S_IWUSR
#endif /* MINIX */

#if (!defined(S_IWRITE) && defined(S_IWUSR))
#  define S_IWRITE S_IWUSR
#endif

#if defined(HAVE_DIRENT_H) || defined(_POSIX_VERSION)
#  include <dirent.h>
#else /* !HAVE_DIRENT_H */
#  ifdef HAVE_NDIR_H
#    include <ndir.h>
#  endif /* HAVE_NDIR_H */
#  ifdef HAVE_SYS_NDIR_H
#    include <sys/ndir.h>
#  endif /* HAVE_SYS_NDIR_H */
#  ifdef HAVE_SYS_DIR_H
#    include <sys/dir.h>
#  endif /* HAVE_SYS_DIR_H */
#  ifndef dirent
#    define dirent direct
#  endif
#endif /* HAVE_DIRENT_H || _POSIX_VERSION */

#ifdef __HAIKU__
#  include <errno.h>
#  include <fs_attr.h>
#  include <Mime.h>
#  include <ByteOrder.h>
#endif /* __HAIKU__ */

#ifdef __APPLE__
#  include "unix/macosx.h"
#endif /* def __APPLE__ */


#define PAD 0
#define PATH_END '/'

/* Library functions not in (most) header files */

#ifdef _POSIX_VERSION
#  include <utime.h>
#else
   int utime OF((char *, time_t *));
#endif

extern char *label;
local ulg label_time = 0;
local ulg label_mode = 0;
local time_t label_utim = 0;

/* Local functions */
local char *readd OF((DIR *));

#ifdef __APPLE__

int get_apl_dbl_info( char *name)
{
  int sts;
  int ret = 1;                  /* Assume failure. */

  /* Attribute request structures for getattrlist(). */
  struct attrlist attr_list_fndr;
  struct attrlist attr_list_rsrc;

  /* Attribute buffer structures for getattrlist(). */
  attr_bufr_fndr_t attr_bufr_fndr;
  attr_bufr_rsrc_t attr_bufr_rsrc;

  /* Clear attribute list structure. */
  memset( &attr_list_fndr, 0, sizeof( attr_list_fndr));
  /* Set attribute list bits for object type and Finder info. */
  attr_list_fndr.bitmapcount = ATTR_BIT_MAP_COUNT;
  attr_list_fndr.commonattr = ATTR_CMN_OBJTYPE| ATTR_CMN_FNDRINFO;

  /* Get file type and Finder info. */
  sts = getattrlist( name,                      /* Path. */
                     &attr_list_fndr,           /* Attrib list. */
                     &attr_bufr_fndr,           /* Dest buffer. */
                     sizeof( attr_bufr_fndr),   /* Dest buffer size. */
                     0);                        /* Options. */

  /* Continue processing if it's a regular file. */
  if ((sts == 0) && (attr_bufr_fndr.obj_type == VREG))
  {
    /* Bytewise OR Finder info data to see if all zero. */
    int fior;
    int i;

    fior = 0;
    for (i = 0; i < 32; i++)
      fior |= attr_bufr_fndr.fndr_info[ i];

    /* Clear attribute list structure. */
    memset( &attr_list_rsrc, 0, sizeof( attr_list_rsrc));
    /* Set attribute list bits for resource fork size. */
    attr_list_rsrc.bitmapcount = ATTR_BIT_MAP_COUNT;
    attr_list_rsrc.fileattr = ATTR_FILE_RSRCLENGTH;

    sts = getattrlist( name,                    /* Path. */
                       &attr_list_rsrc,         /* Attrib list. */
                       &attr_bufr_rsrc,         /* Dest buffer. */
                       sizeof( attr_bufr_rsrc), /* Dest buffer size. */
                       0);                      /* Options. */

    /* Continue processing if there is any non-zero Finder info,
       or if the resource fork size is positive.
    */
    if ((sts == 0) && ((attr_bufr_rsrc.size > 0) || (fior != 0)))
    {
      ret = 0;
    }
  }
  return ret;
}
#endif /* def __APPLE__ */


#ifdef NO_DIR                    /* for AT&T 3B1 */
#include <sys/dir.h>
#ifndef dirent
#  define dirent direct
#endif
typedef FILE DIR;
/*
**  Apparently originally by Rich Salz.
**  Cleaned up and modified by James W. Birdsall.
*/

#define opendir(path) fopen(path, "r")

struct dirent *readdir(dirp)
DIR *dirp;
{
  static struct dirent entry;

  if (dirp == NULL)
    return NULL;
  for (;;)
    if (fread (&entry, sizeof (struct dirent), 1, dirp) == 0)
      return NULL;
    else if (entry.d_ino)
      return (&entry);
} /* end of readdir() */

#define closedir(dirp) fclose(dirp)
#endif /* NO_DIR */

#ifdef NO_SCANDIR
/* TODO Port the FreeBSD libc version */
#error "We need scandir now."
#endif


local char *readd(d)
DIR *d;                 /* directory stream to read from */
/* Return a pointer to the next name in the directory stream d, or NULL if
   no more entries or an error occurs. */
{
  struct dirent *e;

  e = readdir(d);
  return e == NULL ? (char *) NULL : e->d_name;
}

int procname(n, caseflag)
char *n;                /* name to process */
int caseflag;           /* true to force case-sensitive match */
/* Process a name or sh expression to operate on (or exclude).  Return
   an error code in the ZE_ class. */
{
  char *a;              /* path and name for recursion */
  char *e;              /* pointer to name from scandir() */
  int c;                /* number of entries from scandir() */
  int i;                /* entry index */
  int m;                /* matched flag */
  char *p;              /* path for recursion */
  z_stat s;             /* result of stat() */
  struct zlist far *z;  /* steps through zfiles list */
  struct dirent **namelist;

  if (strcmp(n, "-") == 0)   /* if compressing stdin */
    return newname(n, 0, caseflag);
  else if (LSSTAT(n, &s))
  {
    /* Not a file or directory--search for shell expression in zip file */
    p = ex2in(n, 0, (int *)NULL);       /* shouldn't affect matching chars */
    m = 1;
    for (z = zfiles; z != NULL; z = z->nxt) {
      if (MATCH(p, z->iname, caseflag))
      {
        z->mark = pcount ? filter(z->zname, caseflag) : 1;
        if (verbose)
            fprintf(mesg, "zip diagnostic: %scluding %s\n",
               z->mark ? "in" : "ex", z->name);
        m = 0;
      }
    }
    free((zvoid *)p);
    return m ? ZE_MISS : ZE_OK;
  }

  /* Live name--use if file, recurse if directory */
#if defined(S_ISREG) && defined(S_ISLINK)
  if (S_ISREG(s.st_mode) || S_ISLNK(s.st_mode))
#else
#  ifdef S_IFLNK
  if ((s.st_mode & S_IFREG) == S_IFREG || (s.st_mode & S_IFLNK) == S_IFLNK)
#  else
  if ((s.st_mode & S_IFREG) == S_IFREG)
#  endif
#endif
  {
    /* add or remove name of file */
    if ((m = newname(n, 0, caseflag)) != ZE_OK)
      return m;

#ifdef __APPLE__

    /* If saving AppleDouble files, process one for this file. */
    if (data_fork_only <= 0)
    {
      /* Check for non-null Finder info and resource fork. */
      m = get_apl_dbl_info( n);
      if (m == 0)
      {
        /* Process the AppleDouble file. */
        if ((m = newname(n, 2, caseflag)) != ZE_OK)
          return m;
      }
    }
#endif /* def __APPLE__ */

  }
#ifdef S_ISDIR
  else if (S_ISDIR(s.st_mode))
#else
  else if ((s.st_mode & S_IFDIR) == S_IFDIR)
#endif
  {
    /* Add trailing / to the directory name */
    if ((p = malloc(strlen(n)+2)) == NULL)
      return ZE_MEM;
    if (strcmp(n, ".") == 0) {
      *p = '\0';  /* avoid "./" prefix and do not create zip entry */
    } else {
      strcpy(p, n);
      a = p + strlen(p);
      if (a[-1] != '/')
        strcpy(a, "/");
      if (dirnames && (m = newname(p, 1, caseflag)) != ZE_OK) {
        free((zvoid *)p);
        return m;
      }
    }
    /* recurse into directory */
    if (recurse && (c = scandir(n, &namelist, NULL, alphasort)) >= 0)
    {
      for (i = 0; i < c; i++) {
        e = namelist[i]->d_name;
        if (strcmp(e, ".") && strcmp(e, ".."))
        {
          if ((a = malloc(strlen(p) + strlen(e) + 1)) == NULL)
          {
            for (; i < c; i++) free(namelist[i]);
            free(namelist);
            free((zvoid *)p);
            return ZE_MEM;
          }
          strcat(strcpy(a, p), e);
          if ((m = procname(a, caseflag)) != ZE_OK)   /* recurse on name */
          {
            if (m == ZE_MISS)
              zipwarn("name not matched: ", a);
            else
              ziperr(m, a);
          }
          free((zvoid *)a);
        }
        free(namelist[i]);
      }
      free(namelist);
    }
    free((zvoid *)p);
  } /* (s.st_mode & S_IFDIR) */
#ifdef S_ISFIFO
  else if (S_ISFIFO(s.st_mode))
#else
  else if ((s.st_mode & S_IFIFO) == S_IFIFO)
#endif
  {
    if (allow_fifo) {
      /* FIFO (Named Pipe) - handle as normal file */
      /* add or remove name of FIFO */
      /* zip will stop if FIFO is open and wait for pipe to be fed and closed */
      if (noisy) zipwarn("Reading FIFO (Named Pipe): ", n);
      if ((m = newname(n, 0, caseflag)) != ZE_OK)
        return m;
    } else {
      zipwarn("ignoring FIFO (Named Pipe) - use -FI to read: ", n);
      return ZE_OK;
    }
  } /* S_IFIFO */
  else
    zipwarn("ignoring special file: ", n);
  return ZE_OK;
}

char *ex2in(x, isdir, pdosflag)
char *x;                /* external file name */
int isdir;              /* input: x is a directory */
int *pdosflag;          /* output: force MSDOS file attributes? */
/* Convert the external file name to a zip file name, returning the malloc'ed
   string or NULL if not enough memory. */
{
  char *n;              /* internal file name (malloc'ed) */
  char *t = NULL;       /* shortened name */
  int dosflag;

  dosflag = dosify;     /* default for non-DOS and non-OS/2 */

  /* Find starting point in name before doing malloc */
#ifdef __CYGWIN__
  /* Strip drive specification */
  t = *x && isascii((uch)*x) && *(x + 1) == ':' ? x + 2 : x;
  /* Convert backslashes to slashes */
  for (n = t; *n; n++)
    if (*n == '\\')
      *n = '/';
#endif
  /* Strip "//host/share/" part of a UNC name */
  if (!strncmp(x,"//",2) && (x[2] != '\0' && x[2] != '/')) {
    n = x + 2;
    while (*n != '\0' && *n != '/')
      n++;              /* strip host name */
    if (*n != '\0') {
      n++;
      while (*n != '\0' && *n != '/')
        n++;            /* strip `share' name */
    }
    if (*n != '\0')
      t = n + 1;
#ifdef __CYGWIN__
  } /* we may already have adjusted t */
#else
  } else
      t = x;
#endif
  while (*t == '/')
    t++;                /* strip leading '/' chars to get a relative path */
  while (*t == '.' && t[1] == '/')
    t += 2;             /* strip redundant leading "./" sections */

  /* Make changes, if any, to the copied name (leave original intact) */
  if (!pathput)
    t = last(t, PATH_END);

  /* Malloc space for internal name and copy it */
  if ((n = malloc(strlen(t) + 1)) == NULL)
    return NULL;
  strcpy(n, t);

  if (dosify)
    msname(n);

#ifdef EBCDIC
  strtoasc(n, n);       /* here because msname() needs native coding */
#endif

  /* Returned malloc'ed name */
  if (pdosflag)
    *pdosflag = dosflag;

  if (isdir) return n;  /* avoid warning on unused variable */
  return n;
}

char *in2ex(n)
char *n;                /* internal file name */
/* Convert the zip file name to an external file name, returning the malloc'ed
   string or NULL if not enough memory. */
{
  char *x;              /* external file name */

  if ((x = malloc(strlen(n) + 1 + PAD)) == NULL)
    return NULL;
#ifdef EBCDIC
  strtoebc(x, n);
#else
  strcpy(x, n);
#endif
  return x;
}

/*
 * XXX use ztimbuf in both POSIX and non POSIX cases ?
 */
void stamp(f, d)
char *f;                /* name of file to change */
ulg d;                  /* dos-style time to change it to */
/* Set last updated and accessed time of file f to the DOS time d. */
{
#ifdef _POSIX_VERSION
  struct utimbuf u;     /* argument for utime()  const ?? */
#else
  time_t u[2];          /* argument for utime() */
#endif

  /* Convert DOS time to time_t format in u */
#ifdef _POSIX_VERSION
  u.actime = u.modtime = dos2unixtime(d);
  utime(f, &u);
#else
  u[0] = u[1] = dos2unixtime(d);
  utime(f, u);
#endif

}

ulg filetime(f, a, n, t)
  char *f;                /* name of file to get info on */
  ulg *a;                 /* return value: file attributes */
  zoff_t *n;              /* return value: file size */
  iztimes *t;             /* return value: access, modific. and creation times */
/* If file *f does not exist, return 0.  Else, return the file's last
   modified date and time as an MSDOS date and time.  The date and
   time is returned in a long with the date most significant to allow
   unsigned integer comparison of absolute times.  Also, if a is not
   a NULL pointer, store the file attributes there, with the high two
   bytes being the Unix attributes, and the low byte being a mapping
   of that to DOS attributes.  If n is not NULL, store the file size
   there.  If t is not NULL, the file's access, modification and creation
   times are stored there as UNIX time_t values.
   If f is "-", use standard input as the file. If f is a device, return
   a file size of -1 */
{
  z_stat s;         /* results of stat() */
  /* converted to pointer from using FNMAX - 11/8/04 EG */
  char *name;
  size_t len = strlen(f);

  if (f == label) {
    if (a != NULL)
      *a = label_mode;
    if (n != NULL)
      *n = -2L; /* convention for a label name */
    if (t != NULL)
      t->atime = t->mtime = t->ctime = label_utim;
    return label_time;
  }
  if ((name = malloc(len + 1)) == NULL) {
    ZIPERR(ZE_MEM, "filetime");
  }
  strcpy(name, f);
  if (name[len - 1] == '/')
    name[len - 1] = '\0';
  /* not all systems allow stat'ing a file with / appended */
  if (strcmp(f, "-") == 0) {
    if (zfstat(fileno(stdin), &s) != 0) {
      free(name);
      error("fstat(stdin)");
    }
  }
  else if (LSSTAT(name, &s) != 0) {
    /* Accept about any file kind including directories
     * (stored with trailing / with -r option)
     */
    free(name);
    return 0;
  }
  free(name);

  if (a != NULL) {
#ifndef OS390
    *a = ((ulg)s.st_mode << 16) | !(s.st_mode & S_IWRITE);
#else
/*
**  The following defines are copied from the unizip source and represent the
**  legacy Unix mode flags.  These fixed bit masks are no longer required
**  by XOPEN standards - the S_IS### macros being the new recommended method.
**  The approach here of setting the legacy flags by testing the macros should
**  work under any _XOPEN_SOURCE environment (and will just rebuild the same bit
**  mask), but is required if the legacy bit flags differ from legacy Unix.
*/
#define UNX_IFDIR      0040000     /* Unix directory */
#define UNX_IFREG      0100000     /* Unix regular file */
#define UNX_IFSOCK     0140000     /* Unix socket (BSD, not SysV or Amiga) */
#define UNX_IFLNK      0120000     /* Unix symbolic link (not SysV, Amiga) */
#define UNX_IFBLK      0060000     /* Unix block special       (not Amiga) */
#define UNX_IFCHR      0020000     /* Unix character special   (not Amiga) */
#define UNX_IFIFO      0010000     /* Unix fifo    (BCC, not MSC or Amiga) */
    {
    mode_t legacy_modes;

    /* Initialize with permission bits--which are not implementation-optional */
    legacy_modes = s.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX);
    if (S_ISDIR(s.st_mode))
      legacy_modes |= UNX_IFDIR;
    if (S_ISREG(s.st_mode))
      legacy_modes |= UNX_IFREG;
    if (S_ISLNK(s.st_mode))
      legacy_modes |= UNX_IFLNK;
    if (S_ISBLK(s.st_mode))
      legacy_modes |= UNX_IFBLK;
    if (S_ISCHR(s.st_mode))
      legacy_modes |= UNX_IFCHR;
    if (S_ISFIFO(s.st_mode))
      legacy_modes |= UNX_IFIFO;
    if (S_ISSOCK(s.st_mode))
      legacy_modes |= UNX_IFSOCK;
    *a = ((ulg)legacy_modes << 16) | !(s.st_mode & S_IWRITE);
    }
#endif
#ifdef S_ISDIR
    if (S_ISDIR(s.st_mode)) {
#else
    if ((s.st_mode & S_IFMT) == S_IFDIR) {
#endif
      *a |= MSDOS_DIR_ATTR;
    }
  }
  if (n != NULL)
#ifdef S_ISREG
    *n = S_ISREG(s.st_mode) ? s.st_size : -1L;
#else
    *n = ((s.st_mode & S_IFMT) == S_IFREG || (s.st_mode & S_IFMT) == S_IFLNK) ? s.st_size : -1L;
#endif
  if (t != NULL) {
    t->atime = s.st_atime;
    t->mtime = s.st_mtime;
    t->ctime = t->mtime;   /* best guess, (s.st_ctime: last status change!!) */
  }
  return unix2dostime(&s.st_mtime);
}


#ifndef QLZIP /* QLZIP Unix2QDOS cross-Zip supplies an extended variant */
#ifdef __HAIKU__

/* Set a file's MIME type. */
#define BE_FILE_TYPE_NAME   "BEOS:TYPE"

/* ---------------------------------------------------------------------- */
/* Set a file's MIME type.                                                */
void setfiletype( const char *file, const char *type )
{
    int fd;
    attr_info fa;
    ssize_t wrote_bytes;

    fd = open( file, O_RDWR );
    if( fd < 0 ) {
        zipwarn( "can't open zipfile to write file type", "" );
        return;
    }

    fa.type = B_MIME_STRING_TYPE;
    fa.size = (off_t)(strlen( type ) + 1);

    wrote_bytes = fs_write_attr( fd, BE_FILE_TYPE_NAME, fa.type, 0,
                                 type, fa.size );
    if( wrote_bytes != (ssize_t)fa.size ) {
        zipwarn( "couldn't write complete file type", "" );
    }

    close( fd );
}

/* ----------------------------------------------------------------------

Return a malloc()'d buffer containing all of the attributes and their names
for the file specified in name.  You have to free() this yourself.  The length
of the buffer is also returned.

If get_attr_dir() fails, the buffer will be NULL, total_size will be 0,
and an error will be returned:

    ZE_OK    - no errors occurred
    ZE_LOGIC - attr_buff was pointing at a buffer
    ZE_MEM - insufficient memory for attribute buffer

Other errors are possible (whatever is returned by the fs_attr.h functions).

PROBLEMS:

- pointers are 32-bits; attributes are limited to off_t in size so it's
  possible to overflow... in practice, this isn't too likely... your
  machine will thrash like hell before that happens

*/

#define INITIAL_BUFF_SIZE 65536

int get_attr_dir( const char *name, char **attr_buff, off_t *total_size )
{
    int               retval = ZE_OK;
    int               fd;
    DIR              *fa_dir;
    struct dirent    *fa_ent;
    off_t             attrs_size;
    off_t             this_size;
    char             *ptr;
    struct attr_info  fa_info;
    struct attr_info  big_fa_info;

    retval      = ZE_OK;
    attrs_size  = 0;    /* gcc still says this is used uninitialized... */
    *total_size = 0;

    /* ----------------------------------------------------------------- */
    /* Sanity-check.                                                     */
    if( *attr_buff != NULL ) {
        return ZE_LOGIC;
    }

    /* ----------------------------------------------------------------- */
    /* Can we open the file/directory?                                   */
    /*                                                                   */
    /* linkput is a zip global; it's set to 1 if we're storing symbolic  */
    /* links as symbolic links (instead of storing the thing the link    */
    /* points to)... if we're storing the symbolic link as a link, we'll */
    /* want the link's file attributes, otherwise we want the target's.  */
    if( linkput ) {
        fd = open( name, O_RDONLY | O_NOTRAVERSE );
    } else {
        fd = open( name, O_RDONLY );
    }
    if( fd < 0 ) {
        return errno;
    }

    /* ----------------------------------------------------------------- */
    /* Allocate an initial buffer; 64k should usually be enough.         */
    *attr_buff = (char *)malloc( INITIAL_BUFF_SIZE );
    ptr        = *attr_buff;
    if( ptr == NULL ) {
        close( fd );

        return ZE_MEM;
    }

    /* ----------------------------------------------------------------- */
    /* Open the attributes directory for this file.                      */
    fa_dir = fs_fopen_attr_dir( fd );
    if( fa_dir == NULL ) {
        close( fd );

        free( ptr );
        *attr_buff = NULL;

        return retval;
    }

    /* ----------------------------------------------------------------- */
    /* Read all the attributes; the buffer could grow > 64K if there are */
    /* many and/or they are large.                                       */
    fa_ent = fs_read_attr_dir( fa_dir );
    while( fa_ent != NULL ) {
        uint32 attr_type;
        uint64 attr_size;

        retval = fs_stat_attr( fd, fa_ent->d_name, &fa_info );
        /* TODO: check retval != ZE_OK */

        this_size  = strlen( fa_ent->d_name ) + 1;
        this_size += sizeof(uint32) + sizeof(uint64);
        this_size += fa_info.size;

        attrs_size += this_size;

        if( attrs_size > INITIAL_BUFF_SIZE ) {
            unsigned long offset = ptr - *attr_buff;

            *attr_buff = (char *)realloc( *attr_buff, attrs_size );
            if( *attr_buff == NULL ) {
                retval = fs_close_attr_dir( fa_dir );
                /* TODO: check retval != ZE_OK */
                close( fd );

                return ZE_MEM;
            }

            ptr = *attr_buff + offset;
        }

        /* Now copy the data for this attribute into the buffer. */
        strcpy( ptr, fa_ent->d_name );
        ptr += strlen( fa_ent->d_name );
        *ptr++ = '\0';

        /* the attr_info is stored in BigEndian because of PowerPC */
        attr_type = B_HOST_TO_BENDIAN_INT32( fa_info.type );
        attr_size = B_HOST_TO_BENDIAN_INT64( fa_info.size );
        
        memcpy( ptr, &attr_type, sizeof(uint32)); ptr += sizeof(uint32);
        memcpy( ptr, &attr_size, sizeof(uint64)); ptr += sizeof(uint64);

        if( fa_info.size > 0 ) {
            ssize_t read_bytes;

            read_bytes = fs_read_attr( fd, fa_ent->d_name, fa_info.type, 0,
                                       ptr, fa_info.size );
            if( read_bytes != fa_info.size ) {
                /* print a warning about mismatched sizes */
                char buff[80];

                sprintf( buff, "read %ld, expected %ld",
                         (ssize_t)read_bytes, (ssize_t)fa_info.size );
                zipwarn( "attribute size mismatch: ", buff );
            }

            /* Wave my magic wand... this swaps all the Be types to big- */
            /* endian automagically.                                     */
            (void)swap_data( fa_info.type, ptr, fa_info.size,
                             B_SWAP_HOST_TO_BENDIAN );

            ptr += fa_info.size;
        }

        fa_ent = fs_read_attr_dir( fa_dir );
    }

    /* ----------------------------------------------------------------- */
    /* Close the attribute directory.                                    */
    retval = fs_close_attr_dir( fa_dir );
    /* TODO: check retval != ZE_OK */

    /* ----------------------------------------------------------------- */
    /* If the buffer is too big, shrink it.                              */
    if( attrs_size < INITIAL_BUFF_SIZE ) {
        *attr_buff = (char *)realloc( *attr_buff, attrs_size );
        if( *attr_buff == NULL ) {
            /* This really shouldn't happen... */
            close( fd );

            return ZE_MEM;
        }
    }

    *total_size = attrs_size;

    close( fd );

    return ZE_OK;
}

/* ---------------------------------------------------------------------- */
/* Add a 'Be' extra field to the zlist data pointed to by z.              */

#define EB_L_BE_LEN 5           /* min size is an unsigned long and flag */
#define EB_C_BE_LEN 5           /* Length of data in local EF and flag.  */

#define EB_BE_FL_NATURAL    0x01    /* data is 'natural' (not compressed) */

#define EB_L_BE_SIZE    (EB_HEADSIZE + EB_L_BE_LEN) /* + attr size */
#define EB_C_BE_SIZE    (EB_HEADSIZE + EB_C_BE_LEN)

/* maximum memcompress overhead is the sum of the compression header length */
/* (6 = ush compression type, ulg CRC) and the worstcase deflate overhead   */
/* when uncompressible data are kept in 2 "stored" blocks (5 per block =    */
/* byte blocktype + 2 * ush blocklength) */
#define MEMCOMPRESS_OVERHEAD    (EB_MEMCMPR_HSIZ + EB_DEFLAT_EXTRA)

local int add_Be_ef( struct zlist far *z )
{
    char *l_ef       = NULL;
    char *c_ef       = NULL;
    char *attrbuff   = NULL;
    off_t attrsize   = 0;
    char *compbuff   = NULL;
    ulg   compsize   = 0;
    uch   flags      = 0;

    /* Check to make sure we've got enough room in the extra fields. */
    if( z->ext + EB_L_BE_SIZE > USHRT_MAX ||
        z->cext + EB_C_BE_SIZE > USHRT_MAX ) {
        return ZE_MEM;
    }

    /* Attempt to load up a buffer full of the file's attributes. */
    {
        int retval;

        retval = get_attr_dir( z->name, &attrbuff, &attrsize );
        if( retval != ZE_OK ) {
            return ZE_OPEN;
        }
        if( attrsize == 0 ) {
            return ZE_OK;
        }
        if( attrbuff == NULL ) {
            return ZE_LOGIC;
        }

        /* Check for way too much data. */
        if( attrsize > OFF_MAX ) {
            zipwarn( "uncompressed attributes truncated", "" );
            attrsize = OFF_MAX - MEMCOMPRESS_OVERHEAD;
        }
    }

    if( verbose ) {
        printf( "\t[in=%lu]", (unsigned long)attrsize );
    }

    /* Try compressing the data */
    compbuff = (char *)malloc( (size_t)attrsize + MEMCOMPRESS_OVERHEAD );
    if( compbuff == NULL ) {
        return ZE_MEM;
    }
    compsize = memcompress( compbuff,
                            (size_t)attrsize + MEMCOMPRESS_OVERHEAD,
                            attrbuff,
                            (size_t)attrsize );
    if( verbose ) {
        printf( " [out=%u]", compsize );
    }

    /* Attempt to optimise very small attributes. */
    if( compsize > attrsize ) {
        free( compbuff );
        compsize = (ush)attrsize;
        compbuff = attrbuff;

        flags = EB_BE_FL_NATURAL;
    }

    /* Check to see if we really have enough room in the EF for the data. */
    if( ( z->ext + compsize + EB_L_BE_SIZE ) > USHRT_MAX ) {
        zipwarn("not enough space in extra field for attributes\n", "");
        free( compbuff );
        return ZE_MEM;
    }

    /* Allocate memory for the local and central extra fields. */
    if( z->extra && z->ext != 0 ) {
        l_ef = (char *)realloc( z->extra, z->ext + EB_L_BE_SIZE + compsize );
    } else {
        l_ef = (char *)malloc( EB_L_BE_SIZE + compsize );
        z->ext = 0;
    }
    if( l_ef == NULL ) {
        return ZE_MEM;
    }
    z->extra = l_ef;
    l_ef += z->ext;

    if( z->cextra && z->cext != 0 ) {
        c_ef = (char *)realloc( z->cextra, z->cext + EB_C_BE_SIZE );
    } else {
        c_ef = (char *)malloc( EB_C_BE_SIZE );
        z->cext = 0;
    }
    if( c_ef == NULL ) {
        return ZE_MEM;
    }
    z->cextra = c_ef;
    c_ef += z->cext;

    /* Now add the local version of the field. */
    *l_ef++ = 'B';
    *l_ef++ = 'e';
    *l_ef++ = (char)(compsize + EB_L_BE_LEN);
    *l_ef++ = (char)((compsize + EB_L_BE_LEN) >> 8);
    *l_ef++ = (char)((unsigned long)attrsize);
    *l_ef++ = (char)((unsigned long)attrsize >> 8);
    *l_ef++ = (char)((unsigned long)attrsize >> 16);
    *l_ef++ = (char)((unsigned long)attrsize >> 24);
    *l_ef++ = flags;
    memcpy( l_ef, compbuff, (size_t)compsize );

    z->ext += EB_L_BE_SIZE + compsize;

    /* And the central version. */
    *c_ef++ = 'B';
    *c_ef++ = 'e';
    *c_ef++ = (char)(EB_C_BE_LEN);
    *c_ef++ = (char)(EB_C_BE_LEN >> 8);
    *c_ef++ = (char)compsize;
    *c_ef++ = (char)(compsize >> 8);
    *c_ef++ = (char)(compsize >> 16);
    *c_ef++ = (char)(compsize >> 24);
    *c_ef++ = flags;

    z->cext += EB_C_BE_SIZE;

    return ZE_OK;
}

#endif /* __HAIKU__ */

int set_new_unix_extra_field(z, s)
  struct zlist far *z;
  z_stat *s;
  /* New unix extra field.
     Currently only UIDs and GIDs are stored. */
{
  int uid_size;
  int gid_size;
  int ef_data_size;
  char *extra;
  char *cextra;
  ulg id;
  int b;

  uid_size = sizeof(s->st_uid);
  gid_size = sizeof(s->st_gid);

/* New extra field
   tag       (2 bytes)
   size      (2 bytes)
   version   (1 byte)
   uid_size  (1 byte - size in bytes)
   uid       (variable)
   gid_size  (1 byte - size in bytes)
   gid       (variable)
 */
   
  ef_data_size = 1 + 1 + uid_size + 1 + gid_size;

  if ((extra = (char *)malloc(z->ext + 4 + ef_data_size)) == NULL)
    return ZE_MEM;
  if ((cextra = (char *)malloc(z->ext + 4 + ef_data_size)) == NULL) {
    free(extra);
    return ZE_MEM;
  }

  if (z->ext)
    memcpy(extra, z->extra, z->ext);
  if (z->cext)
    memcpy(cextra, z->cextra, z->cext);

  free(z->extra);
  z->extra = extra;
  free(z->cextra);
  z->cextra = cextra;

  z->extra[z->ext + 0] = 'u';
  z->extra[z->ext + 1] = 'x';
  z->extra[z->ext + 2] = (char)ef_data_size;     /* length of data part */
  z->extra[z->ext + 3] = 0;
  z->extra[z->ext + 4] = 1;                      /* version */

  /* UID */
  z->extra[z->ext + 5] = (char)(uid_size);       /* uid size in bytes */
  b = 6;
  id = (ulg)(s->st_uid);
  z->extra[z->ext + b] = (char)(id & 0xFF);
  if (uid_size > 1) {
    b++;
    id = id >> 8;
    z->extra[z->ext + b] = (char)(id & 0xFF);
    if (uid_size > 2) {
      b++;
      id = id >> 8;
      z->extra[z->ext + b] = (char)(id & 0xFF);
      b++;
      id = id >> 8;
      z->extra[z->ext + b] = (char)(id & 0xFF);
      if (uid_size == 8) {
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
      }
    }
  }

  /* GID */
  b++;
  z->extra[z->ext + b] = (char)(gid_size);       /* gid size in bytes */
  b++;
  id = (ulg)(s->st_gid);
  z->extra[z->ext + b] = (char)(id & 0xFF);
  if (gid_size > 1) {
    b++;
    id = id >> 8;
    z->extra[z->ext + b] = (char)(id & 0xFF);
    if (gid_size > 2) {
      b++;
      id = id >> 8;
      z->extra[z->ext + b] = (char)(id & 0xFF);
      b++;
      id = id >> 8;
      z->extra[z->ext + b] = (char)(id & 0xFF);
      if (gid_size == 8) {
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
        b++;
        id = id >> 8;
        z->extra[z->ext + b] = (char)(id & 0xFF);
      }
    }
  }

  /* copy local extra field to central directory extra field */
  memcpy((z->cextra) + z->cext, (z->extra) + z->ext, 4 + ef_data_size);

  z->ext = z->ext + 4 + ef_data_size;
  z->cext = z->cext + 4 + ef_data_size;

  return ZE_OK;
}


int set_extra_field(z, z_utim)
  struct zlist far *z;
  iztimes *z_utim;
  /* store full data in local header but just modification time stamp info
     in central header */
{
  int retval;
  z_stat s;
  char *name;
  size_t len = strlen(z->name);

  /* For the full sized UT local field including the UID/GID fields, we
   * have to stat the file again. */

  if ((name = malloc(len + 1)) == NULL) {
    ZIPERR(ZE_MEM, "set_extra_field");
  }
  strcpy(name, z->name);
  if (name[len - 1] == '/')
    name[len - 1] = '\0';
  /* not all systems allow stat'ing a file with / appended */
  if (LSSTAT(name, &s)) {
    free(name);
    return ZE_OPEN;
  }
  free(name);

#define EB_L_UT_SIZE    (EB_HEADSIZE + EB_UT_LEN(2))
#define EB_C_UT_SIZE    (EB_HEADSIZE + EB_UT_LEN(1))

/* The flag UIDGID_NOT_16BIT should be set by the pre-compile configuration
   script when it detects st_uid or st_gid sizes differing from 16-bit.
 */
#ifndef UIDGID_NOT_16BIT
  /* The following "second-level" check for st_uid and st_gid members being
     16-bit wide is only added as a safety precaution in case the "first-level"
     check failed to define the UIDGID_NOT_16BIT symbol.
     The first-level check should have been implemented in the automatic
     compile configuration process.
   */
# ifdef UIDGID_ARE_16B
#  undef UIDGID_ARE_16B
# endif
  /* The following expression is a compile-time constant and should (hopefully)
     get optimized away by any sufficiently intelligent compiler!
   */
# define UIDGID_ARE_16B  (sizeof(s.st_uid) == 2 && sizeof(s.st_gid) == 2)

# define EB_L_UX2_SIZE   (EB_HEADSIZE + EB_UX2_MINLEN)
# define EB_C_UX2_SIZE   EB_HEADSIZE
# define EF_L_UNIX_SIZE  (EB_L_UT_SIZE + (UIDGID_ARE_16B ? EB_L_UX2_SIZE : 0))
# define EF_C_UNIX_SIZE  (EB_C_UT_SIZE + (UIDGID_ARE_16B ? EB_C_UX2_SIZE : 0))
#else
# define EF_L_UNIX_SIZE EB_L_UT_SIZE
# define EF_C_UNIX_SIZE EB_C_UT_SIZE
#endif /* !UIDGID_NOT_16BIT */

  if ((z->extra = (char *)malloc(EF_L_UNIX_SIZE)) == NULL)
    return ZE_MEM;
  if ((z->cextra = (char *)malloc(EF_C_UNIX_SIZE)) == NULL)
    return ZE_MEM;

  z->extra[0]  = 'U';
  z->extra[1]  = 'T';
  z->extra[2]  = (char)EB_UT_LEN(2);    /* length of data part of local e.f. */
  z->extra[3]  = 0;
  z->extra[4]  = EB_UT_FL_MTIME | EB_UT_FL_ATIME;    /* st_ctime != creation */
  z->extra[5]  = (char)(s.st_mtime);
  z->extra[6]  = (char)(s.st_mtime >> 8);
  z->extra[7]  = (char)(s.st_mtime >> 16);
  z->extra[8]  = (char)(s.st_mtime >> 24);
  z->extra[9]  = (char)(s.st_atime);
  z->extra[10] = (char)(s.st_atime >> 8);
  z->extra[11] = (char)(s.st_atime >> 16);
  z->extra[12] = (char)(s.st_atime >> 24);

#ifndef UIDGID_NOT_16BIT
  /* Only store the UID and GID in the old Ux extra field if the runtime
     system provides them in 16-bit wide variables.  */
  if (UIDGID_ARE_16B) {
    z->extra[13] = 'U';
    z->extra[14] = 'x';
    z->extra[15] = (char)EB_UX2_MINLEN; /* length of data part of local e.f. */
    z->extra[16] = 0;
    z->extra[17] = (char)(s.st_uid);
    z->extra[18] = (char)(s.st_uid >> 8);
    z->extra[19] = (char)(s.st_gid);
    z->extra[20] = (char)(s.st_gid >> 8);
  }
#endif /* !UIDGID_NOT_16BIT */

  z->ext = EF_L_UNIX_SIZE;

  memcpy(z->cextra, z->extra, EB_C_UT_SIZE);
  z->cextra[EB_LEN] = (char)EB_UT_LEN(1);
#ifndef UIDGID_NOT_16BIT
  if (UIDGID_ARE_16B) {
    /* Copy header of Ux extra field from local to central */
    memcpy(z->cextra+EB_C_UT_SIZE, z->extra+EB_L_UT_SIZE, EB_C_UX2_SIZE);
    z->cextra[EB_LEN+EB_C_UT_SIZE] = 0;
  }
#endif
  z->cext = EF_C_UNIX_SIZE;

#if 0  /* UID/GID presence is now signaled by central EF_IZUNIX2 field ! */
  /* lower-middle external-attribute byte (unused until now):
   *   high bit        => (have GMT mod/acc times) >>> NO LONGER USED! <<<
   *   second-high bit => have Unix UID/GID info
   * NOTE: The high bit was NEVER used in any official Info-ZIP release,
   *       but its future use should be avoided (if possible), since it
   *       was used as "GMT mod/acc times local extra field" flags in Zip beta
   *       versions 2.0j up to 2.0v, for about 1.5 years.
   */
  z->atx |= 0x4000;
#endif /* never */

  /* new unix extra field */
  retval = set_new_unix_extra_field(z, &s);
  if (retval != ZE_OK)
      return retval;
  
  retval = add_Be_ef(z);
  if (retval != ZE_OK)
      return retval;

  return ZE_OK;
}

#endif /* !QLZIP */


int deletedir(d)
char *d;                /* directory to delete */
/* Delete the directory *d if it is empty, do nothing otherwise.
   Return the result of rmdir(), delete(), or system().
   For VMS, d must be in format [x.y]z.dir;1  (not [x.y.z]).
 */
{
# ifdef NO_RMDIR
    /* code from Greg Roelofs, who horked it from Mark Edwards (unzip) */
    size_t len;
    int r;
    char *s;              /* malloc'd string for system command */

    len = strlen(d);
    if ((s = malloc(len + 34)) == NULL)
      return 127;

    sprintf(s, "IFS=\" \t\n\" /bin/rmdir %s 2>/dev/null", d);
    r = system(s);
    free(s);
    return r;
# else /* !NO_RMDIR */
    return rmdir(d);
# endif /* ?NO_RMDIR */
}

#endif /* !UTIL */


/******************************/
/*  Function version_local()  */
/******************************/

#if defined(__NetBSD__) || defined(__FreeBSD__) || defined(__386BSD__) || \
    defined(__OpenBSD__) || defined(__bsdi__) || defined( __APPLE__) || \
	defined(__DragonFly__) || defined(__MidnightBSD__)
#include <sys/param.h> /* for the BSD define */
/* if we have something newer than NET/2 we'll use uname(3) */
#if (BSD > 199103)
#include <sys/utsname.h>
#endif /* BSD > 199103 */
#endif /* __{Net,Free,Open,386}BSD__ || __bsdi__ */

void version_local()
{
#ifdef __GNUC__
#  ifdef NX_CURRENT_COMPILER_RELEASE
    char compiler_name[80];
#  endif
#else
#  if (defined( __SUNPRO_C))
    char compiler_name[33];
#  else
#    if (defined( __HP_cc) || defined( __IBMC__))
    char compiler_name[33];
#    else
#      if (defined( __DECC_VER))
    char compiler_name[33];
    int compiler_typ;
#      else
#        if ((defined(CRAY) || defined(cray)) && defined(_RELEASE))
    char compiler_name[40];
#        endif
#      endif
#    endif
#  endif
#endif

#ifdef BSD
# if (BSD > 199103)
    struct utsname u;
    char os_name[40];
# else
# if defined(__NETBSD__))
    static ZCONST char *netbsd[] = { "_ALPHA", "", "A", "B" };
    char os_name[40];
# endif /* __NETBSD__ */
# endif /* BSD > 199103 */
#else /* !BSD */
#if ((defined(CRAY) || defined(cray)) && defined(_UNICOS))
    char os_name[40];
#endif /* (CRAY && defined(_UNICOS)) */
#endif /* ?BSD */

/* Define the compiler name and version string */
#ifdef __GNUC__
#  ifdef NX_CURRENT_COMPILER_RELEASE
    sprintf(compiler_name, "NeXT DevKit %d.%02d (gcc " __VERSION__ ")",
     NX_CURRENT_COMPILER_RELEASE/100, NX_CURRENT_COMPILER_RELEASE%100);
#    define COMPILER_NAME compiler_name
#  else
#    define COMPILER_NAME "gcc " __VERSION__
#  endif
#else /* !__GNUC__ */
#  if defined(__SUNPRO_C)
    sprintf( compiler_name, "Sun C version %x", __SUNPRO_C);
#    define COMPILER_NAME compiler_name
#  else
#    if (defined( __HP_cc))
    if ((__HP_cc% 100) == 0)
    {
      sprintf( compiler_name, "HP C version A.%02d.%02d",
       (__HP_cc/ 10000), ((__HP_cc% 10000)/ 100));
    }
    else
    {
      sprintf( compiler_name, "HP C version A.%02d.%02d.%02d",
       (__HP_cc/ 10000), ((__HP_cc% 10000)/ 100), (__HP_cc% 100));
    }
#      define COMPILER_NAME compiler_name
#    else
#      if (defined( __DECC_VER))
    sprintf( compiler_name, "DEC C version %c%d.%d-%03d",
     ((compiler_typ = (__DECC_VER / 10000) % 10) == 6 ? 'T' :
     (compiler_typ == 8 ? 'S' : 'V')),
     __DECC_VER / 10000000,
     (__DECC_VER % 10000000) / 100000, __DECC_VER % 1000);
#        define COMPILER_NAME compiler_name
#      else
#        if ((defined(CRAY) || defined(cray)) && defined(_RELEASE))
    sprintf(compiler_name, "cc version %d", _RELEASE);
#          define COMPILER_NAME compiler_name
#        else
#          ifdef __IBMC__
    sprintf( compiler_name, "IBM C version %d.%d.%d",
     (__IBMC__/ 100), ((__IBMC__/ 10)% 10), (__IBMC__% 10));
#            define COMPILER_NAME compiler_name
#          else
#            ifdef __VERSION__
#              define COMPILER_NAME "cc " __VERSION__
#            else
#              define COMPILER_NAME "cc "
#            endif
#          endif
#        endif
#      endif
#    endif
#  endif
#endif /* ?__GNUC__ */


/* Define the name to use for the OS we're compiling on */
#if defined(sgi) || defined(__sgi)
#  define OS_NAME "Silicon Graphics IRIX"
#else
#ifdef sun
#  ifdef sparc
#    ifdef __SVR4
#      define OS_NAME "Sun SPARC/Solaris"
#    else /* may or may not be SunOS */
#      define OS_NAME "Sun SPARC"
#    endif
#  else
#  if defined(sun386) || defined(i386)
#    define OS_NAME "Sun 386i"
#  else
#  if defined(mc68020) || defined(__mc68020__)
#    define OS_NAME "Sun 3"
#  else /* mc68010 or mc68000:  Sun 2 or earlier */
#    define OS_NAME "Sun 2"
#  endif
#  endif
#  endif
#else
#ifdef __hpux
#  define OS_NAME "HP-UX"
#else
#ifdef __osf__
#  define OS_NAME "DEC OSF/1"
#else
#ifdef _AIX
#  define OS_NAME "IBM AIX"
#else
#ifdef aiws
#  define OS_NAME "IBM RT/AIX"
#else
#if defined(CRAY) || defined(cray)
#  ifdef _UNICOS
    sprintf(os_name, "Cray UNICOS release %d", _UNICOS);
#    define OS_NAME os_name
#  else
#    define OS_NAME "Cray UNICOS"
#  endif
#else
#if defined(uts) || defined(UTS)
#  define OS_NAME "Amdahl UTS"
#else
#ifdef NeXT
#  ifdef mc68000
#    define OS_NAME "NeXTStep/black"
#  else
#    define OS_NAME "NeXTStep for Intel"
#  endif
#else
#if defined(linux) || defined(__linux__)
#  ifdef __ELF__
#    define OS_NAME "Linux ELF"
#  else
#    define OS_NAME "Linux a.out"
#  endif
#else
#ifdef MINIX
#  define OS_NAME "Minix"
#else
#ifdef M_UNIX
#  define OS_NAME "SCO Unix"
#else
#ifdef M_XENIX
#  define OS_NAME "SCO Xenix"
#else
#ifdef BSD
# if (BSD > 199103)
#    define OS_NAME os_name
    uname(&u);
    sprintf(os_name, "%s %s", u.sysname, u.release);
# else
# ifdef __NetBSD__
#    define OS_NAME "NetBSD"
# else
# ifdef __FreeBSD__
#    define OS_NAME "FreeBSD"
# else
# ifdef __bsdi__
#    define OS_NAME "BSD/386"
# else
# ifdef __386BSD__
#    define OS_NAME "386BSD"
# else
# ifdef __OpenBSD__
#    define OS_NAME "OpenBSD"
# else
# ifdef __DragonFly__
#    define OS_NAME "DragonFly BSD"
# else
# ifdef __MidnightBSD__
#    define OS_NAME "MidnightBSD"
# else
#    define OS_NAME "Unknown BSD"
# endif /* __MidnightBSD__ */
# endif /* __DragonFly__ */
# endif /* __OpenBSD__ */
# endif /* __386BSD__ */
# endif /* __bsdi__ */
# endif /* FreeBSD */
# endif /* NetBSD */
# endif /* BSD > 199103 */
#else
#ifdef __CYGWIN__
#  define OS_NAME "Cygwin"
#else
#if defined(i686) || defined(__i686) || defined(__i686__)
#  define OS_NAME "Intel 686"
#else
#if defined(i586) || defined(__i586) || defined(__i586__)
#  define OS_NAME "Intel 586"
#else
#if defined(i486) || defined(__i486) || defined(__i486__)
#  define OS_NAME "Intel 486"
#else
#if defined(i386) || defined(__i386) || defined(__i386__)
#  define OS_NAME "Intel 386"
#else
#ifdef pyr
#  define OS_NAME "Pyramid"
#else
#if defined(ultrix) || defined(__ultrix)
#  if defined(mips) || defined(__mips)
#    define OS_NAME "DEC/MIPS"
#  else
#  if defined(vax) || defined(__vax)
#    define OS_NAME "DEC/VAX"
#  else /* __alpha? */
#    define OS_NAME "DEC/Alpha"
#  endif
#  endif
#else
#ifdef gould
#  define OS_NAME "Gould"
#else
#ifdef MTS
#  define OS_NAME "MTS"
#else
#ifdef __convexc__
#  define OS_NAME "Convex"
#else
#ifdef __QNX__
#  define OS_NAME "QNX 4"
#else
#ifdef __QNXNTO__
#  define OS_NAME "QNX Neutrino"
#else
#ifdef __APPLE__
#  ifdef __i386__
#    define OS_NAME "Mac OS X Intel"
#  else /* __i386__ */
#    ifdef __ppc__
#      define OS_NAME "Mac OS X PowerPC"
#    else /* __ppc__ */
#      ifdef __ppc64__
#        define OS_NAME "Mac OS X PowerPC64"
#      else /* __ppc64__ */
#        ifdef __x86_64__
#          define OS_NAME "macOS 64-bit Intel"
#        else /* __x86_64__ */
#          ifdef __aarch64__
#            define OS_NAME "macOS Apple Silicon"
#          else /* __aarch64__ */
#            define OS_NAME "One sour Apple"
#          endif /* __aarch64__ */
#        endif /* __x86_64__ */
#      endif /* __ppc64__ */
#    endif /* __ppc__ */
#  endif /* __i386__ */
#else
#  define OS_NAME "Unknown"
#endif /* Apple */
#endif /* QNX Neutrino */
#endif /* QNX 4 */
#endif /* Convex */
#endif /* MTS */
#endif /* Gould */
#endif /* DEC */
#endif /* Pyramid */
#endif /* 386 */
#endif /* 486 */
#endif /* 586 */
#endif /* 686 */
#endif /* Cygwin */
#endif /* BSD */
#endif /* SCO Xenix */
#endif /* SCO Unix */
#endif /* Minix */
#endif /* Linux */
#endif /* NeXT */
#endif /* Amdahl */
#endif /* Cray */
#endif /* RT/AIX */
#endif /* AIX */
#endif /* OSF/1 */
#endif /* HP-UX */
#endif /* Sun */
#endif /* SGI */


/* Define the compile date string */
#if 0
#  define COMPILE_DATE " on " __DATE__
#else
#  define COMPILE_DATE ""
#endif

    printf("Compiled with %s for Unix (%s)%s.\n\n",
           COMPILER_NAME, OS_NAME, COMPILE_DATE);

} /* end function version_local() */


/* 2006-03-23 SMS.
 * Emergency replacement for strerror().  (Useful on SunOS 4.*.)
 * Enable by specifying "LOCAL_UNZIP=-DNEED_STRERROR=1" on the "make"
 * command line.
 */

#ifdef NEED_STRERROR

char *strerror( err)
  int err;
{
    extern char *sys_errlist[];
    extern int sys_nerr;

    static char no_msg[ 64];

    if ((err >= 0) && (err < sys_nerr))
    {
        return sys_errlist[ err];
    }
    else
    {
        sprintf( no_msg, "(no message, code = %d.)", err);
        return no_msg;
    }
}

#endif /* def NEED_STRERROR */


/* 2006-03-23 SMS.
 * Emergency replacement for memmove().  (Useful on SunOS 4.*.)
 * Enable by specifying "LOCAL_UNZIP=-DNEED_MEMMOVE=1" on the "make"
 * command line.
 */

#ifdef NEED_MEMMOVE

/* memmove.c -- copy memory.
   Copy LENGTH bytes from SOURCE to DEST.  Does not null-terminate.
   In the public domain.
   By David MacKenzie <djm@gnu.ai.mit.edu>.
   Adjusted by SMS.
*/

void *memmove(dest0, source0, length)
  void *dest0;
  void const *source0;
  size_t length;
{
    char *dest = dest0;
    char const *source = source0;
    if (source < dest)
        /* Moving from low mem to hi mem; start at end.  */
        for (source += length, dest += length; length; --length)
            *--dest = *--source;
    else if (source != dest)
    {
        /* Moving from hi mem to low mem; start at beginning.  */
        for (; length; --length)
            *dest++ = *source++;
    }
    return dest0;
}

#endif /* def NEED_MEMMOVE */
