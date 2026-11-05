// Fixture-local shim header (host fixtures ONLY — never part of the Switch build).
// Declares just the minizip unz* subset self_updater.cpp uses, with the same names,
// constants and signatures as the real <minizip/unzip.h> that devkitPro's portlibs
// ship. The host has zlib but no minizip, so the runner puts this directory on the
// include path and minizip_shim.cpp implements the subset over zlib's inflate plus
// a minimal central-directory reader.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void* unzFile;
typedef unsigned long uLong;

#define UNZ_OK                          (0)
#define UNZ_END_OF_LIST_OF_FILE         (-100)
#define UNZ_ERRNO                       (-1)
#define UNZ_PARAMERROR                  (-102)
#define UNZ_BADZIPFILE                  (-103)
#define UNZ_INTERNALERROR               (-104)
#define UNZ_CRCERROR                    (-105)

typedef struct unz_file_info64_s {
    uLong version;
    uLong version_needed;
    uLong flag;
    uLong compression_method;
    uLong dos_date;
    uLong crc;
    unsigned long long compressed_size;
    unsigned long long uncompressed_size;
    uLong size_filename;
    uLong size_file_extra;
    uLong size_file_comment;
    uLong disk_num_start;
    uLong internal_fa;
    uLong external_fa;
} unz_file_info64;

unzFile unzOpen(const char* path);
int unzClose(unzFile file);
int unzGoToFirstFile(unzFile file);
int unzGoToNextFile(unzFile file);
int unzGetCurrentFileInfo64(unzFile file, unz_file_info64* pfile_info,
                            char* szFileName, uLong fileNameBufferSize,
                            void* extraField, uLong extraFieldBufferSize,
                            char* szComment, uLong commentBufferSize);
int unzOpenCurrentFile(unzFile file);
int unzReadCurrentFile(unzFile file, void* buf, unsigned len);
int unzCloseCurrentFile(unzFile file);

#ifdef __cplusplus
}
#endif
