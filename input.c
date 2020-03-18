/*
 *
 * honggfuzz - file operations
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2018 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fuzz.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"
#include "mangle.h"
#include "subproc.h"

void input_setSize(run_t* run, size_t sz) {
    if (run->dynamicFileSz == sz) {
        return;
    }
    if (sz > run->global->mutate.maxInputSz) {
        PLOG_F("Too large size requested: %zu > maxSize: %zu", sz, run->global->mutate.maxInputSz);
    }
    /* ftruncate of a mmaped file fails under CygWin, it's also painfully slow under MacOS X */
#if !defined(__CYGWIN__) && !defined(_HF_ARCH_DARWIN)
    if (TEMP_FAILURE_RETRY(ftruncate(run->dynamicFileFd, sz)) == -1) {
        PLOG_W("ftruncate(run->dynamicFileFd=%d, sz=%zu)", run->dynamicFileFd, sz);
    }
#endif /* !defined(__CYGWIN__) && !defined(_HF_ARCH_DARWIN) */
    run->dynamicFileSz = sz;
}

static bool input_getDirStatsAndRewind(honggfuzz_t* hfuzz) {
    rewinddir(hfuzz->io.inputDirPtr);

    size_t fileCnt = 0U;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(hfuzz->io.inputDirPtr);
        if (entry == NULL && errno == EINTR) {
            continue;
        }
        if (entry == NULL && errno != 0) {
            PLOG_W("readdir('%s')", hfuzz->io.inputDir);
            return false;
        }
        if (entry == NULL) {
            break;
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", hfuzz->io.inputDir, entry->d_name);

        LOG_D("Analyzing file '%s'", path);

        struct stat st;
        if (stat(path, &st) == -1) {
            LOG_W("Couldn't stat() the '%s' file", path);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            LOG_D("'%s' is not a regular file, skipping", path);
            continue;
        }
        if (hfuzz->io.maxFileSz && st.st_size > (off_t)hfuzz->io.maxFileSz) {
            LOG_D("File '%s' is bigger than maximal defined file size (-F): %" PRIu64 " > %zu",
                path, (uint64_t)st.st_size, hfuzz->io.maxFileSz);
        }
        if ((size_t)st.st_size > hfuzz->mutate.maxInputSz) {
            hfuzz->mutate.maxInputSz = st.st_size;
        }
        fileCnt++;
    }

    ATOMIC_SET(hfuzz->io.fileCnt, fileCnt);
    if (hfuzz->io.maxFileSz) {
        hfuzz->mutate.maxInputSz = hfuzz->io.maxFileSz;
    } else if (hfuzz->mutate.maxInputSz < _HF_INPUT_DEFAULT_SIZE) {
        hfuzz->mutate.maxInputSz = _HF_INPUT_DEFAULT_SIZE;
    } else if (hfuzz->mutate.maxInputSz > _HF_INPUT_MAX_SIZE) {
        hfuzz->mutate.maxInputSz = _HF_INPUT_MAX_SIZE;
    }

    if (hfuzz->io.fileCnt == 0U) {
        LOG_W("No usable files in the input directory '%s'", hfuzz->io.inputDir);
    }

    LOG_D("Analyzed '%s' directory: maxInputSz:%zu, number of usable files:%zu", hfuzz->io.inputDir,
        hfuzz->mutate.maxInputSz, hfuzz->io.fileCnt);

    rewinddir(hfuzz->io.inputDirPtr);

    return true;
}

bool input_getNext(run_t* run, char fname[PATH_MAX], bool rewind) {
    static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
    MX_SCOPED_LOCK(&input_mutex);

    if (run->global->io.fileCnt == 0U) {
        LOG_W("No useful files in the input directory");
        return false;
    }

    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(run->global->io.inputDirPtr);
        if (entry == NULL && errno == EINTR) {
            continue;
        }
        if (entry == NULL && errno != 0) {
            PLOG_W("readdir_r('%s')", run->global->io.inputDir);
            return false;
        }
        if (entry == NULL && rewind == false) {
            return false;
        }
        if (entry == NULL && rewind == true) {
            if (input_getDirStatsAndRewind(run->global) == false) {
                LOG_E("input_getDirStatsAndRewind('%s')", run->global->io.inputDir);
                return false;
            }
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, PATH_MAX, "%s/%s", run->global->io.inputDir, entry->d_name);
        struct stat st;
        if (stat(path, &st) == -1) {
            LOG_W("Couldn't stat() the '%s' file", path);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            LOG_D("'%s' is not a regular file, skipping", path);
            continue;
        }

        snprintf(fname, PATH_MAX, "%s", entry->d_name);
        return true;
    }
}

bool input_init(honggfuzz_t* hfuzz) {
    hfuzz->io.fileCnt = 0U;

    if (!hfuzz->io.inputDir) {
        LOG_W("No input file/dir specified");
        return false;
    }

    int dir_fd = TEMP_FAILURE_RETRY(open(hfuzz->io.inputDir, O_DIRECTORY | O_RDONLY | O_CLOEXEC));
    if (dir_fd == -1) {
        PLOG_W("open('%s', O_DIRECTORY|O_RDONLY|O_CLOEXEC)", hfuzz->io.inputDir);
        return false;
    }
    if ((hfuzz->io.inputDirPtr = fdopendir(dir_fd)) == NULL) {
        PLOG_W("fdopendir(dir='%s', fd=%d)", hfuzz->io.inputDir, dir_fd);
        close(dir_fd);
        return false;
    }
    if (input_getDirStatsAndRewind(hfuzz) == false) {
        hfuzz->io.fileCnt = 0U;
        LOG_W("input_getDirStatsAndRewind('%s')", hfuzz->io.inputDir);
        return false;
    }

    return true;
}

bool input_parseDictionary(honggfuzz_t* hfuzz) {
    LOG_I("Parsing dictionary file '%s'", hfuzz->mutate.dictionaryFile);

    FILE* fDict = fopen(hfuzz->mutate.dictionaryFile, "rb");
    if (fDict == NULL) {
        PLOG_W("Couldn't open '%s' - R/O mode", hfuzz->mutate.dictionaryFile);
        return false;
    }
    defer {
        fclose(fDict);
    };

    char* lineptr = NULL;
    size_t n = 0;
    defer {
        free(lineptr);
    };
    for (;;) {
        ssize_t len = getdelim(&lineptr, &n, '\n', fDict);
        if (len == -1) {
            break;
        }
        if (hfuzz->mutate.dictionaryCnt == ARRAYSIZE(hfuzz->mutate.dictionary)) {
            LOG_W("Maximum number of dictionary entries '%zu' alread loaded. Skipping the rest",
                ARRAYSIZE(hfuzz->mutate.dictionary));
            break;
        }
        if (len > 1 && lineptr[len - 1] == '\n') {
            lineptr[len - 1] = '\0';
            len--;
        }
        if (lineptr[0] == '#') {
            continue;
        }
        if (lineptr[0] == '\n') {
            continue;
        }
        if (lineptr[0] == '\0') {
            continue;
        }

        const char* start = strchr(lineptr, '"');
        char* end = strrchr(lineptr, '"');
        if (!start || !end) {
            LOG_W("Malformed dictionary line '%s', skipping", lineptr);
            continue;
        }
        if ((uintptr_t)start == (uintptr_t)end) {
            LOG_W("Malformed dictionary line '%s', skipping", lineptr);
            continue;
        }
        *end = '\0';

        char bufv[1025] = {};
        if (sscanf(&start[1], "%1024c", bufv) != 1) {
            LOG_W("Malformed dictionary line '%s', skipping", lineptr);
            continue;
        }

        LOG_D("Parsing dictionary word: '%s'", bufv);

        len = util_decodeCString(bufv);
        size_t dictEntry = ATOMIC_POST_INC(hfuzz->mutate.dictionaryCnt);
        len = HF_MIN((size_t)len, sizeof(hfuzz->mutate.dictionary[dictEntry].val));
        memcpy(hfuzz->mutate.dictionary[dictEntry].val, bufv, len);
        hfuzz->mutate.dictionary[dictEntry].len = len;

        LOG_D("Dictionary: loaded word: '%s' (len=%zd)", bufv, len);
    }
    LOG_I("Loaded %zu words from the dictionary '%s'", hfuzz->mutate.dictionaryCnt,
        hfuzz->mutate.dictionaryFile);
    return true;
}

bool input_parseBlacklist(honggfuzz_t* hfuzz) {
    FILE* fBl = fopen(hfuzz->feedback.blacklistFile, "rb");
    if (fBl == NULL) {
        PLOG_W("Couldn't open '%s' - R/O mode", hfuzz->feedback.blacklistFile);
        return false;
    }
    defer {
        fclose(fBl);
    };

    char* lineptr = NULL;
    /* lineptr can be NULL, but it's fine for free() */
    defer {
        free(lineptr);
    };
    size_t n = 0;
    for (;;) {
        if (getline(&lineptr, &n, fBl) == -1) {
            break;
        }

        if ((hfuzz->feedback.blacklist = util_Realloc(hfuzz->feedback.blacklist,
                 (hfuzz->feedback.blacklistCnt + 1) * sizeof(hfuzz->feedback.blacklist[0]))) ==
            NULL) {
            PLOG_W("realloc failed (sz=%zu)",
                (hfuzz->feedback.blacklistCnt + 1) * sizeof(hfuzz->feedback.blacklist[0]));
            return false;
        }

        hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt] = strtoull(lineptr, 0, 16);
        LOG_D("Blacklist: loaded %'" PRIu64 "'",
            hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt]);

        /* Verify entries are sorted so we can use interpolation search */
        if (hfuzz->feedback.blacklistCnt > 1) {
            if (hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt - 1] >
                hfuzz->feedback.blacklist[hfuzz->feedback.blacklistCnt]) {
                LOG_F("Blacklist file not sorted. Use 'tools/createStackBlacklist.sh' to sort "
                      "records");
                return false;
            }
        }
        hfuzz->feedback.blacklistCnt += 1;
    }

    if (hfuzz->feedback.blacklistCnt > 0) {
        LOG_I("Loaded %zu stack hash(es) from the blacklist file", hfuzz->feedback.blacklistCnt);
    } else {
        LOG_F("Empty stack hashes blacklist file '%s'", hfuzz->feedback.blacklistFile);
    }
    return true;
}

bool input_writeCovFile(const char* dir, const uint8_t* data, size_t len) {
    char fname[PATH_MAX];

    uint64_t crc64f = util_CRC64(data, len);
    uint64_t crc64r = util_CRC64Rev(data, len);
    snprintf(fname, sizeof(fname), "%s/%016" PRIx64 "%016" PRIx64 ".%08" PRIx32 ".honggfuzz.cov",
        dir, crc64f, crc64r, (uint32_t)len);

    if (files_exists(fname)) {
        LOG_D("File '%s' already exists in the output corpus directory '%s'", fname, dir);
        return true;
    }

    LOG_D("Adding file '%s' to the corpus directory '%s'", fname, dir);

    if (!files_writeBufToFile(fname, data, len, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC)) {
        LOG_W("Couldn't write buffer to file '%s'", fname);
        return false;
    }

    return true;
}

/* true if item1 is bigger than item2 */
static bool input_cmpCov(struct dynfile_t* item1, struct dynfile_t* item2) {
    for (size_t j = 0; j < ARRAYSIZE(item1->cov); j++) {
        if (item1->cov[j] > item2->cov[j]) {
            return true;
        }
        if (item1->cov[j] < item2->cov[j]) {
            return false;
        }
    }
    /* Both are equal */
    return false;
}

/* Number of tests taken, based on belonging to a percentile bucket */
static size_t input_numTests(size_t idx, size_t total) {
    if (idx > total) {
        LOG_F("idx (%zu) > total (%zu)", idx, total);
    }
    size_t percentile = (idx * 100) / total;
    static size_t const scaleMap[101] = {
        [0 ... 90] = 1,
        [91 ... 92] = 2,
        [93 ... 94] = 3,
        [95 ... 96] = 4,
        [97 ... 98] = 5,
        [99 ... 100] = 10,
    };
    return scaleMap[percentile];
}

#define TAILQ_FOREACH_HF(var, head, field) \
    for ((var) = TAILQ_FIRST((head)); (var); (var) = TAILQ_NEXT((var), field))

void input_addDynamicInput(
    honggfuzz_t* hfuzz, const uint8_t* data, size_t len, uint64_t cov[4], const char* path) {
    ATOMIC_SET(hfuzz->timing.lastCovUpdate, time(NULL));

    struct dynfile_t* dynfile = (struct dynfile_t*)util_Malloc(sizeof(struct dynfile_t) + len);
    for (size_t i = 0; i < ARRAYSIZE(dynfile->cov); i++) {
        dynfile->cov[i] = cov[i];
    }
    dynfile->size = len;
    dynfile->idx = hfuzz->io.dynfileqCnt;
    dynfile->tested = 0;
    memcpy(dynfile->data, data, len);
    snprintf(dynfile->path, sizeof(dynfile->path), "%s", path);

    MX_SCOPED_RWLOCK_WRITE(&hfuzz->io.dynfileq_mutex);

    if (fuzz_getState(hfuzz) == _HF_STATE_DYNAMIC_MAIN) {
        /* Add it in front with high idx, so it's tested next */
        TAILQ_INSERT_HEAD(&hfuzz->io.dynfileq, dynfile, pointers);
        hfuzz->io.dynfileqCurrent = TAILQ_FIRST(&hfuzz->io.dynfileq);
    } else {
        /* Sort it by coverage - put better coverage earlier in the list */
        struct dynfile_t* iter = NULL;
        TAILQ_FOREACH_HF(iter, &hfuzz->io.dynfileq, pointers) {
            if (input_cmpCov(dynfile, iter)) {
                TAILQ_INSERT_BEFORE(iter, dynfile, pointers);
                break;
            }
        }
        if (iter == NULL) {
            TAILQ_INSERT_TAIL(&hfuzz->io.dynfileq, dynfile, pointers);
        }
    }
    hfuzz->io.dynfileqCnt++;
    hfuzz->io.dynfileqMaxSz = HF_MAX(hfuzz->io.dynfileqMaxSz, len);

    if (hfuzz->socketFuzzer.enabled) {
        /* Don't add coverage data to files in socketFuzzer mode */
        return;
    }
    if (hfuzz->cfg.minimize) {
        /* When minimizing we should only delete files */
        return;
    }

    const char* outDir = hfuzz->io.outputDir ? hfuzz->io.outputDir : hfuzz->io.inputDir;
    if (!input_writeCovFile(outDir, data, len)) {
        LOG_E("Couldn't save the coverage data to '%s'", hfuzz->io.outputDir);
    }

    /* No need to add files to the new coverage dir, if it's not the main phase */
    if (fuzz_getState(hfuzz) != _HF_STATE_DYNAMIC_MAIN) {
        return;
    }

    hfuzz->io.newUnitsAdded++;

    if (hfuzz->io.covDirNew && !input_writeCovFile(hfuzz->io.covDirNew, data, len)) {
        LOG_E("Couldn't save the new coverage data to '%s'", hfuzz->io.covDirNew);
    }
}

bool input_prepareDynamicInput(run_t* run, bool needs_mangle) {
    struct dynfile_t* current = NULL;

    if (ATOMIC_GET(run->global->io.dynfileqCnt) == 0) {
        LOG_F("The dynamic file corpus is empty. This shouldn't happen");
    }

    {
        MX_SCOPED_RWLOCK_WRITE(&run->global->io.dynfileq_mutex);

        if (run->global->io.dynfileqCurrent == NULL) {
            run->global->io.dynfileqCurrent = TAILQ_FIRST(&run->global->io.dynfileq);
        }

        current = run->global->io.dynfileqCurrent;

        /* Number of tests per input depends on the 'idx' of the input */
        size_t testCnt = input_numTests(current->idx, run->global->io.dynfileqCnt);
        current->tested++;

        /*
         * Testing routine:
         * LOG_E("IDX: %zu (%zu/%zu)", current->idx, current->tested, testCnt);
         */

        /* If the current sample has been tested enough, move the pointer to the next sample */
        if (current->tested >= testCnt) {
            current->tested = 0;
            run->global->io.dynfileqCurrent = TAILQ_NEXT(run->global->io.dynfileqCurrent, pointers);
        }
    }

    input_setSize(run, current->size);
    memcpy(run->dynamicFile, current->data, current->size);

    if (needs_mangle) {
        mangle_mangleContent(run);
    }

    return true;
}

/* Assign idx to each input based on it's coverage: better coverage -> higher idx */
void input_renumerateInputs(honggfuzz_t* hfuzz) {
    size_t idx = hfuzz->io.dynfileqCnt;
    struct dynfile_t* iter = NULL;
    TAILQ_FOREACH_HF(iter, &hfuzz->io.dynfileq, pointers) {
        iter->idx = idx--;
    }
}

static bool input_shouldReadNewFile(run_t* run) {
    if (fuzz_getState(run->global) != _HF_STATE_DYNAMIC_DRY_RUN || run->global->cfg.minimize) {
        input_setSize(run, run->global->mutate.maxInputSz);
        return true;
    }

    if (!run->staticFileTryMore) {
        run->staticFileTryMore = true;
        /* Start with a 8kB beginning of a file, increase the size in following iterations */
        input_setSize(run, HF_MIN(1024U, run->global->mutate.maxInputSz));
        return true;
    }

    /* Increase size of the current file by a factor of 2, and return it instead of a new file */
    size_t newsz = run->dynamicFileSz * 2;
    if (newsz >= run->global->mutate.maxInputSz) {
        /* That's the largest size for this specific file that will be ever used */
        newsz = run->global->mutate.maxInputSz;
        run->staticFileTryMore = false;
    }

    input_setSize(run, newsz);
    return false;
}

bool input_prepareStaticFile(run_t* run, bool rewind, bool needs_mangle) {
    if (input_shouldReadNewFile(run)) {
        if (!input_getNext(run, run->origFileName, /* rewind= */ rewind)) {
            return false;
        }
        run->global->io.testedFileCnt++;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", run->global->io.inputDir, run->origFileName);

    ssize_t fileSz = files_readFileToBufMax(path, run->dynamicFile, run->dynamicFileSz);
    if (fileSz < 0) {
        LOG_E("Couldn't read contents of '%s'", path);
        return false;
    }

    if (run->staticFileTryMore && ((size_t)fileSz < run->dynamicFileSz)) {
        /* The file is smaller than the requested size, no need to re-read it anymore */
        run->staticFileTryMore = false;
    }

    input_setSize(run, fileSz);

    if (needs_mangle) {
        mangle_mangleContent(run);
    }

    return true;
}

void input_removeStaticFile(const char* dir, const char* name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (unlink(path) == -1) {
        PLOG_E("unlink('%s') failed", path);
    }
}

bool input_prepareExternalFile(run_t* run) {
    snprintf(run->origFileName, sizeof(run->origFileName), "[EXTERNAL]");

    int fd = files_writeBufToTmpFile(run->global->io.workDir, (const uint8_t*)"", 0, 0);
    if (fd == -1) {
        LOG_E("Couldn't write input file to a temporary buffer");
        return false;
    }
    defer {
        close(fd);
    };

    char fname[PATH_MAX];
    snprintf(fname, sizeof(fname), "/dev/fd/%d", fd);

    const char* const argv[] = {run->global->exe.externalCommand, fname, NULL};
    if (subproc_System(run, argv) != 0) {
        LOG_E("Subprocess '%s' returned abnormally", run->global->exe.externalCommand);
        return false;
    }
    LOG_D("Subporcess '%s' finished with success", run->global->exe.externalCommand);

    input_setSize(run, run->global->mutate.maxInputSz);
    ssize_t sz = files_readFromFdSeek(fd, run->dynamicFile, run->global->mutate.maxInputSz, 0);
    if (sz == -1) {
        LOG_E("Couldn't read file from fd=%d", fd);
        return false;
    }

    input_setSize(run, (size_t)sz);
    return true;
}

bool input_postProcessFile(run_t* run, const char* cmd) {
    int fd =
        files_writeBufToTmpFile(run->global->io.workDir, run->dynamicFile, run->dynamicFileSz, 0);
    if (fd == -1) {
        LOG_E("Couldn't write input file to a temporary buffer");
        return false;
    }
    defer {
        close(fd);
    };

    char fname[PATH_MAX];
    snprintf(fname, sizeof(fname), "/dev/fd/%d", fd);

    const char* const argv[] = {cmd, fname, NULL};
    if (subproc_System(run, argv) != 0) {
        LOG_E("Subprocess '%s' returned abnormally", cmd);
        return false;
    }
    LOG_D("Subporcess '%s' finished with success", cmd);

    input_setSize(run, run->global->mutate.maxInputSz);
    ssize_t sz = files_readFromFdSeek(fd, run->dynamicFile, run->global->mutate.maxInputSz, 0);
    if (sz == -1) {
        LOG_E("Couldn't read file from fd=%d", fd);
        return false;
    }

    input_setSize(run, (size_t)sz);
    return true;
}

bool input_prepareDynamicFileForMinimization(run_t* run) {
    MX_SCOPED_RWLOCK_WRITE(&run->global->io.dynfileq_mutex);

    if (run->global->io.dynfileqCnt == 0) {
        LOG_F("The dynamic file corpus is empty (for minimization). This shouldn't happen");
    }

    if (run->global->io.dynfileqCurrent == NULL) {
        run->global->io.dynfileqCurrent = TAILQ_FIRST(&run->global->io.dynfileq);
    } else {
        run->global->io.dynfileqCurrent = TAILQ_NEXT(run->global->io.dynfileqCurrent, pointers);
    }
    if (run->global->io.dynfileqCurrent == NULL) {
        return false;
    }

    input_setSize(run, run->global->io.dynfileqCurrent->size);
    memcpy(run->dynamicFile, run->global->io.dynfileqCurrent->data,
        run->global->io.dynfileqCurrent->size);
    snprintf(
        run->origFileName, sizeof(run->origFileName), "%s", run->global->io.dynfileqCurrent->path);

    LOG_D("Cov: %" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64,
        run->global->io.dynfileqCurrent->cov[0], run->global->io.dynfileqCurrent->cov[1],
        run->global->io.dynfileqCurrent->cov[2], run->global->io.dynfileqCurrent->cov[3]);

    return true;
}
