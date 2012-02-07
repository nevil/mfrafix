#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

#define FOURCC(c1, c2, c3, c4) \
    (c1 << 24 | c2 << 16 | c3 << 8 | c4)

#define SKIP(x, c) (x += c)

typedef struct
{
    uint32_t trackId;
    uint64_t time;
    uint64_t moof;
    uint64_t moofOffset;
} TFRAEntry;

#define DEBUG
void debug(const char* format, ...)
{
#ifdef DEBUG
    va_list args;
    va_start(args, format);

    vfprintf(stdout, format, args);
    va_end(args);
#endif
}

static uint32_t U32_AT(const uint8_t *ptr) {
    return ptr[0] << 24 | ptr[1] << 16 | ptr[2] << 8 | ptr[3];
}

static uint64_t U64_AT(const uint8_t *ptr) {
    return ((uint64_t)U32_AT(ptr)) << 32 | U32_AT(ptr + 4);
}

static uint32_t U32LE_AT(const uint8_t *ptr) {
    return ptr[3] << 24 | ptr[2] << 16 | ptr[1] << 8 | ptr[0];
}

static uint64_t TO_U64(uint64_t v)
{
    uint8_t* ptr = (uint8_t*)&v;
    return ((uint64_t)U32_AT(ptr)) << 32 | U32_AT(ptr + 4);
}

int main(int argc, char* argv[])
{
    int retval = 1;
    int fdout = -1;
    uint8_t* pIn  = NULL;
    uint8_t* pOut = NULL;

    if (argc < 3 || argc > 3)
    {
        fprintf(stderr, "Usage: mfrafix infile outfile\n");
        return 1;
    }

    int fdin = open(argv[1], O_RDONLY);
    if (fdin < 0)
    {
        fprintf(stderr, "Error: failed to open '%s' for reading\n", argv[1]);
        goto cleanup;
    }

    off_t length = lseek(fdin, 0, SEEK_END);

    lseek(fdin, 0, SEEK_SET);
    debug("Length of in file: %d\n", (int)length);

    pIn = mmap(NULL, length, PROT_READ, MAP_SHARED, fdin, 0);
    if (pIn == NULL)
    {
        fprintf(stderr, "Error: mmap failed with error %d - '%s'\n", errno, strerror(errno));
        goto cleanup;
    }

    // Look for the mfra by checking the last four bytes in the mfro
    // For now we assume that the mfra is at the end of the file...
    uint32_t mfro = U32_AT(pIn + length - sizeof(uint32_t));
    uint8_t* pMFRA = pIn + length - mfro;
    uint32_t mfraSize = U32_AT(pMFRA);
    uint8_t* pMFRAEnd = pMFRA + mfraSize;
    SKIP(pMFRA, 4); // Size processed

    debug("mfro offset %x\n", mfro);
    debug("MFRA size %x\n", mfraSize);
    debug("MFRA box '%08x'\n", U32_AT(pMFRA));

    if (FOURCC('m', 'f', 'r', 'a') != U32_AT(pMFRA))
    {
        fprintf(stderr, "Error: The mfra box was not found\n");
        goto cleanup;
    }
    SKIP(pMFRA, 4); // Type processed

    debug("Found mfra\n");

    TFRAEntry* tfraEntries = NULL;
    int numTFRAEntries = 0;

    while (pMFRA < pMFRAEnd)
    {
        uint32_t boxSize = U32_AT(pMFRA);
        SKIP(pMFRA, 4); // Size read

        debug("Box size %x\n", boxSize);
        debug("Box type '%08x'\n", U32_AT(pMFRA));
        switch (U32_AT(pMFRA))
        {
            case FOURCC('t', 'f', 'r', 'a'):
                debug("Found tfra\n");
                SKIP(pMFRA, 4); // Type processed
                SKIP(pMFRA, 4); // Ignore version and flags

                uint32_t trackId = U32_AT(pMFRA);
                SKIP(pMFRA, 4); // trackId processed

                uint32_t tmp = U32LE_AT(pMFRA);
                int trafNum   = (tmp >> 4) & 0x3;
                int trunNum   = (tmp >> 2) & 0x3;
                int sampleNum = (tmp >> 0) & 0x3;
                SKIP(pMFRA, 4); // Traf, trun and samples processed
                debug("trackId %d\ntraf %d trun %d sample %d\n", trackId, trafNum, trunNum, sampleNum);

                uint32_t numEntry = U32_AT(pMFRA);
                SKIP(pMFRA, 4); // numEntry processed
                debug("numEntry: %d\n", numEntry);

                tfraEntries = realloc(tfraEntries, sizeof(TFRAEntry) * (numEntry + numTFRAEntries));
                if (tfraEntries == NULL)
                {
                    fprintf(stderr, "Error: Failed to allocate memory for tfra entries\n");
                    goto cleanup;
                }

                for (int i = numTFRAEntries; i < (numEntry + numTFRAEntries); ++i)
                {
                    tfraEntries[i].trackId = trackId;

                    debug("time: %lx\n", U64_AT(pMFRA));
                    tfraEntries[i].time = U64_AT(pMFRA);
                    SKIP(pMFRA, 8); // Skip uint64, time

                    debug("moof: %lx\n", U64_AT(pMFRA));
                    tfraEntries[i].moof = U64_AT(pMFRA);
                    tfraEntries[i].moofOffset = (pMFRA - pIn);
                    SKIP(pMFRA, 8); // Skip uint64, moof offset

                    int skip = trafNum + 1 + trunNum + 1 + sampleNum + 1;
                    pMFRA = pMFRA + skip;
                }

                // Update here as we use numTFRAEntries as offset when indexing the array
                numTFRAEntries += numEntry; 

                break;
            case FOURCC('m', 'f', 'r', 'o'):
                debug("found mfro box\n");
                pMFRA = pMFRA + boxSize - sizeof(uint32_t);
                break;

            default:
                fprintf(stderr, "Error: Unknown box found '%08x'\n", U32_AT(pMFRA));
                goto cleanup;
        }

    }

    if (numTFRAEntries == 0)
    {
        fprintf(stderr, "Error: No TFRA entries found.\n");
        goto cleanup;
    }

    // Start by checking if the first entry points to a moof
    uint8_t* p1 = pIn + tfraEntries[0].moof;
    if (FOURCC('m', 'o', 'o', 'f') != U32_AT(p1 + 4))
    {
        uint32_t offset = U32_AT(p1);
        fprintf(stdout, "We found '%08x' instead of moof, try to correct with offset %x\n", U32_AT(p1 + 4), offset);

        for (int i = 0; i < numTFRAEntries; ++i)
        {
            debug("p = %x\n", tfraEntries[i].moof);
            uint8_t *p = pIn + tfraEntries[i].moof + offset;

            if (U32_AT(p + 4) == FOURCC('m', 'o', 'o', 'f'))
                debug("Found moof\n");
            else
            {
                debug("Found '%08x'\n", U32_AT(p + 4));
                goto cleanup;
            }
        }

        debug("Open output file\n");

        fdout = open(argv[2], O_RDWR | O_CREAT | O_TRUNC);
        if (fdout < 0)
        {
            fprintf(stderr, "Error: failed to open '%s' for writing\n", argv[2]);
            goto cleanup;
        }

        debug("Set size of out file\n");

        lseek(fdout, length - 1, SEEK_SET);
        write(fdout, "", 1);

        debug("Call mmap for output file\n");

        pOut = mmap(NULL, length, PROT_WRITE | PROT_READ, MAP_SHARED, fdout, 0);
        if (pOut == NULL)
        {
            fprintf(stderr, "Error: mmap failed with error %d - '%s'\n", errno, strerror(errno));
            goto cleanup;
        }

        debug("Do memcpy\n");
        memcpy(pOut, pIn, length);

        debug("Update moof references\n");
        for (int i = 0; i < numTFRAEntries; ++i)
        {
            debug("p = %x\n", tfraEntries[i].moof);
            debug("offset = %x\n", offset);
            debug("out = %x\n", tfraEntries[i].moof + offset);
            uint64_t *p = (uint64_t*)(pOut + tfraEntries[i].moofOffset);
            (*p) = TO_U64(tfraEntries[i].moof + offset);
        }

        debug("Do msync\n");
        msync(pOut, length, MS_SYNC);
        fprintf(stdout, "Fixed all entries...\n");
    }
    else
    {
        fprintf(stdout, "The first moof reference is correct, assume the rest are as well and exit\n");
    }

    retval = 0;

cleanup:
    if (tfraEntries != NULL)
        free(tfraEntries);

    if (fdout >= 0)
        close(fdout);

    if (pOut != NULL)
        munmap(pOut, length);

    if (pIn != NULL)
        munmap(pIn, length);

    if (fdin >= 0)
        close(fdin);

    return retval;
}

