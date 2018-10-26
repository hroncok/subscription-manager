/**
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This software is licensed to you under the GNU General Public License,
 * version 2 (GPLv2). There is NO WARRANTY for this software, express or
 * implied, including the implied warranties of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. You should have received a copy of GPLv2
 * along with this software; if not, see
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * Red Hat trademarks are not licensed under GPLv2. No permission is
 * granted to use or replicate Red Hat trademarks that are incorporated
 * in this software or its documentation.
 */
#include <libdnf/plugin/plugin.h>
#include <libdnf/libdnf.h>

#include <glib/gstdio.h>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/err.h>

#include <json-c/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <zlib.h>
#include <errno.h>

#include "util.h"
#include "product-id.h"

const PluginInfo *pluginGetInfo() {
    return &pinfo;
}

/**
 * Initialize handle of this plugin
 * @param version
 * @param mode
 * @param initData
 * @return
 */
PluginHandle *pluginInitHandle(int version, PluginMode mode, void *initData) {
    debug("%s initializing handle!", pinfo.name);

    PluginHandle* handle = malloc(sizeof(PluginHandle));

    if (handle) {
        handle->version = version;
        handle->mode = mode;
        handle->initData = initData;
    }

    return handle;
}

/**
 * Free handle and all
 * @param handle
 */
void pluginFreeHandle(PluginHandle *handle) {
    debug("%s freeing handle!", pinfo.name);

    if (handle) {
        free(handle);
    }
}

/**
 * This function tries to remove unused product certificates.
 *
 * @param repoMap hash map of active repositories
 * @return
 */
int removeUnusedProductCerts(GHashTable *repoMap) {
    // "Open" directory with product certificates
    GError *tmp_err = NULL;
    GDir* productDir = g_dir_open(PRODUCT_CERT_DIR, 0, &tmp_err);
    if (productDir != NULL) {
        const gchar *file_name = NULL;
        do {
            // Read all files in the directory. When file_name is NULL, then
            // it usually means that there is no more file.
            file_name = g_dir_read_name(productDir);
            if(file_name != NULL) {
                if(g_str_has_suffix(file_name, ".pem") == TRUE) {
                    gchar *product_id = g_strndup(file_name, strlen(file_name) - 4);
                    gboolean is_num = TRUE;
                    // Test if string represents number
                    for(int i=0; i<strlen(product_id); i++) {
                        if (g_ascii_isdigit(product_id[i]) != TRUE) {
                            is_num = FALSE;
                            break;
                        }
                    }
                    if (is_num != TRUE) {
                        debug("Name of product certificate is wrong (not digits only): %s. Skipping.", file_name);
                    }
                    // When product certificate is not in the hash table of active repositories
                    // then it is IMHO possible to remove this product certificate
                    else if (g_hash_table_contains(repoMap, product_id) == FALSE) {
                        gchar *abs_file_name = g_strconcat(PRODUCT_CERT_DIR, file_name, NULL);
                        debug("Removing product certificate: %s", abs_file_name);
                        int ret = g_remove(abs_file_name);
                        if (ret == -1) {
                            error("Unable to remove product certificate: %s", abs_file_name);
                        }
                        g_free(abs_file_name);
                    }
                    g_free(product_id);
                }
            } else if (errno != 0 && errno != ENODATA && errno != EEXIST) {
                error("Unable to read content of %s directory, %d, %s", PRODUCT_CERT_DIR, errno, strerror(errno));
            }
        } while(file_name != NULL);
        g_dir_close(productDir);
    } else {
        printError("Unable to open directory with product certificates", tmp_err);
    }
    return 0;
}

/**
 * Returns string representation of pluginHookdId. There is no need to free string, because it is statically
 * allocated.
 *
 * @param id Id of plugin hook
 * @return String representing hook
 */
gchar *strHookId(PluginHookId id) {
    switch(id) {
        case PLUGIN_HOOK_ID_CONTEXT_PRE_CONF:
            return "CONTEXT_PRE_CONF";
        case PLUGIN_HOOK_ID_CONTEXT_CONF:
            return "CONTEXT_CONF";
        case PLUGIN_HOOK_ID_CONTEXT_PRE_TRANSACTION:
            return "PRE_TRANSACTION";
        case PLUGIN_HOOK_ID_CONTEXT_TRANSACTION:
            return "CONTEXT_TRANSACTION";
        case PLUGIN_HOOK_ID_CONTEXT_PRE_REPOS_RELOAD:
            return "CONTEXT_PRE_REPOS_RELOAD";
        default:
            return "UNKNOWN";
    }
}

/**
 * Callback function. This method is executed for every libdnf hook. This callback
 * is called several times during transaction, but we are interested only in one situation.
 *
 * @param handle Pointer on structure with data specific for this plugin
 * @param id Id of hook (moment of transaction, when this callback is called)
 * @param hookData
 * @param error
 * @return
 */
int pluginHook(PluginHandle *handle, PluginHookId id, void *hookData, PluginHookError *error) {
    if (!handle) {
        // We must have failed to allocate our handle during init; don't do anything.
        return 0;
    }

    debug("%s v%s, running hook_id: %s on DNF version %d",
            pinfo.name, pinfo.version, strHookId(id), handle->version);

    if (id == PLUGIN_HOOK_ID_CONTEXT_TRANSACTION) {
        // Get DNF context
        DnfContext *dnfContext = handle->initData;
        // List of all repositories
        GPtrArray *repos = dnf_context_get_repos(dnfContext);
        // List of enabled repositories
        GPtrArray *enabledRepos = g_ptr_array_sized_new(repos->len);
        // Enabled repositories with product id certificate
        GPtrArray *repoAndProductIds = g_ptr_array_sized_new(repos->len);
        // Enabled repositories with prouctid cert that are actively used
        GPtrArray *activeRepoAndProductIds = g_ptr_array_sized_new(repos->len);

        getEnabled(repos, enabledRepos);

        for (int i = 0; i < enabledRepos->len; i++) {
            DnfRepo *repo = g_ptr_array_index(enabledRepos, i);
            LrResult *lrResult = dnf_repo_get_lr_result(repo);
            LrYumRepoMd *repoMd;
            GError *tmp_err = NULL;

            debug("Enabled: %s", dnf_repo_get_id(repo));
            lr_result_getinfo(lrResult, &tmp_err, LRR_YUM_REPOMD, &repoMd);
            if (tmp_err) {
                printError("Unable to get information about repository", tmp_err);
            }
            else {
                LrYumRepoMdRecord *repoMdRecord = lr_yum_repomd_get_record(repoMd, "productid");
                if (repoMdRecord) {
                    debug("Repository %s has a productid", dnf_repo_get_id(repo));
                    RepoProductId *repoProductId = (RepoProductId*)malloc(sizeof(RepoProductId));
                    // TODO: do not fetch productid certificate, when dnf context is
                    // set to cache-only mode. Microdnf not package do not support this
                    // feature ATM
                    gboolean cache_only = dnf_context_get_cache_only(dnfContext);
                    if (cache_only == TRUE) {
                        debug("DNF context is set to: cache-only");
                    } else {
                        debug("DNF context is NOT set to: cache-only");
                    }
                    int fetchSuccess = fetchProductId(repo, repoProductId);
                    if(fetchSuccess == 1) {
                        g_ptr_array_add(repoAndProductIds, repoProductId);
                    } else {
                        free(repoProductId);
                    }
                }
            }
        }

        getActive(dnfContext, repoAndProductIds, activeRepoAndProductIds);

        if (activeRepoAndProductIds->len) {
            // open file
        }

        GHashTable *repoMap = g_hash_table_new(g_str_hash, g_str_equal);
        for (int i = 0; i < activeRepoAndProductIds->len; i++) {
            RepoProductId *activeRepoProductId = g_ptr_array_index(activeRepoAndProductIds, i);
            debug("Handling active repo %s\n", dnf_repo_get_id(activeRepoProductId->repo));
            installProductId(activeRepoProductId, repoMap);
        }

        // Handle removals here
        removeUnusedProductCerts(repoMap);

        // RepoMap is now a GHashTable with each product ID mapping to a GList of the repoId's associated
        // with that product.
        writeRepoMap(repoMap);

        // We have to free memory allocated for all items of repoAndProductIds. This should also handle
        // activeRepoAndProductIds since the pointers in that array are pointing to the same underlying
        // values at repoAndProductIds.
        for (int i=0; i < repoAndProductIds->len; i++) {
            RepoProductId *repoProductId = g_ptr_array_index(repoAndProductIds, i);
            free(repoProductId);
        }

        g_hash_table_foreach(repoMap, (GHFunc) clearMyTable, NULL);
        g_hash_table_destroy(repoMap);
        g_ptr_array_unref(repos);
        g_ptr_array_unref(enabledRepos);
        g_ptr_array_unref(repoAndProductIds);
        g_ptr_array_unref(activeRepoAndProductIds);
    }

    return 1;
}

void writeRepoMap(GHashTable *repoMap) {
    json_object *productIdDb = json_object_new_object();

    GList *keys = g_hash_table_get_keys(repoMap);

    // TODO: This is a terrible way to traverse a linked list
    for (guint i = 0; i < g_list_length(keys); i++) {
        char *productId = g_list_nth_data(keys, i);
        json_object *repoIdJson = json_object_new_array();
        GList *values = g_hash_table_lookup(repoMap, productId);

        for (guint j = 0; j < g_list_length(values); j++) {
            char *repoId = g_list_nth_data(values, j);
            json_object_array_add(repoIdJson, json_object_new_string(repoId));
        }
        json_object_object_add(productIdDb, productId, repoIdJson);
    }

    debug("JSON is %s", json_object_to_json_string(productIdDb));

    FILE *productdb_file = fopen(PRODUCTDB_FILE, "w");
    if (productdb_file != NULL) {
        fprintf(productdb_file, "%s", json_object_to_json_string(productIdDb));
        fclose(productdb_file);
    } else {
        error("Unable to write productdb to file: %s", PRODUCTDB_FILE);
    }

    g_list_free(keys);
    json_object_put(productIdDb);
}

/**
 * Find the list of repos that are actually enabled
 * @param repos all available repos
 * @param enabledRepos the list of enabled repos
 */
void getEnabled(const GPtrArray *repos, GPtrArray *enabledRepos) {
    for (int i = 0; i < repos->len; i++) {
        DnfRepo* repo = g_ptr_array_index(repos, i);
        bool enabled = (dnf_repo_get_enabled(repo) & DNF_REPO_ENABLED_PACKAGES) > 0;
        if (enabled) {
            g_ptr_array_add(enabledRepos, repo);
        }
    }
}

/**
 * Find the list of repos that provide packages that are actually installed.
 * @param repos all available repos
 * @param activeRepoAndProductIds the list of repos providing active
 */
void getActive(DnfContext *context, const GPtrArray *repoAndProductIds, GPtrArray *activeRepoAndProductIds) {
    DnfSack *dnfSack = dnf_context_get_sack(context);

    // Create special sack object only for quering current rpmdb to get fresh list
    // of installed packages. Quering dnfSack would not include just installed RPM
    // package(s) or it would still include just removed package(s).
    DnfSack *rpmDbSack = dnf_sack_new();
    if(rpmDbSack == NULL) {
        error("Unable to create new sack object for quering rpmdb");
        return;
    }

    GError *tmp_err = NULL;
    gboolean ret;
    ret = dnf_sack_setup(rpmDbSack, 0, &tmp_err);
    if (ret == FALSE) {
        printError("Unable to setup new sack object", tmp_err);
    }

    ret = dnf_sack_load_system_repo(rpmDbSack, NULL, 0, &tmp_err);
    if (ret == FALSE) {
        printError("Unable to load system repo to sack object", tmp_err);
    }

    HyQuery query = hy_query_create_flags(rpmDbSack, 0);
    hy_query_filter(query, HY_PKG_NAME, HY_GLOB, "*");
    hy_query_filter(query, HY_REPO_NAME, HY_EQ, HY_SYSTEM_REPO_NAME);

    GPtrArray *packageList = hy_query_run(query);
    GPtrArray *installedPackages = g_ptr_array_sized_new(packageList->len);
    hy_query_free(query);

    for (int i = 0; i < packageList->len; i++) {
        DnfPackage *pkg = g_ptr_array_index(packageList, i);

        if (dnf_package_installed(pkg)) {
            // debug("Installed package: %s", dnf_package_get_nevra(pkg));
            g_ptr_array_add(installedPackages, pkg);
        }
    }

    for (int i = 0; i < repoAndProductIds->len; i++) {
        RepoProductId *repoProductId = g_ptr_array_index(repoAndProductIds, i);
        DnfRepo *repo = repoProductId->repo;
        HyQuery availQuery = hy_query_create_flags(dnfSack, 0);
        hy_query_filter(availQuery, HY_PKG_REPONAME, HY_EQ, dnf_repo_get_id(repo));
        GPtrArray *availPackageList = hy_query_run(availQuery);
        hy_query_free(availQuery);

        // NB: Another way to do this would be with a bloom filter.  A bloom filter can give a very quick,
        // accurate answer to the question "Is this item not in this set of things?" if the result is
        // negative.  A positive result is probabilistic and requires a second full scan of the set to get the
        // ultimate answer.  The bloom filter approach would eliminate the need for the O(n^2) nested for-loop
        // solution we have.

        // Go through all available packages from repository
        for (int j = 0; j < availPackageList->len; j++) {
            DnfPackage *pkg = g_ptr_array_index(availPackageList, j);
            gboolean package_found = FALSE;

            // Try to find if this available package is in the list of installed packages
            for(int k = 0; k < installedPackages->len; k++) {
                DnfPackage *instPkg = g_ptr_array_index(installedPackages, k);
                if(strcmp(dnf_package_get_nevra(pkg), dnf_package_get_nevra(instPkg)) == 0) {
                    debug("Repo \"%s\" marked active due to installed package %s",
                           dnf_repo_get_id(repo),
                           dnf_package_get_nevra(pkg));
                    g_ptr_array_add(activeRepoAndProductIds, repoProductId);
                    package_found = TRUE;
                    break;
                }
            }

            if(package_found == TRUE) {
                break;
            }
        }
        g_ptr_array_unref(availPackageList);
    }

    g_ptr_array_unref(installedPackages);
    g_ptr_array_unref(packageList);
    g_object_unref(rpmDbSack);
}

void printError(const char *msg, GError *err) {
    error("%s, error: %d: %s", msg, err->code, err->message);
    g_error_free(err);
}

static void copy_lr_val(LrVar *lr_val, LrUrlVars **newVarSubst) {
    *newVarSubst = lr_urlvars_set(*newVarSubst, lr_val->var, lr_val->val);
}

int fetchProductId(DnfRepo *repo, RepoProductId *repoProductId) {
    int ret = 0;
    GError *tmp_err = NULL;
    LrHandle *lrHandle = dnf_repo_get_lr_handle(repo);
    LrResult *lrResult = dnf_repo_get_lr_result(repo);

    // getinfo uses the LRI* constants while setopt uses LRO*
    char *destdir;
    lr_handle_getinfo(lrHandle, &tmp_err, LRI_DESTDIR, &destdir);
    if (tmp_err) {
        printError("Unable to get information about destination folder", tmp_err);
    }

    char **urls = NULL;
    lr_handle_getinfo(lrHandle, &tmp_err, LRI_URLS, &urls);
    if (tmp_err) {
        printError("Unable to get information about URLs", tmp_err);
    }

    // Getting information about variable substitution
    LrUrlVars *varSubst = NULL;
    lr_handle_getinfo(lrHandle, &tmp_err, LRI_VARSUB, &varSubst);
    if (tmp_err) {
        printError("Unable to get variable substitution for URL", tmp_err);
    }

    // It is necessary to create copy of list of URL variables to avoid memory leaks
    // Two handles cannot share same GSList
    LrUrlVars *newVarSubst = NULL;
    g_slist_foreach(varSubst, (GFunc)copy_lr_val, &newVarSubst);

    /* Set information on our LrHandle instance.  The LRO_UPDATE option is to tell the LrResult to update the
     * repo (i.e. download missing information) rather than attempt to replace it.
     *
     * FIXME: The internals of this are unclear.  Do we need to create our own LrHandle instance or could we
     * use the one provided and just modify the download list?  Is reusing the LrResult going to cause
     * problems?
     */
    char *downloadList[] = {"productid", NULL};
    LrHandle *h = lr_handle_init();
    lr_handle_setopt(h, NULL, LRO_YUMDLIST, downloadList);
    lr_handle_setopt(h, NULL, LRO_URLS, urls);
    lr_handle_setopt(h, NULL, LRO_REPOTYPE, LR_YUMREPO);
    lr_handle_setopt(h, NULL, LRO_DESTDIR, destdir);
    lr_handle_setopt(h, NULL, LRO_VARSUB, newVarSubst);
    lr_handle_setopt(h, NULL, LRO_UPDATE, TRUE);

    if(urls != NULL) {
        int url_id = 0;
        do {
            debug("Downloading metadata from: %s to %s", urls[url_id], destdir);
            url_id++;
        } while(urls[url_id] != NULL);
    }
    gboolean handleSuccess = lr_handle_perform(h, lrResult, &tmp_err);
    if (handleSuccess) {
        LrYumRepo *lrYumRepo = lr_yum_repo_init();
        if (lrYumRepo != NULL) {
            lr_result_getinfo(lrResult, &tmp_err, LRR_YUM_REPO, &lrYumRepo);
            if (tmp_err) {
                printError("Unable to get information about repository", tmp_err);
            }
            repoProductId->repo = repo;
            repoProductId->productIdPath = lr_yum_repo_path(lrYumRepo, "productid");
            info("Product id cert downloaded metadata from repo %s to %s",
                 dnf_repo_get_id(repo),
                 repoProductId->productIdPath);
            ret = 1;
            // Causes a segfault.  LrYumRepo isn't inited properly or something and the LrYumRepoPaths in it
            // are FUBAR
            // lr_yum_repo_free(lrYumRepo);
            // lrYumRepo = NULL;
        } else {
            error("Unable to initialize LrYumRepo");
        }
    } else {
        printError("Unable to download product certificate", tmp_err);
    }

    if(urls) {
        int url_id = 0;
        do {
            free(urls[url_id]);
            url_id++;
        } while(urls[url_id] != NULL);
        free(urls);
        urls = NULL;
    }
    lr_handle_free(h);
    return ret;
}

int installProductId(RepoProductId *repoProductId, GHashTable *repoMap) {
    int ret = 0;

    const char *productIdPath = repoProductId->productIdPath;
    gzFile input = gzopen(productIdPath, "r");

    if (input != NULL) {
        GString *pemOutput = g_string_new("");
        GString *outname = g_string_new("");

        debug("Decompressing product certificate");
        int decompressSuccess = decompress(input, pemOutput);
        if (decompressSuccess == FALSE) {
            goto out;
        }
        debug("Decompressing of certificate finished with status: %d", ret);
        debug("Content of product cert:\n%s", pemOutput->str);

        int productIdFound = findProductId(pemOutput, outname);
        if (productIdFound) {
            char *mapKey = g_strdup(outname->str);
            g_string_prepend(outname, PRODUCT_CERT_DIR);
            g_string_append(outname, ".pem");
            FILE *fileOutput = fopen(outname->str, "w+");
            if (fileOutput != NULL) {
                debug("Content of certificate written to: %s", outname->str);
                fprintf(fileOutput, "%s", pemOutput->str);
                fclose(fileOutput);

                gpointer valueList = g_hash_table_lookup(repoMap, mapKey);
                g_hash_table_insert(
                    repoMap,
                    mapKey,
                    g_list_prepend(valueList, (gpointer) dnf_repo_get_id(repoProductId->repo)
                ));

                ret = 1;
            } else {
                error("Unable write to file with certificate file :%s", outname->str);
            }
        }

        out:
        g_string_free(outname, TRUE);
        g_string_free(pemOutput, TRUE);
    }

    if(input != NULL) {
        gzclose(input);
    } else {
        debug("Unable to open compressed product certificate");
    }

    return ret;
}

/**
 * Look at the PEM of a certificate and figure out what is ID of the product.
 *
 * @param certContent String containing content of product certificate
 * @param result String containing ID of of product certificate
 * @return
 */
int findProductId(GString *certContent, GString *result) {
    BIO *bio = BIO_new_mem_buf(certContent->str, (int) certContent->len);
    if (bio == NULL) {
        debug("Unable to create buffer for content of certificate: %s",
                ERR_error_string(ERR_get_error(), NULL));
        return -1;
    }
    X509 *x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    if (x509 == NULL) {
        debug("Failed to read content of certificate from buffer to X509 structure: %s",
                ERR_error_string(ERR_get_error(), NULL));
        return -1;
    }
    BIO_free(bio);
    bio = NULL;

    int exts = X509_get_ext_count(x509);
    for (int i = 0; i < exts; i++) {
        char oid[MAX_BUFF];
        X509_EXTENSION *ext = X509_get_ext(x509, i);
        if (ext == NULL) {
            debug("Failed to get extension of X509 structure: %s",
                  ERR_error_string(ERR_get_error(), NULL));
            return -1;
        }
        OBJ_obj2txt(oid, MAX_BUFF, X509_EXTENSION_get_object(ext), 1);

        if (strncmp(REDHAT_PRODUCT_OID, oid, strlen(REDHAT_PRODUCT_OID)) == 0) {
            gchar **components = g_strsplit(oid, ".", -1);
            int comp_id=0;
            // Because g_strsplit() returns array of NULL terminated pointers,
            // then we have to make sure that array is long enough to contain
            // required ID
            while(components[comp_id] != NULL) {
                comp_id++;
            }
            debug("Number of OID components: %d", comp_id);
            if (comp_id > 9) {
                debug("ID of product certificate: %s", (char*)components[9]);
                g_string_assign(result, components[9]);
            } else {
                error("Product certificate does not contain required ID");
            }
            g_strfreev(components);
            break;
        }
    }

    X509_free(x509);

    return 1;
}

/**
 * Decompress product certificate
 *
 * @param input This is pointer at input compressed file
 * @param output Pointer at string, where content of certificate will be stored
 * @return Return TRUE, when decompression was successful. Otherwise return FALSE.
 */
int decompress(gzFile input, GString *output) {
    int ret = TRUE;
    while (1) {
        int err;
        int bytes_read;
        unsigned char buffer[CHUNK];
        bytes_read = gzread(input, buffer, CHUNK - 1);
        buffer[bytes_read] = '\0';
        g_string_printf(output, "%s", buffer);
        if (bytes_read < CHUNK - 1) {
            if (gzeof (input)) {
                break;
            }
            else {
                const char * error_string;
                error_string = gzerror(input, & err);
                if (err) {
                    error("Decompressing failed with error: %s.", error_string);
                    ret = FALSE;
                    break;
                }
            }
        }
    }
    return ret;
}

void clearMyTable(gpointer key, gpointer value, gpointer data) {
    g_free(key);
    g_slist_free(value);
}