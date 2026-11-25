#include "file_io.hpp"
#include <fstream>
#ifndef __EMSCRIPTEN__
#include <zstd.h>
#endif

namespace game {

// ---- Raw -------------------------------------------------------------------

class RawFileWriter : public FileWriter {
public:
  explicit RawFileWriter(const std::string& path) {
    out_.open(path, std::ios::binary);
    if (!out_)
      throw std::runtime_error("cannot open for writing: " + path);
  }
  void write(const void* data, size_t size) override {
    if (size == 0)
      return;
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out_)
      throw std::runtime_error("failed to write to output file");
  }
  void close() override {
    if (!out_.is_open())
      return;
    out_.flush();
    const bool bad = !out_;
    out_.close();
    if (bad || out_.fail())
      throw std::runtime_error("failed to close output file");
  }

private:
  std::ofstream out_;
};

class RawFileReader : public FileReader {
public:
  explicit RawFileReader(const std::string& path) {
    in_.open(path, std::ios::binary);
    if (!in_)
      throw std::runtime_error("cannot open for reading: " + path);
  }
  bool read(void* data, size_t size) override {
    in_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return in_.good();
  }
  bool eof() const override { return in_.eof(); }
  void close() override { in_.close(); }

private:
  mutable std::ifstream in_;
};

// ---- Zstd ------------------------------------------------------------------

#ifndef __EMSCRIPTEN__

class ZstdFileWriter : public FileWriter {
public:
  explicit ZstdFileWriter(const std::string& path) {
    out_.open(path, std::ios::binary);
    if (!out_)
      throw std::runtime_error("cannot open for writing: " + path);

    cstream_ = ZSTD_createCStream();
    if (!cstream_)
      throw std::runtime_error("ZSTD_createCStream failed");

    if (ZSTD_isError(ZSTD_initCStream(cstream_, 3))) {
      ZSTD_freeCStream(cstream_);
      throw std::runtime_error("ZSTD_initCStream failed");
    }

    out_buf_.resize(ZSTD_CStreamOutSize());
  }

  ~ZstdFileWriter() override {
    if (cstream_) {
      try { close(); } catch (...) {}
      ZSTD_freeCStream(cstream_);
    }
  }

  void write(const void* data, size_t size) override {
    ZSTD_inBuffer in{data, size, 0};
    while (in.pos < in.size) {
      ZSTD_outBuffer out{out_buf_.data(), out_buf_.size(), 0};
      if (ZSTD_isError(ZSTD_compressStream(cstream_, &out, &in)))
        throw std::runtime_error("ZSTD_compressStream failed");
      if (out.pos > 0) {
        out_.write(reinterpret_cast<const char*>(out_buf_.data()),
                   static_cast<std::streamsize>(out.pos));
        if (!out_)
          throw std::runtime_error("failed to write compressed output");
      }
    }
  }

  void close() override {
    if (!out_.is_open())
      return;
    try {
      size_t remaining;
      do {
        ZSTD_outBuffer out{out_buf_.data(), out_buf_.size(), 0};
        remaining = ZSTD_endStream(cstream_, &out);
        if (ZSTD_isError(remaining))
          throw std::runtime_error("ZSTD_endStream failed");
        if (out.pos > 0) {
          out_.write(reinterpret_cast<const char*>(out_buf_.data()),
                     static_cast<std::streamsize>(out.pos));
          if (!out_)
            throw std::runtime_error("failed to write compressed output");
        }
      } while (remaining > 0);
    } catch (...) {
      out_.close();
      throw;
    }
    out_.flush();
    const bool bad = !out_;
    out_.close();
    if (bad || out_.fail())
      throw std::runtime_error("failed to close compressed output file");
  }

private:
  std::ofstream out_;
  ZSTD_CStream* cstream_{nullptr};
  std::vector<uint8_t> out_buf_;
};

class ZstdFileReader : public FileReader {
public:
  explicit ZstdFileReader(const std::string& path) {
    in_.open(path, std::ios::binary);
    if (!in_)
      throw std::runtime_error("cannot open for reading: " + path);

    dstream_ = ZSTD_createDStream();
    if (!dstream_)
      throw std::runtime_error("ZSTD_createDStream failed");

    if (ZSTD_isError(ZSTD_initDStream(dstream_))) {
      ZSTD_freeDStream(dstream_);
      throw std::runtime_error("ZSTD_initDStream failed");
    }

    in_buf_.resize(ZSTD_DStreamInSize());
    out_buf_.resize(ZSTD_DStreamOutSize());
  }

  ~ZstdFileReader() {
    if (dstream_)
      ZSTD_freeDStream(dstream_);
  }

  bool read(void* data, size_t size) override {
    auto* dest = static_cast<uint8_t*>(data);
    size_t remaining = size;

    while (remaining > 0) {
      // Drain the decoded output buffer first.
      if (decoded_pos_ < decoded_size_) {
        size_t avail = decoded_size_ - decoded_pos_;
        size_t copy = std::min(avail, remaining);
        std::memcpy(dest, out_buf_.data() + decoded_pos_, copy);
        dest += copy;
        remaining -= copy;
        decoded_pos_ += copy;
        continue;
      }

      // Refill the compressed input buffer if exhausted.
      if (in_pos_ >= in_size_) {
        in_.read(reinterpret_cast<char*>(in_buf_.data()),
                 static_cast<std::streamsize>(in_buf_.size()));
        in_size_ = static_cast<size_t>(in_.gcount());
        in_pos_ = 0;
        if (in_size_ == 0) {
          eof_ = true;
          return false;
        }
      }

      // Decompress a chunk.
      const size_t prev_in_pos = in_pos_;
      ZSTD_inBuffer in{in_buf_.data(), in_size_, in_pos_};
      ZSTD_outBuffer out{out_buf_.data(), out_buf_.size(), 0};
      size_t ret = ZSTD_decompressStream(dstream_, &out, &in);
      if (ZSTD_isError(ret))
        throw std::runtime_error("ZSTD_decompressStream failed");
      in_pos_ = in.pos;
      decoded_size_ = out.pos;
      decoded_pos_ = 0;

      // Only bail if zstd made zero progress without emptying in_buf_; otherwise loop to refill.
      if (decoded_size_ == 0 && in_pos_ == prev_in_pos) {
        eof_ = true;
        return false;
      }
    }
    return true;
  }

  bool eof() const override { return eof_; }
  void close() override { in_.close(); }

private:
  std::ifstream in_;
  ZSTD_DStream* dstream_{nullptr};
  std::vector<uint8_t> in_buf_;
  std::vector<uint8_t> out_buf_;
  size_t in_pos_{0};
  size_t in_size_{0};
  size_t decoded_pos_{0};
  size_t decoded_size_{0};
  bool eof_{false};
};

#endif // !__EMSCRIPTEN__

// ---- Extension deduction + factories ----------------------------------------

FileIoType deduce_file_io_type(const std::string& path) {
  auto pos = path.rfind('.');
  if (pos == std::string::npos)
    throw std::runtime_error("cannot deduce file type: no extension in '" + path + "'");
  std::string ext = path.substr(pos);
  if (ext == ".zst" || ext == ".zstd")
#ifndef __EMSCRIPTEN__
    return FileIoType::Zstd;
#else
    throw std::runtime_error("Zstd not supported in browser build — use .ud files");
#endif
  if (ext == ".ud")
    return FileIoType::Raw;
  throw std::runtime_error("unknown file extension '" + ext + "'");
}

std::unique_ptr<FileWriter> make_file_writer(const std::string& path, FileIoType type) {
  switch (type) {
#ifndef __EMSCRIPTEN__
    case FileIoType::Zstd: return std::make_unique<ZstdFileWriter>(path);
#else
    case FileIoType::Zstd: throw std::runtime_error("Zstd not supported in browser build");
#endif
    case FileIoType::Raw:  return std::make_unique<RawFileWriter>(path);
  }
  return std::make_unique<RawFileWriter>(path);
}

std::unique_ptr<FileReader> make_file_reader(const std::string& path, FileIoType type) {
  switch (type) {
#ifndef __EMSCRIPTEN__
    case FileIoType::Zstd: return std::make_unique<ZstdFileReader>(path);
#else
    case FileIoType::Zstd: throw std::runtime_error("Zstd not supported in browser build");
#endif
    case FileIoType::Raw:  return std::make_unique<RawFileReader>(path);
  }
  return std::make_unique<RawFileReader>(path);
}

} // namespace game
