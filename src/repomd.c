/* createrepo_c - Library of routines for manipulation with repodata
 * Copyright (C) 2012  Tomas Mlcoch
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <glib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>
#include <assert.h>
#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>
#include "logging.h"
#include "misc.h"
#include "repomd.h"
#include "compression_wrapper.h"

#undef MODULE
#define MODULE "repomd: "


#define DEFAULT_CHECKSUM                "sha256"
#define DEFAULT_CHECKSUM_ENUM_VAL        CR_CHECKSUM_SHA256

#define DEFAULT_DATABASE_VERSION        10

#define STR_BUFFER_SIZE      32
#define BUFFER_SIZE          8192

#define RPM_NS          "http://linux.duke.edu/metadata/rpm"
#define XMLNS_NS        "http://linux.duke.edu/metadata/repo"

#define XML_ENC         "UTF-8"
#define FORMAT_XML      1

#define REPOMD_OK       0
#define REPOMD_ERR      1


typedef struct _contentStat {
    char *checksum;
    long size;
} contentStat;


typedef struct _cr_Distro * cr_Distro;
struct _cr_Distro {
    gchar *cpeid;
    gchar *val;
};


cr_Distro
cr_new_distro()
{
    return (cr_Distro) g_malloc0(sizeof(struct _cr_Distro));
}


void
cr_free_distro(cr_Distro distro)
{
    if (!distro) return;
    g_free(distro->cpeid);
    g_free(distro->val);
    g_free(distro);
}


cr_RepomdRecord
cr_new_repomdrecord(const char *path)
{
    cr_RepomdRecord md = (cr_RepomdRecord) g_malloc0(sizeof(*md));
    md->chunk = g_string_chunk_new(1024);
    if (path)
        md->location_href = g_string_chunk_insert(md->chunk, path);
    return md;
}



void
cr_free_repomdrecord(cr_RepomdRecord md)
{
    if (!md)
        return;

    g_string_chunk_free(md->chunk);
    g_free(md);
}


contentStat *
get_compressed_content_stat(const char *filename, cr_ChecksumType checksum_type)
{
    if (!g_file_test(filename, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)) {
        return NULL;
    }


    // Open compressed file

    CR_FILE *cwfile;
    if (!(cwfile = cr_open(filename, CR_CW_MODE_READ, CR_CW_AUTO_DETECT_COMPRESSION))) {
        return NULL;
    }


    // Create checksum structure

    GChecksumType gchecksumtype;
    switch (checksum_type) {
        case CR_CHECKSUM_MD5:
            gchecksumtype = G_CHECKSUM_MD5;
            break;
        case CR_CHECKSUM_SHA1:
            gchecksumtype = G_CHECKSUM_SHA1;
            break;
        case CR_CHECKSUM_SHA256:
            gchecksumtype = G_CHECKSUM_SHA256;
            break;
        default:
            g_critical(MODULE"%s: Unknown checksum type", __func__);
            return NULL;
    };


    // Read compressed file and calculate checksum and size

    GChecksum *checksum = g_checksum_new(gchecksumtype);
    if (!checksum) {
        g_critical(MODULE"%s: g_checksum_new() failed", __func__);
        return NULL;
    }

    long size = 0;
    long readed;
    unsigned char buffer[BUFFER_SIZE];

    do {
        readed = cr_read(cwfile, (void *) buffer, BUFFER_SIZE);
        if (readed == CR_CW_ERR) {
            g_debug(MODULE"%s: Error while read compressed file: %s",
                    __func__, filename);
            break;
        }
        g_checksum_update (checksum, buffer, readed);
        size += readed;
    } while (readed == BUFFER_SIZE);


    // Create result structure

    contentStat* result = g_malloc(sizeof(contentStat));
    if (result) {
        result->checksum = g_strdup(g_checksum_get_string(checksum));
        result->size = size;
    }


    // Clean up

    g_checksum_free(checksum);
    cr_close(cwfile);

    return result;
}



int
cr_fill_repomdrecord(const char *base_path, cr_RepomdRecord md,
                     cr_ChecksumType *checksum_type)
{
    if (!md || !(md->location_href) || !strlen(md->location_href)) {
        // Nothing to do
        return REPOMD_ERR;
    }

    const char *checksum_str = DEFAULT_CHECKSUM;
    cr_ChecksumType checksum_t = DEFAULT_CHECKSUM_ENUM_VAL;

    if (checksum_type) {
        checksum_str = cr_checksum_name_str(*checksum_type);
        checksum_t = *checksum_type;
    }

    gchar *path = g_strconcat(base_path, "/", md->location_href, NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS|G_FILE_TEST_IS_REGULAR)) {
        // File doesn't exists
        g_warning(MODULE"%s: File %s doesn't exists", __func__, path);
        return REPOMD_ERR;
    }


    // Compute checksum of compressed file

    if (!md->checksum_type || !md->checksum) {
        gchar *chksum;
        md->checksum_type = g_string_chunk_insert(md->chunk, checksum_str);
        chksum = cr_compute_file_checksum(path, checksum_t);
        md->checksum = g_string_chunk_insert(md->chunk, chksum);
        g_free(chksum);
    }


    // Compute checksum of non compressed content and its size

    if (!md->checksum_open_type || !md->checksum_open || !md->size_open) {
        if (cr_detect_compression(path) != CR_CW_UNKNOWN_COMPRESSION &&
            cr_detect_compression(path) != CR_CW_NO_COMPRESSION)
        {
            // File compressed by supported algorithm
            contentStat *open_stat = NULL;
            open_stat = get_compressed_content_stat(path, checksum_t);
            md->checksum_open_type = g_string_chunk_insert(md->chunk, checksum_str);
            md->checksum_open = g_string_chunk_insert(md->chunk, open_stat->checksum);
            if (!md->size_open) {
                md->size_open = open_stat->size;
            }
            g_free(open_stat->checksum);
            g_free(open_stat);
        } else {
            // Unknown compression
            g_warning(MODULE"%s: File \"%s\" compressed by an unsupported type"
                      " of compression", __func__, path);
            md->checksum_open_type = g_string_chunk_insert(md->chunk, "UNKNOWN");
            md->checksum_open = g_string_chunk_insert(md->chunk,
                        "file_compressed_by_an_unsupported_type_of_compression");
            md->size_open = -1;
        }
    }


    // Get timestamp and size of compressed file

    if (!md->timestamp || !md->size) {
        struct stat buf;
        if (!stat(path, &buf)) {
            if (!md->timestamp) {
                md->timestamp = buf.st_mtime;
            }
            if (!md->size) {
                md->size = buf.st_size;
            }
        } else {
            g_warning(MODULE"%s: Stat on file \"%s\" failed", __func__, path);
        }
    }


    // Set db version

    if (!md->db_ver) {
        md->db_ver = DEFAULT_DATABASE_VERSION;
    }

    g_free(path);

    return REPOMD_OK;
}


void
cr_process_groupfile_repomdrecord(const char *base_path,
                                  cr_RepomdRecord groupfile,
                                  cr_RepomdRecord cgroupfile,
                                  cr_ChecksumType *checksum_type,
                                  cr_CompressionType groupfile_compression)
{
    if (!groupfile || !(groupfile->location_href) || !strlen(groupfile->location_href) || !cgroupfile) {
        return;
    }


    // Checksum stuff

    const char *checksum_str = DEFAULT_CHECKSUM;
    cr_ChecksumType checksum_t = DEFAULT_CHECKSUM_ENUM_VAL;

    if (checksum_type) {
        checksum_str = cr_checksum_name_str(*checksum_type);
        checksum_t = *checksum_type;
    }


    // Paths

    const char *suffix = cr_compression_suffix(groupfile_compression);

    gchar *clocation_href = g_strconcat(groupfile->location_href, suffix, NULL);
    cgroupfile->location_href = g_string_chunk_insert(cgroupfile->chunk,
                                                      clocation_href);
    g_free(clocation_href);

    gchar *path = g_strconcat(base_path, "/", groupfile->location_href, NULL);
    gchar *cpath = g_strconcat(base_path, "/", cgroupfile->location_href, NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS|G_FILE_TEST_IS_REGULAR)) {
        // File doesn't exists
        g_warning(MODULE"%s: File %s doesn't exists", __func__, path);
        g_free(path);
        g_free(cpath);
        return;
    }


    // Compress file + get size of non compressed file

    int readed;
    char buf[BUFFER_SIZE];
    CR_FILE *cw_plain;
    CR_FILE *cw_compressed;

    cw_plain = cr_open(path, CR_CW_MODE_READ, CR_CW_NO_COMPRESSION);
    cw_compressed = cr_open(cpath, CR_CW_MODE_WRITE, groupfile_compression);

    while ((readed = cr_read(cw_plain, buf, BUFFER_SIZE)) > 0) {
        if (cr_write(cw_compressed, buf, (unsigned int) readed) == CR_CW_ERR) {
            g_debug(MODULE"%s: Error while groupfile compression", __func__);
            break;
        }
    }

    cr_close(cw_compressed);
    cr_close(cw_plain);

    if (readed == CR_CW_ERR) {
        g_debug(MODULE"%s: Error while groupfile compression", __func__);
    }


    // Compute checksums

    gchar *checksum;
    gchar *cchecksum;
    checksum = cr_compute_file_checksum(path, checksum_t);
    cchecksum = cr_compute_file_checksum(cpath, checksum_t);


    // Get stats

    long gf_size = -1, cgf_size = -1;
    long gf_time = -1, cgf_time = -1;
    struct stat gf_stat, cgf_stat;

    if (stat(path, &gf_stat)) {
        g_debug(MODULE"%s: Error while stat() on %s", __func__, path);
    } else {
        gf_size = gf_stat.st_size;
        gf_time = gf_stat.st_mtime;
    }

    if (stat(cpath, &cgf_stat)) {
        g_debug(MODULE"%s: Error while stat() on %s", __func__, path);
    } else {
        cgf_size = cgf_stat.st_size;
        cgf_time = cgf_stat.st_mtime;
    }


    // Results

    groupfile->checksum = g_string_chunk_insert(groupfile->chunk, checksum);
    groupfile->checksum_type = g_string_chunk_insert(groupfile->chunk, checksum_str);
    groupfile->checksum_open = NULL;
    groupfile->checksum_open_type = NULL;
    groupfile->timestamp = gf_time;
    groupfile->size = gf_size;
    groupfile->size_open = -1;

    cgroupfile->checksum = g_string_chunk_insert(cgroupfile->chunk, cchecksum);
    cgroupfile->checksum_type = g_string_chunk_insert(cgroupfile->chunk, checksum_str);
    cgroupfile->checksum_open = g_string_chunk_insert(groupfile->chunk, checksum);
    cgroupfile->checksum_open_type = g_string_chunk_insert(groupfile->chunk, checksum_str);
    cgroupfile->timestamp = cgf_time;
    cgroupfile->size = cgf_size;
    cgroupfile->size_open = gf_size;

    g_free(checksum);
    g_free(cchecksum);
    g_free(path);
    g_free(cpath);
}


void
dump_data_items(xmlNodePtr root, cr_RepomdRecord md, const xmlChar *type)
{
    xmlNodePtr data, node;
    gchar str_buffer[STR_BUFFER_SIZE];

    if (!root || !md || !type) {
        return;
    }

    data = xmlNewChild(root, NULL, BAD_CAST "data", NULL);
    xmlNewProp(data, BAD_CAST "type", BAD_CAST type);


    node = xmlNewTextChild(data, NULL, BAD_CAST "checksum",
                           BAD_CAST md->checksum);
    xmlNewProp(node, BAD_CAST "type", BAD_CAST md->checksum_type);

    if (md->checksum_open) {
        node = xmlNewTextChild(data, NULL, BAD_CAST "open-checksum",
                               BAD_CAST md->checksum_open);
        xmlNewProp(node, BAD_CAST "type", BAD_CAST md->checksum_open_type);
    }

    node = xmlNewChild(data, NULL, BAD_CAST "location", NULL);
    xmlNewProp(node, BAD_CAST "href", BAD_CAST md->location_href);

    g_snprintf(str_buffer, STR_BUFFER_SIZE, "%ld", md->timestamp);
    xmlNewChild(data, NULL, BAD_CAST "timestamp", BAD_CAST str_buffer);

    g_snprintf(str_buffer, STR_BUFFER_SIZE, "%ld", md->size);
    xmlNewChild(data, NULL, BAD_CAST "size", BAD_CAST str_buffer);

    if (md->size_open != -1) {
        g_snprintf(str_buffer, STR_BUFFER_SIZE, "%ld", md->size_open);
        xmlNewChild(data, NULL, BAD_CAST "open-size", BAD_CAST str_buffer);
    }

    if (g_str_has_suffix((char *)type, "_db")) {
        g_snprintf(str_buffer, STR_BUFFER_SIZE, "%d", md->db_ver);
        xmlNewChild(data, NULL, BAD_CAST "database_version",
                    BAD_CAST str_buffer);
    }
}


char *
repomd_xml_dump(cr_Repomd repomd)
{
    xmlDocPtr doc;
    xmlNodePtr root;


    // Start of XML document

    doc = xmlNewDoc(BAD_CAST "1.0");
    root = xmlNewNode(NULL, BAD_CAST "repomd");
    xmlNewNs(root, BAD_CAST XMLNS_NS, BAD_CAST NULL);
    xmlNewNs(root, BAD_CAST RPM_NS, BAD_CAST "rpm");
    xmlDocSetRootElement(doc, root);

    xmlNewChild(root, NULL, BAD_CAST "revision", BAD_CAST repomd->revision);


    // Tags

    if (repomd->repo_tags || repomd->distro_tags || repomd->content_tags) {
        GSList *element;
        xmlNodePtr tags = xmlNewChild(root, NULL, BAD_CAST "tags", NULL);
        element = repomd->content_tags;
        for (; element; element = g_slist_next(element))
            xmlNewChild(tags, NULL,
                        BAD_CAST "content",
                        BAD_CAST (element->data ? element->data : ""));
        element = repomd->repo_tags;
        for (; element; element = g_slist_next(element))
            xmlNewChild(tags, NULL,
                        BAD_CAST "repo",
                        BAD_CAST (element->data ? element->data : ""));
        element = repomd->distro_tags;
        for (; element; element = g_slist_next(element)) {
            xmlNodePtr distro_elem;
            cr_Distro distro = (cr_Distro) element->data;
            distro_elem = xmlNewChild(tags,
                                      NULL,
                                      BAD_CAST "distro",
                                      BAD_CAST (distro->val ? distro->val : ""));
            if (distro->cpeid)
                xmlNewProp(distro_elem,
                           BAD_CAST "cpeid",
                           BAD_CAST distro->cpeid);
        }
    }

    dump_data_items(root, repomd->pri_xml, (const xmlChar *) "primary");
    dump_data_items(root, repomd->fil_xml, (const xmlChar *) "filelists");
    dump_data_items(root, repomd->oth_xml, (const xmlChar *) "other");
    dump_data_items(root, repomd->pri_sql, (const xmlChar *) "primary_db");
    dump_data_items(root, repomd->fil_sql, (const xmlChar *) "filelists_db");
    dump_data_items(root, repomd->oth_sql, (const xmlChar *) "other_db");
    dump_data_items(root, repomd->groupfile, (const xmlChar *) "group");
    dump_data_items(root, repomd->cgroupfile, (const xmlChar *) "group_gz");
    dump_data_items(root, repomd->updateinfo, (const xmlChar *) "updateinfo");


    // Dump IT!

    char *result;
    xmlDocDumpFormatMemoryEnc(doc,
                              (xmlChar **) &result,
                              NULL,
                              XML_ENC,
                              FORMAT_XML);


    // Clean up

    xmlFreeDoc(doc);

    return result;
}


void
cr_rename_repomdrecord_file(const char *base_path, cr_RepomdRecord md)
{
    if (!md || !(md->location_href) || !strlen(md->location_href)) {
        return;
    }

    if (!md->checksum) {
        return;
    }

    gchar *path = g_strconcat(base_path, "/", md->location_href, NULL);
    gchar *location_href_path_prefix = NULL;
    const gchar *location_href_filename_only = NULL;

    int x = strlen(md->location_href);
    for (; x > 0; x--) {
        if (md->location_href[x] == '/') {
            location_href_path_prefix = g_strndup(md->location_href, x+1);
            location_href_filename_only = cr_get_filename(md->location_href+x+1);
            break;
        }
    }

    // Check if the rename is necessary
    // During update with --keep-all-metadata some files (groupfile,
    // updateinfo, ..) could already have checksum in filenames
    if (g_str_has_prefix(location_href_filename_only, md->checksum)) {
        // The filename constains checksum and it is current
        g_free(location_href_path_prefix);
        g_free(path);
        return;
    }

    // Skip existing obsolete checksum in the name if there is any
    int len = strlen(location_href_filename_only);
    if (len > 32) {
        // The filename is long -> it could contains a checksum
        for (x = 0; x < len; x++) {
            if (location_href_filename_only[x] == '-' && (
                   x == 32  // Prefix is MD5 checksum
                || x == 40  // Prefix is SHA1 checksum
                || x == 64  // Prefix is SHA256 checksum
                || x == 128 // Prefix is SHA512 checksum
               ))
            {
                location_href_filename_only = location_href_filename_only + x + 1;
                break;
            }
        }
    }

    // Prepare new name and path
    gchar *new_location_href = g_strconcat(location_href_path_prefix,
                                           md->checksum,
                                           "-",
                                           location_href_filename_only,
                                           NULL);
    gchar *new_path = g_strconcat(base_path, "/", new_location_href, NULL);

    g_free(location_href_path_prefix);

    if (g_file_test (new_path, G_FILE_TEST_EXISTS)) {
        if (remove(new_path)) {
            g_critical(MODULE"%s: Cannot delete old %s", __func__, new_path);
            g_free(path);
            g_free(new_location_href);
            g_free(new_path);
            return;
        }
    }
    if (rename(path, new_path)) {
        g_critical(MODULE"%s: Cannot rename %s to %s", __func__, path, new_path);
        g_free(path);
        g_free(new_location_href);
        g_free(new_path);
        return;
    }

    md->location_href = g_string_chunk_insert(md->chunk, new_location_href);

    g_free(path);
    g_free(new_path);
    g_free(new_location_href);
}


cr_Repomd
cr_new_repomd()
{
    return (cr_Repomd) g_malloc0(sizeof(struct _cr_Repomd));
}


void
cr_free_repomd(cr_Repomd repomd)
{
    if (!repomd) return;
    cr_free_repomdrecord(repomd->pri_xml);
    cr_free_repomdrecord(repomd->fil_xml);
    cr_free_repomdrecord(repomd->oth_xml);
    cr_free_repomdrecord(repomd->pri_sql);
    cr_free_repomdrecord(repomd->fil_sql);
    cr_free_repomdrecord(repomd->oth_sql);
    cr_free_repomdrecord(repomd->groupfile);
    cr_free_repomdrecord(repomd->cgroupfile);
    cr_free_repomdrecord(repomd->updateinfo);
    g_slist_free_full(repomd->repo_tags, g_free);
    g_slist_free_full(repomd->distro_tags, (GDestroyNotify) cr_free_distro);
    g_slist_free_full(repomd->content_tags, g_free);
    g_free(repomd->revision);
    g_free(repomd);
}


void
cr_repomd_set_record(cr_Repomd repomd,
                     cr_RepomdRecord record,
                     cr_RepomdRecordType type)
{
    cr_RepomdRecord *rec;

    if (!repomd || !record) return;

    switch (type) {
        case CR_MD_PRIMARY_XML:          rec = &(repomd->pri_xml);    break;
        case CR_MD_FILELISTS_XML:        rec = &(repomd->fil_xml);    break;
        case CR_MD_OTHER_XML:            rec = &(repomd->oth_xml);    break;
        case CR_MD_PRIMARY_SQLITE:       rec = &(repomd->pri_sql);    break;
        case CR_MD_FILELISTS_SQLITE:     rec = &(repomd->fil_sql);    break;
        case CR_MD_OTHER_SQLITE:         rec = &(repomd->oth_sql);    break;
        case CR_MD_GROUPFILE:            rec = &(repomd->groupfile);  break;
        case CR_MD_COMPRESSED_GROUPFILE: rec = &(repomd->cgroupfile); break;
        case CR_MD_UPDATEINFO:           rec = &(repomd->updateinfo); break;
        default: return;
    }

    if (*rec)  // A record already exists
        cr_free_repomdrecord(*rec);

    *rec = record;
}


void
cr_repomd_set_revision(cr_Repomd repomd, const char *revision)
{
    if (!repomd) return;
    if (repomd->revision)  // A revision string already exists
        g_free(repomd->revision);
    repomd->revision = g_strdup(revision);
}


void
cr_repomd_add_distro_tag(cr_Repomd repomd, const char *cpeid, const char *tag)
{
    cr_Distro distro;
    if (!repomd || !tag) return;
    distro = cr_new_distro();
    distro->cpeid = g_strdup(cpeid);
    distro->val   = g_strdup(tag);
    repomd->distro_tags = g_slist_prepend(repomd->distro_tags,
                                          (gpointer) distro);
}


void
cr_repomd_add_repo_tag(cr_Repomd repomd, const char *tag)
{
    if (!repomd) return;
    repomd->repo_tags = g_slist_append(repomd->repo_tags, g_strdup(tag));
}


void
cr_repomd_add_content_tag(cr_Repomd repomd, const char *tag)
{
    if (!repomd) return;
    repomd->content_tags = g_slist_append(repomd->content_tags, g_strdup(tag));
}


gchar *
cr_generate_repomd_xml(cr_Repomd repomd)
{
    if (!repomd->revision) {
        gchar *rev = g_strdup_printf("%ld", time(NULL));
        cr_repomd_set_revision(repomd, rev);
        g_free(rev);
    }

    return repomd_xml_dump(repomd);
}
