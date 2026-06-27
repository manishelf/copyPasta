#include <FileReaderWriter.hpp>
#include <CacheAndPool.hpp>
#include <Logger.hpp>
#include <iostream>
#include <assert.h>
#include <algorithm>

namespace copypasta {

    void File::loadFromEntry() {
        std::error_code ec;
        status = dir_entry.status(ec);

        if (!ec) {
            DEBUG_FULL("File loadFromEntry");
            // TODO: this might be too expensive
            path = fs::absolute(dir_entry.path().lexically_normal());

            const auto filename = path.filename();
            name = filename.string();
            ext = filename.extension().string();
            
            isDir = status.type() == fs::file_type::directory;
            isReg = status.type() == fs::file_type::regular;

            // TODO: this might be too expensive
            size = isReg ? dir_entry.file_size(ec) : 0;

            isValid = true;
        }
        else {
            LERROR("File is invalid - " << dir_entry.path().string() << " error: " << ec.value() << ": " << ec.message());
            isValid = false;
        }
    }

    File::File(std::string path) : dir_entry(path), pathStr(path) {
        DEBUG_FULL("File ctor - " << path);
        level = 0;
        loadFromEntry();
    };

    File::File(fs::directory_entry entry) : dir_entry(entry), pathStr(entry.path().string()) {
        DEBUG_FULL("File ctor - " << pathStr);
        level = 0;
        loadFromEntry();
    };

    File::File() {
        size = 0;
        isValid = false;
        level = 0;
    }

    void File::sync() {
        dir_entry.refresh();
        status = dir_entry.status();
        size = dir_entry.file_size();
        isValid = dir_entry.exists();
        isReg = dir_entry.is_regular_file();
        path = dir_entry.path();
        DEBUG("File sync - " << pathStr);
    };

    int File::deleteFile(File& target) {
        if (target.isDir)
            return -1;
        INFO("File delete file - " << target.pathStr);
        return fs::remove(target.path);
    };

    int File::deleteDir(File& target) {
        if (!target.isDir)
            return false;
        INFO("File delete dir" << target.pathStr);
        return fs::remove_all(target.path);
    };

    bool File::rename(File& file, std::string name) {
        fs::rename(file.pathStr, name);
        file.path = fs::path(name);
        file.pathStr = name;
        file.dir_entry = fs::directory_entry(file.path);
        file.sync();
        INFO("File rename from - " << file.name << " to - " << name);
        return true;
    };

    File::~File() {
        DEBUG_FULL("File destroyed");
    };

    // FileReader

#define UPDATE_ROW_OFFSETS(data, len)                                            \
  if(!rowOffsetsValid){                                                          \
    DEBUG_FULL("Updating row offsets - " << len);                                \
    rowOffsets.clear();                                                          \
    rowOffsets.push_back(0);                                                     \
    for (size_t i = 0; i < (len); ++i) {                                         \
      if ((data)[i] == '\n') {                                                   \
        rowOffsets.push_back(i + 1);                                             \
      }                                                                          \
    }                                                                            \
    rowOffsetsValid = true;                                                      \
  }

    const std::vector<size_t>& FileReader::getRowOffsets() {
        UPDATE_ROW_OFFSETS(buf, bufSize);
        return rowOffsets;
    }

    FileReader::FileReader(File file, size_t blockSize) {
        DEBUG_FULL("FileReader ctor");
        this->file = file;
        if (file.isValid) {
            this->blockSize = blockSize;
            _isValid = !file.isDir;
            readFileMetadata();
        }
    };

    FileReader::FileReader(std::string filePath, size_t blockSize) {
        DEBUG_FULL("FileReader ctor");
        file = File(filePath);
        if (file.isValid) {
            _isValid = !file.isDir;
            this->blockSize = blockSize;
            readFileMetadata();
        }
    };

    FileReader::FileReader(const FileSnapshot snap, size_t blockSize) {
        DEBUG_FULL("FileReader ctor with snap");
        snapshotMode = true;
        buf.reserve(snap.cont.length());
        buf.assign(snap.cont.begin(), snap.cont.end());
        bufStart = 0;
        bufSize = snap.cont.length();
        file = snap.file;
        this->blockSize = blockSize;
        _isValid = true;
        rowOffsetsValid = !snap.rowOffsets.empty();
        rowOffsets = snap.rowOffsets;
    };

    FileReader::FileReader(const FileReader& copy) {
        DEBUG_FULL("FileReader copy");
        file = copy.file;
        _isValid = copy._isValid;
        pos = copy.pos;
        buf = copy.buf;
        blockSize = copy.blockSize;
        level = copy.level;
        rowOffsets = copy.rowOffsets;
        bufStart = copy.bufStart;
        bufSize = copy.bufSize;
        readReverse = copy.readReverse;
        snapshotMode = copy.snapshotMode;
    }

    void FileReader::readFileMetadata() {
        if (file.isValid && file.size != 0) {

            DEBUG_FULL("FileReader readFileMetadata");

            bufStart = 0;
            rowOffsets.reserve(file.size / 50);

            size_t blockSize = std::min(this->blockSize, file.size);
            DEBUG_FULL("FileReader readFileMetadata block size - " << blockSize);

            load(0, blockSize);
        }
        else {
            LERROR("FileReader readFileMetadata failed");
            bufSize = 0;
            bufStart = 0;
            _isValid = false;
        }
    };

    FileReader::block FileReader::sync() {
        if (!_isValid)
            return { nullptr, 0 };

        if (snapshotMode)
            return { buf.data(), bufSize };

        DEBUG("FileReader sync");

        file.sync();

        buf.clear();
        bufSize = 0;
        bufStart = 0;
        return load(0, file.size);
    };

    std::string_view FileReader::get() { return get(bufStart, bufSize); }

    std::string_view FileReader::get(size_t from, size_t to) {
        if (!_isValid)
            return {};

        if (file.isValid && (from > file.size || to > file.size))
            return {};

        DEBUG_FULL("FileReader get");

        auto block = load(from, to);
        if (block.cont == nullptr)
            return {};

        return std::string_view(block.cont, block.size);
    };

    std::string_view FileReader::getLine(size_t row) {

        DEBUG_FULL("FileReader getLine " << row);

        UPDATE_ROW_OFFSETS(buf, bufSize);
        // this has caused OOM due to unbounded access over the array :)
        if (row + 1 == rowOffsets.size()) {
            return get(rowOffsets[row], this->file.size);
        }
        else if (row >= rowOffsets.size()) {
            return "";
        }

        return get(rowOffsets[row], rowOffsets[row + 1]);
    }

    std::string_view FileReader::getIndent(size_t row) {
        std::string_view line = getLine(row);
        int end = 0;
        for (int i = 0; i < line.length(); i++) {
            auto ch = line[i];
            if (!(ch == ' ' || ch == '\t')) {
                end = i;
                break;
            }
        }
        return line.substr(0, end);
    }

    FileReader::block FileReader::load(size_t from, size_t to) {
        if (!_isValid)
            return { nullptr, 0 };

        size_t fileEnd = snapshotMode ? bufSize : file.size;

        if (from > fileEnd || to > fileEnd || to == 0)
            return { nullptr, 0 };

        size_t length = to - from;

        if (from >= bufStart && to <= bufSize) {
            return { &buf.data()[from - bufStart], length };
        }

        DEBUG("FileReader load from - " << from << " to - " << to);

        buf.resize(to);

        std::ifstream iFileStream(file.path.c_str(), std::ios::binary | std::ios::ate);

        iFileStream.seekg(bufSize, std::ios::beg);
        iFileStream.read(&buf.data()[from], to - bufSize);
        size_t bytesRead = iFileStream.gcount();

        iFileStream.close();
        bufSize = to;

        if (bytesRead == 0)
            return { nullptr, 0 };

        return { &buf.data()[from], length };
    };

    FileReader::block FileReader::readBlockAt(size_t pos) {
        if (!_isValid)
            return { nullptr, 0 };
        if (pos >= file.size)
            return { nullptr, 0 };

        DEBUG_FULL("FileReader readBlockAt - " << pos);

        size_t size = std::min(FileReader::defaultBlockSize, file.size - pos);

        return load(pos, pos + size);
    }

    TSInput FileReader::asTsInput() {
        TSInput input;
        input.payload = this;
        input.read = &FileReader::tsRead;
        input.encoding = TSInputEncodingUTF8;
        DEBUG_FULL("FileReader asTsInput");
        return input;
    };

    const char* FileReader::tsRead(void* payload, uint32_t byte_index,
        TSPoint position, uint32_t* bytes_read) {
        auto* reader = static_cast<FileReader*>(payload);

        if (byte_index >= reader->file.size) {
            DEBUG_FULL("FileReader asTsInput finished");
            *bytes_read = 0;
            return nullptr;
        }

        size_t blockSize =
            std::min(reader->blockSize, reader->file.size - byte_index);

        DEBUG_FULL("FileReader asTsInput read from - " << byte_index << " to - " << blockSize);

        // Ensure buffer covers requested range
        if (reader->buf.empty() || byte_index < reader->bufStart ||
            byte_index + blockSize > reader->bufStart + reader->bufSize) {

            reader->load(byte_index, byte_index + blockSize);
            reader->bufStart = byte_index;
        }

        *bytes_read = static_cast<uint32_t>(blockSize);
        return &reader->buf.data()[byte_index - reader->bufStart];
    }

    TSPoint _getP(size_t byteOffset, const std::vector<size_t>& rowOffsets) {

        if (rowOffsets.empty())
            return { 0, static_cast<uint32_t>(byteOffset) };

        DEBUG_FULL("_getP called - " << rowOffsets.size());
        auto begin = rowOffsets.data();
        auto end = begin + rowOffsets.size();
        // Find the first row offset that is GREATER than our byte
        auto it = std::upper_bound(begin, end, byteOffset);

        if (it == begin) {
            return { 0, static_cast<uint32_t>(byteOffset) };
        }

        // The row number is the index of the element before 'it'
        uint32_t row = static_cast<uint32_t>((it - begin) - 1);

        // The column is the difference between our offset and the rows start offset
        uint32_t col = byteOffset - rowOffsets[row];

        return { row, col };
    }

    TSPoint FileReader::getP(size_t byteOffset) {
        UPDATE_ROW_OFFSETS(buf, bufSize);
        // :: is needed to scope it from outside
        return copypasta::_getP(byteOffset, rowOffsets);
    }


    TSRange _makeRange(size_t start, size_t end, const std::vector<size_t>& rowOffsets) {
        TSRange r;
        r.start_byte = static_cast<uint32_t>(start);
        r.end_byte = static_cast<uint32_t>(end);
        r.start_point = _getP(start, rowOffsets);
        r.end_point = _getP(end, rowOffsets);
        return r;
    }

    std::vector<FileReader::MatchResult> FileReader::find(std::string pattern,
        bool regex, uint32_t opt_compile) {


        std::vector<MatchResult> matches;


        DEBUG("FileReader find called with - " + pattern);
        if (regex) {
            pcre2_code* re = PcreCache::global().get(pattern, opt_compile);

            return findWith(re);
        }
        else {
            STORE_ITER_INFO;
            for (auto block = next(); block.cont && block.size != 0; block = next()) {
                std::string_view searchSpace(block.cont, block.size);

                size_t foundPos = 0;
                size_t offset = 0;
                while ((foundPos = searchSpace.find(pattern, offset)) !=
                    std::string_view::npos) {

                    size_t matchStart = foundPos;
                    size_t matchEnd = matchStart + pattern.size();

                    MatchResult match;
                    match.match = _makeRange(matchStart, matchEnd, rowOffsets);
                    matches.push_back(match);

                    offset = matchEnd;
                }
            }
            RESTORE_ITER_INFO;
            DEBUG("FileReader find done for - " + pattern);
            return matches;
        }
    };

    std::vector<FileReader::MatchResult> FileReader::findIn(const std::string& text,
        std::string pattern,
        bool regex,
        uint32_t opt_compile) {

        DEBUG("FileReader findIn  for - " << pattern << " over size - " << text.length());
        FileReader fr({ text });

        auto matches = fr.find(pattern, regex, opt_compile);

        DEBUG("FileReader findIn done  for - " << pattern << " over size - " << text.length());
        return matches;
    };

    std::vector<FileReader::MatchResult> FileReader::findWith(pcre2_code* re,
        uint32_t opt_match) {

        DEBUG("FileReader findWith");
        std::vector<MatchResult> matches;
        STORE_ITER_INFO;
        for (auto block = next(); block.cont && block.size != 0; block = next()) {

            PCRE2_SPTR subject = (PCRE2_SPTR)block.cont;
            PCRE2_SIZE subject_length = block.size;
            PCRE2_SIZE* ovector;

            pcre2_match_data* match_data;
            match_data = pcre2_match_data_create_from_pattern(re, NULL);

            int rc = 0;
            PCRE2_SIZE startOffset = 0;
            while (true) {
                rc = pcre2_match(re, subject, subject_length, startOffset, opt_match, match_data,
                    NULL);

                if (rc == PCRE2_ERROR_NOMATCH)
                    break;

                if (rc < 0) {
                    PCRE2_UCHAR buffer[256];
                    int len = pcre2_get_error_message(rc, buffer, sizeof(buffer));

                    if (len > 0) {
                        LERROR("PCRE2 error: " << buffer);
                    }
                    else {
                        LERROR("Unknown PCRE2 error: " << rc);
                    }
                    pcre2_match_data_free(match_data);
                    throw std::runtime_error("PCRE2 match error");
                }
                ovector = pcre2_get_ovector_pointer(match_data);

                MatchResult match;
                TSRange range;

                range.start_byte = static_cast<uint32_t>(ovector[0]);
                range.end_byte = static_cast<uint32_t>(ovector[1]);

                range.start_point = getP(range.start_byte);
                range.end_point = getP(range.end_byte);

                match.match = range;

                for (int i = 1; i < rc; i++) {
                    PCRE2_SIZE start = ovector[2 * i];
                    PCRE2_SIZE end = ovector[2 * i + 1];

                    if (start == PCRE2_UNSET || end == PCRE2_UNSET)
                        continue;

                    TSRange capture = _makeRange(start, end, rowOffsets);
                    match.captures.push_back(capture);
                }

                startOffset = ovector[1];
                if (ovector[0] == ovector[1]) { // 0 length matches can exist
                    if (startOffset < subject_length)
                        startOffset++;
                    else
                        break;
                }

                if (startOffset >= subject_length)
                    break;

                matches.push_back(match);
            };

            pcre2_match_data_free(match_data);
        }
        RESTORE_ITER_INFO;

        DEBUG("FileReader findWith done");
        return matches;
    };

    FileSnapshot FileReader::snapshot() {

        DEBUG("FileReader snapshot");
        FileSnapshot snap = {};

        snap.dirty = false;
        snap.file = file;
        if (file.isValid) {
            snap.file.sync();
            fs::file_time_type mtimCurr = snap.file.dir_entry.last_write_time();
            snap.lastModified = mtimCurr.time_since_epoch().count();

            fs::file_time_type mtimOld = file.dir_entry.last_write_time();
            size_t selfLastModified = mtimOld.time_since_epoch().count();

            if (selfLastModified < snap.lastModified) {
                sync();
            }
        }

        STORE_ITER_INFO;
        while (next().cont != nullptr) {
            // load all remaining blocks
            DEBUG_FULL("FileReader snapshot Loaded block");
        }
        RESTORE_ITER_INFO;

        UPDATE_ROW_OFFSETS(buf, bufSize);
        snap.cont = std::string(buf.data(), bufSize);
        snap.rowOffsets = rowOffsets;
        return snap;
    }

    FileReader::block FileReader::next() {
        size_t fileEnd = snapshotMode ? bufSize : file.size;

        if (pos >= fileEnd || fileEnd == 0) {
            return { nullptr, 0 };
        }

        DEBUG_FULL("FileReader next - \n" << file.pathStr);

        size_t currentBlockSize = 0;

        if (fileEnd - pos < defaultBlockSize) {
            currentBlockSize = fileEnd - pos;
        }
        else {
            currentBlockSize = defaultBlockSize;
        }

        if (pos < bufStart || pos + currentBlockSize > bufSize) {
            load(pos, pos + currentBlockSize);
            bufStart = pos;
        }

        char* currPtr = &buf.data()[pos];

        if (readReverse) {
            pos = (pos >= currentBlockSize) ? pos - currentBlockSize : 0;
        }
        else {
            pos += currentBlockSize;
        }

        return { currPtr, currentBlockSize };
    };

    FileReader::block FileReader::prev() {
        size_t fileEnd = snapshotMode ? bufSize : file.size;

        if (pos <= 0 || fileEnd == 0) {
            return { nullptr, 0 };
        }

        DEBUG_FULL("FileReader prev - \n" << file.pathStr);

        size_t currentBlockSize = std::min(FileReader::defaultBlockSize, pos);

        if (pos < bufStart || pos + currentBlockSize > bufSize) {
            load(pos, pos + currentBlockSize);
            bufStart = pos;
        }

        char* currPtr = &buf.data()[pos];

        if (readReverse) {
            if (pos < fileEnd - 1) {
                pos += currentBlockSize;
            }
        }
        else if (pos > 0) {
            pos -= currentBlockSize;
        }

        return { currPtr, currentBlockSize };
    };

    void FileReader::reset() {
        DEBUG("FileReader reset");
        bufStart = 0;
        if (readReverse) {
            pos = file.size;
        }
        else {
            pos = 0;
        }
    };

    FileReader::~FileReader() {
        DEBUG_FULL("FileReader destroyed");
    };

    // FileWriter

    const std::vector<size_t>& FileWriter::getRowOffsets() {
        UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());
        return rowOffsets;
    }

    FileWriter::FileWriter(const FileSnapshot snap) {
        DEBUG_FULL("FileWriter ctor with snap");
        this->snap = snap;
        file = snap.file;
        _isValid = file.isValid;
        rowOffsetsValid = !snap.rowOffsets.empty();
        rowOffsets = snap.rowOffsets;
    };

    FileWriter::FileWriter(std::string path) {
        DEBUG_FULL("FileWriter ctor with path");
        FileReader tmp(path);
        snap = tmp.snapshot();
        file = tmp.getFile();
        rowOffsets = tmp.getRowOffsets();
        _isValid = file.isValid;
    }

    FileWriter::FileWriter(File f) {
        DEBUG_FULL("FileWriter ctor with file");
        FileReader tmp(f);
        if (tmp.bufSize != tmp.getFile().size) {
            tmp.sync();
        }
        snap = tmp.snapshot();
        file = tmp.getFile();
        rowOffsets = tmp.getRowOffsets();
        _isValid = file.isValid;
    }

    FileWriter::FileWriter(const FileWriter& copy) {
        DEBUG_FULL("FileWriter copy ctor");
        snap = copy.snap;
        file = copy.file;
        rowOffsets = copy.rowOffsets;
        _isValid = copy.file.isValid;
    };

    FileWriter::~FileWriter() {
        DEBUG_FULL("FileReader destroyed");
        if (oFileStream.is_open())
            oFileStream.close();
    };

    bool FileWriter::backup(const std::string& suffix) {
        std::string bkpPath;
        bkpPath = file.pathStr + suffix;
        if (fs::exists(bkpPath)) {
            bkpPath =
                file.pathStr + "." + std::to_string(snap.lastModified) + suffix;
        }
        INFO("FileWriter backup - \n" << bkpPath);
        bool ok = writeTo(bkpPath);
        if (ok) {
            snap.dirty = false;
        }
        return ok;
    };

    TSPoint FileWriter::getP(size_t byteOffset) {
        return copypasta::_getP(byteOffset, rowOffsets);
    };

    bool FileWriter::save() {
        INFO("FileWriter save - \n" << file.pathStr);
        std::ofstream bkp =
            std::ofstream(file.pathStr, std::ios::out | std::ios::trunc);
        snap.cont.shrink_to_fit();
        bkp << snap.cont;

        bkp.flush();
        bool res = bkp.good();
        bkp.close();
        file.sync();
        if (res) {
            snap.dirty = false;
        }
        return res;
    };

    bool FileWriter::writeTo(const std::string& path) {
        INFO("FileWriter write to " << path);
        std::ofstream target = std::ofstream(path, std::ios::out | std::ios::trunc);
        target << snap.cont;
        target.flush();
        bool res = target.good();
        target.close();
        return res;
    };

#define UPDATE_SNAP_META(snap)                                                 \
  DEBUG("FileWriter update meta");                                             \
  snap.dirty = true;                                                           \
  snap.lastModified =                                                          \
      std::chrono::system_clock::now().time_since_epoch().count();             \
  snap.file.size = snap.cont.length();                                         \
  rowOffsetsValid = false;                                                     \
  return *this;

    FileWriter& FileWriter::copy(std::string& sourcePath) {
        if (!fs::exists(sourcePath)) {
            throw std::invalid_argument(
                "path to source file does not exist for: copy path-" + sourcePath);
        }
        DEBUG_FULL("FileWriter copy ctor");
        FileReader tmp(sourcePath);
        File curr = snap.file;
        snap = tmp.snapshot();
        snap.file = curr;
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::append(const std::string& cont) {
        DEBUG("FileWriter append");
        snap.cont.append(cont);
        UPDATE_SNAP_META(snap);
    }

    FileWriter& FileWriter::insert(size_t offset, const std::string& slice) {
        assert(offset < snap.cont.size());
        DEBUG("FileWriter insert at - " << offset);
        snap.cont.insert(offset, slice);
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::write(const std::string& content) {
        DEBUG("FileWriter write content");
        snap.cont = std::string(content);
        UPDATE_SNAP_META(snap);
    }

    FileWriter& FileWriter::write(size_t offset, char* newCont, size_t newContLen) {
        assert(offset < snap.cont.size());
        DEBUG("FileWriter write offset - " << offset);
        snap.cont.erase(offset, newContLen);
        snap.cont.insert(offset, newCont, newContLen);
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::write(size_t offset, std::string& cont) {
        assert(offset < snap.cont.size());
        DEBUG("FileWriter write offset - " << offset);
        snap.cont.erase(offset, cont.length());
        snap.cont.insert(offset, cont);
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::write(size_t from, size_t to, std::string& cont) {
        assert(to < snap.cont.size());
        DEBUG("FileWriter write from - " << from << " to - " << to);
        snap.cont.erase(from, to - from);
        snap.cont.insert(from, cont);
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::deleteCont(size_t from, size_t to) {
        assert(to < snap.cont.size());
        DEBUG("FileWriter delete " << from << " to " << to);
        snap.cont.erase(from, to - from);
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::deleteRow(size_t row) {
        UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());

        assert(rowOffsets.size() > row);

        DEBUG("FileWriter deleteRow - " << row);

        size_t row1Offset = rowOffsets[row];
        size_t row2Offset = rowOffsets[row + 1];

        snap.cont.erase(row1Offset, row1Offset - row2Offset);
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::insertRowBefore(size_t row, const std::string& cont, bool preserveIndent) {
        UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());

        assert(rowOffsets.size() > row);

        DEBUG("FileWriter insertRow - " << row);

        bool hasEndl = cont[cont.length() - 1] == '\n';
        size_t rowOffset = rowOffsets[row];

        std::string indent = "";

        if (preserveIndent) {
            std::string line = snap.cont.substr(rowOffset, rowOffset - rowOffsets[row + 1] + 1);
            int end = 0;
            for (int i = 0; i < line.length(); i++) {
                auto ch = line[i];
                if (!(ch == ' ' || ch == '\t')) {
                    end = i;
                    break;
                }
            }
            indent = line.substr(0, end);
        }

        snap.cont.insert(rowOffset, indent + cont);
        if (!hasEndl)
            snap.cont.insert(rowOffset + indent.length() + cont.length(), 1, '\n');
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::insertRowAfter(size_t row, const std::string& cont, bool preserveIndent) {
        return insertRowBefore(row + 1, cont, preserveIndent);
    }
    FileWriter& FileWriter::replaceAll(const std::string& pattern,
        const std::string& templateOrResult, uint32_t opt) {

        DEBUG("FileWriter replaceAll start - " << pattern << " to " << templateOrResult);
        pcre2_code* re = PcreCache::global().get(pattern, opt);

        PCRE2_SIZE outLength = snap.cont.length() * 2;

        if (outLength == 0) {
            return *this;
        }

        std::vector<PCRE2_UCHAR> buffer;
        int rc = -1;
    substitute:
        buffer.resize(outLength);
        // expands the template with captures and replcaes match
        rc = pcre2_substitute(
            re,
            (PCRE2_SPTR)snap.cont.c_str(), // in buf
            snap.cont.length(),            // in len
            0,                              // startOffset 
            opt,                            // options
            nullptr,                        // matchData
            nullptr,                        // matchContext
            (PCRE2_SPTR)templateOrResult.c_str(), // replacement
            templateOrResult.length(),           // rlegth
            (PCRE2_UCHAR*)buffer.data(),        // out buf
            &outLength);                        // out len

        if (rc == PCRE2_ERROR_NOMEMORY) {
            LERROR("FileReader replaceAll PCR2_ERROR_NOMEMORY - " << outLength);
            goto substitute;
        }

        pcre2_code_free(re);

        if (rc < 0) {
            throw std::runtime_error("PCRE2 substitution failed");
        }

        snap.cont.assign(reinterpret_cast<char*>(buffer.data()), outLength);

        DEBUG("FileWriter replaceAll done - " << pattern << " to " << templateOrResult);
        UPDATE_SNAP_META(snap);
    };

    FileWriter& FileWriter::replace(const std::string& pattern,
        const std::string& templateOrResult,
        size_t nth,
        uint32_t opt) {

        DEBUG("FileWriter replace start - " << pattern << " to " << templateOrResult);
        FileReader snapReader(snap);

        pcre2_code* re = PcreCache::global().get(pattern, opt);

        auto results = snapReader.findWith(re, true);

        if (results.empty()) {
            return *this;
        }

        // (a%b + b)%b for -10 % 100 = 90 && 10 % 100 = 10
        nth = (nth % results.size() + results.size()) % results.size();
        auto target = results[nth];

        size_t start_offset = target.match.start_byte;
        size_t end_offset = target.match.end_byte;

        PCRE2_SIZE outLength = templateOrResult.length() * 2;

        if (outLength == 0) {
            return *this;
        }

        std::vector<PCRE2_UCHAR> buffer;
        int rc = -1;
    substitute:
        buffer.resize(outLength);
        rc = pcre2_substitute(re,
            (PCRE2_SPTR)(snap.cont.c_str() + start_offset),
            end_offset - start_offset,
            0,
            opt,
            nullptr,
            nullptr,
            (PCRE2_SPTR)templateOrResult.c_str(),
            templateOrResult.length(),
            (PCRE2_UCHAR*)buffer.data(),
            &outLength);

        if (rc == PCRE2_ERROR_NOMEMORY) {
            LERROR("FileReader replace PCR2_ERROR_NOMEMORY - " << outLength);
            goto substitute;
        }

        pcre2_code_free(re);

        if (rc < 0) {
            throw std::runtime_error("PCRE2 substitution failed");
        }

        DEBUG("FileWriter replace done - " << pattern << " to " << templateOrResult);
        return write(start_offset, reinterpret_cast<char*>(buffer.data()),
            outLength);
    };

} // namespace copypasta
