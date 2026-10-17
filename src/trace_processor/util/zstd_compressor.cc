/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/util/zstd_compressor.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
#include <zstd.h>
#endif

namespace perfetto::trace_processor::util {

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

// static
std::unique_ptr<uint8_t, base::FreeDeleter> ZstdCompressor::CompressFully(
    const uint8_t* data,
    size_t len,
    size_t* out_size,
    int level) {
  size_t bound = ZSTD_compressBound(len);
  std::unique_ptr<uint8_t, base::FreeDeleter> out(
      static_cast<uint8_t*>(malloc(bound)));
  // ZSTD_compress allocates and frees a ZSTD_CCtx internally on every call.
  // That is fine for the handful of blobs this is used on; if it is ever placed
  // on a hot per-row path, reuse a ZSTD_compressCCtx with a (thread_local)
  // context to avoid the per-call allocation.
  size_t n = ZSTD_compress(out.get(), bound, data, len, level);
  if (ZSTD_isError(n)) {
    return nullptr;
  }
  *out_size = n;
  return out;
}

ZstdStreamCompressor::ZstdStreamCompressor(int level)
    : cctx_(ZSTD_createCCtx()),
      staging_(new uint8_t[ZSTD_CStreamOutSize()]),
      level_(level) {}

ZstdStreamCompressor::~ZstdStreamCompressor() = default;

void ZstdStreamCompressor::Deleter::operator()(ZSTD_CCtx_s* cctx) const {
  ZSTD_freeCCtx(cctx);
}

bool ZstdStreamCompressor::BeginFrame(uint64_t size) {
  if (!cctx_) {
    return false;
  }
  return !ZSTD_isError(ZSTD_CCtx_reset(cctx_.get(), ZSTD_reset_session_only)) &&
         !ZSTD_isError(ZSTD_CCtx_setParameter(
             cctx_.get(), ZSTD_c_compressionLevel, level_)) &&
         !ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(cctx_.get(), size));
}

bool ZstdStreamCompressor::Feed(const uint8_t* data,
                                size_t size,
                                std::vector<uint8_t>* out) {
  return Compress(data, size, /*end=*/false, out);
}

bool ZstdStreamCompressor::EndFrame(std::vector<uint8_t>* out) {
  return Compress(nullptr, 0, /*end=*/true, out);
}

bool ZstdStreamCompressor::Compress(const uint8_t* data,
                                    size_t size,
                                    bool end,
                                    std::vector<uint8_t>* out) {
  ZSTD_inBuffer input{data, size, 0};
  for (;;) {
    // Compress into the staging buffer so that only the (much smaller) output
    // is copied into |out|, rather than zero-filling room for it.
    ZSTD_outBuffer output{staging_.get(), ZSTD_CStreamOutSize(), 0};
    size_t remaining = ZSTD_compressStream2(cctx_.get(), &output, &input,
                                            end ? ZSTD_e_end : ZSTD_e_continue);
    if (ZSTD_isError(remaining)) {
      return false;
    }
    out->insert(out->end(), staging_.get(), staging_.get() + output.pos);
    // With ZSTD_e_end, |remaining| counts the output still buffered in the
    // context. With ZSTD_e_continue, consuming all input is enough.
    if (end ? remaining == 0 : input.pos == input.size) {
      return true;
    }
  }
}

#else  // !PERFETTO_ZSTD

// static
std::unique_ptr<uint8_t, base::FreeDeleter>
ZstdCompressor::CompressFully(const uint8_t*, size_t, size_t*, int) {
  return nullptr;
}

ZstdStreamCompressor::ZstdStreamCompressor(int level) : level_(level) {}

ZstdStreamCompressor::~ZstdStreamCompressor() = default;

void ZstdStreamCompressor::Deleter::operator()(ZSTD_CCtx_s*) const {}

bool ZstdStreamCompressor::BeginFrame(uint64_t) {
  return false;
}

bool ZstdStreamCompressor::Feed(const uint8_t*, size_t, std::vector<uint8_t>*) {
  return false;
}

bool ZstdStreamCompressor::EndFrame(std::vector<uint8_t>*) {
  return false;
}

bool ZstdStreamCompressor::Compress(const uint8_t*,
                                    size_t,
                                    bool,
                                    std::vector<uint8_t>*) {
  return false;
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

}  // namespace perfetto::trace_processor::util
