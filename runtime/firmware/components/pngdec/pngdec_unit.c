/* The PNGdec implementation lives in png.inl; in C mode it exports the plain
 * C API and PNGRGB565, which is exactly what the vendor's C++
 * getLineAsRGB565() wraps. PNGdec.cpp includes the header first, so do we. */
#include "PNGdec.h"
#include "png.inl"

/* PNGdec 1.1.7 declares these in its C API but defines them only as C++
 * methods. Same bodies as PNG::decode/getBpp/getPixelType/hasAlpha in
 * PNGdec.cpp, so behaviour matches the vendor's C++ calls exactly. */
int PNG_decode(PNGIMAGE *pPNG, void *pUser, int iOptions) { return DecodePNG(pPNG, pUser, iOptions); }
int PNG_getBpp(PNGIMAGE *pPNG) { return (int)pPNG->ucBpp; }
int PNG_getPixelType(PNGIMAGE *pPNG) { return (int)pPNG->ucPixelType; }
int PNG_hasAlpha(PNGIMAGE *pPNG) { return pPNG->iHasAlpha; }
