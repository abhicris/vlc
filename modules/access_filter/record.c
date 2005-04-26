/*****************************************************************************
 * record.c
 *****************************************************************************
 * Copyright (C) 2005 VideoLAN
 * $Id: demux.c 7546 2004-04-29 13:53:29Z gbazin $
 *
 * Author: Laurent Aimar <fenrir@via.ecp.fr>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>

#include <vlc/vlc.h>
#include <vlc/input.h>
#include <vlc/vout.h>

#include "vlc_keys.h"
#include <osd.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define RECORD_PATH_TXT N_("Record directory")
#define RECORD_PATH_LONGTXT N_( \
    "Allows you to specify the directory where the record will be stored" )

static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin();
    set_shortname( _("Record") );
    set_description( _("Record") );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_ACCESS_FILTER );
    set_capability( "access_filter", 0 );
    add_shortcut( "record" );

    add_string( "record-path", NULL, NULL,
                RECORD_PATH_TXT, RECORD_PATH_LONGTXT, VLC_TRUE );

    set_callbacks( Open, Close );

vlc_module_end();

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

static block_t *Block  ( access_t * );
static int      Read   ( access_t *, uint8_t *, int );
static int      Control( access_t *, int i_query, va_list args );
static int      Seek   ( access_t *, int64_t );

static void Dump( access_t *, uint8_t *, int );

static int EventKey( vlc_object_t *, char const *,
                     vlc_value_t, vlc_value_t, void * );

struct access_sys_t
{
    vlc_bool_t b_dump;

    char *psz_path;
    char *psz_ext;
    char *psz_file;
    int64_t i_size;
    FILE *f;

    vout_thread_t *p_vout;
    int            i_vout_chan;
};

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    access_t *p_access = (access_t*)p_this;
    access_t *p_src = p_access->p_source;
    access_sys_t *p_sys;
    char *psz;

    /* */
    p_access->pf_read  = p_src->pf_read  ? Read : NULL;
    p_access->pf_block = p_src->pf_block ? Block : NULL;
    p_access->pf_seek  = p_src->pf_seek  ? Seek : NULL;
    p_access->pf_control = Control;

    /* */
    p_access->info = p_src->info;

    /* */
    p_access->p_sys = p_sys = malloc( sizeof( access_t ) );

    /* */
    p_sys->f = NULL;
    p_sys->i_size = 0;
    p_sys->psz_file = NULL;
    p_sys->psz_ext = "dat";
    p_sys->b_dump = VLC_FALSE;
    p_sys->p_vout = NULL;
    p_sys->i_vout_chan = -1;

    if( !strncasecmp( p_src->psz_access, "dvb", 3 ) ||
        !strncasecmp( p_src->psz_access, "udp", 3 ) )
        p_sys->psz_ext = "ts";

    psz = var_CreateGetString( p_access, "record-path" );
    if( *psz == '\0' )
    {
        free( psz );
        if( p_access->p_vlc->psz_homedir )
            psz = strdup( p_access->p_vlc->psz_homedir );
    }
    p_sys->psz_path = psz;
    msg_Dbg( p_access, "Record access filter path %s", psz );

    /* catch all key event */
    var_AddCallback( p_access->p_vlc, "key-pressed", EventKey, p_access );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys = p_access->p_sys;

    var_DelCallback( p_access->p_vlc, "key-pressed", EventKey, p_access );

    if( p_sys->f )
    {
        fclose( p_sys->f );
        free( p_sys->psz_file );
    }

    free( p_sys->psz_path );
    free( p_sys );
}

/*****************************************************************************
 *
 *****************************************************************************/
static block_t *Block( access_t *p_access )
{
    access_t     *p_src = p_access->p_source;
    block_t      *p_block;

    /* */
    p_block = p_src->pf_block( p_src );
    if( p_block && p_block->i_buffer )
        Dump( p_access, p_block->p_buffer, p_block->i_buffer );

    /* */
    p_access->info = p_src->info;

    return p_block;
}

/*****************************************************************************
 *
 *****************************************************************************/
static int Read( access_t *p_access, uint8_t *p_buffer, int i_len )
{
    access_t     *p_src = p_access->p_source;
    int i_ret;

    i_ret = p_src->pf_read( p_src, p_buffer, i_len );

    if( i_ret > 0 )
        Dump( p_access, p_buffer, i_ret );

    /* */
    p_access->info = p_src->info;

    return i_ret;
}

/*****************************************************************************
 *
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    access_t     *p_src = p_access->p_source;
    int i_ret;

    i_ret = p_src->pf_control( p_src, i_query, args );

    /* */
    p_access->info = p_src->info;

    return i_ret;
}

/*****************************************************************************
 *
 *****************************************************************************/
static int Seek( access_t *p_access, int64_t i_pos )
{
    access_t     *p_src = p_access->p_source;
    int i_ret;

    i_ret = p_src->pf_seek( p_src, i_pos );

    /* */
    p_access->info = p_src->info;

    return i_ret;
}

/*****************************************************************************
 *
 *****************************************************************************/
static int EventKey( vlc_object_t *p_this, char const *psz_var,
                     vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    access_t     *p_access = p_data;
    access_sys_t *p_sys = p_access->p_sys;

    struct hotkey *p_hotkeys = p_access->p_vlc->p_hotkeys;
    int i_action = -1, i;

    for( i = 0; p_hotkeys[i].psz_action != NULL; i++ )
    {
        if( p_hotkeys[i].i_key == newval.i_int )
        {
            i_action = p_hotkeys[i].i_action;
        }
    }

    if( i_action == ACTIONID_RECORD )
    {
        if( p_sys->b_dump )
            p_sys->b_dump = VLC_FALSE;
        else
            p_sys->b_dump = VLC_TRUE;
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 *
 *****************************************************************************/
static void Notify( access_t *p_access, vlc_bool_t b_dump )
{
    access_sys_t *p_sys = p_access->p_sys;
    vout_thread_t *p_vout;

    p_vout = vlc_object_find( p_access, VLC_OBJECT_VOUT, FIND_ANYWHERE );

    if( p_vout != p_sys->p_vout )
    {
        p_sys->p_vout = p_vout;
        if( spu_Control( p_vout->p_spu, SPU_CHANNEL_REGISTER,
                         &p_sys->i_vout_chan  ) )
            p_sys->i_vout_chan = -1;
    }

    if( p_sys->i_vout_chan != -1 )
    {
        if( b_dump )
            vout_OSDMessage( p_vout, p_sys->i_vout_chan, "Recording" );
        else
            vout_OSDMessage( p_vout, p_sys->i_vout_chan, "Recording done" );
    }
    vlc_object_release( p_vout );
}

/*****************************************************************************
 *
 *****************************************************************************/
static void Dump( access_t *p_access, uint8_t *p_buffer, int i_buffer )
{
    access_sys_t *p_sys = p_access->p_sys;
    int i_write;

    /* */
    if( !p_sys->b_dump )
    {
        if( p_sys->f )
        {
            msg_Dbg( p_access, "dumped "I64Fd" kb (%s)",
                     p_sys->i_size/1024, p_sys->psz_file );

            Notify( p_access, VLC_FALSE );

            fclose( p_sys->f );
            p_sys->f = NULL;

            free( p_sys->psz_file );
            p_sys->psz_file = NULL;

            p_sys->i_size = 0;
        }
        return;
    }

    /* */
    if( !p_sys->f )
    {
        input_thread_t *p_input;
        char *psz_name = NULL;
        time_t t = time(NULL);
        struct tm l;

        if( !localtime_r( &t, &l ) )
            memset( &l, 0, sizeof(l) );

        p_input = vlc_object_find( p_access, VLC_OBJECT_INPUT, FIND_PARENT );
        if( p_input )
        {
            vlc_mutex_lock( &p_input->input.p_item->lock );
            if( p_input->input.p_item->psz_name &&
                strlen( p_input->input.p_item->psz_name ) < 64 )
                psz_name = strdup( p_input->input.p_item->psz_name );
            vlc_mutex_unlock( &p_input->input.p_item->lock );

            vlc_object_release( p_input );
        }
        if( psz_name == NULL )
            psz_name = strdup( "Unknown" );

        asprintf( &p_sys->psz_file, "%s/%s %d-%d-%d %.2dh%.2dm%.2ds.%s",
                  p_sys->psz_path, psz_name,
                  l.tm_mday, l.tm_mon+1, l.tm_year+1900,
                  l.tm_hour, l.tm_min, l.tm_sec,
                  p_sys->psz_ext );

        free( psz_name );

        msg_Dbg( p_access, "dump in file '%s'", p_sys->psz_file );

        p_sys->f = fopen( p_sys->psz_file, "wb" );
        if( p_sys->f == NULL )
        {
            msg_Err( p_access, "cannot open file '%s' (%s)",
                     p_sys->psz_file, strerror(errno) );
            free( p_sys->psz_file );
            p_sys->psz_file = NULL;
            p_sys->b_dump = VLC_FALSE;
            return;
        }

        Notify( p_access, VLC_TRUE );

        p_sys->i_size = 0;
    }

    /* */
    if( ( i_write = fwrite( p_buffer, 1, i_buffer, p_sys->f ) ) > 0 )
        p_sys->i_size += i_write;
}

