/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 * Purpose:  Adjusted minizip "zip.c" source code for zip services.
 *
 * Modified version by Even Rouault. :
 *   - Decoration of symbol names unz* -> cpl_unz*
 *   - Undef EXPORT so that we are sure the symbols are not exported
 *   - Remove old C style function prototypes
 *   - Added CPL* simplified API at bottom.
 *
 *   Original license available in port/LICENCE_minizip
 *
 *****************************************************************************/

/* zip.c -- IO on .zip files using zlib
   Version 1.01e, February 12th, 2005

   27 Dec 2004 Rolf Kalbermatter
   Modification to zipOpen2 to support globalComment retrieval.

   Copyright (C) 1998-2005 Gilles Vollant
 * Copyright (c) 2010-2012, Even Rouault <even dot rouault at mines-paris dot org>

   Read zip.h for more info
*/

#include "cpl_port.h"
#include "cpl_minizip_zip.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_minizip_unzip.h"
#include "cpl_string.h"

#ifdef NO_ERRNO_H
    extern int errno;
#else
#   include <errno.h>
#endif

CPL_CVSID("$Id: cpl_minizip_zip.cpp 0f654dda9faabf9d86a44293f0f89903a8e97dd7 2018-04-15 20:18:32 +0200 Even Rouault $")

#ifndef VERSIONMADEBY
# define VERSIONMADEBY   (0x0) /* platform dependent */
#endif

#ifndef Z_BUFSIZE
#define Z_BUFSIZE (16384)
#endif

#ifndef ALLOC
# define ALLOC(size) (malloc(size))
#endif
#ifndef TRYFREE
# define TRYFREE(p) {if (p) free(p);}
#endif

/*
#define SIZECENTRALDIRITEM (0x2e)
#define SIZEZIPLOCALHEADER (0x1e)
*/

/* I've found an old Unix (a SunOS 4.1.3_U1) without all SEEK_* defined... */

#ifndef SEEK_CUR
#define SEEK_CUR    1
#endif

#ifndef SEEK_END
#define SEEK_END    2
#endif

#ifndef SEEK_SET
#define SEEK_SET    0
#endif

#ifndef DEF_MEM_LEVEL
#if MAX_MEM_LEVEL >= 8
#  define DEF_MEM_LEVEL 8
#else
#  define DEF_MEM_LEVEL  MAX_MEM_LEVEL
#endif
#endif

CPL_UNUSED static const char zip_copyright[] =
    " zip 1.01 Copyright 1998-2004 Gilles Vollant - http://www.winimage.com/zLibDll";

#define SIZEDATA_INDATABLOCK (4096-(4*4))

#define LOCALHEADERMAGIC    (0x04034b50)
#define CENTRALHEADERMAGIC  (0x02014b50)
#define ENDHEADERMAGIC      (0x06054b50)

#define FLAG_LOCALHEADER_OFFSET (0x06)
#define CRC_LOCALHEADER_OFFSET  (0x0e)

#define SIZECENTRALHEADER (0x2e) /* 46 */

typedef struct linkedlist_datablock_internal_s
{
  struct linkedlist_datablock_internal_s* next_datablock;
  uLong  avail_in_this_block;
  uLong  filled_in_this_block;
  uLong  unused;  // For future use and alignment.
  unsigned char data[SIZEDATA_INDATABLOCK];
} linkedlist_datablock_internal;

typedef struct linkedlist_data_s
{
    linkedlist_datablock_internal* first_block;
    linkedlist_datablock_internal* last_block;
} linkedlist_data;

typedef struct
{
    z_stream stream;            /* zLib stream structure for inflate */
    int  stream_initialised;    /* 1 is stream is initialised */
    uInt pos_in_buffered_data;  /* last written byte in buffered_data */

    uLong pos_local_header;     /* offset of the local header of the file
                                     currently writing */
    char* central_header;       /* central header data for the current file */
    uLong size_centralheader;   /* size of the central header for cur file */
    uLong flag;                 /* flag of the file currently writing */

    // TODO: What is "wr"?  "to"?
    int  method;                /* compression method of file currently wr.*/
    int  raw;                   /* 1 for directly writing raw data */
    Byte buffered_data[Z_BUFSIZE]; /* buffer contain compressed data to be
                                        written. */
    uLong dosDate;
    uLong crc32;
    int  encrypt;
#ifndef NOCRYPT
    unsigned long keys[3];     /* keys defining the pseudo-random sequence */
    const unsigned long* pcrc_32_tab;
    int crypt_header_size;
#endif
} curfile_info;

typedef struct
{
    zlib_filefunc_def z_filefunc;
    voidpf filestream;        /* IO structure of the zipfile */
    linkedlist_data central_dir;/* datablock with central dir in construction*/
    int  in_opened_file_inzip;  /* 1 if a file in the zip is currently writ.*/
    curfile_info ci;            /* info on the file currently writing */

    uLong begin_pos;            /* position of the beginning of the zipfile */
    uLong add_position_when_writing_offset;
    uLong number_entry;
#ifndef NO_ADDFILEINEXISTINGZIP
    char *globalcomment;
#endif
} zip_internal;

#ifndef NOCRYPT
#define INCLUDECRYPTINGCODE_IFCRYPTALLOWED
#include "crypt.h"
#endif

static linkedlist_datablock_internal* allocate_new_datablock()
{
    linkedlist_datablock_internal* ldi;
    ldi = static_cast<linkedlist_datablock_internal*>(
                 ALLOC(sizeof(linkedlist_datablock_internal)));
    if (ldi!=nullptr)
    {
        ldi->next_datablock = nullptr;
        ldi->filled_in_this_block = 0;
        ldi->avail_in_this_block = SIZEDATA_INDATABLOCK;
    }
    return ldi;
}

static void free_datablock(linkedlist_datablock_internal*ldi)
{
    while (ldi!=nullptr)
    {
        linkedlist_datablock_internal* ldinext = ldi->next_datablock;
        TRYFREE(ldi);
        ldi = ldinext;
    }
}

static void init_linkedlist(linkedlist_data*ll)
{
    ll->first_block = ll->last_block = nullptr;
}

static int add_data_in_datablock(linkedlist_data*ll,
                                const void *buf, uLong len)
{
    linkedlist_datablock_internal* ldi;
    const unsigned char* from_copy;

    if (ll==nullptr)
        return ZIP_INTERNALERROR;

    if (ll->last_block == nullptr)
    {
        ll->first_block = ll->last_block = allocate_new_datablock();
        if (ll->first_block == nullptr)
            return ZIP_INTERNALERROR;
    }

    ldi = ll->last_block;
    from_copy = reinterpret_cast<const unsigned char*>(buf);

    while (len>0)
    {
        uInt copy_this;
        uInt i;
        unsigned char* to_copy;

        if (ldi->avail_in_this_block==0)
        {
            ldi->next_datablock = allocate_new_datablock();
            if (ldi->next_datablock == nullptr)
                return ZIP_INTERNALERROR;
            ldi = ldi->next_datablock;
            ll->last_block = ldi;
        }

        if (ldi->avail_in_this_block < len)
            copy_this = static_cast<uInt>(ldi->avail_in_this_block);
        else
            copy_this = static_cast<uInt>(len);

        to_copy = &(ldi->data[ldi->filled_in_this_block]);

        for (i=0;i<copy_this;i++)
            *(to_copy+i)=*(from_copy+i);

        ldi->filled_in_this_block += copy_this;
        ldi->avail_in_this_block -= copy_this;
        from_copy += copy_this;
        len -= copy_this;
    }
    return ZIP_OK;
}

/****************************************************************************/

#ifndef NO_ADDFILEINEXISTINGZIP
/* ===========================================================================
   Inputs a long in LSB order to the given file
   nbByte == 1, 2 or 4 (byte, short or long)
*/

static int ziplocal_putValue (const zlib_filefunc_def*pzlib_filefunc_def,
                              voidpf filestream, uLong x, int nbByte)
{
    unsigned char buf[4];
    for (int n = 0; n < nbByte; n++)
    {
        buf[n] = static_cast<unsigned char>(x & 0xff);
        x >>= 8;
    }
    if (x != 0)
      {     /* data overflow - hack for ZIP64 (X Roche) */
      for (int n = 0; n < nbByte; n++)
        {
          buf[n] = 0xff;
        }
      }

    if (ZWRITE(*pzlib_filefunc_def,filestream,buf,nbByte)!=static_cast<uLong>(nbByte))
        return ZIP_ERRNO;
    else
        return ZIP_OK;
}

static void ziplocal_putValue_inmemory (void *dest, uLong x, int nbByte)
{
    unsigned char* buf= reinterpret_cast<unsigned char*>(dest);
    for (int n = 0; n < nbByte; n++) {
        buf[n] = static_cast<unsigned char>(x & 0xff);
        x >>= 8;
    }

    if (x != 0)
    {     /* data overflow - hack for ZIP64 */
       for (int n = 0; n < nbByte; n++)
       {
          buf[n] = 0xff;
       }
    }
}

/****************************************************************************/

static uLong ziplocal_TmzDateToDosDate( const tm_zip *ptm,
                                        uLong /* dosDate */ )
{
    uLong year = static_cast<uLong>(ptm->tm_year);
    if (year>1980)
        year-=1980;
    else if (year>80)
        year-=80;
    return
      static_cast<uLong>(((ptm->tm_mday) + (32 * (ptm->tm_mon+1)) + (512 * year)) << 16) |
        ((ptm->tm_sec/2) + (32* ptm->tm_min) + (2048 * static_cast<uLong>(ptm->tm_hour)));
}

/****************************************************************************/

static int ziplocal_getByte( const zlib_filefunc_def* pzlib_filefunc_def,
                             voidpf filestream, int *pi )
{
    unsigned char c = 0;
    const int err =
        static_cast<int>(ZREAD(*pzlib_filefunc_def, filestream, &c, 1));
    if (err==1)
    {
        *pi = static_cast<int>(c);
        return ZIP_OK;
    }
    else
    {
        if (ZERROR(*pzlib_filefunc_def,filestream))
            return ZIP_ERRNO;
        else
            return ZIP_EOF;
    }
}

/* ===========================================================================
   Reads a long in LSB order from the given gz_stream. Sets
*/
static int ziplocal_getShort (const zlib_filefunc_def* pzlib_filefunc_def,
                              voidpf filestream, uLong *pX)
{
    int i = 0;
    int err = ziplocal_getByte(pzlib_filefunc_def,filestream,&i);
    uLong x = static_cast<uLong>(i);

    if (err==ZIP_OK)
        err = ziplocal_getByte(pzlib_filefunc_def,filestream,&i);
    x += static_cast<uLong>(i)<<8;

    if (err==ZIP_OK)
        *pX = x;
    else
        *pX = 0;
    return err;
}

static int ziplocal_getLong (
    const zlib_filefunc_def* pzlib_filefunc_def,
    voidpf filestream,
    uLong *pX )
{
    int i = 0;
    int err = ziplocal_getByte(pzlib_filefunc_def,filestream,&i);
    uLong x = static_cast<uLong>(i);

    if (err==ZIP_OK)
        err = ziplocal_getByte(pzlib_filefunc_def,filestream,&i);
    x += static_cast<uLong>(i)<<8;

    if (err==ZIP_OK)
        err = ziplocal_getByte(pzlib_filefunc_def,filestream,&i);
    x += static_cast<uLong>(i)<<16;

    if (err==ZIP_OK)
        err = ziplocal_getByte(pzlib_filefunc_def,filestream,&i);
    x += static_cast<uLong>(i)<<24;

    if (err==ZIP_OK)
        *pX = x;
    else
        *pX = 0;
    return err;
}

#ifndef BUFREADCOMMENT
#define BUFREADCOMMENT (0x400)
#endif
/*
  Locate the Central directory of a zipfile (at the end, just before
    the global comment)
*/
static uLong ziplocal_SearchCentralDir(
    const zlib_filefunc_def* pzlib_filefunc_def,
    voidpf filestream )
{
    uLong uMaxBack=0xffff; /* maximum size of global comment */
    uLong uPosFound=0;

    if (ZSEEK(*pzlib_filefunc_def,filestream,0,ZLIB_FILEFUNC_SEEK_END) != 0)
        return 0;

    uLong uSizeFile = static_cast<uLong>(ZTELL(*pzlib_filefunc_def,filestream));

    if (uMaxBack>uSizeFile)
        uMaxBack = uSizeFile;

    unsigned char* buf = static_cast<unsigned char*>(ALLOC(BUFREADCOMMENT+4));
    if (buf==nullptr)
        return 0;

    uLong uBackRead = 4;
    while (uBackRead<uMaxBack)
    {
        if (uBackRead+BUFREADCOMMENT>uMaxBack)
            uBackRead = uMaxBack;
        else
            uBackRead+=BUFREADCOMMENT;
        uLong uReadPos = uSizeFile-uBackRead;

        uLong uReadSize = ((BUFREADCOMMENT+4) < (uSizeFile-uReadPos)) ?
                     (BUFREADCOMMENT+4) : (uSizeFile-uReadPos);
        if( ZSEEK(*pzlib_filefunc_def, filestream, uReadPos,
                  ZLIB_FILEFUNC_SEEK_SET) != 0 )
            break;

        if (ZREAD(*pzlib_filefunc_def,filestream,buf,uReadSize)!=uReadSize)
            break;

        for( int i = static_cast<int>(uReadSize) - 3; (i--) > 0;)
            if (((*(buf+i))==0x50) && ((*(buf+i+1))==0x4b) &&
                ((*(buf+i+2))==0x05) && ((*(buf+i+3))==0x06))
            {
                uPosFound = uReadPos+i;
                break;
            }

        if (uPosFound!=0)
            break;
    }
    TRYFREE(buf);
    return uPosFound;
}
#endif /* !NO_ADDFILEINEXISTINGZIP*/

/************************************************************/
extern zipFile ZEXPORT cpl_zipOpen2 (
    const char *pathname,
    int append,
    zipcharpc* globalcomment,
    zlib_filefunc_def* pzlib_filefunc_def )
{
    zip_internal ziinit;

    if (pzlib_filefunc_def==nullptr)
        cpl_fill_fopen_filefunc(&ziinit.z_filefunc);
    else
        ziinit.z_filefunc = *pzlib_filefunc_def;

    ziinit.filestream = (*(ziinit.z_filefunc.zopen_file))
                 (ziinit.z_filefunc.opaque,
                  pathname,
                  (append == APPEND_STATUS_CREATE) ?
                  (ZLIB_FILEFUNC_MODE_READ | ZLIB_FILEFUNC_MODE_WRITE | ZLIB_FILEFUNC_MODE_CREATE) :
                    (ZLIB_FILEFUNC_MODE_READ | ZLIB_FILEFUNC_MODE_WRITE | ZLIB_FILEFUNC_MODE_EXISTING));

    if (ziinit.filestream == nullptr)
        return nullptr;
    ziinit.begin_pos = static_cast<uLong>(ZTELL(ziinit.z_filefunc,ziinit.filestream));
    ziinit.in_opened_file_inzip = 0;
    ziinit.ci.stream_initialised = 0;
    ziinit.number_entry = 0;
    ziinit.add_position_when_writing_offset = 0;
    init_linkedlist(&(ziinit.central_dir));

    zip_internal* zi = static_cast<zip_internal*>(ALLOC(sizeof(zip_internal)));
    if (zi==nullptr)
    {
        ZCLOSE(ziinit.z_filefunc,ziinit.filestream);
        return nullptr;
    }

    /* now we add file in a zipfile */
#    ifndef NO_ADDFILEINEXISTINGZIP
    ziinit.globalcomment = nullptr;

    int err=ZIP_OK;
    if (append == APPEND_STATUS_ADDINZIP)
    {
        uLong byte_before_the_zipfile;/* byte before the zipfile, (>0 for sfx)*/

        uLong size_central_dir;     /* size of the central directory  */
        uLong offset_central_dir;   /* offset of start of central directory */
        uLong central_pos,uL;

        uLong number_disk;          /* number of the current dist, used for
                                    spaning ZIP, unsupported, always 0*/
        uLong number_disk_with_CD;  /* number the disk with central dir, used
                                    for spaning ZIP, unsupported, always 0*/
        uLong number_entry;
        uLong number_entry_CD;      /* total number of entries in
                                    the central dir
                                    (same than number_entry on nospan) */
        uLong size_comment;

        central_pos = ziplocal_SearchCentralDir(&ziinit.z_filefunc,ziinit.filestream);
        if (central_pos==0)
            err=ZIP_ERRNO;

        if (ZSEEK(ziinit.z_filefunc, ziinit.filestream,
                                        central_pos,ZLIB_FILEFUNC_SEEK_SET)!=0)
            err=ZIP_ERRNO;

        /* the signature, already checked */
        if (ziplocal_getLong(&ziinit.z_filefunc, ziinit.filestream,&uL)!=ZIP_OK)
            err=ZIP_ERRNO;

        /* number of this disk */
        if (ziplocal_getShort(&ziinit.z_filefunc, ziinit.filestream,&number_disk)!=ZIP_OK)
            err=ZIP_ERRNO;

        /* number of the disk with the start of the central directory */
        if (ziplocal_getShort(&ziinit.z_filefunc, ziinit.filestream,&number_disk_with_CD)!=ZIP_OK)
            err=ZIP_ERRNO;

        /* total number of entries in the central dir on this disk */
        if (ziplocal_getShort(&ziinit.z_filefunc, ziinit.filestream,&number_entry)!=ZIP_OK)
            err=ZIP_ERRNO;

        /* total number of entries in the central dir */
        if (ziplocal_getShort(&ziinit.z_filefunc, ziinit.filestream,&number_entry_CD)!=ZIP_OK)
            err=ZIP_ERRNO;

        if ((number_entry_CD!=number_entry) ||
            (number_disk_with_CD!=0) ||
            (number_disk!=0))
            err=ZIP_BADZIPFILE;

        /* size of the central directory */
        if (ziplocal_getLong(&ziinit.z_filefunc, ziinit.filestream,&size_central_dir)!=ZIP_OK)
            err=ZIP_ERRNO;

        /* offset of start of central directory with respect to the
            starting disk number */
        if (ziplocal_getLong(&ziinit.z_filefunc, ziinit.filestream,&offset_central_dir)!=ZIP_OK)
            err=ZIP_ERRNO;

        /* zipfile global comment length */
        if (ziplocal_getShort(&ziinit.z_filefunc, ziinit.filestream,&size_comment)!=ZIP_OK)
            err=ZIP_ERRNO;

        if ((central_pos<offset_central_dir+size_central_dir) &&
            (err==ZIP_OK))
            err=ZIP_BADZIPFILE;

        if (err!=ZIP_OK)
        {
            ZCLOSE(ziinit.z_filefunc, ziinit.filestream);
            TRYFREE(zi);
            return nullptr;
        }

        if (size_comment>0)
        {
            ziinit.globalcomment = static_cast<char*>(ALLOC(size_comment+1));
            if (ziinit.globalcomment)
            {
               size_comment = ZREAD(ziinit.z_filefunc, ziinit.filestream,ziinit.globalcomment,size_comment);
               ziinit.globalcomment[size_comment]=0;
            }
        }

        byte_before_the_zipfile = central_pos -
                                (offset_central_dir+size_central_dir);
        ziinit.add_position_when_writing_offset = byte_before_the_zipfile;

        {
            uLong size_central_dir_to_read = size_central_dir;
            size_t buf_size = SIZEDATA_INDATABLOCK;
            void* buf_read = ALLOC(buf_size);
            if (ZSEEK(ziinit.z_filefunc, ziinit.filestream,
                  offset_central_dir + byte_before_the_zipfile,
                  ZLIB_FILEFUNC_SEEK_SET) != 0)
                  err=ZIP_ERRNO;

            while ((size_central_dir_to_read>0) && (err==ZIP_OK))
            {
                uLong read_this = SIZEDATA_INDATABLOCK;
                if (read_this > size_central_dir_to_read)
                    read_this = size_central_dir_to_read;
                if (ZREAD(ziinit.z_filefunc, ziinit.filestream,buf_read,read_this) != read_this)
                    err=ZIP_ERRNO;

                if (err==ZIP_OK)
                    err = add_data_in_datablock(&ziinit.central_dir,buf_read,
                                                static_cast<uLong>(read_this));
                size_central_dir_to_read-=read_this;
            }
            TRYFREE(buf_read);
        }
        ziinit.begin_pos = byte_before_the_zipfile;
        ziinit.number_entry = number_entry_CD;

        if (ZSEEK(ziinit.z_filefunc, ziinit.filestream,
                  offset_central_dir+byte_before_the_zipfile,ZLIB_FILEFUNC_SEEK_SET)!=0)
            err=ZIP_ERRNO;
    }

    if (globalcomment)
    {
      *globalcomment = ziinit.globalcomment;
    }
#    endif /* !NO_ADDFILEINEXISTINGZIP*/

    if (err != ZIP_OK)
    {
#    ifndef NO_ADDFILEINEXISTINGZIP
        TRYFREE(ziinit.globalcomment);
#    endif /* !NO_ADDFILEINEXISTINGZIP*/
        TRYFREE(zi);
        return nullptr;
    }
    else
    {
        *zi = ziinit;
        return static_cast<zipFile>(zi);
    }
}

extern zipFile ZEXPORT cpl_zipOpen (const char *pathname, int append)
{
    return cpl_zipOpen2(pathname,append,nullptr,nullptr);
}

extern int ZEXPORT cpl_zipOpenNewFileInZip3 (
    zipFile file,
    const char* filename,
    const zip_fileinfo* zipfi,
    const void* extrafield_local,
    uInt size_extrafield_local,
    const void* extrafield_global,
    uInt size_extrafield_global,
    const char* comment,
    int method,
    int level,
    int raw,
    int windowBits,
    int memLevel,
    int strategy,
    const char* password,
#ifdef NOCRYPT
    uLong /* crcForCrypting */
#else
    uLong crcForCrypting
#endif
 )
{
    zip_internal* zi;
    uInt size_filename;
    uInt size_comment;
    uInt i;
    int err = ZIP_OK;

#    ifdef NOCRYPT
    if (password != nullptr)
        return ZIP_PARAMERROR;
#    endif

    if (file == nullptr)
        return ZIP_PARAMERROR;
    if ((method!=0) && (method!=Z_DEFLATED))
        return ZIP_PARAMERROR;

    zi = reinterpret_cast<zip_internal*>(file);

    if (zi->in_opened_file_inzip == 1)
    {
        err = cpl_zipCloseFileInZip (file);
        if (err != ZIP_OK)
            return err;
    }

    if (filename==nullptr)
        filename="-";

    if (comment==nullptr)
        size_comment = 0;
    else
        size_comment = static_cast<uInt>(strlen(comment));

    size_filename = static_cast<uInt>(strlen(filename));

    if (zipfi == nullptr)
        zi->ci.dosDate = 0;
    else
    {
        if (zipfi->dosDate != 0)
            zi->ci.dosDate = zipfi->dosDate;
        else zi->ci.dosDate = ziplocal_TmzDateToDosDate(&zipfi->tmz_date,zipfi->dosDate);
    }

    zi->ci.flag = 0;
    if ((level==8) || (level==9))
      zi->ci.flag |= 2;
    if (level==2)
      zi->ci.flag |= 4;
    if (level==1)
      zi->ci.flag |= 6;
#ifndef NOCRYPT
    if (password != nullptr)
      zi->ci.flag |= 1;
#endif

    zi->ci.crc32 = 0;
    zi->ci.method = method;
    zi->ci.encrypt = 0;
    zi->ci.stream_initialised = 0;
    zi->ci.pos_in_buffered_data = 0;
    zi->ci.raw = raw;
    zi->ci.pos_local_header = static_cast<uLong>(ZTELL(zi->z_filefunc,zi->filestream));
    zi->ci.size_centralheader = SIZECENTRALHEADER + size_filename +
                                      size_extrafield_global + size_comment;
    zi->ci.central_header = static_cast<char*>(ALLOC(static_cast<uInt>(zi->ci.size_centralheader)));

    ziplocal_putValue_inmemory(zi->ci.central_header,CENTRALHEADERMAGIC,4);
    /* version info */
    ziplocal_putValue_inmemory(zi->ci.central_header+4,VERSIONMADEBY,2);
    ziplocal_putValue_inmemory(zi->ci.central_header+6,20,2);
    ziplocal_putValue_inmemory(zi->ci.central_header+8,static_cast<uLong>(zi->ci.flag),2);
    ziplocal_putValue_inmemory(zi->ci.central_header+10,static_cast<uLong>(zi->ci.method),2);
    ziplocal_putValue_inmemory(zi->ci.central_header+12,static_cast<uLong>(zi->ci.dosDate),4);
    ziplocal_putValue_inmemory(zi->ci.central_header+16,0,4); /*crc*/
    ziplocal_putValue_inmemory(zi->ci.central_header+20,0,4); /*compr size*/
    ziplocal_putValue_inmemory(zi->ci.central_header+24,0,4); /*uncompr size*/
    ziplocal_putValue_inmemory(zi->ci.central_header+28,static_cast<uLong>(size_filename),2);
    ziplocal_putValue_inmemory(zi->ci.central_header+30,static_cast<uLong>(size_extrafield_global),2);
    ziplocal_putValue_inmemory(zi->ci.central_header+32,static_cast<uLong>(size_comment),2);
    ziplocal_putValue_inmemory(zi->ci.central_header+34,0,2); /*disk nm start*/

    if (zipfi==nullptr)
        ziplocal_putValue_inmemory(zi->ci.central_header+36,0,2);
    else
        ziplocal_putValue_inmemory(zi->ci.central_header+36,static_cast<uLong>(zipfi->internal_fa),2);

    if (zipfi==nullptr)
        ziplocal_putValue_inmemory(zi->ci.central_header+38,0,4);
    else
        ziplocal_putValue_inmemory(zi->ci.central_header+38,static_cast<uLong>(zipfi->external_fa),4);

    ziplocal_putValue_inmemory(zi->ci.central_header+42,static_cast<uLong>(zi->ci.pos_local_header)- zi->add_position_when_writing_offset,4);

    for (i=0;i<size_filename;i++)
        *(zi->ci.central_header+SIZECENTRALHEADER+i) = *(filename+i);

    for (i=0;i<size_extrafield_global;i++)
        *(zi->ci.central_header+SIZECENTRALHEADER+size_filename+i) =
              *((reinterpret_cast<const char*>(extrafield_global))+i);

    for (i=0;i<size_comment;i++)
        *(zi->ci.central_header+SIZECENTRALHEADER+size_filename+
              size_extrafield_global+i) = *(comment+i);
    if (zi->ci.central_header == nullptr)
        return ZIP_INTERNALERROR;

    /* write the local header */
    err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,LOCALHEADERMAGIC,4);

    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,20,2);/* version needed to extract */
    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(zi->ci.flag),2);

    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(zi->ci.method),2);

    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(zi->ci.dosDate),4);

    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,0,4); /* crc 32, unknown */
    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,0,4); /* compressed size, unknown */
    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,0,4); /* uncompressed size, unknown */

    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(size_filename),2);

    if (err==ZIP_OK)
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(size_extrafield_local),2);

    if ((err==ZIP_OK) && (size_filename>0))
        if (ZWRITE(zi->z_filefunc,zi->filestream,filename,size_filename)!=size_filename)
                err = ZIP_ERRNO;

    if ((err==ZIP_OK) && (size_extrafield_local>0))
        if (ZWRITE(zi->z_filefunc,zi->filestream,extrafield_local,size_extrafield_local)
                                                                           !=size_extrafield_local)
                err = ZIP_ERRNO;

    zi->ci.stream.avail_in = 0;
    zi->ci.stream.avail_out = Z_BUFSIZE;
    zi->ci.stream.next_out = zi->ci.buffered_data;
    zi->ci.stream.total_in = 0;
    zi->ci.stream.total_out = 0;

    if ((err==ZIP_OK) && (zi->ci.method == Z_DEFLATED) && (!zi->ci.raw))
    {
        zi->ci.stream.zalloc = nullptr;
        zi->ci.stream.zfree = nullptr;
        zi->ci.stream.opaque = nullptr;

        if (windowBits>0)
            windowBits = -windowBits;

        err = deflateInit2(&zi->ci.stream, level,
               Z_DEFLATED, windowBits, memLevel, strategy);

        if (err==Z_OK)
            zi->ci.stream_initialised = 1;
    }
#ifndef NOCRYPT
    zi->ci.crypt_header_size = 0;
    if ((err==Z_OK) && (password != nullptr))
    {
        unsigned char bufHead[RAND_HEAD_LEN];
        unsigned int sizeHead = 0;
        zi->ci.encrypt = 1;
        zi->ci.pcrc_32_tab = get_crc_table();
        /*init_keys(password,zi->ci.keys,zi->ci.pcrc_32_tab);*/

        sizeHead=crypthead(password,bufHead,RAND_HEAD_LEN,zi->ci.keys,zi->ci.pcrc_32_tab,crcForCrypting);
        zi->ci.crypt_header_size = sizeHead;

        if (ZWRITE(zi->z_filefunc,zi->filestream,bufHead,sizeHead) != sizeHead)
                err = ZIP_ERRNO;
    }
#endif

    if (err==Z_OK)
        zi->in_opened_file_inzip = 1;
    return err;
}

extern int ZEXPORT cpl_zipOpenNewFileInZip2(
    zipFile file,
    const char* filename,
    const zip_fileinfo* zipfi,
    const void* extrafield_local,
    uInt size_extrafield_local,
    const void* extrafield_global,
    uInt size_extrafield_global,
    const char* comment,
    int method,
    int level,
    int raw )
{
    return cpl_zipOpenNewFileInZip3 (file, filename, zipfi,
                                 extrafield_local, size_extrafield_local,
                                 extrafield_global, size_extrafield_global,
                                 comment, method, level, raw,
                                 -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY,
                                 nullptr, 0);
}

extern int ZEXPORT cpl_zipOpenNewFileInZip (
    zipFile file,
    const char* filename,
    const zip_fileinfo* zipfi,
    const void* extrafield_local,
    uInt size_extrafield_local,
    const void* extrafield_global,
    uInt size_extrafield_global,
    const char* comment,
    int method,
    int level )
{
    return cpl_zipOpenNewFileInZip2 (file, filename, zipfi,
                                 extrafield_local, size_extrafield_local,
                                 extrafield_global, size_extrafield_global,
                                 comment, method, level, 0);
}

static int zipFlushWriteBuffer(
    zip_internal* zi )
{
    int err=ZIP_OK;

    if (zi->ci.encrypt != 0)
    {
#ifndef NOCRYPT
        int t = 0;
        for (uInt i=0;i<zi->ci.pos_in_buffered_data;i++)
            zi->ci.buffered_data[i] = zencode(zi->ci.keys, zi->ci.pcrc_32_tab,
                                       zi->ci.buffered_data[i],t);
#endif
    }
    if (ZWRITE(zi->z_filefunc,zi->filestream,zi->ci.buffered_data,zi->ci.pos_in_buffered_data)
                                                                    !=zi->ci.pos_in_buffered_data)
      err = ZIP_ERRNO;
    zi->ci.pos_in_buffered_data = 0;
    return err;
}

extern int ZEXPORT cpl_zipWriteInFileInZip (
    zipFile file,
    const void* buf,
    unsigned len )
{
    if (file == nullptr)
        return ZIP_PARAMERROR;

    zip_internal* zi = reinterpret_cast<zip_internal*>(file);

    if (zi->in_opened_file_inzip == 0)
        return ZIP_PARAMERROR;

    zi->ci.stream.next_in = reinterpret_cast<Bytef*>(const_cast<void*>(buf));
    zi->ci.stream.avail_in = len;
    zi->ci.crc32 = crc32(zi->ci.crc32, reinterpret_cast<const Bytef *>(buf),len);

    int err=ZIP_OK;
    while ((err==ZIP_OK) && (zi->ci.stream.avail_in>0))
    {
        if (zi->ci.stream.avail_out == 0)
        {
            if (zipFlushWriteBuffer(zi) == ZIP_ERRNO)
                err = ZIP_ERRNO;
            zi->ci.stream.avail_out = Z_BUFSIZE;
            zi->ci.stream.next_out = zi->ci.buffered_data;
        }

        if(err != ZIP_OK)
            break;

        if ((zi->ci.method == Z_DEFLATED) && (!zi->ci.raw))
        {
            uLong uTotalOutBefore = zi->ci.stream.total_out;
            err=deflate(&zi->ci.stream,  Z_NO_FLUSH);
            zi->ci.pos_in_buffered_data += static_cast<uInt>(zi->ci.stream.total_out - uTotalOutBefore) ;
        }
        else
        {
            uInt copy_this;
            if (zi->ci.stream.avail_in < zi->ci.stream.avail_out)
                copy_this = zi->ci.stream.avail_in;
            else
                copy_this = zi->ci.stream.avail_out;
            for (uInt i=0;i<copy_this;i++)
                *((reinterpret_cast<char*>(zi->ci.stream.next_out))+i) =
                    *((reinterpret_cast<const char*>(zi->ci.stream.next_in))+i);
            {
                zi->ci.stream.avail_in -= copy_this;
                zi->ci.stream.avail_out-= copy_this;
                zi->ci.stream.next_in+= copy_this;
                zi->ci.stream.next_out+= copy_this;
                zi->ci.stream.total_in+= copy_this;
                zi->ci.stream.total_out+= copy_this;
                zi->ci.pos_in_buffered_data += copy_this;
            }
        }
    }

    return err;
}

extern int ZEXPORT cpl_zipCloseFileInZipRaw (
    zipFile file,
    uLong uncompressed_size,
    uLong crc32 )
{
    if (file == nullptr)
        return ZIP_PARAMERROR;

    zip_internal* zi = reinterpret_cast<zip_internal*>(file);

    if (zi->in_opened_file_inzip == 0)
        return ZIP_PARAMERROR;
    zi->ci.stream.avail_in = 0;

    int err=ZIP_OK;
    if ((zi->ci.method == Z_DEFLATED) && (!zi->ci.raw))
    {
        while (err==ZIP_OK)
        {
            if (zi->ci.stream.avail_out == 0)
            {
                if (zipFlushWriteBuffer(zi) == ZIP_ERRNO)
                {
                    err = ZIP_ERRNO;
                    break;
                }
                zi->ci.stream.avail_out = Z_BUFSIZE;
                zi->ci.stream.next_out = zi->ci.buffered_data;
            }
            uLong uTotalOutBefore = zi->ci.stream.total_out;
            err=deflate(&zi->ci.stream,  Z_FINISH);
            zi->ci.pos_in_buffered_data += static_cast<uInt>(zi->ci.stream.total_out - uTotalOutBefore) ;
        }
    }

    if (err==Z_STREAM_END)
        err=ZIP_OK; /* this is normal */

    if ((zi->ci.pos_in_buffered_data>0) && (err==ZIP_OK))
        if (zipFlushWriteBuffer(zi)==ZIP_ERRNO)
            err = ZIP_ERRNO;

    if ((zi->ci.method == Z_DEFLATED) && (!zi->ci.raw))
    {
        err=deflateEnd(&zi->ci.stream);
        zi->ci.stream_initialised = 0;
    }

    if (!zi->ci.raw)
    {
        crc32 = static_cast<uLong>(zi->ci.crc32);
        uncompressed_size = static_cast<uLong>(zi->ci.stream.total_in);
    }
    uLong compressed_size = static_cast<uLong>(zi->ci.stream.total_out);
#ifndef NOCRYPT
    compressed_size += zi->ci.crypt_header_size;
#endif

    ziplocal_putValue_inmemory(zi->ci.central_header+16,crc32,4); /*crc*/
    ziplocal_putValue_inmemory(zi->ci.central_header+20,
                                compressed_size,4); /*compr size*/
    if (zi->ci.stream.data_type == Z_ASCII)
        ziplocal_putValue_inmemory(zi->ci.central_header+36,Z_ASCII,2);
    ziplocal_putValue_inmemory(zi->ci.central_header+24,
                                uncompressed_size,4); /*uncompr size*/

    if (err==ZIP_OK)
        err = add_data_in_datablock(&zi->central_dir,zi->ci.central_header,
                                       static_cast<uLong>(zi->ci.size_centralheader));
    free(zi->ci.central_header);

    if (err==ZIP_OK)
    {
        long cur_pos_inzip = static_cast<uLong>(ZTELL(zi->z_filefunc,zi->filestream));
        if (ZSEEK(zi->z_filefunc,zi->filestream,
                  zi->ci.pos_local_header + 14,ZLIB_FILEFUNC_SEEK_SET)!=0)
            err = ZIP_ERRNO;

        if (err==ZIP_OK)
            err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,crc32,4); /* crc 32, unknown */

        if (err==ZIP_OK) /* compressed size, unknown */
            err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,compressed_size,4);

        if (err==ZIP_OK) /* uncompressed size, unknown */
            err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,uncompressed_size,4);

        if (ZSEEK(zi->z_filefunc,zi->filestream,
                  cur_pos_inzip,ZLIB_FILEFUNC_SEEK_SET)!=0)
            err = ZIP_ERRNO;
    }

    zi->number_entry ++;
    zi->in_opened_file_inzip = 0;

    return err;
}

extern int ZEXPORT cpl_zipCloseFileInZip (
    zipFile file )
{
    return cpl_zipCloseFileInZipRaw (file,0,0);
}

extern int ZEXPORT cpl_zipClose (
    zipFile file,
    const char* global_comment)
{
    int err = 0;
    uLong size_centraldir = 0;
    uLong centraldir_pos_inzip;
    uInt size_global_comment;

    if (file == nullptr)
        return ZIP_PARAMERROR;

    zip_internal* zi = reinterpret_cast<zip_internal*>(file);

    if (zi->in_opened_file_inzip == 1)
    {
        err = cpl_zipCloseFileInZip (file);
    }

#ifndef NO_ADDFILEINEXISTINGZIP
    if (global_comment==nullptr)
        global_comment = zi->globalcomment;
#endif
    if (global_comment==nullptr)
        size_global_comment = 0;
    else
        size_global_comment = static_cast<uInt>(strlen(global_comment));

    centraldir_pos_inzip = static_cast<uLong>(ZTELL(zi->z_filefunc,zi->filestream));
    if (err==ZIP_OK)
    {
        linkedlist_datablock_internal* ldi = zi->central_dir.first_block;
        while (ldi!=nullptr)
        {
            if ((err==ZIP_OK) && (ldi->filled_in_this_block>0))
                if (ZWRITE(zi->z_filefunc,zi->filestream,
                           ldi->data,ldi->filled_in_this_block)
                              !=ldi->filled_in_this_block )
                    err = ZIP_ERRNO;

            size_centraldir += ldi->filled_in_this_block;
            ldi = ldi->next_datablock;
        }
    }
    free_datablock(zi->central_dir.first_block);

    if (err==ZIP_OK) /* Magic End */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,ENDHEADERMAGIC,4);

    if (err==ZIP_OK) /* number of this disk */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,0,2);

    if (err==ZIP_OK) /* number of the disk with the start of the central directory */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,0,2);

    if (err==ZIP_OK) /* total number of entries in the central dir on this disk */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(zi->number_entry),2);

    if (err==ZIP_OK) /* total number of entries in the central dir */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(zi->number_entry),2);

    if (err==ZIP_OK) /* size of the central directory */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(size_centraldir),4);

    if (err==ZIP_OK) /* offset of start of central directory with respect to the
                            starting disk number */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,
                                static_cast<uLong>(centraldir_pos_inzip - zi->add_position_when_writing_offset),4);

    if (err==ZIP_OK) /* zipfile comment length */
        err = ziplocal_putValue(&zi->z_filefunc,zi->filestream,static_cast<uLong>(size_global_comment),2);

    if ((err==ZIP_OK) && (size_global_comment>0))
        if (ZWRITE(zi->z_filefunc,zi->filestream,
                   global_comment,size_global_comment) != size_global_comment)
                err = ZIP_ERRNO;

    if (ZCLOSE(zi->z_filefunc,zi->filestream) != 0)
        if (err == ZIP_OK)
            err = ZIP_ERRNO;

#ifndef NO_ADDFILEINEXISTINGZIP
    TRYFREE(zi->globalcomment);
#endif
    TRYFREE(zi);

    return err;
}

/************************************************************************/
/* ==================================================================== */
/*   The following is a simplified CPL API for creating ZIP files       */
/*   exported from cpl_conv.h.                                          */
/* ==================================================================== */
/************************************************************************/

#include "cpl_minizip_unzip.h"

typedef struct
{
    zipFile   hZip;
    char    **papszFilenames;
} CPLZip;

/************************************************************************/
/*                            CPLCreateZip()                            */
/************************************************************************/

/** Create ZIP file */
void *CPLCreateZip( const char *pszZipFilename, char **papszOptions )

{
    const bool bAppend =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "APPEND", "FALSE"));
    char** papszFilenames = nullptr;

    if( bAppend )
    {
        zipFile unzF = cpl_unzOpen(pszZipFilename);
        if( unzF != nullptr )
        {
            if( cpl_unzGoToFirstFile(unzF) == UNZ_OK )
            {
                do
                {
                    char fileName[8193];
                    unz_file_info file_info;
                    cpl_unzGetCurrentFileInfo (unzF, &file_info, fileName,
                                            sizeof(fileName) - 1, nullptr, 0, nullptr, 0);
                    fileName[sizeof(fileName) - 1] = '\0';
                    papszFilenames = CSLAddString(papszFilenames, fileName);
                }
                while( cpl_unzGoToNextFile(unzF) == UNZ_OK );
            }
            cpl_unzClose(unzF);
        }
    }

    zipFile hZip = cpl_zipOpen( pszZipFilename, bAppend ? APPEND_STATUS_ADDINZIP : APPEND_STATUS_CREATE);
    if( hZip == nullptr )
    {
        CSLDestroy(papszFilenames);
        return nullptr;
    }

    CPLZip* psZip = static_cast<CPLZip *>(CPLMalloc(sizeof(CPLZip)));
    psZip->hZip = hZip;
    psZip->papszFilenames = papszFilenames;
    return psZip;
}

/************************************************************************/
/*                         CPLCreateFileInZip()                         */
/************************************************************************/

/** Create a file in a ZIP file */
CPLErr CPLCreateFileInZip( void *hZip, const char *pszFilename,
                           char **papszOptions )

{
    if( hZip == nullptr )
        return CE_Failure;

    CPLZip* psZip = static_cast<CPLZip*>(hZip);

    if( CSLFindString(psZip->papszFilenames, pszFilename ) >= 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                "%s already exists in ZIP file", pszFilename);
        return CE_Failure;
    }

    const bool bCompressed =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "COMPRESSED", "TRUE"));

    // If the filename is ASCII only, then no need for an extended field
    bool bIsAscii = true;
    for( int i=0; pszFilename[i] != '\0'; i++ )
    {
        if( (reinterpret_cast<const GByte*>(pszFilename))[i] > 127 )
        {
            bIsAscii = false;
            break;
        }
    }

    char* pszCPFilename = nullptr;
    unsigned int nExtraLength = 0;
    GByte* pabyExtra = nullptr;
    if( !bIsAscii )
    {
        const char* pszDestEncoding = CPLGetConfigOption("CPL_ZIP_ENCODING",
#if defined(_WIN32) && !defined(HAVE_ICONV)
                                                        "CP_OEMCP"
#else
                                                        "CP437"
#endif
                                                        );

        pszCPFilename = CPLRecode(pszFilename, CPL_ENC_UTF8, pszDestEncoding);

        /* Create a Info-ZIP Unicode Path Extra Field (0x7075) */
        const GUInt16 nDataLength = 1 + 4 +
                                    static_cast<GUInt16>(strlen(pszFilename));
        nExtraLength = 2 + 2 + nDataLength;
        pabyExtra = static_cast<GByte*>(CPLMalloc(nExtraLength));
        const GUInt16 nHeaderIdLE = CPL_LSBWORD16(0x7075);
        memcpy(pabyExtra, &nHeaderIdLE, 2);
        const GUInt16 nDataLengthLE = CPL_LSBWORD16(nDataLength);
        memcpy(pabyExtra + 2, &nDataLengthLE, 2);
        const GByte nVersion = 1;
        memcpy(pabyExtra + 2 + 2, &nVersion, 1);
        const GUInt32 nNameCRC32 = static_cast<GUInt32>(crc32(0,
                reinterpret_cast<const Bytef*>(pszCPFilename),
                static_cast<uInt>(strlen(pszCPFilename))));
        const GUInt32 nNameCRC32LE = CPL_LSBWORD32(nNameCRC32);
        memcpy(pabyExtra + 2 + 2 + 1, &nNameCRC32LE, 4);
        memcpy(pabyExtra + 2 + 2 + 1 + 4, pszFilename, strlen(pszFilename));
    }
    else
    {
        pszCPFilename = CPLStrdup(pszFilename);
    }

    const int nErr =
        cpl_zipOpenNewFileInZip(
            psZip->hZip, pszCPFilename, nullptr,
            nullptr, 0, pabyExtra, nExtraLength, "",
            bCompressed ? Z_DEFLATED : 0,
            bCompressed ? Z_DEFAULT_COMPRESSION : 0 );

    CPLFree( pabyExtra );
    CPLFree( pszCPFilename );

    if( nErr != ZIP_OK )
        return CE_Failure;

    psZip->papszFilenames = CSLAddString(psZip->papszFilenames, pszFilename);
    return CE_None;
}

/************************************************************************/
/*                         CPLWriteFileInZip()                          */
/************************************************************************/

/** Write in current file inside a ZIP file */
CPLErr CPLWriteFileInZip( void *hZip, const void *pBuffer, int nBufferSize )

{
    if( hZip == nullptr )
        return CE_Failure;

    CPLZip* psZip = static_cast<CPLZip*>(hZip);

    int nErr = cpl_zipWriteInFileInZip( psZip->hZip, pBuffer,
                                    static_cast<unsigned int>(nBufferSize) );

    if( nErr != ZIP_OK )
        return CE_Failure;

    return CE_None;
}

/************************************************************************/
/*                         CPLCloseFileInZip()                          */
/************************************************************************/

/** Close current file inside ZIP file */
CPLErr CPLCloseFileInZip( void *hZip )

{
    if( hZip == nullptr )
        return CE_Failure;

    CPLZip* psZip = static_cast<CPLZip*>(hZip);

    int nErr = cpl_zipCloseFileInZip( psZip->hZip );

    if( nErr != ZIP_OK )
        return CE_Failure;

    return CE_None;
}

/************************************************************************/
/*                            CPLCloseZip()                             */
/************************************************************************/

/** Close ZIP file */
CPLErr CPLCloseZip( void *hZip )

{
    if( hZip == nullptr )
        return CE_Failure;

    CPLZip* psZip = static_cast<CPLZip*>(hZip);

    int nErr = cpl_zipClose(psZip->hZip, nullptr);

    psZip->hZip = nullptr;
    CSLDestroy(psZip->papszFilenames);
    psZip->papszFilenames = nullptr;
    CPLFree(psZip);

    if( nErr != ZIP_OK )
        return CE_Failure;

    return CE_None;
}
