/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   In addition, as a special exception, the copyright holders give
 *   permission to link the code of portions of this program with the
 *   OpenSSL library under certain conditions as described in each
 *   individual source file, and distribute linked combinations including
 *   the two.
 *
 *   You must obey the GNU General Public License in all respects for all
 *   of the code used other than OpenSSL. If you modify file(s) with this
 *   exception, you may extend this exception to your version of the
 *   file(s), but you are not obligated to do so. If you do not wish to do
 *   so, delete this exception statement from your version. If you delete
 *   this exception statement from all source files in the program, then
 *   also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "decoderVVCDec.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <cstring>

#include "common/typedef.h"

// Debug the decoder ( 0:off 1:interactive deocder only 2:caching decoder only 3:both)
#define DECODERVVCDEC_DEBUG_OUTPUT 1
#if DECODERVVCDEC_DEBUG_OUTPUT && !NDEBUG
#include <QDebug>
#if DECODERVVCDEC_DEBUG_OUTPUT == 1
#define DEBUG_VVCDEC                                                                               \
  if (!isCachingDecoder)                                                                           \
  qDebug
#elif DECODERVVCDEC_DEBUG_OUTPUT == 2
#define DEBUG_VVCDEC                                                                               \
  if (isCachingDecoder)                                                                            \
  qDebug
#elif DECODERVVCDEC_DEBUG_OUTPUT == 3
#define DEBUG_VVCDEC                                                                               \
  if (isCachingDecoder)                                                                            \
    qDebug("c:");                                                                                  \
  else                                                                                             \
    qDebug("i:");                                                                                  \
  qDebug
#endif
#else
#define DEBUG_VVCDEC(fmt, ...) ((void)0)
#endif

// Restrict is basically a promise to the compiler that for the scope of the pointer, the target of
// the pointer will only be accessed through that pointer (and pointers copied from it).
#if __STDC__ != 1
#define restrict __restrict /* use implementation __ format */
#else
#ifndef __STDC_VERSION__
#define restrict __restrict /* use implementation __ format */
#else
#if __STDC_VERSION__ < 199901L
#define restrict __restrict /* use implementation __ format */
#else
#/* all ok */
#endif
#endif
#endif

namespace
{

const std::vector<libvvcdec_ColorComponent>
    colorComponentMap({LIBVVCDEC_LUMA, LIBVVCDEC_CHROMA_U, LIBVVCDEC_CHROMA_V});

}

decoderVVCDec::decoderVVCDec(int signalID, bool cachingDecoder)
    : decoderBaseSingleLib(cachingDecoder)
{
  // For now we don't support different signals (like prediction, residual)
  (void)signalID;

  this->rawFormat = YUView::raw_YUV;

  // Try to load the decoder library (.dll on Windows, .so on Linux, .dylib on Mac)
  QSettings settings;
  settings.beginGroup("Decoders");
  this->loadDecoderLibrary(settings.value("libVVCDecFile", "").toString());
  settings.endGroup();

  if (this->decoderState != DecoderState::Error)
    this->allocateNewDecoder();
}

decoderVVCDec::~decoderVVCDec()
{
  if (this->decoder != nullptr)
    this->functions.libvvcdec_free_decoder(this->decoder);
}

QStringList decoderVVCDec::getLibraryNames()
{
  // If the file name is not set explicitly, QLibrary will try to open the .so file first.
  // Since this has been compiled for linux it will fail and not even try to open the .dylib.
  // On windows and linux ommitting the extension works
  auto names = is_Q_OS_MAC ? QStringList() << "libvvcdec.dylib" : QStringList() << "libvvcdec";

  return names;
}

void decoderVVCDec::resolveLibraryFunctionPointers()
{
  // Get/check function pointers
  if (!resolve(this->functions.libvvcdec_get_version, "libvvcdec_get_version"))
    return;
  if (!resolve(this->functions.libvvcdec_new_decoder, "libvvcdec_new_decoder"))
    return;
  if (!resolve(this->functions.libvvcdec_free_decoder, "libvvcdec_free_decoder"))
    return;
  if (!resolve(this->functions.libvvcdec_push_nal_unit, "libvvcdec_push_nal_unit"))
    return;

  if (!resolve(this->functions.libvvcdec_get_picture_POC, "libvvcdec_get_picture_POC"))
    return;
  if (!resolve(this->functions.libvvcdec_get_picture_width, "libvvcdec_get_picture_width"))
    return;
  if (!resolve(this->functions.libvvcdec_get_picture_height, "libvvcdec_get_picture_height"))
    return;
  if (!resolve(this->functions.libvvcdec_get_picture_stride, "libvvcdec_get_picture_stride"))
    return;
  if (!resolve(this->functions.libvvcdec_get_picture_plane, "libvvcdec_get_picture_plane"))
    return;
  if (!resolve(this->functions.libvvcdec_get_picture_chroma_format,
               "libvvcdec_get_picture_chroma_format"))
    return;
  if (!resolve(this->functions.libvvcdec_get_picture_bit_depth, "libvvcdec_get_picture_bit_depth"))
    return;
}

template <typename T> T decoderVVCDec::resolve(T &fun, const char *symbol, bool optional)
{
  auto ptr = this->library.resolve(symbol);
  if (!ptr)
  {
    if (!optional)
      setError(QStringLiteral("Error loading the libde265 library: Can't find function %1.")
                   .arg(symbol));
    return nullptr;
  }

  return fun = reinterpret_cast<T>(ptr);
}

void decoderVVCDec::resetDecoder()
{
  if (this->decoder != nullptr)
    if (this->functions.libvvcdec_free_decoder(decoder) != LIBVVCDEC_OK)
      return setError("Reset: Freeing the decoder failded.");

  this->decoder = nullptr;

  this->allocateNewDecoder();
}

void decoderVVCDec::allocateNewDecoder()
{
  if (this->decoder != nullptr)
    return;

  DEBUG_VVCDEC("decoderVVCDec::allocateNewDecoder - decodeSignal %d", decodeSignal);

  // Create new decoder object
  this->decoder = this->functions.libvvcdec_new_decoder();
}

bool decoderVVCDec::decodeNextFrame()
{
  if (this->decoderState != DecoderState::RetrieveFrames)
  {
    DEBUG_VVCDEC("decoderVVCDec::decodeNextFrame: Wrong decoder state.");
    return false;
  }

  return this->getNextFrameFromDecoder();
}

bool decoderVVCDec::getNextFrameFromDecoder()
{
  DEBUG_VVCDEC("decoderVVCDec::getNextFrameFromDecoder");

  // Check the validity of the picture
  auto picSize = QSize(this->functions.libvvcdec_get_picture_width(this->decoder, LIBVVCDEC_LUMA),
                       this->functions.libvvcdec_get_picture_height(this->decoder, LIBVVCDEC_LUMA));
  if (picSize.width() == 0 || picSize.height() == 0)
    DEBUG_VVCDEC("decoderVVCDec::getNextFrameFromDecoder got invalid size");
  auto subsampling = convertFromInternalSubsampling(
      this->functions.libvvcdec_get_picture_chroma_format(this->decoder));
  if (subsampling == YUV_Internals::Subsampling::UNKNOWN)
    DEBUG_VVCDEC("decoderVVCDec::getNextFrameFromDecoder got invalid chroma format");
  auto bitDepth = this->functions.libvvcdec_get_picture_bit_depth(this->decoder, LIBVVCDEC_LUMA);
  if (bitDepth < 8 || bitDepth > 16)
    DEBUG_VVCDEC("decoderVVCDec::getNextFrameFromDecoder got invalid bit depth");

  if (!this->frameSize.isValid() && !this->formatYUV.isValid())
  {
    // Set the values
    this->frameSize = picSize;
    this->formatYUV = YUV_Internals::yuvPixelFormat(subsampling, bitDepth);
  }
  else
  {
    // Check the values against the previously set values
    if (this->frameSize != picSize)
      return setErrorB("Received a frame of different size");
    if (this->formatYUV.subsampling != subsampling)
      return setErrorB("Received a frame with different subsampling");
    if (this->formatYUV.bitsPerSample != bitDepth)
      return setErrorB("Received a frame with different bit depth");
  }

  DEBUG_VVCDEC("decoderVVCDec::getNextFrameFromDecoder got a valid frame");
  return true;
}

bool decoderVVCDec::pushData(QByteArray &data)
{
  if (decoderState != DecoderState::NeedsMoreData)
  {
    DEBUG_VVCDEC("decoderVVCDec::pushData: Wrong decoder state.");
    return false;
  }

  bool endOfFile = (data.length() == 0);
  if (endOfFile)
    DEBUG_VVCDEC("decoderFFmpeg::pushData: Received empty packet. Setting EOF.");

  bool checkOutputPictures = false;
  bool bNewPicture         = false;
  auto err                 = this->functions.libvvcdec_push_nal_unit(this->decoder,
                                                     (const unsigned char *)data.data(),
                                                     data.length(),
                                                     endOfFile,
                                                     bNewPicture,
                                                     checkOutputPictures);
  if (err != LIBVVCDEC_OK)
  {
    DEBUG_VVCDEC("decoderVVCDec::pushData Error pushing data");
    return setErrorB(QString("Error pushing data to decoder (libvvcdec_push_nal_unit) length %1")
                         .arg(data.length()));
  }
  DEBUG_VVCDEC("decoderVVCDec::pushData pushed NAL length %d%s%s",
               data.length(),
               bNewPicture ? " bNewPicture" : "",
               checkOutputPictures ? " checkOutputPictures" : "");

  if (checkOutputPictures && this->getNextFrameFromDecoder())
  {
    this->decoderState = DecoderState::RetrieveFrames;
    this->currentOutputBuffer.clear();
  }

  return true;
}

QByteArray decoderVVCDec::getRawFrameData()
{
  if (this->decoderState != DecoderState::RetrieveFrames)
  {
    DEBUG_VVCDEC("decoderVVCDec::getRawFrameData: Wrong decoder state.");
    return {};
  }

  if (this->currentOutputBuffer.isEmpty())
  {
    // Put image data into buffer
    copyImgToByteArray(this->currentOutputBuffer);
    DEBUG_VVCDEC("decoderVVCDec::getRawFrameData copied frame to buffer");
  }

  this->decoderState = DecoderState::NeedsMoreData;

  return currentOutputBuffer;
}

void decoderVVCDec::copyImgToByteArray(QByteArray &dst)
{
  auto fmt = this->functions.libvvcdec_get_picture_chroma_format(this->decoder);
  if (fmt == LIBVVCDEC_CHROMA_UNKNOWN)
  {
    DEBUG_VVCDEC("decoderVVCDec::copyImgToByteArray picture format is unknown");
    return;
  }
  auto nrPlanes = (fmt == LIBVVCDEC_CHROMA_400) ? 1u : 3u;

  bool outputTwoByte =
      (this->functions.libvvcdec_get_picture_bit_depth(this->decoder, LIBVVCDEC_LUMA) > 8);
  if (nrPlanes > 1)
  {
    auto bitDepthU =
        this->functions.libvvcdec_get_picture_bit_depth(this->decoder, LIBVVCDEC_CHROMA_U);
    auto bitDepthV =
        this->functions.libvvcdec_get_picture_bit_depth(this->decoder, LIBVVCDEC_CHROMA_V);
    if ((outputTwoByte != bitDepthU > 8) || (outputTwoByte != bitDepthV > 8))
    {
      DEBUG_VVCDEC("decoderVVCDec::copyImgToByteArray different bit depth in YUV components. This "
                   "is not supported.");
      return;
    }
  }

  // How many samples are in each component?
  const uint32_t width[2] = {
      this->functions.libvvcdec_get_picture_width(this->decoder, LIBVVCDEC_LUMA),
      this->functions.libvvcdec_get_picture_width(this->decoder, LIBVVCDEC_CHROMA_U)};
  const uint32_t height[2] = {
      this->functions.libvvcdec_get_picture_height(this->decoder, LIBVVCDEC_LUMA),
      this->functions.libvvcdec_get_picture_height(this->decoder, LIBVVCDEC_CHROMA_U)};

  if (this->functions.libvvcdec_get_picture_width(this->decoder, LIBVVCDEC_CHROMA_V) != width[1] ||
      this->functions.libvvcdec_get_picture_height(this->decoder, LIBVVCDEC_CHROMA_V) != height[1])
  {
    DEBUG_VVCDEC("decoderVVCDec::copyImgToByteArray chroma components have different size");
    return;
  }

  auto outSizeLumaBytes   = width[0] * height[0] * (outputTwoByte ? 2 : 1);
  auto outSizeChromaBytes = (nrPlanes == 1) ? 0u : (width[1] * height[1] * (outputTwoByte ? 2 : 1));
  // How many bytes do we need in the output buffer?
  auto nrBytesOutput = (outSizeLumaBytes + outSizeChromaBytes * 2);
  DEBUG_VVCDEC("decoderVVCDec::copyImgToByteArray nrBytesOutput %d", nrBytesOutput);

  // Is the output big enough?
  if (dst.capacity() < int(nrBytesOutput))
    dst.resize(nrBytesOutput);

  for (unsigned c = 0; c < nrPlanes; c++)
  {
    auto component = colorComponentMap[c];
    auto cIdx      = (c == 0 ? 0 : 1);

    auto plane      = this->functions.libvvcdec_get_picture_plane(this->decoder, component);
    auto stride     = this->functions.libvvcdec_get_picture_stride(this->decoder, component);
    auto widthBytes = width[cIdx] * (outputTwoByte ? 2 : 1);

    if (plane == nullptr)
    {
      DEBUG_VVCDEC("decoderVVCDec::copyImgToByteArray unable to get plane for component %d", c);
      return;
    }

    unsigned char *restrict d = (unsigned char *)dst.data();
    if (c > 0)
      d += outSizeLumaBytes;
    if (c == 2)
      d += outSizeChromaBytes;

    for (unsigned y = 0; y < height[cIdx]; y++)
    {
      std::memcpy(d, plane, widthBytes);
      plane += stride;
      d += widthBytes;
    }
  }
}

QString decoderVVCDec::getDecoderName() const
{
  return (decoderState == DecoderState::Error) ? "VVCDec" : this->functions.libvvcdec_get_version();
}

bool decoderVVCDec::checkLibraryFile(QString libFilePath, QString &error)
{
  decoderVVCDec testDecoder;

  // Try to load the library file
  testDecoder.library.setFileName(libFilePath);
  if (!testDecoder.library.load())
  {
    error = "Error opening QLibrary.";
    return false;
  }

  // Now let's see if we can retrive all the function pointers that we will need.
  // If this works, we can be fairly certain that this is a valid library.
  testDecoder.resolveLibraryFunctionPointers();
  error = testDecoder.decoderErrorString();
  return !testDecoder.errorInDecoder();
}

YUV_Internals::Subsampling decoderVVCDec::convertFromInternalSubsampling(libvvcdec_ChromaFormat fmt)
{
  if (fmt == LIBVVCDEC_CHROMA_400)
    return YUV_Internals::Subsampling::YUV_400;
  if (fmt == LIBVVCDEC_CHROMA_420)
    return YUV_Internals::Subsampling::YUV_420;
  if (fmt == LIBVVCDEC_CHROMA_422)
    return YUV_Internals::Subsampling::YUV_422;
  if (fmt == LIBVVCDEC_CHROMA_444)
    return YUV_Internals::Subsampling::YUV_444;

  return YUV_Internals::Subsampling::UNKNOWN;
}
