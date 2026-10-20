#include <nitro.h>

#include <nnsys/g2d/load/g2d_NCL_load.h>
#include <nnsys/g2d/g2d_Load.h>

#include "g2di_Debug.h"

#ifndef SDK_FINALROM
static const char * StrNNSG2dPaletteFmt_ [] = {
    "INVALID_PixelFmt !! ",
    "INVALID_PixelFmt !! ",
    "INVALID_PixelFmt !! ",
    "GX_TEXFMT_PLTT16",
    "GX_TEXFMT_PLTT256",
    "INVALID_PixelFmt !! ",
    "INVALID_PixelFmt !! ",
    "INVALID_PixelFmt !! "
};
#endif

#ifdef SDK_PORT
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

_Static_assert(sizeof(WIN_NNSG2dPaletteData) == 16, "NCLR serialized palette must remain 16 bytes");
_Static_assert(sizeof(WIN_NNSG2dPaletteCompressInfo) == 8, "NCLR serialized compression record must remain 8 bytes");
_Static_assert(sizeof(NNSG2dBinaryFileHeader) == 16 && sizeof(NNSG2dBinaryBlockHeader) == 8, "NCLR serialized headers changed");

/* Serialized offsets remain 32-bit on desktop. Native descriptors are separate
 * allocations; their raw data/index tables borrow the source file's lifetime.
 * The game heap releases these views before recycling the source allocation. */
typedef struct WIN_NCLView {
    const void * source;
    BOOL compressed;
    NNSG2dPaletteData palette;
    u32 storedBytes;
    NNSG2dPaletteCompressInfo compression;
    struct WIN_NCLView * next;
} WIN_NCLView;
static WIN_NCLView * s_paletteViews;
static atomic_flag s_paletteViewLock = ATOMIC_FLAG_INIT;

static void WIN_LockPaletteViews(void)
{
    while (atomic_flag_test_and_set_explicit(&s_paletteViewLock, memory_order_acquire)) {}
}
static void WIN_UnlockPaletteViews(void)
{
    atomic_flag_clear_explicit(&s_paletteViewLock, memory_order_release);
}
static WIN_NCLView * WIN_FindOrCreatePaletteView(const void * source, BOOL compressed)
{
    WIN_NCLView * view;
    for (view = s_paletteViews; view; view = view->next) {
        if (view->source == source && view->compressed == compressed) return view;
    }
    view = malloc(sizeof(*view));
    if (!view) return NULL;
    memset(view, 0, sizeof(*view));
    view->source = source; view->compressed = compressed;
    view->next = s_paletteViews; s_paletteViews = view;
    return view;
}
void WIN_CheckAndFreePaletteData(void * start, void * end)
{
    WIN_NCLView ** link;
    uintptr_t first = (uintptr_t)start, last = (uintptr_t)end;
    if (last <= first) return;
    WIN_LockPaletteViews();
    link = &s_paletteViews;
    while (*link) {
        WIN_NCLView * view = *link;
        uintptr_t source = (uintptr_t)view->source;
        if (source >= first && source < last) { *link = view->next; free(view); }
        else link = &view->next;
    }
    WIN_UnlockPaletteViews();
}

static const u8 * WIN_FindPaletteBlock(const void * file, u32 available, u32 kind, u32 * dataSize)
{
    NNSG2dBinaryFileHeader header;
    const u8 * found = NULL;
    u32 cursor, i;
    if (!file || available < sizeof(header)) return NULL;
    memcpy(&header, file, sizeof(header));
    if ((header.signature != NNS_G2D_BINFILE_SIG_PALETTEDATA && header.signature != NNS_G2D_BINFILE_SIG_PALETTEDATA_OLD)
        || header.byteOrder != NNS_G2D_LITTLEENDIAN_BITMARK || header.version < BIN_FILE_VERSION(NCLR)
        || header.headerSize < sizeof(header) || header.fileSize > available || header.headerSize > header.fileSize
        || header.dataBlocks > (header.fileSize - header.headerSize) / sizeof(NNSG2dBinaryBlockHeader)) return NULL;
    cursor = header.headerSize;
    for (i = 0; i < header.dataBlocks; ++i) {
        NNSG2dBinaryBlockHeader block;
        if (sizeof(block) > header.fileSize - cursor) return NULL;
        memcpy(&block, (const u8 *)file + cursor, sizeof(block));
        if (block.size < sizeof(block) || block.size > header.fileSize - cursor) return NULL;
        if (!found && block.kind == kind) {
            found = (const u8 *)file + cursor + sizeof(block);
            *dataSize = block.size - sizeof(block);
        }
        cursor += block.size;
    }
    return found;
}

static BOOL WIN_ReadPalette(const void * source, WIN_NNSG2dPaletteData * serial)
{
    if (!source) return FALSE;
    memcpy(serial, source, sizeof(*serial));
    return serial->fmt >= GX_TEXFMT_PLTT16 && serial->fmt <= GX_TEXFMT_PLTT256
        && serial->szByte && !(serial->szByte & 1) && serial->pRawData >= sizeof(*serial)
        && !(serial->pRawData & 1)
        && serial->pRawData <= UINTPTR_MAX - (uintptr_t)source
        && serial->szByte <= UINTPTR_MAX - ((uintptr_t)source + serial->pRawData);
}

static BOOL WIN_ValidatePalette(void * file, u32 available, const u8 ** source, WIN_NNSG2dPaletteData * serial, u32 * storedBytes)
{
    u32 size = 0, cmpSize = 0;
    const u8 * cmp;
    *source = WIN_FindPaletteBlock(file, available, NNS_G2D_BINBLK_SIG_PALETTEDATA, &size);
    if (!*source || size < sizeof(*serial) || !WIN_ReadPalette(*source, serial) || serial->pRawData > size) return FALSE;
    *storedBytes = serial->szByte;
    cmp = WIN_FindPaletteBlock(file, available, NNS_G2D_BINBLK_SIG_PALETTECOMPINFO, &cmpSize);
    if (cmp) {
        WIN_NNSG2dPaletteCompressInfo info;
        u32 onePalette = serial->fmt == GX_TEXFMT_PLTT16 ? 32 : 512;
        u32 i;
        if (cmpSize < sizeof(info)) return FALSE;
        memcpy(&info, cmp, sizeof(info));
        if (info.pPlttIdxTbl < sizeof(info) || (info.pPlttIdxTbl & 1) || info.pPlttIdxTbl > cmpSize
            || (u32)info.numPalette * sizeof(u16) > cmpSize - info.pPlttIdxTbl) return FALSE;
        /* PCMP stores packed palettes. szByte is their expanded VRAM extent,
         * not the number of bytes physically following the PLTT record. */
        *storedBytes = (u32)info.numPalette * onePalette;
        for (i = 0; i < info.numPalette; ++i) {
            u16 index;
            memcpy(&index, cmp + info.pPlttIdxTbl + i * sizeof(index), sizeof(index));
            if (((u32)index + 1) * onePalette > serial->szByte) return FALSE;
        }
    }
    return *storedBytes <= size - serial->pRawData;
}

static u32 WIN_PaletteStoredBytes(const NNSG2dPaletteData * data)
{
    WIN_NCLView * view;
    u32 size = data->szByte;
    WIN_LockPaletteViews();
    for (view = s_paletteViews; view; view = view->next) {
        if (!view->compressed && &view->palette == data) { size = view->storedBytes; break; }
    }
    WIN_UnlockPaletteViews();
    return size;
}

BOOL NNS_G2dGetUnpackedPaletteDataSized(void * file, u32 available, NNSG2dPaletteData ** output)
{
    u32 storedBytes = 0;
    const u8 * source;
    WIN_NNSG2dPaletteData serial;
    WIN_NCLView * view;
    if (!output) return FALSE;
    *output = NULL;
    if (!WIN_ValidatePalette(file, available, &source, &serial, &storedBytes)) return FALSE;
    WIN_LockPaletteViews();
    view = WIN_FindOrCreatePaletteView(source, FALSE);
    if (view) {
        view->palette.fmt = serial.fmt; view->palette.bExtendedPlt = serial.bExtendedPlt;
        view->palette.szByte = serial.szByte; view->palette.pRawData = (void *)(source + serial.pRawData);
        view->storedBytes = storedBytes;
        *output = &view->palette;
    }
    WIN_UnlockPaletteViews();
    return view != NULL;
}

BOOL NNS_G2dGetUnpackedPaletteCompressInfoSized(void * file, u32 available, NNSG2dPaletteCompressInfo ** output)
{
    u32 size = 0;
    const u8 * source;
    WIN_NNSG2dPaletteCompressInfo serial;
    WIN_NNSG2dPaletteData palette;
    const u8 * paletteSource;
    u32 storedBytes;
    if (!output) return FALSE;
    *output = NULL;
    source = WIN_FindPaletteBlock(file, available, NNS_G2D_BINBLK_SIG_PALETTECOMPINFO, &size);
    if (!source || size < sizeof(serial)) return FALSE;
    memcpy(&serial, source, sizeof(serial));
    if (serial.pPlttIdxTbl < sizeof(serial) || (serial.pPlttIdxTbl & 1)
        || serial.pPlttIdxTbl > size || (u32)serial.numPalette * sizeof(u16) > size - serial.pPlttIdxTbl) return FALSE;
    if (!WIN_ValidatePalette(file, available, &paletteSource, &palette, &storedBytes)) return FALSE;
    *output = NNSi_G2dUnpackNCLCmpInfo((NNSG2dPaletteCompressInfo *)(void *)source);
    return *output != NULL;
}
#endif

static void DumpPalette_ (const NNSG2dPaletteData * pPlttData)
{
    NNS_G2D_NULL_ASSERT(pPlttData);
    NNS_G2D_NULL_ASSERT(pPlttData->pRawData);
    {
#ifdef SDK_PORT
        u32 i;
        const u32 colorCount = WIN_PaletteStoredBytes(pPlttData) / sizeof(u16);
#else
        u16 i;
#endif
        const u16 * pColor = (const u16 *)pPlttData->pRawData;
        OS_Printf("\n ---------------- palette ---------------- \n");
        for (i = 0; i <
#ifdef SDK_PORT
             colorCount
#else
             16 * 16
#endif
             ; i++) {
            if ((i & 0xF) == 0) {
                OS_Printf("\n");
            }

            OS_Printf(" %04X ", pColor[i]);
        }
        OS_Printf("\n ---------------- palette ---------------- \n");
    }
}

BOOL NNS_G2dGetUnpackedPaletteData (void * pNclrFile, NNSG2dPaletteData ** ppPltData)
{
#ifdef SDK_PORT
    NNSG2dBinaryFileHeader header;
    if (!ppPltData) return FALSE;
    *ppPltData = NULL;
    if (!pNclrFile) return FALSE;
    /* Legacy API has no allocation length. It requires a readable header and
     * the complete declared fileSize; use the Sized API for bounded input. */
    memcpy(&header, pNclrFile, sizeof(header));
    return NNS_G2dGetUnpackedPaletteDataSized(pNclrFile, header.fileSize, ppPltData);
#else
    NNS_G2D_NULL_ASSERT(pNclrFile);
    NNS_G2D_NULL_ASSERT(ppPltData);

    {
        const NNSG2dBinaryFileHeader * pBinFile = pNclrFile;

        NNS_G2D_ASSERTMSG(pBinFile->signature == NNS_G2D_BINFILE_SIG_PALETTEDATA ||
                          pBinFile->signature == NNS_G2D_BINFILE_SIG_PALETTEDATA_OLD,
                          "Input file signature is invalid for this method.");
    }

    NNS_G2D_ASSERTMSG(NNSi_G2dIsBinFileVersionValid(pNclrFile,
                                                    BIN_FILE_VERSION(NCLR)),
                      "Input file is obsolete. Please use the new g2dcvtr.exe.");

    {
        NNSG2dBinaryFileHeader * pBinFile = (NNSG2dBinaryFileHeader *)pNclrFile;
        {
            NNSG2dPaletteDataBlock * pBinBlk =
                (NNSG2dPaletteDataBlock *)NNS_G2dFindBinaryBlock(pBinFile,
                                                                 NNS_G2D_BINBLK_SIG_PALETTEDATA);
            if (pBinBlk) {
                NNS_G2dUnpackNCL((void *)&pBinBlk->paletteData);
                *ppPltData = &pBinBlk->paletteData;
                return TRUE;
            } else {
                *ppPltData = NULL;
                return FALSE;

            }
        }
    }
#endif
}

BOOL NNS_G2dGetUnpackedPaletteCompressInfo (void * pNclrFile, NNSG2dPaletteCompressInfo ** ppPltCmpInfo)
{
#ifdef SDK_PORT
    NNSG2dBinaryFileHeader header;
    if (!ppPltCmpInfo) return FALSE;
    *ppPltCmpInfo = NULL;
    if (!pNclrFile) return FALSE;
    memcpy(&header, pNclrFile, sizeof(header));
    return NNS_G2dGetUnpackedPaletteCompressInfoSized(pNclrFile, header.fileSize, ppPltCmpInfo);
#else
    NNS_G2D_NULL_ASSERT(pNclrFile);
    NNS_G2D_NULL_ASSERT(ppPltCmpInfo);

    {
        const NNSG2dBinaryFileHeader * pBinFile = pNclrFile;

        NNS_G2D_ASSERTMSG(pBinFile->signature == NNS_G2D_BINFILE_SIG_PALETTEDATA ||
                          pBinFile->signature == NNS_G2D_BINFILE_SIG_PALETTEDATA_OLD,
                          "Input file signature is invalid for this method.");
    }

    NNS_G2D_ASSERTMSG(NNSi_G2dIsBinFileVersionValid(pNclrFile,
                                                    BIN_FILE_VERSION(NCLR)),
                      "Input file is obsolete. Please use the new g2dcvtr.exe.");

    {
        NNSG2dBinaryFileHeader * pBinFile = (NNSG2dBinaryFileHeader *)pNclrFile;
        {
            NNSG2dPaletteCompressDataBlock * pBinBlk =
                (NNSG2dPaletteCompressDataBlock *)NNS_G2dFindBinaryBlock(pBinFile,
                                                                         NNS_G2D_BINBLK_SIG_PALETTECOMPINFO);
            if (pBinBlk) {
                NNSi_G2dUnpackNCLCmpInfo((void *)&pBinBlk->plttCmpInfo);
                *ppPltCmpInfo = &pBinBlk->plttCmpInfo;
                return TRUE;
            } else {
                *ppPltCmpInfo = NULL;
                return FALSE;
            }
        }
    }
#endif
}

#ifdef SDK_PORT
void * NNSi_G2dUnpackNCLCmpInfo(NNSG2dPaletteCompressInfo* pPlttCmpInfo)
#else
void NNSi_G2dUnpackNCLCmpInfo (NNSG2dPaletteCompressInfo * pPlttCmpInfo)
#endif
{
    NNS_G2D_NULL_ASSERT(pPlttCmpInfo);

#ifdef SDK_PORT
    WIN_NNSG2dPaletteCompressInfo serial;
    WIN_NCLView * view;
    void * result = NULL;
    if (!pPlttCmpInfo) return NULL;
    memcpy(&serial, pPlttCmpInfo, sizeof(serial));
    if (serial.pPlttIdxTbl < sizeof(serial) || (serial.pPlttIdxTbl & 1)
        || serial.pPlttIdxTbl > UINTPTR_MAX - (uintptr_t)pPlttCmpInfo
        || (u32)serial.numPalette * sizeof(u16) > UINTPTR_MAX - ((uintptr_t)pPlttCmpInfo + serial.pPlttIdxTbl)) return NULL;
    WIN_LockPaletteViews();
    view = WIN_FindOrCreatePaletteView(pPlttCmpInfo, TRUE);
    if (view) {
        view->compression.numPalette = serial.numPalette; view->compression.pad16 = serial.pad16;
        view->compression.pPlttIdxTbl = (u8 *)pPlttCmpInfo + serial.pPlttIdxTbl;
        result = &view->compression;
    }
    WIN_UnlockPaletteViews();
    return result;
#else
    pPlttCmpInfo->pPlttIdxTbl = NNS_G2D_UNPACK_OFFSET_PTR(pPlttCmpInfo->pPlttIdxTbl, pPlttCmpInfo);
    NNSI_G2D_DEBUGMSG0("Unpacking NCLR(Compressed) file is successful.\n");
#endif
}

#ifdef SDK_PORT
void * NNS_G2dUnpackNCL(NNSG2dPaletteData* pPlttData)
#else
void NNS_G2dUnpackNCL (NNSG2dPaletteData * pPlttData)
#endif
{
    NNS_G2D_NULL_ASSERT(pPlttData);

    #ifdef SDK_PORT
    WIN_NNSG2dPaletteData serial;
    if (!WIN_ReadPalette(pPlttData, &serial)) return NULL;
    return (u8 *)pPlttData + serial.pRawData;
    #else
    pPlttData->pRawData = NNS_G2D_UNPACK_OFFSET_PTR(pPlttData->pRawData, pPlttData);
    #endif

    NNS_G2D_MINMAX_ASSERT(pPlttData->fmt, GX_TEXFMT_PLTT16, GX_TEXFMT_PLTT256);
    NNSI_G2D_DEBUGMSG0("Unpacking NCLR file is successful.\n");
}

#ifndef SDK_FINALROM
void NNS_G2dPrintPaletteData (const NNSG2dPaletteData * pPlttData)
{
    NNS_G2D_NULL_ASSERT(pPlttData);
    {
        OS_Printf("---------------------------------------------\n");
        OS_Printf(" Palette Data (NCL)");
        OS_Printf(" fmt          = %s \n", StrNNSG2dPaletteFmt_[ pPlttData->fmt ]);
        OS_Printf(" bExtendedPlt = %d \n", pPlttData->bExtendedPlt);
        DumpPalette_(pPlttData);
        OS_Printf("---------------------------------------------\n");
    }
}
#endif
