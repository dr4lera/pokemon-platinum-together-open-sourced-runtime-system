#ifndef NNS_G2D_NCP_LOAD_H_
#define NNS_G2D_NCP_LOAD_H_

#include <nitro.h>

#include <nnsys/g2d/g2d_config.h>
#include <nnsys/g2d/g2d_Data.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL NNS_G2dGetUnpackedPaletteData(void * pNclrFile, NNSG2dPaletteData ** ppPltData);
BOOL NNS_G2dGetUnpackedPaletteCompressInfo(void * pNclrFile, NNSG2dPaletteCompressInfo ** ppPltCmpInfo);

#ifdef SDK_PORT
/* Desktop descriptors borrow source bytes and remain valid until that source
 * allocation is freed. Non-expheap owners must call the cleanup hook themselves.
 * Legacy unsized entry points cannot verify the actual allocation extent.
 * With PCMP, szByte retains the expanded VRAM extent; physical raw bytes are
 * numPalette times 32/512. Use PCMP-aware loading or an explicit packed count. */
BOOL NNS_G2dGetUnpackedPaletteDataSized(void * file, u32 available, NNSG2dPaletteData ** output);
BOOL NNS_G2dGetUnpackedPaletteCompressInfoSized(void * file, u32 available, NNSG2dPaletteCompressInfo ** output);
void WIN_CheckAndFreePaletteData(void * start, void * end);
/* These record-only helpers also lack an allocation length. */
void * NNS_G2dUnpackNCL (NNSG2dPaletteData* pPlttData);
void * NNSi_G2dUnpackNCLCmpInfo(NNSG2dPaletteCompressInfo* pPlttCmpData);
#else
void NNS_G2dUnpackNCL(NNSG2dPaletteData * pPlttData);
void NNSi_G2dUnpackNCLCmpInfo(NNSG2dPaletteCompressInfo * pPlttCmpData);
#endif

#ifdef __SNC__
    NNS_G2D_DEBUG_FUNC_DECL_BEGIN void NNS_G2dPrintPaletteData (const NNSG2dPaletteData * p1) NNS_G2D_DEBUG_FUNC_DECL_END
#else
    NNS_G2D_DEBUG_FUNC_DECL_BEGIN void NNS_G2dPrintPaletteData (const NNSG2dPaletteData *) NNS_G2D_DEBUG_FUNC_DECL_END
#endif

#ifdef __cplusplus
}
#endif

#endif
