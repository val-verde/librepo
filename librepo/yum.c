#define _POSIX_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "setup.h"
#include "yum.h"
#include "util.h"
#include "metalink.h"
#include "mirrorlist.h"
#include "repomd.h"
#include "curl.h"
#include "checksum.h"
#include "handle_internal.h"

/** TODO:
 * - GPG check
 */

/* helper function for YumRepo manipulation */

lr_YumRepo
lr_yum_repo_init()
{
    return lr_malloc0(sizeof(struct _lr_YumRepo));
}

void
lr_yum_repo_clear(lr_YumRepo repo)
{
    if (!repo)
        return;
    lr_free(repo->repomd);
    lr_free(repo->primary);
    lr_free(repo->filelists);
    lr_free(repo->other);
    lr_free(repo->primary_db);
    lr_free(repo->filelists_db);
    lr_free(repo->other_db);
    lr_free(repo->group);
    lr_free(repo->group_gz);
    lr_free(repo->deltainfo);
    lr_free(repo->updateinfo);
    lr_free(repo->origin);
    lr_free(repo->url);
    lr_free(repo->destdir);
    memset(repo, 0, sizeof(struct _lr_YumRepo));
 }

void
lr_yum_repo_free(lr_YumRepo repo)
{
    if (!repo)
        return;
    lr_yum_repo_clear(repo);
    lr_free(repo);
}

/* main bussines logic */

int
lr_yum_download_repomd(lr_Handle handle, lr_YumRepo repo, int fd)
{
    int rc = LRE_OK;

    if (handle->baseurl) {
        /* Try base url */
        DEBUGF(fprintf(stderr, "Downloading repomd.xml via baseurl: %s\n", handle->baseurl));
        rc = lr_curl_single_download(handle,
                                     handle->baseurl,
                                     fd,
                                     "/repodata/repomd.xml");
    }

    if (handle->mirrorlist && (!handle->baseurl || rc != LRE_OK)) {
        /* Use mirrorlist */
        int mirrors_fd = lr_gettmpfile();

        DEBUGF(fprintf(stderr, "Downloading repomd.xml via mirrorlist\n"));

        rc = lr_curl_single_download(handle, handle->mirrorlist, mirrors_fd, NULL);
        if (rc != LRE_OK) {
            close(mirrors_fd);
            return rc;
        }

        if (lseek(mirrors_fd, 0, SEEK_SET) != 0) {
            close(mirrors_fd);
            return LRE_IO;
        }

        if (strstr(handle->mirrorlist, "metalink")) {
            /* Metalink */
            lr_Metalink metalink;
            lr_ChecksumType checksum_type = LR_CHECKSUM_UNKNOWN;
            char *checksum;

            DEBUGF(fprintf(stderr, "Got metalink\n"));

            /* Parse metalink */
            metalink = lr_metalink_init();
            rc = lr_metalink_parse_file(metalink, mirrors_fd);
            if (rc != LRE_OK) {
                DEBUGF(fprintf(stderr, "Cannot parse metalink (%d)\n", rc));
                close(mirrors_fd);
                lr_metalink_free(metalink);
                return rc;
            }

            /* Select the best checksum type */
            for (int x = 0; x < metalink->noh; x++) {
                if (!(handle->checks & LR_CHECK_CHECKSUM))
                    break;  /* Do not want to check checksum */

                lr_ChecksumType mtype;
                lr_MetalinkHash mhash = metalink->hashes[x];

                if (!mhash->type || !mhash->value)
                    continue;

                mtype = lr_checksum_type(mhash->type);
                if (mtype != LR_CHECKSUM_UNKNOWN && mtype > checksum_type) {
                    checksum_type = mtype;
                    checksum = mhash->value;
                }
            }

            /* Download repomd.xml from any of mirror */
            for (int x = 0; x < metalink->nou; x++) {
                char *used_mirror = metalink->urls[x]->url;

                DEBUGF(fprintf(stderr, "Downloading from mirror: %s\n", used_mirror));

                lseek(fd, 0, SEEK_SET);
                rc = lr_curl_single_download(handle,
                                             used_mirror,
                                             fd,
                                             "/repodata/repomd.xml");
                DEBUGF(fprintf(stderr, "Download rc: %d\n", rc));
                if (rc == LRE_OK) {
                    /* Download successful */
                    DEBUGF(fprintf(stderr, "Download successful\n"));

                    /* Check checksum if possible and wanted */
                    if (checksum_type != LR_CHECKSUM_UNKNOWN) {
                        DEBUGF(fprintf(stderr, "Checking checksum\n"));
                        lseek(fd, 0, SEEK_SET);
                        if (lr_checksum_fd_cmp(checksum_type, fd, checksum)) {
                            DEBUGF(fprintf(stderr, "Bad checksum\n"));
                            rc = LRE_BAD_CHECKSUM;
                            continue; /* Try next mirror */
                        }
                    }

                    /* Store used mirror into the handler */
                    handle->used_mirror = lr_strdup(used_mirror);
                    if (lr_ends_with(used_mirror, "/repodata/repomd.xml")) {
                        /* Strip "/repodata/repomd.xml" from mirror url */
                        size_t len = strlen(handle->used_mirror);
                        handle->used_mirror[len-20] = '\0';
                    }
                    break;
                }
            }

            lr_metalink_free(metalink);
        } else {
            /* Mirrorlist */
            lr_Mirrorlist mirrorlist;

            DEBUGF(fprintf(stderr, "Got mirrorlist\n"));

            mirrorlist = lr_mirrorlist_init();
            rc = lr_mirrorlist_parse_file(mirrorlist, mirrors_fd);
            if (rc != LRE_OK) {
                DEBUGF(fprintf(stderr, "Cannot parse mirrorlist (%d)\n", rc));
                close(mirrors_fd);
                lr_mirrorlist_free(mirrorlist);
                return rc;
            }

            /* Download repomd.xml from any of mirror */
            for (int x = 0; x < mirrorlist->nou; x++) {
                char *used_mirror = mirrorlist->urls[x];

                DEBUGF(fprintf(stderr, "Downloading from mirror: %s\n", used_mirror));

                lseek(fd, 0, SEEK_SET);
                rc = lr_curl_single_download(handle,
                                          used_mirror,
                                          fd,
                                          "/repodata/repomd.xml");
                DEBUGF(fprintf(stderr, "Download rc: %d\n", rc));
                if (rc == LRE_OK) {
                    DEBUGF(fprintf(stderr, "Download successful\n"));
                    handle->used_mirror = lr_strdup(used_mirror);
                    break;
                }
            }

            lr_mirrorlist_free(mirrorlist);
        }

        close(mirrors_fd);
    }

    if (rc != LRE_OK) {
        /* Download of repomd.xml was not successful */
        DEBUGF(fprintf(stderr, "All downloads was unsuccessful"));
        return rc;
    }

    return LRE_OK;
}

/* Returns 1 if target was added and 0 otherwise */
int
lr_add_target(lr_CurlTarget targets[],
              const char *destdir,
              const char *url,
              lr_YumRepoMdRecord rec,
              char **local_path,
              int used,
              int use_this)
{
    int fd;
    lr_CurlTarget target;

    if (!use_this)
        return 0;

    DEBUGASSERT(destdir);
    DEBUGASSERT(strlen(destdir));

    if (!rec || !rec->location_href)
        return 0;

    if (*local_path)  /* During update local_path may be already existing */
        lr_free(*local_path);
    *local_path = lr_pathconcat(destdir, rec->location_href, NULL);
    fd = open(*local_path, O_CREAT|O_TRUNC|O_RDWR, 0660);
    if (fd < 0) {
        lr_free(*local_path);
        return 0;
    }

    target = lr_target_init();
    target->url = lr_pathconcat(url, rec->location_href, NULL);
    target->fd = fd;
    targets[used] = target;

    return 1;
}

int
lr_yum_download_repo(lr_Handle handle, lr_YumRepo repo, lr_YumRepoMd repomd)
{
    int ret = LRE_OK;
    char *dir;                                        /* Destination dir */
    char *url;                                        /* Base url */
    lr_CurlTarget targets[LR_NUM_OF_YUM_REPOMD_RECORDS];/* Targets to download */
    int used = 0;                                     /* Number of targets */

    dir = handle->destdir;
    url = handle->used_mirror ? handle->used_mirror : handle->baseurl;

    used += lr_add_target(targets, dir, url, repomd->primary,
                          &repo->primary, used,
                          handle->yumflags & LR_YUM_PRI);
    used += lr_add_target(targets, dir, url, repomd->filelists,
                          &repo->filelists, used,
                          handle->yumflags & LR_YUM_FIL);
    used += lr_add_target(targets, dir, url, repomd->other,
                          &repo->other, used,
                          handle->yumflags & LR_YUM_OTH);
    used += lr_add_target(targets, dir, url, repomd->primary_db,
                          &repo->primary_db, used,
                          handle->yumflags & LR_YUM_PRI_DB);
    used += lr_add_target(targets, dir, url, repomd->filelists_db,
                          &repo->filelists_db, used,
                          handle->yumflags & LR_YUM_FIL_DB);
    used += lr_add_target(targets, dir, url, repomd->other_db,
                          &repo->other_db, used,
                          handle->yumflags & LR_YUM_OTH_DB);
    used += lr_add_target(targets, dir, url, repomd->group,
                          &repo->group, used,
                          handle->yumflags & LR_YUM_GROUP);
    used += lr_add_target(targets, dir, url, repomd->group,
                          &repo->group, used,
                          handle->yumflags & LR_YUM_GROUP_GZ);
    used += lr_add_target(targets, dir, url, repomd->deltainfo,
                          &repo->deltainfo, used,
                          handle->yumflags & LR_YUM_DELTAINFO);
    used += lr_add_target(targets, dir, url, repomd->updateinfo,
                          &repo->updateinfo, used,
                          handle->yumflags & LR_YUM_UPDATEINFO);

    if (used == 0)
        return ret;

    ret = lr_curl_multi_download(handle, targets, used);

    /* Clean up */
    for (int x = 0; x < used; x++) {
        close(targets[x]->fd);
        lr_free(targets[x]->url);
        lr_target_free(targets[x]);
    }

    return ret;
}

int
lr_yum_check_checksum_of_md_record(lr_YumRepoMdRecord rec, char *path)
{
    int ret, fd;
    char *expected_checksum;
    lr_ChecksumType checksum_type;

    if (!rec || !path)
        return LRE_OK;

    expected_checksum = rec->checksum;
    checksum_type = lr_checksum_type(rec->checksum_type);

    DEBUGF(fprintf(stderr, "Checking checksum of %s (expected: %s [%s])\n",
                           path, expected_checksum, rec->checksum_type));

    if (!expected_checksum) {
        DEBUGF(fprintf(stderr, "No checksum in repomd\n"));
        return LRE_OK;  /* Empty checksum - suppose it's ok */
    }

    if (checksum_type == LR_CHECKSUM_UNKNOWN) {
        DEBUGF(fprintf(stderr, "Unknown checksum: %s\n", rec->checksum_type));
        return LRE_UNKNOWN_CHECKSUM;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        DEBUGF(fprintf(stderr, "Cannot open %s\n", path));
        return LRE_IO;
    }

    ret = lr_checksum_fd_cmp(checksum_type, fd, expected_checksum);

    close(fd);

    if (ret) {
        DEBUGF(fprintf(stderr, "Checksum check - Failed\n"));
        return LRE_BAD_CHECKSUM;
    }

    DEBUGF(fprintf(stderr, "Checksum check - Passed\n"));

    return LRE_OK;
}

int
lr_yum_check_repo_checksums(lr_YumRepo repo, lr_YumRepoMd repomd)
{
    int ret;

    ret = lr_yum_check_checksum_of_md_record(repomd->primary,
                                             repo->primary);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (primary)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->filelists,
                                             repo->filelists);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (filelists)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->other,
                                             repo->other);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (other)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->primary_db,
                                             repo->primary_db);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (primary_db)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->filelists_db,
                                             repo->filelists_db);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (filelists_db)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->other_db,
                                             repo->other_db);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (other_db)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->group,
                                             repo->group);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (group)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->group_gz,
                                             repo->group_gz);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (group_gz)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->deltainfo,
                                             repo->deltainfo);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (deltainfo)\n", ret));
    if (ret != LRE_OK) return ret;
    ret = lr_yum_check_checksum_of_md_record(repomd->updateinfo,
                                             repo->updateinfo);
    DEBUGF(fprintf(stderr, "Checksum rc: %d (updateinfo)\n", ret));
    if (ret != LRE_OK) return ret;

    return LRE_OK;
}

int
lr_yum_perform(lr_Handle handle, lr_Result result)
{
    int rc = LRE_OK;
    int fd;
    char *path_to_repodata, *path;
    lr_YumRepo repo;
    lr_YumRepoMd repomd;

    assert(result);

    if (!handle->baseurl && !handle->mirrorlist)
        return LRE_NOURL;

    if (handle->update) {
        /* Download/Locate only specified files */
        if (!result->yum_repo || !result->yum_repomd)
            return LRE_INCOMPLETERESULT;
    } else {
        /* Download/Locate from scratch */
        if (result->yum_repo || result->yum_repomd)
            return LRE_ALREADYUSEDRESULT;
        result->yum_repo = lr_yum_repo_init();
        result->yum_repomd = lr_yum_repomd_init();
    }

    repo   = result->yum_repo;
    repomd = result->yum_repomd;

    if (handle->local) {
        /* Do not duplicate repoata, just locate the local one */
        if (strncmp(handle->baseurl, "file://", 7) &&
            strstr(handle->baseurl, "://")) {
            return LRE_NOTLOCAL;
        }

        if (!handle->update) {
            /* Open and parse repomd */
            path = lr_pathconcat(handle->baseurl, "repodata/repomd.xml", NULL);
            fd = open(path, O_RDONLY);
            if (fd < 0) {
                DEBUGF(fprintf(stderr, "%s doesn't exists\n", path));
                lr_free(path);
                return LRE_IO;
            }

            DEBUGF(fprintf(stderr, "Parsing repomd.xml\n"));
            repomd = lr_yum_repomd_init();
            rc = lr_yum_repomd_parse_file(repomd, fd);
            if (rc != LRE_OK) {
                DEBUGF(fprintf(stderr, "Parsing unsuccessful (%d)\n", rc));
                lr_free(path);
                return rc;
            }

            close(fd);

            /* Fill result object */
            result->destdir = lr_strdup(handle->baseurl);
            repo->repomd = path;

            DEBUGF(fprintf(stderr, "Repomd revision: %s\n", repomd->revision));
        }

        /* Locate rest of metadata files */
        /* First condition !repo->* is for cases when handle->update is true,
         * then we don't need to generate paths we already have */
        if (!repo->primary && repomd->primary && handle->yumflags & LR_YUM_PRI)
            repo->primary = lr_pathconcat(handle->baseurl,
                                repomd->primary->location_href,
                                NULL);
        if (!repo->filelists && repomd->filelists && handle->yumflags & LR_YUM_FIL)
            repo->filelists = lr_pathconcat(handle->baseurl,
                                repomd->filelists->location_href,
                                NULL);
        if (!repo->other && repomd->other && handle->yumflags & LR_YUM_OTH)
            repo->other = lr_pathconcat(handle->baseurl,
                                repomd->other->location_href,
                                NULL);
        if (!repo->primary_db && repomd->primary_db && handle->yumflags & LR_YUM_PRI_DB)
            repo->primary_db = lr_pathconcat(handle->baseurl,
                                repomd->primary_db->location_href,
                                NULL);
        if (!repo->filelists_db && repomd->filelists_db && handle->yumflags & LR_YUM_FIL_DB)
            repo->filelists_db = lr_pathconcat(handle->baseurl,
                                repomd->filelists_db->location_href,
                                NULL);
        if (!repo->other_db && repomd->other_db && handle->yumflags & LR_YUM_OTH_DB)
            repo->other_db = lr_pathconcat(handle->baseurl,
                                repomd->other_db->location_href,
                                NULL);
        if (!repo->group && repomd->group && handle->yumflags & LR_YUM_GROUP)
            repo->group = lr_pathconcat(handle->baseurl,
                                repomd->group->location_href,
                                NULL);
        if (!repo->group_gz && repomd->group_gz && handle->yumflags & LR_YUM_GROUP_GZ)
            repo->group_gz = lr_pathconcat(handle->baseurl,
                                repomd->group_gz->location_href,
                                NULL);
        if (!repo->deltainfo && repomd->deltainfo && handle->yumflags & LR_YUM_DELTAINFO)
            repo->deltainfo = lr_pathconcat(handle->baseurl,
                                repomd->deltainfo->location_href,
                                NULL);
        if (!repo->updateinfo && repomd->updateinfo && handle->yumflags & LR_YUM_UPDATEINFO)
            repo->updateinfo = lr_pathconcat(handle->baseurl,
                                repomd->updateinfo->location_href,
                                NULL);
        DEBUGF(fprintf(stderr, "Repository was successfully located\n"));
    } else {
        /* Download remote metadata */
        int create_repodata_dir = 1;

        path_to_repodata = lr_pathconcat(handle->destdir, "repodata", NULL);

        if (handle->update) {  /* Check if shoud create repodata/ subdir */
            struct stat buf;
            if (stat(path_to_repodata, &buf) != -1)
                if (S_ISDIR(buf.st_mode))
                    create_repodata_dir = 0;
        }

        if (create_repodata_dir) {
            /* Prepare repodata/ subdir */
            rc = mkdir(path_to_repodata, S_IRWXU|S_IRWXG|S_IROTH|S_IXOTH);
            if (rc == -1) {
                lr_free(path_to_repodata);
                return LRE_CANNOT_CREATE_DIR;
            }
        }
        lr_free(path_to_repodata);

        if (!handle->update) {
            /* Prepare repomd.xml file */
            path = lr_pathconcat(handle->destdir, "/repodata/repomd.xml", NULL);
            fd = open(path, O_CREAT|O_TRUNC|O_RDWR, 0660);
            if (fd == -1) {
                lr_free(path);
                return LRE_IO;
            }

            /* Download repomd.xml */
            rc = lr_yum_download_repomd(handle, repo, fd);
            close(fd);
            if (rc != LRE_OK) {
                lr_free(path);
                return rc;
            }

            lseek(fd, 0, SEEK_SET);

            /* Parse repomd */
            DEBUGF(fprintf(stderr, "Parsing repomd.xml\n"));
            repomd = lr_yum_repomd_init();
            rc = lr_yum_repomd_parse_file(repomd, fd);
            if (rc != LRE_OK) {
                DEBUGF(fprintf(stderr, "Parsing unsuccessful (%d)\n", rc));
                lr_free(path);
                return rc;
            }

            close(fd);

            /* Fill result object */
            result->destdir = lr_strdup(handle->destdir);
            repo->repomd = path;
            if (handle->used_mirror)
                repo->url = lr_strdup(handle->used_mirror);
            else
                repo->url = lr_strdup(handle->baseurl);

            DEBUGF(fprintf(stderr, "Repomd revision: %s\n", repomd->revision));
        }

        /* Download rest of metadata files */
        if ((rc = lr_yum_download_repo(handle, repo, repomd)) != LRE_OK)
            return rc;

        DEBUGF(fprintf(stderr, "Repository was successfully downloaded\n"));
    }

    /* Check checksums */
    if (handle->checks & LR_CHECK_CHECKSUM) {
        if ((rc = lr_yum_check_repo_checksums(repo, repomd)) != LRE_OK)
            return rc;
        DEBUGF(fprintf(stderr, "All checksums in repository seems to be valid\n"));
    }

    return rc;
}