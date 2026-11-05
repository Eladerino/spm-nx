// Fixture-local minizip shim (host fixtures ONLY). Implements the unz* subset the
// self-updater core uses, backed by zlib inflate and a minimal central-directory
// reader. Not a general minizip replacement: single-disk, no encryption, methods
// 0 (stored) and 8 (deflate) only — everything python3's zipfile emits for the
// fixture archives. CRC is verified at unzCloseCurrentFile like the real API.
#include "minizip/unzip.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

struct Entry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t comp_size = 0;
    uint64_t uncomp_size = 0;
    uint64_t local_offset = 0;
};

struct Zip {
    std::vector<unsigned char> data;
    std::vector<Entry> entries;
    size_t current = 0;
    bool entry_open = false;
    std::vector<unsigned char> plain;
    size_t plain_pos = 0;
};

uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

Zip* parse_zip(std::vector<unsigned char> data) {
    if (data.size() < 22) return nullptr;
    // End Of Central Directory: scan backwards for 0x06054b50.
    size_t eocd = SIZE_MAX;
    size_t scan = data.size() - 22;
    size_t floor = data.size() > 65557 ? data.size() - 65557 : 0;
    for (size_t i = scan + 1; i-- > floor;) {
        if (rd32(&data[i]) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) return nullptr;
    uint16_t count = rd16(&data[eocd + 10]);
    uint32_t cd_off = rd32(&data[eocd + 16]);
    auto* zip = new Zip();
    zip->data = std::move(data);
    size_t p = cd_off;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > zip->data.size() || rd32(&zip->data[p]) != 0x02014b50u) { delete zip; return nullptr; }
        const unsigned char* h = &zip->data[p];
        Entry e;
        e.method = rd16(h + 10);
        e.crc = rd32(h + 16);
        e.comp_size = rd32(h + 20);
        e.uncomp_size = rd32(h + 24);
        uint16_t nlen = rd16(h + 28), elen = rd16(h + 30), clen = rd16(h + 32);
        e.local_offset = rd32(h + 42);
        if (p + 46 + nlen > zip->data.size()) { delete zip; return nullptr; }
        e.name.assign(reinterpret_cast<const char*>(h + 46), nlen);
        zip->entries.push_back(std::move(e));
        p += 46 + nlen + elen + clen;
    }
    return zip;
}

} // namespace

extern "C" {

unzFile unzOpen(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return nullptr; }
    std::vector<unsigned char> data((size_t)sz);
    if (sz > 0 && std::fread(data.data(), 1, (size_t)sz, f) != (size_t)sz) { std::fclose(f); return nullptr; }
    std::fclose(f);
    return parse_zip(std::move(data));
}

int unzClose(unzFile file) {
    if (!file) return UNZ_PARAMERROR;
    delete static_cast<Zip*>(file);
    return UNZ_OK;
}

int unzGoToFirstFile(unzFile file) {
    if (!file) return UNZ_PARAMERROR;
    auto* z = static_cast<Zip*>(file);
    if (z->entries.empty()) return UNZ_END_OF_LIST_OF_FILE;
    z->current = 0;
    z->entry_open = false;
    return UNZ_OK;
}

int unzGoToNextFile(unzFile file) {
    if (!file) return UNZ_PARAMERROR;
    auto* z = static_cast<Zip*>(file);
    if (z->current + 1 >= z->entries.size()) return UNZ_END_OF_LIST_OF_FILE;
    ++z->current;
    z->entry_open = false;
    return UNZ_OK;
}

int unzGetCurrentFileInfo64(unzFile file, unz_file_info64* info, char* name, uLong name_cap,
                            void*, uLong, char*, uLong) {
    if (!file) return UNZ_PARAMERROR;
    auto* z = static_cast<Zip*>(file);
    if (z->current >= z->entries.size()) return UNZ_PARAMERROR;
    const Entry& e = z->entries[z->current];
    if (info) {
        std::memset(info, 0, sizeof *info);
        info->compression_method = e.method;
        info->crc = e.crc;
        info->compressed_size = e.comp_size;
        info->uncompressed_size = e.uncomp_size;
        info->size_filename = (uLong)e.name.size();
    }
    if (name && name_cap > 0) {
        size_t n = e.name.size(); if (n > name_cap - 1) n = name_cap - 1;
        std::memcpy(name, e.name.data(), n);
        name[n] = '\0';
    }
    return UNZ_OK;
}

int unzOpenCurrentFile(unzFile file) {
    if (!file) return UNZ_PARAMERROR;
    auto* z = static_cast<Zip*>(file);
    if (z->current >= z->entries.size()) return UNZ_PARAMERROR;
    const Entry& e = z->entries[z->current];
    size_t off = (size_t)e.local_offset;
    if (off + 30 > z->data.size() || rd32(&z->data[off]) != 0x04034b50u) return UNZ_BADZIPFILE;
    uint16_t nlen = rd16(&z->data[off + 26]), elen = rd16(&z->data[off + 28]);
    size_t data_off = off + 30 + nlen + elen;
    if (data_off + e.comp_size > z->data.size()) return UNZ_BADZIPFILE;
    const unsigned char* comp = &z->data[data_off];
    z->plain.assign((size_t)e.uncomp_size, 0);
    if (e.method == 0) {
        if (e.comp_size != e.uncomp_size) return UNZ_BADZIPFILE;
        std::memcpy(z->plain.data(), comp, (size_t)e.comp_size);
    } else if (e.method == 8) {
        z_stream zs{};
        zs.next_in = const_cast<Bytef*>(comp);
        zs.avail_in = (uInt)e.comp_size;
        zs.next_out = z->plain.data();
        zs.avail_out = (uInt)e.uncomp_size;
        if (inflateInit2(&zs, -15) != Z_OK) return UNZ_INTERNALERROR;
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END && rc != Z_OK) return UNZ_INTERNALERROR;
        if (zs.total_out != e.uncomp_size) return UNZ_INTERNALERROR;
    } else {
        return UNZ_INTERNALERROR;
    }
    z->plain_pos = 0;
    z->entry_open = true;
    return UNZ_OK;
}

int unzReadCurrentFile(unzFile file, void* buf, unsigned len) {
    if (!file) return UNZ_PARAMERROR;
    auto* z = static_cast<Zip*>(file);
    if (!z->entry_open) return UNZ_PARAMERROR;
    size_t left = z->plain.size() - z->plain_pos;
    size_t n = left < len ? left : len;
    std::memcpy(buf, z->plain.data() + z->plain_pos, n);
    z->plain_pos += n;
    return (int)n;
}

int unzCloseCurrentFile(unzFile file) {
    if (!file) return UNZ_PARAMERROR;
    auto* z = static_cast<Zip*>(file);
    if (!z->entry_open) return UNZ_PARAMERROR;
    z->entry_open = false;
    const Entry& e = z->entries[z->current];
    uLong crc = crc32(0L, Z_NULL, 0);
    if (!z->plain.empty()) crc = crc32(crc, z->plain.data(), (uInt)z->plain.size());
    return crc == e.crc ? UNZ_OK : UNZ_CRCERROR;
}

} // extern "C"
