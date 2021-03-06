/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <cstdarg>
#include <math.h>
#include <osl/file.h>
#include <tools/stream.hxx>
#include <sane.hxx>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sal/config.h>
#include <sal/macros.h>
#include <boost/scoped_array.hpp>

#if (OSL_DEBUG_LEVEL > 1) || defined DBG_UTIL
#include <stdarg.h>
#define dump_state( a, b, c, d ) fprintf( stderr, a, b, c, d );
#else
#define dump_state( a, b, c, d ) ;
#endif
inline void dbg_msg( const char* pString, ... )
{
#if (OSL_DEBUG_LEVEL > 1) || defined DBG_UTIL
    va_list ap;
    va_start( ap, pString );
    vfprintf( stderr, pString, ap );
    va_end( ap );
#else
    (void)pString;
#endif
}

#define FAIL_SHUTDOWN_STATE( x, y, z ) \
    if( x != SANE_STATUS_GOOD )                             \
    {                                                       \
        dump_state( "%s returned error %d (%s)\n",          \
                 y, x, p_strstatus( x ) );                  \
        DeInit();                                           \
        return z;                                           \
    }

#define FAIL_STATE( x, y, z ) \
    if( x != SANE_STATUS_GOOD )                             \
    {                                                       \
        dump_state( "%s returned error %d (%s)\n",          \
                 y, x, p_strstatus( x ) );                  \
        return z;                                           \
    }

#define DUMP_STATE( x, y ) \
    if( x != SANE_STATUS_GOOD )                             \
    {                                                       \
        dump_state( "%s returned error %d (%s)\n",          \
                 y, x, p_strstatus( x ) );                  \
    }

int             Sane::nRefCount = 0;
oslModule       Sane::pSaneLib = 0;
SANE_Int        Sane::nVersion = 0;
SANE_Device**   Sane::ppDevices = 0;
int             Sane::nDevices = 0;

SANE_Status     (*Sane::p_init)( SANE_Int*,
                                 SANE_Auth_Callback ) = 0;
void            (*Sane::p_exit)() = 0;
SANE_Status     (*Sane::p_get_devices)( const SANE_Device***,
                                        SANE_Bool ) = 0;
SANE_Status     (*Sane::p_open)( SANE_String_Const, SANE_Handle ) = 0;
void            (*Sane::p_close)( SANE_Handle ) = 0;
const SANE_Option_Descriptor* (*Sane::p_get_option_descriptor)(
    SANE_Handle, SANE_Int ) = 0;
SANE_Status     (*Sane::p_control_option)( SANE_Handle, SANE_Int,
                                           SANE_Action, void*,
                                           SANE_Int* ) = 0;
SANE_Status     (*Sane::p_get_parameters)( SANE_Handle,
                                           SANE_Parameters* ) = 0;
SANE_Status     (*Sane::p_start)( SANE_Handle ) = 0;
SANE_Status     (*Sane::p_read)( SANE_Handle, SANE_Byte*, SANE_Int,
                                 SANE_Int* ) = 0;
void            (*Sane::p_cancel)( SANE_Handle ) = 0;
SANE_Status     (*Sane::p_set_io_mode)( SANE_Handle, SANE_Bool ) = 0;
SANE_Status     (*Sane::p_get_select_fd)( SANE_Handle, SANE_Int* ) = 0;
SANE_String_Const (*Sane::p_strstatus)( SANE_Status ) = 0;

static bool bSaneSymbolLoadFailed = false;

inline oslGenericFunction Sane::LoadSymbol( const char* pSymbolname )
{
    oslGenericFunction pFunction = osl_getAsciiFunctionSymbol( pSaneLib, pSymbolname );
    if( ! pFunction )
    {
        fprintf( stderr, "Could not load symbol %s\n",
                 pSymbolname );
        bSaneSymbolLoadFailed = true;
    }
    return pFunction;
}

SANE_Status Sane::ControlOption( int nOption, SANE_Action nAction,
                                 void* pData )
{
    SANE_Int    nInfo = 0;

    SANE_Status nStatus = p_control_option( maHandle, (SANE_Int)nOption,
                                nAction, pData, &nInfo );
    DUMP_STATE( nStatus, "sane_control_option" );
#if OSL_DEBUG_LEVEL > 1
    if( nStatus != SANE_STATUS_GOOD )
    {
        const char* pAction = "Unknown";
        switch( nAction )
        {
            case SANE_ACTION_GET_VALUE:
                pAction = "SANE_ACTION_GET_VALUE";break;
            case SANE_ACTION_SET_VALUE:
                pAction = "SANE_ACTION_SET_VALUE";break;
            case SANE_ACTION_SET_AUTO:
                pAction = "SANE_ACTION_SET_AUTO";break;
        }
        dbg_msg( "Option: \"%s\" action: %s\n",
                 OUStringToOString(GetOptionName(nOption), osl_getThreadTextEncoding()).getStr(),
                 pAction );
    }
#endif
    if( nInfo &  SANE_INFO_RELOAD_OPTIONS )
        ReloadOptions();
    return nStatus;
}

Sane::Sane() :
        mppOptions( 0 ),
        mnOptions( 0 ),
        mnDevice( -1 ),
        maHandle( 0 )
{
    if( ! nRefCount || ! pSaneLib )
        Init();
    nRefCount++;
};

Sane::~Sane()
{
    if( IsOpen() )
        Close();
    nRefCount--;
    if( ! nRefCount && pSaneLib )
        DeInit();
}

void Sane::Init()
{
    OUString sSaneLibName( "libsane" SAL_DLLEXTENSION  );
    pSaneLib = osl_loadModule( sSaneLibName.pData, SAL_LOADMODULE_LAZY );
    if( ! pSaneLib )
    {
        sSaneLibName = "libsane" SAL_DLLEXTENSION ".1";
        pSaneLib = osl_loadModule( sSaneLibName.pData, SAL_LOADMODULE_LAZY );
    }
    // try reasonable places that might not be in the library search path
    if( ! pSaneLib )
    {
        OUString sSaneLibSystemPath( "/usr/local/lib/libsane" SAL_DLLEXTENSION  );
        osl_getFileURLFromSystemPath( sSaneLibSystemPath.pData, &sSaneLibName.pData );
        pSaneLib = osl_loadModule( sSaneLibName.pData, SAL_LOADMODULE_LAZY );
    }

    if( pSaneLib )
    {
        bSaneSymbolLoadFailed = false;
        p_init = (SANE_Status(*)(SANE_Int*, SANE_Auth_Callback ))
            LoadSymbol( "sane_init" );
        p_exit = (void(*)())
            LoadSymbol( "sane_exit" );
        p_get_devices = (SANE_Status(*)(const SANE_Device***,
                                        SANE_Bool ))
            LoadSymbol( "sane_get_devices" );
        p_open = (SANE_Status(*)(SANE_String_Const, SANE_Handle ))
            LoadSymbol( "sane_open" );
        p_close = (void(*)(SANE_Handle))
            LoadSymbol( "sane_close" );
        p_get_option_descriptor = (const SANE_Option_Descriptor*(*)(SANE_Handle,
                                                              SANE_Int))
            LoadSymbol( "sane_get_option_descriptor" );
        p_control_option = (SANE_Status(*)(SANE_Handle, SANE_Int,
                                           SANE_Action, void*, SANE_Int*))
            LoadSymbol( "sane_control_option" );
        p_get_parameters = (SANE_Status(*)(SANE_Handle,SANE_Parameters*))
            LoadSymbol( "sane_get_parameters" );
        p_start = (SANE_Status(*)(SANE_Handle))
            LoadSymbol( "sane_start" );
        p_read = (SANE_Status(*)(SANE_Handle, SANE_Byte*,
                                 SANE_Int, SANE_Int* ))
            LoadSymbol( "sane_read" );
        p_cancel = (void(*)(SANE_Handle))
            LoadSymbol( "sane_cancel" );
        p_set_io_mode = (SANE_Status(*)(SANE_Handle, SANE_Bool))
            LoadSymbol( "sane_set_io_mode" );
        p_get_select_fd = (SANE_Status(*)(SANE_Handle, SANE_Int*))
            LoadSymbol( "sane_get_select_fd" );
        p_strstatus = (SANE_String_Const(*)(SANE_Status))
            LoadSymbol( "sane_strstatus" );
        if( bSaneSymbolLoadFailed )
            DeInit();
        else
        {
            SANE_Status nStatus = p_init( &nVersion, 0 );
            FAIL_SHUTDOWN_STATE( nStatus, "sane_init", );
            nStatus = p_get_devices( (const SANE_Device***)&ppDevices,
                                     SANE_FALSE );
            FAIL_SHUTDOWN_STATE( nStatus, "sane_get_devices", );
            for( nDevices = 0 ; ppDevices[ nDevices ]; nDevices++ ) ;
        }
    }
#if (OSL_DEBUG_LEVEL > 1) || defined DBG_UTIL
    else
        fprintf( stderr, "libsane%s could not be opened: %s\n", SAL_DLLEXTENSION,
                 dlerror() );
#endif
}

void Sane::DeInit()
{
    if( pSaneLib )
    {
        p_exit();
        osl_unloadModule( pSaneLib );
        pSaneLib = 0;
    }
}

void Sane::ReloadDevices()
{
    if( IsOpen() )
        Close();
    DeInit();
    Init();
}

void Sane::ReloadOptions()
{
    if( ! IsOpen() )
        return;

    const SANE_Option_Descriptor* pZero = p_get_option_descriptor( maHandle, 0 );
    SANE_Word pOptions[2];
    SANE_Status nStatus = p_control_option( maHandle, 0, SANE_ACTION_GET_VALUE,
                                            (void*)pOptions, NULL );
    if( nStatus != SANE_STATUS_GOOD )
        fprintf( stderr, "Error: sane driver returned %s while reading number of options !\n", p_strstatus( nStatus ) );

    mnOptions = pOptions[ 0 ];
    if( (size_t)pZero->size > sizeof( SANE_Word ) )
        fprintf( stderr, "driver returned numer of options with larger size tha SANE_Word !!!\n" );
    if( mppOptions )
        delete [] mppOptions;
    mppOptions = new const SANE_Option_Descriptor*[ mnOptions ];
    mppOptions[ 0 ] = pZero;
    for( int i = 1; i < mnOptions; i++ )
        mppOptions[ i ] =  (SANE_Option_Descriptor*)
            p_get_option_descriptor( maHandle, i );

    CheckConsistency( NULL, true );

    maReloadOptionsLink.Call( this );
}

bool Sane::Open( const char* name )
{
    SANE_Status nStatus = p_open( (SANE_String_Const)name, &maHandle );
    FAIL_STATE( nStatus, "sane_open", false );

    ReloadOptions();

    if( mnDevice == -1 )
    {
        OString aDevice( name );
        for( int i = 0; i < nDevices; i++ )
        {
            if( aDevice.equals( ppDevices[i]->name ) )
            {
                mnDevice = i;
                break;
            }
        }
    }

    return true;
}

bool Sane::Open( int n )
{
    if( n >= 0 && n < nDevices )
    {
        mnDevice = n;
        return Open( (char*)ppDevices[n]->name );
    }
    return false;
}

void Sane::Close()
{
    if( maHandle )
    {
        p_close( maHandle );
        delete [] mppOptions;
        mppOptions = 0;
        maHandle = 0;
        mnDevice = -1;
    }
}

int Sane::GetOptionByName( const char* rName )
{
    int i;
    OString aOption( rName );
    for( i = 0; i < mnOptions; i++ )
    {
        if( mppOptions[i]->name && aOption.equals( mppOptions[i]->name ) )
            return i;
    }
    return -1;
}

bool Sane::GetOptionValue( int n, bool& rRet )
{
    if( ! maHandle  ||  mppOptions[n]->type != SANE_TYPE_BOOL )
        return false;
    SANE_Word nRet;
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_GET_VALUE, &nRet );
    if( nStatus != SANE_STATUS_GOOD )
        return false;

    rRet = nRet;
    return true;
}

bool Sane::GetOptionValue( int n, OString& rRet )
{
    bool bSuccess = false;
    if( ! maHandle  ||  mppOptions[n]->type != SANE_TYPE_STRING )
        return false;
    boost::scoped_array<char> pRet(new char[mppOptions[n]->size+1]);
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_GET_VALUE, pRet.get() );
    if( nStatus == SANE_STATUS_GOOD )
    {
        bSuccess = true;
        rRet = pRet.get();
    }
    return bSuccess;
}

bool Sane::GetOptionValue( int n, double& rRet, int nElement )
{
    bool bSuccess = false;

    if( ! maHandle  ||  ( mppOptions[n]->type != SANE_TYPE_INT &&
                          mppOptions[n]->type != SANE_TYPE_FIXED ) )
        return false;

    boost::scoped_array<SANE_Word> pRet(new SANE_Word[mppOptions[n]->size/sizeof(SANE_Word)]);
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_GET_VALUE, pRet.get() );
    if( nStatus == SANE_STATUS_GOOD )
    {
        bSuccess = true;
        if( mppOptions[n]->type == SANE_TYPE_INT )
            rRet = (double)pRet[ nElement ];
        else
            rRet = SANE_UNFIX( pRet[nElement] );
    }
    return bSuccess;
}

bool Sane::GetOptionValue( int n, double* pSet )
{
    if( ! maHandle  || ! ( mppOptions[n]->type == SANE_TYPE_FIXED ||
                           mppOptions[n]->type == SANE_TYPE_INT ) )
        return false;

    boost::scoped_array<SANE_Word> pFixedSet(new SANE_Word[mppOptions[n]->size/sizeof(SANE_Word)]);
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_GET_VALUE, pFixedSet.get() );
    if( nStatus != SANE_STATUS_GOOD )
        return false;
    for( size_t i = 0; i <mppOptions[n]->size/sizeof(SANE_Word); i++ )
    {
        if( mppOptions[n]->type == SANE_TYPE_FIXED )
            pSet[i] = SANE_UNFIX( pFixedSet[i] );
        else
            pSet[i] = (double) pFixedSet[i];
    }
    return true;
}

bool Sane::SetOptionValue( int n, bool bSet )
{
    if( ! maHandle  ||  mppOptions[n]->type != SANE_TYPE_BOOL )
        return false;
    SANE_Word nRet = bSet ? SANE_TRUE : SANE_FALSE;
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_SET_VALUE, &nRet );
    if( nStatus != SANE_STATUS_GOOD )
        return false;
    return true;
}

bool Sane::SetOptionValue( int n, const OUString& rSet )
{
    if( ! maHandle  ||  mppOptions[n]->type != SANE_TYPE_STRING )
        return false;
    OString aSet(OUStringToOString(rSet, osl_getThreadTextEncoding()));
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_SET_VALUE, (void*)aSet.getStr() );
    if( nStatus != SANE_STATUS_GOOD )
        return false;
    return true;
}

bool Sane::SetOptionValue( int n, double fSet, int nElement )
{
    bool bSuccess = false;

    if( ! maHandle  ||  ( mppOptions[n]->type != SANE_TYPE_INT &&
                          mppOptions[n]->type != SANE_TYPE_FIXED ) )
        return false;

    SANE_Status nStatus;
    if( mppOptions[n]->size/sizeof(SANE_Word) > 1 )
    {
        boost::scoped_array<SANE_Word> pSet(new SANE_Word[mppOptions[n]->size/sizeof(SANE_Word)]);
        nStatus = ControlOption( n, SANE_ACTION_GET_VALUE, pSet.get() );
        if( nStatus == SANE_STATUS_GOOD )
        {
            pSet[nElement] = mppOptions[n]->type == SANE_TYPE_INT ?
                (SANE_Word)fSet : SANE_FIX( fSet );
            nStatus = ControlOption(  n, SANE_ACTION_SET_VALUE, pSet.get() );
        }
    }
    else
    {
        SANE_Word nSetTo =
            mppOptions[n]->type == SANE_TYPE_INT ?
            (SANE_Word)fSet : SANE_FIX( fSet );

        nStatus = ControlOption( n, SANE_ACTION_SET_VALUE, &nSetTo );
        if( nStatus == SANE_STATUS_GOOD )
            bSuccess = true;
    }
    return bSuccess;
}

bool Sane::SetOptionValue( int n, double* pSet )
{
    if( ! maHandle  ||  ( mppOptions[n]->type != SANE_TYPE_INT &&
                          mppOptions[n]->type != SANE_TYPE_FIXED ) )
        return false;
    boost::scoped_array<SANE_Word> pFixedSet(new SANE_Word[mppOptions[n]->size/sizeof(SANE_Word)]);
    for( size_t i = 0; i < mppOptions[n]->size/sizeof(SANE_Word); i++ )
    {
        if( mppOptions[n]->type == SANE_TYPE_FIXED )
            pFixedSet[i] = SANE_FIX( pSet[i] );
        else
            pFixedSet[i] = (SANE_Word)pSet[i];
    }
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_SET_VALUE, pFixedSet.get() );
    if( nStatus != SANE_STATUS_GOOD )
        return false;
    return true;
}

enum FrameStyleType {
    FrameStyle_BW, FrameStyle_Gray, FrameStyle_RGB, FrameStyle_Separated
};

#define BYTE_BUFFER_SIZE 32768

static inline sal_uInt8 _ReadValue( FILE* fp, int depth )
{
    if( depth == 16 )
    {
        sal_uInt16 nWord;
        // data always come in native byte order !
        // 16 bits is not really supported by backends as of now
        // e.g. UMAX Astra 1200S delivers 16 bit but in BIGENDIAN
        // against SANE documentation (xscanimage gets the same result
        // as we do
        size_t items_read = fread( &nWord, 1, 2, fp );

        if (items_read != 2)
        {
             SAL_WARN( "extensions.scanner", "short read, abandoning" );
             return 0;
        }

        return (sal_uInt8)( nWord / 256 );
    }
    sal_uInt8 nByte;
    size_t items_read = fread( &nByte, 1, 1, fp );
    if (items_read != 1)
    {
        SAL_WARN( "extensions.scanner", "short read, abandoning" );
        return 0;
    }
    return nByte;
}

bool Sane::CheckConsistency( const char* pMes, bool bInit )
{
    static const SANE_Option_Descriptor** pDescArray = NULL;
    static const SANE_Option_Descriptor*  pZero = NULL;

    if( bInit )
    {
        pDescArray = mppOptions;
        if( mppOptions )
            pZero = mppOptions[0];
        return true;
    }

    bool bConsistent = true;

    if( pDescArray != mppOptions )
        bConsistent = false;
    if( pZero != mppOptions[0] )
        bConsistent = false;

    if( ! bConsistent )
        dbg_msg( "Sane is not consistent. (%s)\n", pMes );

    return bConsistent;
}

bool Sane::Start( BitmapTransporter& rBitmap )
{
    int nStream = 0, nLine = 0, i = 0;
    SANE_Parameters aParams;
    FrameStyleType eType = FrameStyle_Gray;
    bool bSuccess = true;
    bool bWidthSet = false;

    if( ! maHandle )
        return false;

    int nWidthMM    = 0;
    int nHeightMM   = 0;
    double fTLx, fTLy, fBRx, fBRy, fResl = 0.0;
    int nOption;
    if( ( nOption = GetOptionByName( "tl-x" ) ) != -1   &&
        GetOptionValue( nOption, fTLx, 0 )              &&
        GetOptionUnit( nOption ) == SANE_UNIT_MM )
    {
        if( ( nOption = GetOptionByName( "br-x" ) ) != -1   &&
            GetOptionValue( nOption, fBRx, 0 )              &&
            GetOptionUnit( nOption ) == SANE_UNIT_MM )
        {
            nWidthMM = (int)fabs(fBRx - fTLx);
        }
    }
    if( ( nOption = GetOptionByName( "tl-y" ) ) != -1   &&
        GetOptionValue( nOption, fTLy, 0 )              &&
        GetOptionUnit( nOption ) == SANE_UNIT_MM )
    {
        if( ( nOption = GetOptionByName( "br-y" ) ) != -1   &&
            GetOptionValue( nOption, fBRy, 0 )              &&
            GetOptionUnit( nOption ) == SANE_UNIT_MM )
        {
            nHeightMM = (int)fabs(fBRy - fTLy);
        }
    }
    if( ( nOption = GetOptionByName( "resolution" ) ) != -1 )
        GetOptionValue( nOption, fResl );

    sal_uInt8* pBuffer = NULL;

    SANE_Status nStatus = SANE_STATUS_GOOD;

    rBitmap.lock();
    SvMemoryStream& aConverter = rBitmap.getStream();
    aConverter.Seek( 0 );
    aConverter.SetNumberFormatInt( NUMBERFORMAT_INT_LITTLEENDIAN );

    // write bitmap stream header
    aConverter.WriteChar( 'B' ).WriteChar( 'M' );
    aConverter.WriteUInt32( (sal_uInt32) 0 );
    aConverter.WriteUInt32( (sal_uInt32) 0 );
    aConverter.WriteUInt32( (sal_uInt32) 60 );

    // write BITMAPINFOHEADER
    aConverter.WriteUInt32( (sal_uInt32)40 );
    aConverter.WriteUInt32( (sal_uInt32)0 ); // fill in width later
    aConverter.WriteUInt32( (sal_uInt32)0 ); // fill in height later
    aConverter.WriteUInt16( (sal_uInt16)1 );
    // create header for 24 bits
    // correct later if necessary
    aConverter.WriteUInt16( (sal_uInt16)24 );
    aConverter.WriteUInt32( (sal_uInt32)0 );
    aConverter.WriteUInt32( (sal_uInt32)0 );
    aConverter.WriteUInt32( (sal_uInt32)0 );
    aConverter.WriteUInt32( (sal_uInt32)0 );
    aConverter.WriteUInt32( (sal_uInt32)0 );
    aConverter.WriteUInt32( (sal_uInt32)0 );

    for( nStream=0; nStream < 3 && bSuccess ; nStream++ )
    {
        nStatus = p_start( maHandle );
        DUMP_STATE( nStatus, "sane_start" );
        CheckConsistency( "sane_start" );
        if( nStatus == SANE_STATUS_GOOD )
        {
            nStatus = p_get_parameters( maHandle, &aParams );
            DUMP_STATE( nStatus, "sane_get_parameters" );
            CheckConsistency( "sane_get_parameters" );
            if (nStatus != SANE_STATUS_GOOD || aParams.bytes_per_line == 0)
            {
                bSuccess = false;
                break;
            }
#if (OSL_DEBUG_LEVEL > 1) || defined DBG_UTIL
            const char* ppFormats[] = { "SANE_FRAME_GRAY", "SANE_FRAME_RGB",
                                  "SANE_FRAME_RED", "SANE_FRAME_GREEN",
                                  "SANE_FRAME_BLUE", "Unknown !!!" };
            fprintf( stderr, "Parameters for frame %d:\n", nStream );
            if( aParams.format < 0 || aParams.format > 4 )
                aParams.format = (SANE_Frame)5;
            fprintf( stderr, "format:           %s\n", ppFormats[ (int)aParams.format ] );
            fprintf( stderr, "last_frame:       %s\n", aParams.last_frame ? "TRUE" : "FALSE" );
            fprintf( stderr, "depth:            %d\n", (int)aParams.depth );
            fprintf( stderr, "pixels_per_line:  %d\n", (int)aParams.pixels_per_line );
            fprintf( stderr, "bytes_per_line:   %d\n", (int)aParams.bytes_per_line );
#endif
            if( ! pBuffer )
            {
                pBuffer = new sal_uInt8[ BYTE_BUFFER_SIZE < 4*aParams.bytes_per_line ? 4*aParams.bytes_per_line : BYTE_BUFFER_SIZE ];
            }

            if( aParams.last_frame )
                nStream=3;

            switch( aParams.format )
            {
                case SANE_FRAME_GRAY:
                    eType = FrameStyle_Gray;
                    if( aParams.depth == 1 )
                        eType = FrameStyle_BW;
                    break;
                case SANE_FRAME_RGB:
                    eType = FrameStyle_RGB;
                    break;
                case SANE_FRAME_RED:
                case SANE_FRAME_GREEN:
                case SANE_FRAME_BLUE:
                    eType = FrameStyle_Separated;
                    break;
                default:
                    fprintf( stderr, "Warning: unknown frame style !!!\n" );
            }

            bool bSynchronousRead = true;

            // should be fail safe, but ... ??
            nStatus = p_set_io_mode( maHandle, SANE_FALSE );
            CheckConsistency( "sane_set_io_mode" );
            if( nStatus != SANE_STATUS_GOOD )
            {
                bSynchronousRead = false;
                nStatus = p_set_io_mode( maHandle, SANE_TRUE );
                CheckConsistency( "sane_set_io_mode" );
#if (OSL_DEBUG_LEVEL > 1) || defined DBG_UTIL
                if( nStatus != SANE_STATUS_GOOD )
                    // what ?!?
                    fprintf( stderr, "Sane::Start: driver is confused\n" );
#endif
            }

            SANE_Int nLen=0;
            SANE_Int fd = 0;

            if( ! bSynchronousRead )
            {
                nStatus = p_get_select_fd( maHandle, &fd );
                DUMP_STATE( nStatus, "sane_get_select_fd" );
                CheckConsistency( "sane_get_select_fd" );
                if( nStatus != SANE_STATUS_GOOD )
                    bSynchronousRead = true;
            }
            FILE* pFrame = tmpfile();
            if( ! pFrame )
            {
                bSuccess = false;
                break;
            }
            do {
                if( ! bSynchronousRead )
                {
                    fd_set fdset;
                    struct timeval tv;

                    FD_ZERO( &fdset );
                    FD_SET( (int)fd, &fdset );
                    tv.tv_sec = 5;
                    tv.tv_usec = 0;
                    if( select( fd+1, &fdset, NULL, NULL, &tv ) == 0 )
                        fprintf( stderr, "Timout on sane_read descriptor\n" );
                }
                nLen = 0;
                nStatus = p_read( maHandle, pBuffer, BYTE_BUFFER_SIZE, &nLen );
                CheckConsistency( "sane_read" );
                if( nLen && ( nStatus == SANE_STATUS_GOOD ||
                              nStatus == SANE_STATUS_EOF ) )
                {
                    bSuccess = (static_cast<size_t>(nLen) == fwrite( pBuffer, 1, nLen, pFrame ));
                    if (!bSuccess)
                        break;
                }
                else
                    DUMP_STATE( nStatus, "sane_read" );
            } while( nStatus == SANE_STATUS_GOOD );
            if (nStatus != SANE_STATUS_EOF || !bSuccess)
            {
                fclose( pFrame );
                bSuccess = false;
                break;
            }

            int nFrameLength = ftell( pFrame );
            fseek( pFrame, 0, SEEK_SET );
            sal_uInt32 nWidth = (sal_uInt32) aParams.pixels_per_line;
            sal_uInt32 nHeight = (sal_uInt32) (nFrameLength / aParams.bytes_per_line);
            if( ! bWidthSet )
            {
                if( ! fResl )
                    fResl = 300; // if all else fails that's a good guess
                if( ! nWidthMM )
                    nWidthMM = (int)(((double)nWidth / fResl) * 25.4);
                if( ! nHeightMM )
                    nHeightMM = (int)(((double)nHeight / fResl) * 25.4);
#if OSL_DEBUG_LEVEL > 1
                fprintf( stderr, "set dimensions to (%d, %d) Pixel, (%d, %d) mm, resolution is %lg\n", (int)nWidth, (int)nHeight, (int)nWidthMM, (int)nHeightMM, fResl );
#endif

                aConverter.Seek( 18 );
                aConverter.WriteUInt32( (sal_uInt32)nWidth );
                aConverter.WriteUInt32( (sal_uInt32)nHeight );
                aConverter.Seek( 38 );
                aConverter.WriteUInt32( (sal_uInt32)(1000*nWidth/nWidthMM) );
                aConverter.WriteUInt32( (sal_uInt32)(1000*nHeight/nHeightMM) );
                bWidthSet = true;
            }
            aConverter.Seek(60);

            if( eType == FrameStyle_BW )
            {
                aConverter.Seek( 10 );
                aConverter.WriteUInt32( (sal_uInt32)64 );
                aConverter.Seek( 28 );
                aConverter.WriteUInt16( (sal_uInt16) 1 );
                aConverter.Seek( 54 );
                // write color table
                aConverter.WriteUInt16( (sal_uInt16)0xffff );
                aConverter.WriteUChar( (sal_uInt8)0xff );
                aConverter.WriteUChar( (sal_uInt8)0 );
                aConverter.WriteUInt32( (sal_uInt32)0 );
                aConverter.Seek( 64 );
            }
            else if( eType == FrameStyle_Gray )
            {
                 aConverter.Seek( 10 );
                 aConverter.WriteUInt32( (sal_uInt32)1084 );
                aConverter.Seek( 28 );
                aConverter.WriteUInt16( (sal_uInt16) 8 );
                aConverter.Seek( 54 );
                // write color table
                for( nLine = 0; nLine < 256; nLine++ )
                {
                    aConverter.WriteUChar( (sal_uInt8)nLine );
                    aConverter.WriteUChar( (sal_uInt8)nLine );
                    aConverter.WriteUChar( (sal_uInt8)nLine );
                    aConverter.WriteUChar( (sal_uInt8)0 );
                }
                aConverter.Seek( 1084 );
            }

            for (nLine = nHeight-1; nLine >= 0; --nLine)
            {
                fseek( pFrame, nLine * aParams.bytes_per_line, SEEK_SET );
                if( eType == FrameStyle_BW ||
                    ( eType == FrameStyle_Gray && aParams.depth == 8 )
                    )
                {
                    SANE_Int items_read = fread( pBuffer, 1, aParams.bytes_per_line, pFrame );
                    if (items_read != aParams.bytes_per_line)
                    {
                        SAL_WARN( "extensions.scanner", "short read, padding with zeros" );
                        memset(pBuffer + items_read, 0, aParams.bytes_per_line - items_read);
                    }
                    aConverter.Write( pBuffer, aParams.bytes_per_line );
                }
                else if( eType == FrameStyle_Gray )
                {
                    for( i = 0; i < (aParams.pixels_per_line); i++ )
                    {
                        sal_uInt8 nGray = _ReadValue( pFrame, aParams.depth );
                        aConverter.WriteUChar( nGray );
                    }
                }
                else if( eType == FrameStyle_RGB )
                {
                    for( i = 0; i < (aParams.pixels_per_line); i++ )
                    {
                        sal_uInt8 nRed, nGreen, nBlue;
                        nRed    = _ReadValue( pFrame, aParams.depth );
                        nGreen  = _ReadValue( pFrame, aParams.depth );
                        nBlue   = _ReadValue( pFrame, aParams.depth );
                        aConverter.WriteUChar( nBlue );
                        aConverter.WriteUChar( nGreen );
                        aConverter.WriteUChar( nRed );
                    }
                }
                else if( eType == FrameStyle_Separated )
                {
                    for( i = 0; i < (aParams.pixels_per_line); i++ )
                    {
                        sal_uInt8 nValue = _ReadValue( pFrame, aParams.depth );
                        switch( aParams.format )
                        {
                            case SANE_FRAME_RED:
                                aConverter.SeekRel( 2 );
                                aConverter.WriteUChar( nValue );
                                break;
                            case SANE_FRAME_GREEN:
                                aConverter.SeekRel( 1 );
                                aConverter.WriteUChar( nValue );
                                aConverter.SeekRel( 1 );
                                break;
                            case SANE_FRAME_BLUE:
                                aConverter.WriteUChar( nValue );
                                aConverter.SeekRel( 2 );
                                break;
                            case SANE_FRAME_GRAY:
                            case SANE_FRAME_RGB:
                                break;
                        }
                    }
                }
                 int nGap = aConverter.Tell() & 3;
                 if( nGap )
                     aConverter.SeekRel( 4-nGap );
            }
            fclose( pFrame ); // deletes tmpfile
            if( eType != FrameStyle_Separated )
                break;
        }
        else
            bSuccess = false;
    }
    // get stream length
    aConverter.Seek( STREAM_SEEK_TO_END );
    int nPos = aConverter.Tell();

    aConverter.Seek( 2 );
    aConverter.WriteUInt32( (sal_uInt32) nPos+1 );
    aConverter.Seek( 0 );

    rBitmap.unlock();

    if( bSuccess )
    {
        // only cancel a successful operation
        // sane disrupts memory else
        p_cancel( maHandle );
        CheckConsistency( "sane_cancel" );
    }
    if( pBuffer )
        delete [] pBuffer;

    ReloadOptions();


    dbg_msg( "Sane::Start returns with %s\n", bSuccess ? "TRUE" : "FALSE" );

    return bSuccess;
}

int Sane::GetRange( int n, double*& rpDouble )
{
    if( mppOptions[n]->constraint_type != SANE_CONSTRAINT_RANGE &&
        mppOptions[n]->constraint_type != SANE_CONSTRAINT_WORD_LIST )
    {
        return -1;
    }

    rpDouble = 0;
    int nItems, i;
    bool bIsFixed = mppOptions[n]->type == SANE_TYPE_FIXED ? sal_True : sal_False;

    dbg_msg( "Sane::GetRange of option %s ", mppOptions[n]->name );
    if(mppOptions[n]->constraint_type == SANE_CONSTRAINT_RANGE )
    {
        double fMin, fMax, fQuant;
        if( bIsFixed )
        {
            fMin = SANE_UNFIX( mppOptions[n]->constraint.range->min );
            fMax = SANE_UNFIX( mppOptions[n]->constraint.range->max );
            fQuant = SANE_UNFIX( mppOptions[n]->constraint.range->quant );
        }
        else
        {
            fMin = (double)mppOptions[n]->constraint.range->min;
            fMax = (double)mppOptions[n]->constraint.range->max;
            fQuant = (double)mppOptions[n]->constraint.range->quant;
        }
        if( fQuant != 0.0 )
        {
            dbg_msg( "quantum range [ %lg ; %lg ; %lg ]\n",
                     fMin, fQuant, fMax );
            nItems = (int)((fMax - fMin)/fQuant)+1;
            rpDouble = new double[ nItems ];
            double fValue = fMin;
            for( i = 0; i < nItems; i++, fValue += fQuant )
                rpDouble[i] = fValue;
            rpDouble[ nItems-1 ] = fMax;
            return nItems;
        }
        else
        {
            dbg_msg( "normal range [ %lg %lg ]\n",
                     fMin, fMax );
            rpDouble = new double[2];
            rpDouble[0] = fMin;
            rpDouble[1] = fMax;
            return 0;
        }
    }
    else
    {
        nItems = mppOptions[n]->constraint.word_list[0];
        rpDouble = new double[nItems];
        for( i=0; i<nItems; i++ )
        {
            rpDouble[i] = bIsFixed ?
                SANE_UNFIX( mppOptions[n]->constraint.word_list[i+1] ) :
                (double)mppOptions[n]->constraint.word_list[i+1];
        }
        dbg_msg( "wordlist [ %lg ... %lg ]\n",
                 rpDouble[ 0 ], rpDouble[ nItems-1 ] );
        return nItems;
    }
}

static const char *ppUnits[] = {
    "",
    "[Pixel]",
    "[Bit]",
    "[mm]",
    "[DPI]",
    "[%]",
    "[usec]"
};

OUString Sane::GetOptionUnitName( int n )
{
    OUString aText;
    SANE_Unit nUnit = mppOptions[n]->unit;
    size_t nUnitAsSize = (size_t)nUnit;
    if (nUnitAsSize >= SAL_N_ELEMENTS( ppUnits ))
        aText = "[unknown units]";
    else
        aText = OUString( ppUnits[ nUnit ], strlen(ppUnits[ nUnit ]), osl_getThreadTextEncoding() );
    return aText;
}

bool Sane::ActivateButtonOption( int n )
{
    SANE_Status nStatus = ControlOption( n, SANE_ACTION_SET_VALUE, NULL );
    if( nStatus != SANE_STATUS_GOOD )
        return false;
    return true;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
