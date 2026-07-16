/**
 * Copyright © 2025 NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include <systemd/sd-bus.h>


#define ARCHIVE_INTF        "xyz.openbmc_project.Console.Archive"
#define ARCHIVE_OBJ_PATH    "/xyz/openbmc_project/console"
#define ARCHIVE_SERVICE_NAME \
    "xyz.openbmc_project.Console.ArchiveManager"

#define CONSOLE_LOG_DIR_CPU0 "/var/log/console_cpu0"
#define CONSOLE_LOG_DIR_CPU1 "/var/log/console_cpu1"
#define USTAR_FILE_SIZE_MAX 077777777777ULL

/* D-Bus error names */
#define ERROR_NO_LOG_FILES_FOUND \
    "xyz.openbmc_project.Console.Archive.Error.NoLogFilesFound"
#define ERROR_FILE_OPEN \
    "xyz.openbmc_project.Common.File.Error.Open"
#define ERROR_FILE_WRITE \
    "xyz.openbmc_project.Common.File.Error.Write"
#define ERROR_INTERNAL_FAILURE \
    "xyz.openbmc_project.Common.Error.InternalFailure"

/* Archive service context */
struct archive_context {
    sd_bus *bus;
    char current_archive_path[256];
};

/* Global signal flag */
static volatile sig_atomic_t sigterm_received = 0;

static void sigterm_handler(int sig __attribute__((unused)))
{
    sigterm_received = 1;
}

/*
 * POSIX ustar tar header
 * Only fields actually used by this implementation are filled.
 */
struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

/* Error codes for create_archive() */
enum archive_error {
    ARCHIVE_SUCCESS        = 0,
    ARCHIVE_ERROR_NO_FILES = -1,
    ARCHIVE_ERROR_FILE_OPEN = -2,
    ARCHIVE_ERROR_FILE_WRITE = -3,
    ARCHIVE_ERROR_INTERNAL = -4,
};

/* Internal status from add_directory_to_tar() */
enum add_dir_status {
    ADD_DIR_OK       = 0,
    ADD_DIR_ERR_OPEN = -1,
    ADD_DIR_ERR_WRITE = -2,
};

static int write_tar_header(FILE *tar_fp, const char *archive_name,
                            const struct stat *st)
{
    struct tar_header header;
    uint32_t checksum = 0;
    size_t i;

    memset(&header, 0, sizeof(header));

    strncpy(header.name, archive_name, sizeof(header.name) - 1);
    snprintf(header.mode, sizeof(header.mode), "%07o",
             st->st_mode & 07777);
    snprintf(header.uid, sizeof(header.uid), "%07o", st->st_uid);
    snprintf(header.gid, sizeof(header.gid), "%07o", st->st_gid);
    snprintf(header.size, sizeof(header.size), "%011llo",
             (unsigned long long)st->st_size);
    snprintf(header.mtime, sizeof(header.mtime), "%011llo",
             (unsigned long long)st->st_mtime);
    header.typeflag = '0';
    memcpy(header.magic, "ustar", 5);
    memcpy(header.version, "00", 2);
    memset(header.chksum, ' ', sizeof(header.chksum));
    for (i = 0; i < sizeof(header); i++) {
        checksum += ((unsigned char *)&header)[i];
    }
    snprintf(header.chksum, sizeof(header.chksum), "%06o", checksum);
    if (fwrite(&header, sizeof(header), 1, tar_fp) != 1) {
        return -1;
    }
    return 0;
}

static int add_file_to_tar(FILE *tar_fp, const char *filepath,
                           const char *archive_name)
{
    FILE *src_fp = NULL;
    char buffer[8192];
    size_t bytes_read;
    off_t remaining;
    struct stat st;
    int src_fd;

    src_fd = open(filepath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (src_fd < 0) {
        int saved_errno = errno;
        warn("Failed to open %s for reading", filepath);
        errno = saved_errno;
        return -1;
    }

    src_fp = fdopen(src_fd, "rb");
    if (!src_fp) {
        int saved_errno = errno;
        warn("Failed to create stream for %s", filepath);
        close(src_fd);
        errno = saved_errno;
        return -1;
    }

    if (fstat(src_fd, &st) < 0) {
        int saved_errno = errno;
        warn("Failed to stat open file %s", filepath);
        fclose(src_fp);
        errno = saved_errno;
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        warnx("Refusing to archive non-regular file %s", filepath);
        fclose(src_fp);
        errno = EINVAL;
        return -1;
    }

    if (st.st_size < 0 || (uintmax_t)st.st_size > USTAR_FILE_SIZE_MAX) {
        warnx("Refusing to archive %s: file too large for USTAR", filepath);
        fclose(src_fp);
        errno = EFBIG;
        return -1;
    }

    errno = 0;
    if (write_tar_header(tar_fp, archive_name, &st) < 0) {
        int saved_errno = errno != 0 ? errno : EIO;
        warnx("Failed to write tar header for %s", filepath);
        fclose(src_fp);
        errno = saved_errno;
        return -1;
    }

    /* Copy exactly the size encoded in the TAR header even if the log grows */
    remaining = st.st_size;
    while (remaining > 0) {
        size_t to_read = remaining > (off_t)sizeof(buffer)
                             ? sizeof(buffer)
                             : (size_t)remaining;

        errno = 0;
        bytes_read = fread(buffer, 1, to_read, src_fp);
        if (bytes_read == 0) {
            int saved_errno = errno != 0 ? errno : EIO;

            if (ferror(src_fp)) {
                warnx("Error reading %s", filepath);
            } else {
                warnx("Unexpected EOF while reading %s", filepath);
            }
            fclose(src_fp);
            errno = saved_errno;
            return -1;
        }

        errno = 0;
        if (fwrite(buffer, 1, bytes_read, tar_fp) != bytes_read) {
            int saved_errno = errno != 0 ? errno : EIO;

            warnx("Failed to write file content for %s", filepath);
            fclose(src_fp);
            errno = saved_errno;
            return -1;
        }

        remaining -= (off_t)bytes_read;
    }

    fclose(src_fp);

    /* Pad to a 512-byte boundary using the size encoded in the TAR header */
    if (st.st_size % 512 != 0) {
        size_t padding = 512 - (st.st_size % 512);
        memset(buffer, 0, padding);
        errno = 0;
        if (fwrite(buffer, 1, padding, tar_fp) != padding) {
            int saved_errno = errno != 0 ? errno : EIO;

            warnx("Failed to write padding for %s", filepath);
            errno = saved_errno;
            return -1;
        }
    }

    return 0;
}

/*
 * Add all files from a directory into the tar stream.
 *
 * Returns:
 *   ADD_DIR_OK       - success or directory does not exist
 *   ADD_DIR_ERR_OPEN - permission / existence issues on directory or file
 *   ADD_DIR_ERR_WRITE- write/IO errors while adding files
 */
static int add_directory_to_tar(FILE *tar_fp, const char *dirpath,
                                const char *archive_prefix, bool *files_added)
{
    DIR *dir;
    struct dirent *entry;
    char full_path[512];
    char archive_name[512];
    struct stat st;
    bool found_files = false;

    dir = opendir(dirpath);
    if (!dir) {
        if (errno == ENOENT) {
            /* Directory absent: treated as no-logs for this CPU. */
            return ADD_DIR_OK;
        }
        warn("Failed to open directory %s", dirpath);
        return ADD_DIR_ERR_OPEN;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s",
                 dirpath, entry->d_name);
        snprintf(archive_name, sizeof(archive_name), "%s/%s",
                 archive_prefix, entry->d_name);

        if (stat(full_path, &st) < 0) {
            /* Treat inability to stat as an access/open error. */
            warn("Failed to stat %s", full_path);
            closedir(dir);
            return ADD_DIR_ERR_OPEN;
        }

        if (add_file_to_tar(tar_fp, full_path, archive_name) < 0) {
            int saved_errno = errno;
            warnx("Failed to add %s to archive", full_path);
            closedir(dir);
            if (saved_errno == EACCES || saved_errno == EPERM ||
                saved_errno == ENOENT || saved_errno == EINVAL ||
                saved_errno == EFBIG || saved_errno == ELOOP) {
                return ADD_DIR_ERR_OPEN;
            }
            return ADD_DIR_ERR_WRITE;
        }
        found_files = true;
    }

    closedir(dir);
    if (files_added && found_files) {
        *files_added = true;
    }
    return ADD_DIR_OK;
}

/*
 * Create a tar.gz archive containing all files from console_cpu0 and
 * console_cpu1.
 *
 * Returns:
 *   >= 0 : file descriptor to archive on success
 *   <  0 : one of enum archive_error
 */
static int create_archive(char *archive_path_out, size_t path_size)
{
    /* Randomized temp paths — mkstemps creates with O_EXCL, no TOCTOU */
    char archive_path[] = "/tmp/console_archive_XXXXXX.tar.gz";
    char temp_tar_path[] = "/tmp/console_archive_XXXXXX.tar";
    FILE *tar_fp = NULL;
    gzFile gz_fp = NULL;
    int archive_fd = -1;
    char buffer[8192];
    size_t bytes_read;
    struct stat st_cpu0;
    struct stat st_cpu1;
    bool files_added = false;

    {
        int tar_fd = mkstemps(temp_tar_path, 4); /* 4 = strlen(".tar") */
        if (tar_fd < 0) {
            warn("Failed to create temporary tar file");
            return ARCHIVE_ERROR_INTERNAL;
        }
        tar_fp = fdopen(tar_fd, "wb");
        if (!tar_fp) {
            close(tar_fd);
            unlink(temp_tar_path);
            warn("fdopen failed for temporary tar file");
            return ARCHIVE_ERROR_INTERNAL;
        }
    }

    /* Add console_cpu0 directory if it exists */
    if (stat(CONSOLE_LOG_DIR_CPU0, &st_cpu0) == 0 &&
        S_ISDIR(st_cpu0.st_mode)) {
        int status = add_directory_to_tar(tar_fp, CONSOLE_LOG_DIR_CPU0,
                                          "console_cpu0", &files_added);
        if (status < 0) {
            fclose(tar_fp);
            unlink(temp_tar_path);
            if (status == ADD_DIR_ERR_OPEN) {
                return ARCHIVE_ERROR_FILE_OPEN;
            }
            return ARCHIVE_ERROR_FILE_WRITE;
        }
    }

    /* Add console_cpu1 directory if it exists */
    if (stat(CONSOLE_LOG_DIR_CPU1, &st_cpu1) == 0 &&
        S_ISDIR(st_cpu1.st_mode)) {
        int status = add_directory_to_tar(tar_fp, CONSOLE_LOG_DIR_CPU1,
                                          "console_cpu1", &files_added);
        if (status < 0) {
            fclose(tar_fp);
            unlink(temp_tar_path);
            if (status == ADD_DIR_ERR_OPEN) {
                return ARCHIVE_ERROR_FILE_OPEN;
            }
            return ARCHIVE_ERROR_FILE_WRITE;
        }
    }

    if (!files_added) {
        fclose(tar_fp);
        unlink(temp_tar_path);
        return ARCHIVE_ERROR_NO_FILES;
    }

    memset(buffer, 0, sizeof(buffer));
    if (fwrite(buffer, 512, 2, tar_fp) != 2) {
        warnx("Failed to write tar end markers");
        fclose(tar_fp);
        unlink(temp_tar_path);
        return ARCHIVE_ERROR_FILE_WRITE;
    }

    fclose(tar_fp);
    tar_fp = NULL;

    tar_fp = fopen(temp_tar_path, "rb");
    if (!tar_fp) {
        warn("Failed to open temporary tar file for compression");
        unlink(temp_tar_path);
        return ARCHIVE_ERROR_INTERNAL;
    }

    {
        int gz_fd = mkstemps(archive_path, 7); /* 7 = strlen(".tar.gz") */
        if (gz_fd < 0) {
            warn("Failed to create archive file");
            fclose(tar_fp);
            unlink(temp_tar_path);
            return ARCHIVE_ERROR_INTERNAL;
        }
        gz_fp = gzdopen(gz_fd, "wb");
        if (!gz_fp) {
            warn("Failed to open gzip stream");
            close(gz_fd);
            fclose(tar_fp);
            unlink(temp_tar_path);
            unlink(archive_path);
            return ARCHIVE_ERROR_INTERNAL;
        }
    }

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), tar_fp)) > 0) {
        if (gzwrite(gz_fp, buffer, (unsigned int)bytes_read) !=
            (int)bytes_read) {
            warnx("Failed to write compressed data");
            gzclose(gz_fp);
            fclose(tar_fp);
            unlink(temp_tar_path);
            unlink(archive_path);
            return ARCHIVE_ERROR_FILE_WRITE;
        }
    }

    if (ferror(tar_fp)) {
        warnx("Error reading tar file");
        gzclose(gz_fp);
        fclose(tar_fp);
        unlink(temp_tar_path);
        unlink(archive_path);
        return ARCHIVE_ERROR_INTERNAL;
    }

    if (gzclose(gz_fp) != Z_OK) {
        warnx("Failed to close gzip stream");
        fclose(tar_fp);
        unlink(temp_tar_path);
        unlink(archive_path);
        return ARCHIVE_ERROR_FILE_WRITE;
    }

    fclose(tar_fp);
    unlink(temp_tar_path);

    archive_fd = open(archive_path, O_RDONLY | O_CLOEXEC);
    if (archive_fd < 0) {
        warn("Failed to open archive file %s", archive_path);
        unlink(archive_path);
        return ARCHIVE_ERROR_INTERNAL;
    }

    if (archive_path_out && path_size > 0) {
        strncpy(archive_path_out, archive_path, path_size - 1);
        archive_path_out[path_size - 1] = '\0';
    }

    return archive_fd;
}

/* Returns 0 when the caller is authorized, or a negative errno. */
static int consoleCheckCaller(sd_bus_message *msg)
{
    sd_bus_creds *creds = NULL;
    uid_t uid;
    int rc;

    rc = sd_bus_query_sender_creds(msg, SD_BUS_CREDS_UID, &creds);
    if (rc < 0) {
        return rc;
    }

    if (!creds) {
        return -EPERM;
    }

    rc = sd_bus_creds_get_uid(creds, &uid);
    sd_bus_creds_unref(creds);
    if (rc < 0) {
        return rc;
    }

    return (uid == 0 || uid == geteuid()) ? 0 : -EPERM;
}

static int method_get_log(sd_bus_message *msg, void *userdata,
                          sd_bus_error *err)
{
    struct archive_context *ctx = userdata;
    int archive_fd;
    int rc;

    if (!ctx || !ctx->bus) {
        sd_bus_error_set_const(err, ERROR_INTERNAL_FAILURE,
                               "Internal error: Invalid context");
        return sd_bus_reply_method_error(msg, err);
    }

    rc = consoleCheckCaller(msg);
    if (rc < 0) {
        warnx("Rejected unauthorized console GetLog request");
        sd_bus_error_set_const(
            err, SD_BUS_ERROR_ACCESS_DENIED,
            "Unauthorized: console log access requires root");
        return sd_bus_reply_method_error(msg, err);
    }

    archive_fd = create_archive(ctx->current_archive_path,
                                sizeof(ctx->current_archive_path));
    if (archive_fd < 0) {
        switch (archive_fd) {
        case ARCHIVE_ERROR_NO_FILES:
            sd_bus_error_set_const(err, ERROR_NO_LOG_FILES_FOUND,
                                   "No console log files found");
            break;
        case ARCHIVE_ERROR_FILE_OPEN:
            sd_bus_error_set_const(err, ERROR_FILE_OPEN,
                                   "Cannot access log files or create archive");
            break;
        case ARCHIVE_ERROR_FILE_WRITE:
            sd_bus_error_set_const(err, ERROR_FILE_WRITE,
                                   "Failed to write archive file");
            break;
        case ARCHIVE_ERROR_INTERNAL:
        default:
            sd_bus_error_set_const(err, ERROR_INTERNAL_FAILURE,
                                   "Internal error during archive creation");
            break;
        }
        warnx("GetLog() returning error: %s - %s", err->name, err->message);
        rc = sd_bus_reply_method_error(msg, err);
        if (rc < 0) {
            warnx("Failed to send error reply: %s", strerror(-rc));
        }
        return rc;
    }

    rc = sd_bus_reply_method_return(msg, "h", archive_fd);
    close(archive_fd);
    if (ctx->current_archive_path[0] != '\0') {
        unlink(ctx->current_archive_path);
        ctx->current_archive_path[0] = '\0';
    }
    if (rc < 0) {
        warnx("Failed to send unixfd reply: %s", strerror(-rc));
    }

    return rc;
}

static const sd_bus_vtable archive_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetLog", SD_BUS_NO_ARGS, "h", method_get_log,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

int archive_service_init(sd_bus **bus)
{
    struct archive_context *ctx;
    sd_bus *b;
    int r;

    if (!bus) {
        return -EINVAL;
    }

    r = sd_bus_default_system(&b);
    if (r < 0) {
        warnx("Failed to connect to system bus: %s", strerror(-r));
        return -1;
    }

    ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        warnx("Failed to allocate archive context");
        sd_bus_unref(b);
        return -1;
    }

    ctx->bus = b;
    ctx->current_archive_path[0] = '\0';

    r = sd_bus_add_object_vtable(b, NULL,
                                 ARCHIVE_OBJ_PATH, ARCHIVE_INTF,
                                 archive_vtable, ctx);
    if (r < 0) {
        warnx("Failed to register archive interface: %s", strerror(-r));
        free(ctx);
        sd_bus_unref(b);
        return -1;
    }

    r = sd_bus_request_name(b, ARCHIVE_SERVICE_NAME,
                            SD_BUS_NAME_ALLOW_REPLACEMENT |
                            SD_BUS_NAME_REPLACE_EXISTING);
    if (r < 0) {
        warnx("Failed to acquire archive service name: %s",
              strerror(-r));
        free(ctx);
        sd_bus_unref(b);
        return -1;
    }

    *bus = b;
    return 0;
}

/*
 * Run the service loop.
 * Returns 0 on clean shutdown, -1 on error.
 * Checks *should_exit periodically if non-NULL.
 */
int archive_service_run(sd_bus *bus, const volatile sig_atomic_t *should_exit)
{
    int r;

    if (!bus) {
        return -1;
    }

    for (;;) {
        if (sigterm_received ||
            (should_exit && *should_exit)) {
            return 0;
        }

        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            warnx("Failed to process bus: %s", strerror(-r));
            return -1;
        }
        if (r > 0) {
            continue;
        }

        r = sd_bus_wait(bus, 1000000); /* 1 second */
        if (r < 0) {
            if (r == -ETIMEDOUT) {
                continue;
            }
            warnx("Failed to wait on bus: %s", strerror(-r));
            return -1;
        }
    }

    return 0;
}

void archive_service_fini(sd_bus *bus)
{
    if (bus) {
        sd_bus_unref(bus);
    }
}

int main(int argc, char **argv)
{
    sd_bus *bus = NULL;
    int r;

    if (argc > 1) {
        errx(EXIT_FAILURE, "Usage: %s", argv[0]);
    }

    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    r = archive_service_init(&bus);
    if (r < 0) {
        errx(EXIT_FAILURE, "Failed to initialize archive service");
    }

    r = archive_service_run(bus, NULL);
    if (r < 0) {
        errx(EXIT_FAILURE, "Archive service failed");
    }

    archive_service_fini(bus);
    return EXIT_SUCCESS;
}
