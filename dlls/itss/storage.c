/*
 *    ITSS Storage implementation
 *
 * Copyright 2004 Mike McCormack
 *
 *  see http://bonedaddy.net/pabs3/hhm/#chmspec
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"
#include "winreg.h"
#include "ole2.h"

#include "uuids.h"

#include "itss.h"
#include "chm_lib.h"
#include "itsstor.h"

#include "wine/unicode.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(itss);

/************************************************************************/

typedef struct _ITSS_IStorageImpl
{
    IStorageVtbl *vtbl_IStorage;
    DWORD ref;
    struct chmFile *chmfile;
    WCHAR dir[1];
} ITSS_IStorageImpl;

struct enum_info
{
    struct enum_info *next, *prev;
    struct chmUnitInfo ui;
};

typedef struct _IEnumSTATSTG_Impl
{
    IEnumSTATSTGVtbl *vtbl_IEnumSTATSTG;
    DWORD ref;
    struct enum_info *first, *last, *current;
} IEnumSTATSTG_Impl;

typedef struct _IStream_Impl
{
    IStreamVtbl *vtbl_IStream;
    DWORD ref;
    ITSS_IStorageImpl *stg;
    ULONGLONG addr;
    struct chmUnitInfo ui;
} IStream_Impl;

static HRESULT ITSS_create_chm_storage(
           struct chmFile *chmfile, const WCHAR *dir, IStorage** ppstgOpen );
static IStream_Impl* ITSS_create_stream( 
           ITSS_IStorageImpl *stg, struct chmUnitInfo *ui );

/************************************************************************/

static HRESULT WINAPI ITSS_IEnumSTATSTG_QueryInterface(
    IEnumSTATSTG* iface,
    REFIID riid,
    void** ppvObject)
{
    ICOM_THIS(IEnumSTATSTG_Impl,iface);

    if (IsEqualGUID(riid, &IID_IUnknown)
	|| IsEqualGUID(riid, &IID_IEnumSTATSTG))
    {
	IEnumSTATSTG_AddRef(iface);
	*ppvObject = This;
	return S_OK;
    }

    WARN("(%p)->(%s,%p),not found\n",This,debugstr_guid(riid),ppvObject);
    return E_NOINTERFACE;
}

static ULONG WINAPI ITSS_IEnumSTATSTG_AddRef(
    IEnumSTATSTG* iface)
{
    ICOM_THIS(IEnumSTATSTG_Impl,iface);
    return ++(This->ref);
}

static ULONG WINAPI ITSS_IEnumSTATSTG_Release(
    IEnumSTATSTG* iface)
{
    ICOM_THIS(IEnumSTATSTG_Impl,iface);

    ULONG ref = --This->ref;

    if (ref == 0)
    {
        while( This->first )
        {
            struct enum_info *t = This->first->next;
            HeapFree( GetProcessHeap(), 0, This->first );
            This->first = t;
        }
	HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

static HRESULT WINAPI ITSS_IEnumSTATSTG_Next(
        IEnumSTATSTG* iface,
        ULONG celt,
        STATSTG* rgelt,
        ULONG* pceltFetched)
{
    ICOM_THIS(IEnumSTATSTG_Impl,iface);
    DWORD len, n;
    struct enum_info *cur;

    TRACE("%p %lu %p %p\n", This, celt, rgelt, pceltFetched );

    cur = This->current;
    n = 0;
    while( (n<celt) && cur) 
    {
        WCHAR *str;

        memset( rgelt, 0, sizeof *rgelt );

        /* copy the name */
        str = cur->ui.path;
        if( *str == '/' )
            str++;
        len = strlenW( str ) + 1;
        rgelt->pwcsName = CoTaskMemAlloc( len*sizeof(WCHAR) );
        strcpyW( rgelt->pwcsName, str );

        /* determine the type */
        if( rgelt->pwcsName[len-2] == '/' )
        {
            rgelt->pwcsName[len-2] = 0;
            rgelt->type = STGTY_STORAGE;
        }
        else
            rgelt->type = STGTY_STREAM;

        /* copy the size */
        rgelt->cbSize.QuadPart = cur->ui.length;

        /* advance to the next item if it exists */
        n++;
        cur = cur->next;
    }

    This->current = cur;
    *pceltFetched = n;

    if( n < celt )
        return S_FALSE;

    return S_OK;
}

static HRESULT WINAPI ITSS_IEnumSTATSTG_Skip(
        IEnumSTATSTG* iface,
        ULONG celt)
{
    ICOM_THIS(IEnumSTATSTG_Impl,iface);
    DWORD n;
    struct enum_info *cur;

    TRACE("%p %lu\n", This, celt );

    cur = This->current;
    n = 0;
    while( (n<celt) && cur) 
    {
        n++;
        cur = cur->next;
    }
    This->current = cur;

    if( n < celt )
        return S_FALSE;

    return S_OK;
}

static HRESULT WINAPI ITSS_IEnumSTATSTG_Reset(
        IEnumSTATSTG* iface)
{
    ICOM_THIS(IEnumSTATSTG_Impl,iface);

    TRACE("%p\n", This );

    This->current = This->first;

    return S_OK;
}

static HRESULT WINAPI ITSS_IEnumSTATSTG_Clone(
        IEnumSTATSTG* iface,
        IEnumSTATSTG** ppenum)
{
    FIXME("\n");
    return E_NOTIMPL;
}

struct IEnumSTATSTGVtbl IEnumSTATSTG_vtbl =
{
    ITSS_IEnumSTATSTG_QueryInterface,
    ITSS_IEnumSTATSTG_AddRef,
    ITSS_IEnumSTATSTG_Release,
    ITSS_IEnumSTATSTG_Next,
    ITSS_IEnumSTATSTG_Skip,
    ITSS_IEnumSTATSTG_Reset,
    ITSS_IEnumSTATSTG_Clone
};

static IEnumSTATSTG_Impl *ITSS_create_enum( void )
{
    IEnumSTATSTG_Impl *stgenum;

    stgenum = HeapAlloc( GetProcessHeap(), 0, sizeof (IEnumSTATSTG_Impl) );
    stgenum->vtbl_IEnumSTATSTG = &IEnumSTATSTG_vtbl;
    stgenum->ref = 1;
    stgenum->first = NULL;
    stgenum->last = NULL;
    stgenum->current = NULL;

    TRACE(" -> %p\n", stgenum );

    return stgenum;
}

/************************************************************************/

HRESULT WINAPI ITSS_IStorageImpl_QueryInterface(
    IStorage* iface,
    REFIID riid,
    void** ppvObject)
{
    ICOM_THIS(ITSS_IStorageImpl,iface);

    if (IsEqualGUID(riid, &IID_IUnknown)
	|| IsEqualGUID(riid, &IID_IStorage))
    {
	IStorage_AddRef(iface);
	*ppvObject = This;
	return S_OK;
    }

    WARN("(%p)->(%s,%p),not found\n",This,debugstr_guid(riid),ppvObject);
    return E_NOINTERFACE;
}

ULONG WINAPI ITSS_IStorageImpl_AddRef(
    IStorage* iface)
{
    ICOM_THIS(ITSS_IStorageImpl,iface);
    return ++(This->ref);
}

ULONG WINAPI ITSS_IStorageImpl_Release(
    IStorage* iface)
{
    ICOM_THIS(ITSS_IStorageImpl,iface);

    ULONG ref = --This->ref;

    if (ref == 0)
    {
	HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

HRESULT WINAPI ITSS_IStorageImpl_CreateStream(
    IStorage* iface,
    LPCOLESTR pwcsName,
    DWORD grfMode,
    DWORD reserved1,
    DWORD reserved2,
    IStream** ppstm)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_OpenStream(
    IStorage* iface,
    LPCOLESTR pwcsName,
    void* reserved1,
    DWORD grfMode,
    DWORD reserved2,
    IStream** ppstm)
{
    ICOM_THIS(ITSS_IStorageImpl,iface);
    IStream_Impl *stm;
    DWORD len;
    struct chmUnitInfo ui;
    int r;
    WCHAR *path;

    TRACE("%p %s %p %lu %lu %p\n", This, debugstr_w(pwcsName),
          reserved1, grfMode, reserved2, ppstm );

    len = strlenW( This->dir ) + strlenW( pwcsName ) + 1;
    path = HeapAlloc( GetProcessHeap(), 0, len*sizeof(WCHAR) );
    strcpyW( path, This->dir );
    if( pwcsName[0] == '/' )
    {
        WCHAR *p = &path[strlenW( path ) - 1];
        while( ( path <= p ) && ( *p = '/' ) )
            *p-- = 0;
    }
    strcatW( path, pwcsName );

    TRACE("Resolving %s\n", debugstr_w(path));

    r = chm_resolve_object(This->chmfile, path, &ui);
    HeapFree( GetProcessHeap(), 0, path );

    if( r != CHM_RESOLVE_SUCCESS )
        return STG_E_FILENOTFOUND;

    stm = ITSS_create_stream( This, &ui );
    if( !stm )
        return E_FAIL;

    *ppstm = (IStream*) stm;

    return S_OK;
}

HRESULT WINAPI ITSS_IStorageImpl_CreateStorage(
    IStorage* iface,
    LPCOLESTR pwcsName,
    DWORD grfMode,
    DWORD dwStgFmt,
    DWORD reserved2,
    IStorage** ppstg)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_OpenStorage(
    IStorage* iface,
    LPCOLESTR pwcsName,
    IStorage* pstgPriority,
    DWORD grfMode,
    SNB snbExclude,
    DWORD reserved,
    IStorage** ppstg)
{
    ICOM_THIS(ITSS_IStorageImpl,iface);

    FIXME("%p %s %p %lu %p %lu %p\n", This, debugstr_w(pwcsName),
          pstgPriority, grfMode, snbExclude, reserved, ppstg);
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_CopyTo(
    IStorage* iface,
    DWORD ciidExclude,
    const IID* rgiidExclude,
    SNB snbExclude,
    IStorage* pstgDest)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_MoveElementTo(
    IStorage* iface,
    LPCOLESTR pwcsName,
    IStorage* pstgDest,
    LPCOLESTR pwcsNewName,
    DWORD grfFlags)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_Commit(
    IStorage* iface,
    DWORD grfCommitFlags)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_Revert(
    IStorage* iface)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static int ITSS_chm_enumerator(
    struct chmFile *h,
    struct chmUnitInfo *ui,
    void *context)
{
    struct enum_info *info;
    IEnumSTATSTG_Impl* stgenum = context;

    TRACE("adding %s to enumeration\n", debugstr_w(ui->path) );

    info = HeapAlloc( GetProcessHeap(), 0, sizeof (struct enum_info) );
    memcpy( &info->ui, ui, sizeof info->ui );

    info->next = NULL;
    info->prev = stgenum->last;
    if( stgenum->last )
        stgenum->last->next = info;
    else
        stgenum->first = info;
    stgenum->last = info;
    
    return CHM_ENUMERATOR_CONTINUE;
}

HRESULT WINAPI ITSS_IStorageImpl_EnumElements(
    IStorage* iface,
    DWORD reserved1,
    void* reserved2,
    DWORD reserved3,
    IEnumSTATSTG** ppenum)
{
    ICOM_THIS(ITSS_IStorageImpl,iface);
    IEnumSTATSTG_Impl* stgenum;

    TRACE("%p %ld %p %ld %p\n", This, reserved1, reserved2, reserved3, ppenum );

    stgenum = ITSS_create_enum();
    if( !stgenum )
        return E_FAIL;

    chm_enumerate_dir(This->chmfile,
                  This->dir,
                  CHM_ENUMERATE_ALL,
                  ITSS_chm_enumerator,
                  stgenum );

    stgenum->current = stgenum->first;

    *ppenum = (IEnumSTATSTG*) stgenum;

    return S_OK;
}

HRESULT WINAPI ITSS_IStorageImpl_DestroyElement(
    IStorage* iface,
    LPCOLESTR pwcsName)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_RenameElement(
    IStorage* iface,
    LPCOLESTR pwcsOldName,
    LPCOLESTR pwcsNewName)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_SetElementTimes(
    IStorage* iface,
    LPCOLESTR pwcsName,
    const FILETIME* pctime,
    const FILETIME* patime,
    const FILETIME* pmtime)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_SetClass(
    IStorage* iface,
    REFCLSID clsid)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_SetStateBits(
    IStorage* iface,
    DWORD grfStateBits,
    DWORD grfMask)
{
    FIXME("\n");
    return E_NOTIMPL;
}

HRESULT WINAPI ITSS_IStorageImpl_Stat(
    IStorage* iface,
    STATSTG* pstatstg,
    DWORD grfStatFlag)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static IStorageVtbl ITSS_IStorageImpl_Vtbl =
{
    ITSS_IStorageImpl_QueryInterface,
    ITSS_IStorageImpl_AddRef,
    ITSS_IStorageImpl_Release,
    ITSS_IStorageImpl_CreateStream,
    ITSS_IStorageImpl_OpenStream,
    ITSS_IStorageImpl_CreateStorage,
    ITSS_IStorageImpl_OpenStorage,
    ITSS_IStorageImpl_CopyTo,
    ITSS_IStorageImpl_MoveElementTo,
    ITSS_IStorageImpl_Commit,
    ITSS_IStorageImpl_Revert,
    ITSS_IStorageImpl_EnumElements,
    ITSS_IStorageImpl_DestroyElement,
    ITSS_IStorageImpl_RenameElement,
    ITSS_IStorageImpl_SetElementTimes,
    ITSS_IStorageImpl_SetClass,
    ITSS_IStorageImpl_SetStateBits,
    ITSS_IStorageImpl_Stat,
};

static HRESULT ITSS_create_chm_storage(
      struct chmFile *chmfile, const WCHAR *dir, IStorage** ppstgOpen )
{
    ITSS_IStorageImpl *stg;
    DWORD len;

    TRACE("%p %s\n", chmfile, debugstr_w( dir ) );

    len = strlenW( dir ) + 1;
    stg = HeapAlloc( GetProcessHeap(), 0, 
                     sizeof (ITSS_IStorageImpl) + len*sizeof(WCHAR) );
    stg->vtbl_IStorage = &ITSS_IStorageImpl_Vtbl;
    stg->ref = 1;
    stg->chmfile = chmfile;
    strcpyW( stg->dir, dir );

    *ppstgOpen = (IStorage*) stg;

    return S_OK;
}

HRESULT ITSS_StgOpenStorage( 
    const WCHAR* pwcsName,
    IStorage* pstgPriority,
    DWORD grfMode,
    SNB snbExclude,
    DWORD reserved,
    IStorage** ppstgOpen)
{
    struct chmFile *chmfile;
    static const WCHAR szRoot[] = { '/', 0 };

    TRACE("%s\n", debugstr_w(pwcsName) );

    chmfile = chm_openW( pwcsName );
    if( !chmfile )
        return E_FAIL;

    return ITSS_create_chm_storage( chmfile, szRoot, ppstgOpen );
}

/************************************************************************/

static HRESULT WINAPI ITSS_IStream_QueryInterface(
    IStream* iface,
    REFIID riid,
    void** ppvObject)
{
    ICOM_THIS(IStream_Impl,iface);

    if (IsEqualGUID(riid, &IID_IUnknown)
	|| IsEqualGUID(riid, &IID_ISequentialStream)
	|| IsEqualGUID(riid, &IID_IStream))
    {
	IStream_AddRef(iface);
	*ppvObject = This;
	return S_OK;
    }

    WARN("(%p)->(%s,%p),not found\n",This,debugstr_guid(riid),ppvObject);
    return E_NOINTERFACE;
}

static ULONG WINAPI ITSS_IStream_AddRef(
    IStream* iface)
{
    ICOM_THIS(IStream_Impl,iface);
    return ++(This->ref);
}

static ULONG WINAPI ITSS_IStream_Release(
    IStream* iface)
{
    ICOM_THIS(IStream_Impl,iface);

    ULONG ref = --This->ref;

    if (ref == 0)
    {
        IStorage_Release( (IStorage*) This->stg );
	HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

static HRESULT WINAPI ITSS_IStream_Read(
        IStream* iface,
        void* pv,
        ULONG cb,
        ULONG* pcbRead)
{
    ICOM_THIS(IStream_Impl,iface);
    ULONG count;

    TRACE("%p %p %lu %p\n", This, pv, cb, pcbRead);

    count = chm_retrieve_object(This->stg->chmfile, 
                          &This->ui, pv, This->addr, cb);
    This->addr += count;
    if( pcbRead )
        *pcbRead = count;
    
    return S_OK;
}

static HRESULT WINAPI ITSS_IStream_Write(
        IStream* iface,
        const void* pv,
        ULONG cb,
        ULONG* pcbWritten)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ITSS_IStream_Seek(
        IStream* iface,
        LARGE_INTEGER dlibMove,
        DWORD dwOrigin,
        ULARGE_INTEGER* plibNewPosition)
{
    ICOM_THIS(IStream_Impl,iface);
    LONGLONG newpos;

    TRACE("%p %s %lu %p\n", This,
          wine_dbgstr_longlong( dlibMove.QuadPart ), dwOrigin, plibNewPosition );

    newpos = This->addr;
    switch( dwOrigin )
    {
    case STREAM_SEEK_CUR:
        newpos = This->addr + dlibMove.QuadPart;
        break;
    case STREAM_SEEK_SET:
        newpos = dlibMove.QuadPart;
        break;
    case STREAM_SEEK_END:
        newpos = This->ui.length + dlibMove.QuadPart;
        break;
    }

    if( ( newpos < 0 ) || ( newpos > This->ui.length ) )
        return STG_E_INVALIDPOINTER;

    This->addr = newpos;
    if( plibNewPosition )
        plibNewPosition->QuadPart = This->addr;

    return S_OK;
}

static HRESULT WINAPI ITSS_IStream_SetSize(
        IStream* iface,
        ULARGE_INTEGER libNewSize)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ITSS_IStream_CopyTo(
        IStream* iface,
        IStream* pstm,
        ULARGE_INTEGER cb,
        ULARGE_INTEGER* pcbRead,
        ULARGE_INTEGER* pcbWritten)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ITSS_IStream_Commit(
        IStream* iface,
        DWORD grfCommitFlags)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ITSS_IStream_Revert(
        IStream* iface)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ITSS_IStream_LockRegion(
        IStream* iface,
        ULARGE_INTEGER libOffset,
        ULARGE_INTEGER cb,
        DWORD dwLockType)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ITSS_IStream_UnlockRegion(
        IStream* iface,
        ULARGE_INTEGER libOffset,
        ULARGE_INTEGER cb,
        DWORD dwLockType)
{
    FIXME("\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ITSS_IStream_Stat(
        IStream* iface,
        STATSTG* pstatstg,
        DWORD grfStatFlag)
{
    ICOM_THIS(IStream_Impl,iface);

    TRACE("%p %p %ld\n", This, pstatstg, grfStatFlag);

    memset( pstatstg, 0, sizeof *pstatstg );
    if( !( grfStatFlag & STATFLAG_NONAME ) )
    {
        FIXME("copy the name\n");
    }
    pstatstg->type = STGTY_STREAM;
    pstatstg->cbSize.QuadPart = This->ui.length;
    pstatstg->grfMode = STGM_READ;
    memcpy( &pstatstg->clsid, &CLSID_ITStorage, sizeof (CLSID) );

    return S_OK;
}

static HRESULT WINAPI ITSS_IStream_Clone(
        IStream* iface,
        IStream** ppstm)
{
    FIXME("\n");
    return E_NOTIMPL;
}

struct IStreamVtbl ITSS_IStream_vtbl =
{
    ITSS_IStream_QueryInterface,
    ITSS_IStream_AddRef,
    ITSS_IStream_Release,
    ITSS_IStream_Read,
    ITSS_IStream_Write,
    ITSS_IStream_Seek,
    ITSS_IStream_SetSize,
    ITSS_IStream_CopyTo,
    ITSS_IStream_Commit,
    ITSS_IStream_Revert,
    ITSS_IStream_LockRegion,
    ITSS_IStream_UnlockRegion,
    ITSS_IStream_Stat,
    ITSS_IStream_Clone,
};

static IStream_Impl *ITSS_create_stream(
           ITSS_IStorageImpl *stg, struct chmUnitInfo *ui )
{
    IStream_Impl *stm;

    stm = HeapAlloc( GetProcessHeap(), 0, sizeof (IStream_Impl) );
    stm->vtbl_IStream = &ITSS_IStream_vtbl;
    stm->ref = 1;
    stm->addr = 0;
    memcpy( &stm->ui, ui, sizeof stm->ui );
    stm->stg = stg;
    IStorage_AddRef( (IStorage*) stg );

    TRACE(" -> %p\n", stm );

    return stm;
}
