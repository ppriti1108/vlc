/*****************************************************************************
 * file.c: file input (file: access plug-in)
 *****************************************************************************
 * Copyright (C) 2001-2006 the VideoLAN team
 * Copyright © 2006 Rémi Denis-Courmont
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Rémi Denis-Courmont <rem # videolan # org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>
#include <vlc/input.h>
#include <vlc_interaction.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
#   include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#   include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#   include <fcntl.h>
#endif

#if defined( WIN32 ) && !defined( UNDER_CE )
#   include <io.h>
#else
#   include <unistd.h>
#   include <poll.h>
#endif

#if defined( WIN32 ) && !defined( UNDER_CE )
/* fstat() support for large files on win32 */
#   define fstat(a,b) _fstati64(a,b)
#   ifdef lseek
#      undef lseek
#   endif
#   define lseek _lseeki64
#elif defined( UNDER_CE )
#   ifdef read
#      undef read
#   endif
#   define read(a,b,c) fread(b,1,c,a)
#   define close(a) fclose(a)
#   ifdef lseek
#      undef lseek
#   endif
#   define lseek fseek
#endif

#include "charset.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define CACHING_TEXT N_("Caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Caching value for files. This " \
    "value should be set in milliseconds." )
#define CAT_TEXT N_("Concatenate with additional files")
#define CAT_LONGTEXT N_( \
    "Play split files as if they were part of a unique file. " \
    "You need to specify a comma-separated list of files." )

vlc_module_begin();
    set_description( _("File input") );
    set_shortname( _("File") );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_ACCESS );
    add_integer( "file-caching", DEFAULT_PTS_DELAY / 1000, NULL, CACHING_TEXT, CACHING_LONGTEXT, VLC_TRUE );
    add_string( "file-cat", NULL, NULL, CAT_TEXT, CAT_LONGTEXT, VLC_TRUE );
    set_capability( "access2", 50 );
    add_shortcut( "file" );
    add_shortcut( "stream" );
    add_shortcut( "kfir" );
    set_callbacks( Open, Close );
vlc_module_end();


/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static int  Seek( access_t *, int64_t );
static int  Read( access_t *, uint8_t *, int );
static int  Control( access_t *, int, va_list );

static int  open_file( access_t *, const char * );

struct access_sys_t
{
    unsigned int i_nb_reads;
    vlc_bool_t   b_kfir;

    /* Files list */
    unsigned filec;
    int     *filev;
    int64_t *sizev;

    /* Current file */
    unsigned filep;

    /* */
    vlc_bool_t b_seekable;
    vlc_bool_t b_pace_control;
};

/*****************************************************************************
 * Open: open the file
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys;
    char         *catlist;

    vlc_bool_t    b_stdin = !strcmp (p_access->psz_path, "-");

    /* Update default_pts to a suitable value for file access */
    var_Create( p_access, "file-caching", VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );

    STANDARD_READ_ACCESS_INIT;
    p_sys->i_nb_reads = 0;
    p_sys->b_kfir = VLC_FALSE;
    p_sys->filec = 1;
    p_sys->filep = 0;

    if (!strcasecmp (p_access->psz_access, "stream"))
    {
        p_sys->b_seekable = VLC_FALSE;
        p_sys->b_pace_control = VLC_FALSE;
    }
    else if (!strcasecmp (p_access->psz_access, "kfir"))
    {
        p_sys->b_seekable = VLC_FALSE;
        p_sys->b_pace_control = VLC_FALSE;
        p_sys->b_kfir = VLC_TRUE;
    }
    else
    {
        p_sys->b_seekable = VLC_TRUE;
        p_sys->b_pace_control = VLC_TRUE;
    }

    /* Count number of files */
    catlist = var_CreateGetString (p_access, "file-cat");
    if (catlist == NULL)
    {
        free (p_sys);
        return VLC_ENOMEM;
    }

    if (*catlist)
    {
        for (const char *ptr = catlist; ptr != NULL; p_sys->filec++)
        {
            ptr = strchr (ptr, ',');
            if (ptr != NULL)
                ptr++;
        }
    }

    p_sys->filev = calloc (p_sys->filec, sizeof (p_sys->filev[0]));
    if (p_sys->filev == NULL)
    {
        free (catlist);
        free (p_sys);
        return VLC_ENOMEM;
    }

    p_sys->sizev = calloc (p_sys->filec, sizeof (p_sys->sizev[0]));
    if (p_sys->sizev == NULL)
    {
        free (catlist);
        free (p_sys->filev);
        free (p_sys);
        return VLC_ENOMEM;
    }

    /* Open files */
    char *filename = catlist;
    for (unsigned i = 0; i < p_sys->filec; i++)
    {
        int fd = -1;

        if (i == 0)
        {
            msg_Dbg (p_access, "opening file `%s'", p_access->psz_path);

            if (b_stdin)
                fd = dup (0);
            else
                fd = open_file (p_access, p_access->psz_path);
        }
        else
        {
            assert (filename != NULL);

            char *ptr = strchr (filename, ',');
            if (ptr != NULL)
                *ptr = 0;

            msg_Dbg (p_access, "opening additionnal file `%s'", filename);
            fd = open_file (p_access, filename);
            filename = ptr + 1;
        }

#ifdef HAVE_SYS_STAT_H
        struct stat st;

        while (fd != -1)
        {
            if (fstat (fd, &st))
                msg_Err (p_access, "fstat(%d): %s", fd, strerror (errno));
            else
            if (S_ISDIR (st.st_mode))
                /* The directory plugin takes care of that */
                msg_Dbg (p_access, "file is a directory, aborting");
            else
                break; // success

            close (fd);
            fd = -1;
        }
#endif

        if (fd == -1)
        {
            free (catlist);
            p_sys->filec = i;
            Close (p_this);
            return VLC_EGENERIC;
        }
        p_sys->filev[i] = fd;

#ifdef HAVE_SYS_STAT_H
        p_sys->sizev[i] = st.st_size;
        p_access->info.i_size += st.st_size;

        if (!S_ISREG (st.st_mode) && !S_ISBLK (st.st_mode)
         && (!S_ISCHR (st.st_mode) || (st.st_size == 0)))
            // If one file is not seekable, the concatenation is not either
            p_sys->b_seekable = VLC_FALSE;
#else
        p_sys->b_seekable = !b_stdin;
#endif

    }

    free (catlist);

    if (p_sys->b_seekable && !p_access->info.i_size)
    {
        /* FIXME that's bad because all others access will be probed */
        msg_Err (p_access, "file is empty, aborting");
        Close (p_this);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: close the target
 *****************************************************************************/
static void Close (vlc_object_t * p_this)
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys = p_access->p_sys;

    for (unsigned i = 0; i < p_sys->filec; i++)
        close (p_sys->filev[i]);

    free (p_sys->filev);
    free (p_sys->sizev);
    free (p_sys);
}

/*****************************************************************************
 * Read: standard read on a file descriptor.
 *****************************************************************************/
static int Read( access_t *p_access, uint8_t *p_buffer, int i_len )
{
    access_sys_t *p_sys = p_access->p_sys;
    int i_ret;
    int fd = p_sys->filev[p_sys->filep];

#if !defined(WIN32) && !defined(UNDER_CE)
    if( !p_sys->b_pace_control )
    {
        if( !p_sys->b_kfir )
        {
            /* Find if some data is available. This won't work under Windows. */
            do
            {
                struct pollfd ufd;

                if( p_access->b_die )
                    return 0;

                memset (&ufd, 0, sizeof (ufd));
                ufd.fd = fd;
                ufd.events = POLLIN;

                i_ret = poll (&ufd, 1, 500);
            }
            while (i_ret <= 0);

            i_ret = read (fd, p_buffer, i_len);
        }
        else
        {
            /* b_kfir ; work around a buggy poll() driver implementation */
            while (((i_ret = read (fd, p_buffer, i_len)) == 0)
                && !p_access->b_die)
            {
                msleep( INPUT_ERROR_SLEEP );
            }
        }
    }
    else
#endif /* WIN32 || UNDER_CE */
        /* b_pace_control || WIN32 */
        i_ret = read( fd, p_buffer, i_len );

    if( i_ret < 0 )
    {
        switch (errno)
        {
            case EINTR:
            case EAGAIN:
                break;

            default:
                msg_Err (p_access, "read failed (%s)", strerror (errno));
                intf_UserFatal (p_access, VLC_FALSE, _("File reading failed"),
                                _("VLC could not read file \"%s\"."),
                                strerror (errno));
        }

        /* Delay a bit to avoid consuming all the CPU. This is particularly
         * useful when reading from an unconnected FIFO. */
        msleep( INPUT_ERROR_SLEEP );
    }

    p_sys->i_nb_reads++;

#ifdef HAVE_SYS_STAT_H
    if( p_access->info.i_size != 0 &&
        (p_sys->i_nb_reads % INPUT_FSTAT_NB_READS) == 0 )
    {
        struct stat st;
        int i = p_sys->filep;

        if ((fstat (fd, &st) == 0)
         && (p_sys->sizev[i] != st.st_size))
        {
            p_access->info.i_size += st.st_size - p_sys->sizev[i];
            p_sys->sizev[i] = st.st_size;
            p_access->info.i_update |= INPUT_UPDATE_SIZE;
        }
    }
#endif

    /* If we reached an EOF then switch to the next file in the list */
    if (i_ret == 0)
    {
        if  (++p_sys->filep < p_sys->filec)
            /* We have to read some data */
            return Read (p_access, p_buffer, i_len);
        else
            p_sys->filep--;
    }

    if( i_ret > 0 )
        p_access->info.i_pos += i_ret;
    else if( i_ret == 0 )
        p_access->info.b_eof = VLC_TRUE;

    return i_ret;
}

/*****************************************************************************
 * Seek: seek to a specific location in a file
 *****************************************************************************/
static int Seek (access_t *p_access, int64_t i_pos)
{
    access_sys_t *p_sys = p_access->p_sys;

    if (p_access->info.i_size < p_access->info.i_pos)
    {
        msg_Err (p_access, "seeking too far");
        i_pos = p_access->info.i_pos = p_access->info.i_size;
    }
    else if (p_access->info.i_pos < 0)
    {
        msg_Err (p_access, "seeking too early");
        i_pos = p_access->info.i_pos = 0;
    }

    p_access->info.i_pos = i_pos;
    p_access->info.b_eof = VLC_FALSE;

    /* Determine which file we need to access */
    unsigned i = 0;
    assert (p_sys->filec > 0);

    while (i_pos > p_sys->sizev[i])
    {
        i_pos -= p_sys->sizev[i++];
        assert (i < p_sys->filec);
    }
    p_sys->filep = i;

    lseek (p_sys->filev[i], i_pos, SEEK_SET);
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    access_sys_t *p_sys = p_access->p_sys;
    vlc_bool_t   *pb_bool;
    int          *pi_int;
    int64_t      *pi_64;

    switch( i_query )
    {
        /* */
        case ACCESS_CAN_SEEK:
        case ACCESS_CAN_FASTSEEK:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = p_sys->b_seekable;
            break;

        case ACCESS_CAN_PAUSE:
        case ACCESS_CAN_CONTROL_PACE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = p_sys->b_pace_control;
            break;

        /* */
        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = 0;
            break;

        case ACCESS_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            *pi_64 = var_GetInteger( p_access, "file-caching" ) * I64C(1000);
            break;

        /* */
        case ACCESS_SET_PAUSE_STATE:
            /* Nothing to do */
            break;

        case ACCESS_GET_TITLE_INFO:
        case ACCESS_SET_TITLE:
        case ACCESS_SET_SEEKPOINT:
        case ACCESS_SET_PRIVATE_ID_STATE:
        case ACCESS_GET_META:
            return VLC_EGENERIC;

        default:
            msg_Warn( p_access, "unimplemented query in control" );
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}


static char *expand_path (const access_t *p_access, const char *path)
{
    if (strncmp (path, "~/", 2) == 0)
    {
        char *res;

         // TODO: we should also support the ~cmassiot/ syntax
         if (asprintf (&res, "%s/%s", p_access->p_libvlc->psz_homedir, path + 2) == -1)
             return NULL;
         return res;
    }

#if defined(WIN32)
    if (!strcasecmp (p_access->psz_access, "file")
      && ('/' == path[0]) && path[1] && (':' == path[2]) && ('/' == path[3]))
        // Explorer can open path such as file:/C:/ or file:///C:/
        // hence remove leading / if found
        return strdup (path + 1);
#endif

    return strdup (path);
}


/*****************************************************************************
 * open_file: Opens a specific file
 *****************************************************************************/
static int open_file (access_t *p_access, const char *psz_name)
{
    char *path = expand_path (p_access, psz_name);

#ifdef UNDER_CE
    p_sys->fd = utf8_fopen( path, "rb" );
    if ( !p_sys->fd )
    {
        msg_Err( p_access, "cannot open file %s", psz_name );
        intf_UserFatal( p_access, VLC_FALSE, _("File reading failed"), 
                        _("VLC could not open file \"%s\"."), psz_name );
        free (path);
        return VLC_EGENERIC;
    }

    fseek( p_sys->fd, 0, SEEK_END );
    p_access->info.i_size = ftell( p_sys->fd );
    p_access->info.i_update |= INPUT_UPDATE_SIZE;
    fseek( p_sys->fd, 0, SEEK_SET );
#else
    const char *psz_localname = ToLocale (path);
    if (psz_localname == NULL)
    {
        msg_Err (p_access, "incorrect file name %s", psz_name);
        free (path);
        return -1;
    }

    // FIXME: support non-ANSI filenames on Win32
    int fd = open (path, O_NONBLOCK /*| O_LARGEFILE*/);
    LocaleFree (psz_localname);
    free (path);

    if (fd == -1)
    {
        msg_Err (p_access, "cannot open file %s (%s)", psz_name,
                 strerror (errno));
        intf_UserFatal (p_access, VLC_FALSE, _("File reading failed"), 
                        _("VLC could not open file \"%s\" (%s)."),
                        psz_name, strerror (errno));
        return -1;
    }

# if defined(HAVE_FCNTL_H) && defined(F_FDAHEAD) && defined(F_NOCACHE)
    /* We'd rather use any available memory for reading ahead
     * than for caching what we've already seen/heard */
    fcntl (fd, F_RDAHEAD, 1);
    fcntl (fd, F_NOCACHE, 1);
# endif
#endif

    return fd;
}
