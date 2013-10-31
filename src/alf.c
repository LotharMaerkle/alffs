/*
   FUSE Alfresco adapter
   */

// strptime
#define _XOPEN_SOURCE 700

// see fuse.h
#define FUSE_USE_VERSION 26

// O_DIRECT, O_LARGEFILE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <attr/xattr.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#include <syslog.h>
#include <fuse.h>
#include <fuse_opt.h>
#include <curl/curl.h>
#include <json/json.h>
#include <time.h>
#include <pthread.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <uthash.h>
#include <utlist.h>
#include <dirent.h>

/*
 * Holds a reference to nodes.
 */
struct alf_ref {
	const char * uuid; // nodes remote uuid
	const char * path; // fs remote path
	int flags; // open flags
	int cachefd; // fd of locally cached file
	bool dirty; // was cached filed modified and needs to be updated on the remote side?
	int opens; // open counter
	UT_hash_handle hh; // ut libs handle
};


struct alf_stat_op {
	const char * op;
	struct alf_stat_op * next;
};

/*
 * Used to test out open behaviours and collect some stats.
 *
 */
struct alf_stat_record {
	const char * path;
	int opens;
	int closes;
	int writes;
	int reads;
	pid_t pid;
	char * opslist;
	UT_hash_handle hh;
};

// con cache
struct alf_con_cache {
	CURL * curl;
	struct alf_con_cache * next;
};



// caches, hashes, lists and locks
static struct alf_stat_record * alf_stats = NULL;
static pthread_rwlock_t alf_stats_lock;
static struct alf_ref * alf_refs = NULL;
static pthread_rwlock_t alf_refs_lock;
static struct alf_con_cache * alf_con_cache = NULL;
static pthread_rwlock_t alf_con_cache_lock;
// openssl thread locking support
static pthread_mutex_t * openssl_mutex_buf= NULL;
// housekeeping thread mutex
static pthread_mutex_t hk_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hk_wait_condition = PTHREAD_COND_INITIALIZER;

/*
 * required connection and endpoint to alfresco
 *
 */
struct alf_context {
	char * userid;
	char * password;
	// plain hostname
	char * host;
	// either http or https
	char * protocol;
	// port
	int port;
	// io module api base, defaults to /alfresco/service/ecm4u/io
	char * io_base;
	// noderef or alfresco url to represent the remote mountpoint
	char * mount_base;
	// some debug flags
	int debug;
	// http network timeouts
	int timeout;
	// local cache dir
	char * cache_dir;
	// precomputed endpoint
	char * endpoint;
	// connection cache size
	int con_cache_max;
	// housekeeping cancelRequest
	int hk_cancelRequest;
	// housekeeping thread
	pthread_t cache_housekeeping_thread;
	// housekeeping sleep time seconds
	int hk_sleepseconds;
	// housekeeping cache max time
	int hk_maxcacheseconds;
};

struct json_parse_helper {
	json_tokener * tokener;
	json_object * object;
};

struct reader_helper {
	char * target;
	size_t written;
	size_t size;
};

struct file_reader_helper {
	int fd;
	size_t written;
};


// mount options
#define ALFFS_OPT(t, p, v) { t, offsetof(struct alf_context, p), v }
static struct fuse_opt alf_mount_options[] = {
	ALFFS_OPT("alfuserid=%s", userid, 0),
	ALFFS_OPT("alfpassword=%s", password, 0),
	ALFFS_OPT("alfprotocol=%s", protocol, 0),
	ALFFS_OPT("alfhost=%s", host, 0),
	ALFFS_OPT("alfport=%i", port, 0),
	ALFFS_OPT("alfiobase=%s", io_base, 0),
	ALFFS_OPT("alfmountbase=%s", mount_base, 0),
	ALFFS_OPT("alfcachedir=%s", cache_dir, 0),
	ALFFS_OPT("alfdebug=%i", debug, 0),
	ALFFS_OPT("alftimeout=%i", timeout, 0),
	ALFFS_OPT("alfconcachemax=%i", con_cache_max, 0),
	ALFFS_OPT("alfhksleep=%i", hk_sleepseconds, 0),
	ALFFS_OPT("alfhkcachemax=%i", hk_maxcacheseconds, 0),

	FUSE_OPT_END
};

// save sprintf version for arbitrary sized result strings
// caller must free the returned char *
static char * alf_sprintf(const char * format, ...)
{
	size_t size = 0;
	int written = 0;
	char * buffer = NULL;
	while(written >= size) {
		size += 128;
		if(buffer != NULL) {
			free(buffer);
		}
		buffer = (char *)malloc(size);
		va_list args;
		va_start (args, format);
		written = vsnprintf(buffer, size, format, args);
		va_end (args);
	}
	return buffer;
}

// stat recorder
static struct alf_stat_record * alf_record_stats(const char * path, const char * op)
{
	struct alf_stat_record * stat = NULL;
	if(!pthread_rwlock_wrlock(&alf_stats_lock)) {
		HASH_FIND_STR(alf_stats, path, stat);
		if(!stat) {
			stat = (struct alf_stat_record*)malloc(sizeof(struct alf_stat_record));
			stat->path = alf_sprintf("%s", path);
			stat->opens = 0;
			stat->writes = 0;
			stat->reads = 0;
			stat->closes = 0;
			stat->opslist = strdup(op);
			HASH_ADD_KEYPTR( hh, alf_stats, stat->path, strlen(stat->path), stat );	
		} else {
			char * oldlist = stat->opslist;
			stat->opslist = alf_sprintf("%s,%s", oldlist, op);
			free(oldlist);
		}
		pthread_rwlock_unlock(&alf_stats_lock);
	}

	return stat;
}

static size_t receive_json(void *buffer, size_t size, size_t nmemb, void *userp)
{
	//syslog(LOG_DEBUG, "received data: size=%zu, nmemb=%zu", size, nmemb);
	char* text = (char *)buffer;
	//enum json_tokener_error jerr;
	struct json_parse_helper * json_result;
	json_result = (struct json_parse_helper*)userp;

	json_result->object = json_tokener_parse_ex(json_result->tokener, text, size * nmemb);
	return size * nmemb;
}
/*
static size_t receive_read(void *buffer, size_t size, size_t nmemb, void *userp)
{
	syslog(LOG_DEBUG, "received data: size=%zu, nmemb=%zu", size, nmemb);
	struct reader_helper * read_info;

	read_info = (struct reader_helper*)userp;

	size_t n = size * nmemb;
	if(read_info->size < (read_info->written + n)) {
		// to much data
		return 0;
	}

	memcpy(read_info->target + read_info->written, buffer, n);
	read_info->written += n;

	return size * nmemb;
}
*/
static size_t alf_receive_headers( void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t n = size * nmemb;
	struct curl_slist ** header_ptr = (struct curl_slist ** )userdata;
	// header is not zero terminated
	// header has \n\r at end
	char h[n - 2 + 1];
	memcpy(h, ptr, n - 2);
	h[n - 2] = '\0';
	//syslog(LOG_DEBUG, "received header: size=%zu, nmemb=%zu, header=%s", size, nmemb, h);

	*header_ptr = curl_slist_append(*header_ptr, h); 

	return n;
}

static size_t alf_receive_file(void *buffer, size_t size, size_t nmemb, void *userp)
{
	struct file_reader_helper * read_info = NULL;
	ssize_t written = 0;

	syslog(LOG_DEBUG, "received file data: size=%zu, nmemb=%zu", size, nmemb);

	read_info = (struct file_reader_helper*)userp;
	size_t n = size * nmemb;
	written = write(read_info->fd, buffer, n);
	read_info->written += written;

	return written;
}

// caller must free returned string
static char * alf_get_header(struct curl_slist * headers, const char * header)
{
	struct curl_slist * current = headers;
	size_t len = strlen(header);

	while(current != NULL) {
		if(!strncmp(header, current->data, len)) {
			char * colon = strchr(current->data, ':');
			if(colon != NULL) {
				size_t valuelen = strlen(colon + 1);				
				char * value = malloc(valuelen + 1);
				memcpy(value, colon + 1, valuelen);
				value[valuelen] = '\0';
				return value;
			} else {
				return NULL;
			}
		}
		current = current->next;
	}

	return NULL;
}

// map strings to error codes
static int alf_convert_string(const char * str)
{
	if(!strcmp("ENOTEMPTY", str)) {
		return ENOTEMPTY;
	}
	if(!strcmp("ENOTDIR", str))
		return ENOTDIR;

	if(!strcmp("ENOENT", str)) {
		return ENOENT;
	}
	if(!strcmp("EIO", str)) {
		return EIO;
	}
	if(!strcmp("ENOTSUP", str)) {
		return ENOTSUP;
	}
	if(!strcmp("ENOATTR", str)) {
		return ENOATTR;
	}
	if(!strcmp("EEXIST", str)) {
		return EEXIST;
	}
	if(!strcmp("EISDIR", str)) {
		return EISDIR;
	}
	return 0;
}

static int json_get_int(json_object * json_object, const char * key) {
	return json_object_get_int(json_object_object_get(json_object, key));
}
/*
static bool json_get_bool(json_object * json_object, const char * key) {
	return json_object_get_boolean(json_object_object_get(json_object, key)) ? true : false;
}
*/
static const char * json_get_string(json_object * json, const char * key) {
	return json_object_get_string(json_object_object_get(json, key));
}
/*
static int json_get_tm(json_object * json, const char * key, struct tm * datetime) {
	const char * iso = json_get_string(json, key);
	// dates are expected in ISO UTC
	char * result = strptime(iso, "%Y-%m-%dT%H:%M:%S", datetime);
	if(result == NULL) {
		return -1;
	}
	if(result != '\0') {
		return -2;
	}
	return 0;
}
*/

// setup a curl request with auth and debugging
// caller must curl_cleanup
static CURL * alf_create_curl(const struct alf_context * alf_ctx)
{
	CURL * curl = NULL;
	struct alf_con_cache * con_cache = NULL;

	// check for available curl handle for reuse with persistent connections
	pthread_rwlock_wrlock(&alf_con_cache_lock);
        if(alf_con_cache != NULL) {
		syslog(LOG_DEBUG, "reusing a curl handle from the pool");
		curl = alf_con_cache->curl;
		con_cache = alf_con_cache;
		LL_DELETE(alf_con_cache, con_cache);
		free(con_cache);
		curl_easy_reset(curl);
	} else {
		syslog(LOG_DEBUG, "creating a new curl handle");
		curl = curl_easy_init();
	}
	pthread_rwlock_unlock(&alf_con_cache_lock);

	// just basic auth for now
	curl_easy_setopt(curl, CURLOPT_USERNAME, alf_ctx->userid);
	curl_easy_setopt(curl, CURLOPT_PASSWORD, alf_ctx->password);

	// required for threading with ssl
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	if(alf_ctx->debug) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
	}

	return curl;
}

// release a curl handle back to the cache
static void alf_release_curl(const struct alf_context * alf_ctx, CURL * curl) {
	struct alf_con_cache * elt = NULL;
	struct alf_con_cache * con_cache = NULL;
	int count = 0;
	// lock list
	// if list size >= con cache max, unlock list, do curl_cleanup
	// else
	// prepend to list
	// unlock list

	pthread_rwlock_wrlock(&alf_con_cache_lock);
	LL_FOREACH(alf_con_cache, elt) {
		count++;
	}	
	if(count >= alf_ctx->con_cache_max) {
		// throw away if cache is full
		syslog(LOG_DEBUG, "con pool is full, discarding handle: count=%d, max=%d", count, alf_ctx->con_cache_max);
		curl_easy_cleanup(curl);
	} else {
		curl_easy_reset(curl);
		con_cache = (struct alf_con_cache*) malloc(sizeof(struct alf_con_cache)); 
		// freed in cache get
		con_cache->curl = curl;
		LL_PREPEND(alf_con_cache, con_cache);
		syslog(LOG_DEBUG, "added handle back to con pool: count=%d", (count + 1));
	}
	pthread_rwlock_unlock(&alf_con_cache_lock);
}

// json request
// if request is null does a get, else a post
static int alf_perform_json_curl(CURL * curl, const char * url, json_object * request, json_object ** response)
{
	json_tokener * tok = NULL;
	struct json_parse_helper json_result;
	int status = 1;
	long http_code = 400L;
	char curl_error[CURL_ERROR_SIZE];
	int val = EIO;
	const char * json_text = NULL;
	struct curl_slist *headers=NULL;

	tok = json_tokener_new();
	json_result.tokener = tok;
	json_result.object = NULL;

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_json);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json_result);

	if(request != NULL) {
		headers = curl_slist_append(headers, "Content-Type: application/json;charset=utf8");
		// json_put will free this
		json_text = json_object_to_json_string(request);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_text);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(json_text));
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);         
	}
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	status = curl_easy_perform(curl);
	if(status) {
		syslog(LOG_DEBUG, "curl error: status=%i, error=%s", status, curl_error);
		status = EIO;
		goto end_free;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if(http_code != 200) {
		// todo check content type for application/json
		syslog(LOG_DEBUG, "remote error: http status=%li", http_code);
		syslog(LOG_DEBUG, "json response=%s", json_object_to_json_string(json_result.object));
		json_object * errCode = json_object_object_get(json_result.object, "errno");

		switch(http_code) {
			case 400L:
				if( errCode != NULL) {
					val = alf_convert_string(json_get_string(json_result.object, "errno"));				
					if(val) {
						status = val;
					}
				} else {
					status = EIO;
				}
				break;
			case 404L:
				status = ENOENT;
				break;
			default:
				status = EIO;
		}
		goto end_free;
	} else {
		status = 0;	
		*response = json_result.object; 
		//syslog(LOG_DEBUG, "json request data: %s", json_object_to_json_string(json_result.object));
	}

end_free:
	if(tok != NULL)
		json_tokener_free(tok);
	if(headers != NULL)
		curl_slist_free_all(headers);

	return status;
}

static void log_file_info(struct fuse_file_info * fi) {
	syslog(LOG_DEBUG, "file info: flags=%o, fh_old=%lu, writepage=%i,  direct_io=%u, keep_cache=%u, flush=%u, nonseekable=%u, padding=%u, fh=%lu, lock_owner=%lu", fi->flags, fi->fh_old, fi->writepage, fi->direct_io, fi->keep_cache, fi->flush, fi->nonseekable, fi->padding, fi->fh, fi->lock_owner);

	syslog(LOG_DEBUG, "file info flags: O_RDONLY=%i, O_WRONLY=%i, O_RDWR=%i, O_APPEND=%i, O_ASYNC=%i, O_CLOEXEC=%i, O_CREAT=%i, O_DIRECT=%i, O_DIRECTORY=%i, O_EXCL=%i, O_LARGEFILE=%i, O_NOATIME=%i, O_NOCTTY=%i, O_NOFOLLOW=%i, O_NONBLOCK=%i, O_NDELAY=%i, O_SYNC=%i, O_TRUNC=%i", fi->flags & O_RDONLY, fi->flags & O_WRONLY, fi->flags & O_RDWR, fi->flags & O_APPEND, fi->flags & O_ASYNC, fi->flags & O_CLOEXEC, fi->flags & O_CREAT, fi->flags & O_DIRECT, fi->flags & O_DIRECTORY, fi->flags & O_EXCL, fi->flags & O_LARGEFILE, fi->flags & O_NOATIME, fi->flags & O_NOCTTY, fi->flags & O_NOFOLLOW, fi->flags & O_NONBLOCK, fi->flags & O_NDELAY, fi->flags & O_SYNC, fi->flags & O_TRUNC); 
	//syslog(LOG_DEBUG, "flag values: O_RDONLY=%o", O_RDONLY);
} 

static void alf_cache_update_etag(const struct alf_context * alf_ctx, const struct alf_ref * alf_ref, const char * etag)
{
	char * tmpetagpath = NULL;
	char * cache_path_etag = NULL;
	int etagfd = -1;
	
	syslog(LOG_DEBUG, "updateing local cache: uuid=%s", alf_ref->uuid);
	cache_path_etag = alf_sprintf("%s/alf-%s.etag", alf_ctx->cache_dir, alf_ref->uuid);
	tmpetagpath = alf_sprintf("%s/alf-tmp-XXXXXX", alf_ctx->cache_dir);

	etagfd = mkstemp(tmpetagpath);
	write(etagfd, etag, strlen(etag));
	close(etagfd);
	if(rename(tmpetagpath, cache_path_etag) == -1) {
		syslog(LOG_DEBUG, "rename failed: errno=%d, from=%s, to=%s", errno, tmpetagpath, cache_path_etag);
	}

	free(tmpetagpath);
	free(cache_path_etag);
}

/*
static void alf_cache_mark_dirty(const struct alf_context * alf_ctx, struct alf_ref * alf_ref)
{
	char * cache_path_etag = NULL;

	cache_path_etag = alf_sprintf("%s/alf-%s.etag", alf_ctx->cache_dir, alf_ref->uuid);

	unlink(cache_path_etag);

	free (cache_path_etag);
}
*/
static void alf_cache_update(const struct alf_context * alf_ctx, struct alf_ref * alf_ref)
{
	CURL * curl = NULL;
	CURLcode status = 1;
	char * url = NULL;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	struct curl_slist *headers = NULL;
	char * ifnonematch = NULL;
	struct file_reader_helper read_info;
	struct curl_slist * rec_headers = NULL;
	char * tempfilepath = NULL;
	char * cache_etag = NULL;
	char * etagHeaderValue = NULL;
	char * etag = NULL;
	long http_code = 400L;
	char curl_error[CURL_ERROR_SIZE];
	char * cache_path_etag = NULL;
	char * cache_path_bin = NULL;
	struct stat etagstat;

	syslog(LOG_DEBUG, "ALF UPDATE CACHE");

	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, alf_ref->path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

	url = alf_sprintf("%s/read?base=%s&path=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path);


	cache_path_etag = alf_sprintf("%s/alf-%s.etag", alf_ctx->cache_dir, alf_ref->uuid);
	cache_path_bin = alf_sprintf("%s/alf-%s.bin", alf_ctx->cache_dir, alf_ref->uuid);
	syslog(LOG_DEBUG, "open cache bin %s", cache_path_bin);
	int binfd = open(cache_path_bin, O_RDWR);
	syslog(LOG_DEBUG, "open cache etag %s", cache_path_etag);
	int efd = open(cache_path_etag, O_RDONLY);
	if(binfd >= 0 && efd >= 0) {
		fstat(efd, &etagstat);
		cache_etag = malloc(etagstat.st_size + 1);
		read(efd, cache_etag, etagstat.st_size);
		close(efd);
		cache_etag[etagstat.st_size] = '\0';
	}

	if(cache_etag != NULL) {
		syslog(LOG_DEBUG, "using etag=%s", cache_etag);
		ifnonematch = alf_sprintf("If-None-Match: \"%s\"", cache_etag);
		headers = curl_slist_append(headers, ifnonematch);
	}
	tempfilepath = alf_sprintf("%s/alf-dl-XXXXXX", alf_ctx->cache_dir);
	read_info.fd = mkstemp(tempfilepath);
	read_info.written = 0;

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);         
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, alf_receive_file);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_info);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, alf_receive_headers);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &rec_headers);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	status = curl_easy_perform(curl);
	if(status) {
		close(read_info.fd);
		close(binfd);
		unlink(tempfilepath);
		syslog(LOG_DEBUG, "curl error: status=%i, error=%s", status, curl_error);
		status = EIO;
		goto end_free;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if(http_code == 304) {
		close(read_info.fd);
		unlink(tempfilepath);
		// not modified
		syslog(LOG_DEBUG, "content not modified");
		alf_record_stats(alf_ref->path, "!");
		alf_ref->cachefd = binfd;
		goto end_free;
	} else if(http_code == 200) {
		close(binfd);
		alf_record_stats(alf_ref->path, "$");
		// read content and update cache
		etagHeaderValue = alf_get_header(rec_headers, "Etag");
		// etag is enclosed in "
		etagHeaderValue[strlen(etagHeaderValue)-1] = '\0';
		etag = etagHeaderValue + 2; // leader space and "
		syslog(LOG_DEBUG, "got etag=%s", etag);

		alf_cache_update_etag(alf_ctx, alf_ref, etag);

		rename(tempfilepath, cache_path_bin);
		//TODO: close existing fd 

		alf_ref->cachefd = read_info.fd; // dont close here

	} else {
		syslog(LOG_DEBUG, "remote error: http status=%li", http_code);
	}

end_free:
	if(url != NULL)
		free(url);
	if(etagHeaderValue != NULL)
		free(etagHeaderValue);
	if(ifnonematch != NULL)
		free(ifnonematch);
	if(tempfilepath != NULL)
		free(tempfilepath);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(headers != NULL)
		curl_slist_free_all(headers);
	if(rec_headers != NULL)
		curl_slist_free_all(rec_headers);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);
	if(cache_path_etag != NULL)
		free(cache_path_etag);
	if(cache_path_bin)
		free(cache_path_bin);
	syslog(LOG_DEBUG, "exit update cache");
}


// FUSE operations
static int alf_getattr(const char *path, struct stat *stbuf)
{
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	struct tm atimestamp;
	struct tm mtimestamp;
	struct tm ctimestamp;
	json_object * json_res = NULL;
	char * url = NULL;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	struct alf_ref * ref = NULL;

	syslog(LOG_DEBUG, "FUSE GETATTR");
	syslog(LOG_DEBUG, "path=%s", path);

	int res = 0;
	memset(stbuf, 0, sizeof(struct stat));
	memset(&atimestamp, 0, sizeof(struct tm));
	memset(&mtimestamp, 0, sizeof(struct tm));
	memset(&ctimestamp, 0, sizeof(struct tm));

	pthread_rwlock_wrlock(&alf_refs_lock);
	HASH_FIND_STR(alf_refs, path, ref);
	pthread_rwlock_unlock(&alf_refs_lock);
	if(!ref) {
		syslog(LOG_DEBUG, "no wip entry, doing remote: path=%s", path);	
		fuse_ctx = fuse_get_context();
		alf_ctx = (struct alf_context *) fuse_ctx->private_data;

		escaped_path = curl_easy_escape(curl, path, 0);
		escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

		url = alf_sprintf("%s/stat?base=%s&path=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path);
		curl = alf_create_curl(alf_ctx);

		syslog(LOG_DEBUG, "url=%s", url);	

		status = alf_perform_json_curl(curl, url, NULL, &json_res);
		if(status) {
			res = -status;
			goto end_free;
		} else {
			res = 0;
			syslog(LOG_DEBUG, "json: %s", json_object_to_json_string(json_res));

			stbuf->st_mode = json_get_int(json_res, "st_mode");
			stbuf->st_nlink = json_get_int(json_res, "st_nlink");
			stbuf->st_blksize = json_get_int(json_res, "st_blksize");
			stbuf->st_blocks = json_get_int(json_res, "st_blocks");
			stbuf->st_size = json_get_int(json_res, "st_size");
			stbuf->st_size = json_get_int(json_res, "st_size");
			if(json_object_object_get(json_res, "st_atime_epoch_sec") != NULL) {
				int t = json_get_int(json_res, "st_atime_epoch_sec");
				stbuf->st_atime = (time_t) t;
			}
			if(json_object_object_get(json_res, "st_ctime_epoch_sec") != NULL) {
				int t = json_get_int(json_res, "st_ctime_epoch_sec");
				stbuf->st_ctime = (time_t) t;
			}
			if(json_object_object_get(json_res, "st_mtime_epoch_sec") != NULL) {
				int t = json_get_int(json_res, "st_mtime_epoch_sec");
				stbuf->st_mtime = (time_t) t;
			}
		}
	} else {
		syslog(LOG_DEBUG, "wip entry found, doing local: path=%s", path);	
		if(fstat(ref->cachefd, stbuf) == -1) {
			syslog(LOG_DEBUG, "fstat error: errno=%d", errno);	
			res = -errno;
		}	
	}

	syslog(LOG_DEBUG, "stat: path=%s, size=%lu, mtime=%s", path, stbuf->st_size, ctime(&stbuf->st_mtime));

end_free:
	if(url != NULL)
		free(url);
	if(json_res != NULL) 
		json_object_put(json_res);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;
}

static int alf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	int res = 0;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	char * url = NULL;
	json_object * json_res = NULL;

	syslog(LOG_DEBUG, "FUSE READDIR");
	syslog(LOG_DEBUG, "path=%s, offset=%zu", path, offset);

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;

	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

	url = alf_sprintf("%s/readdir?base=%s&path=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path);

	syslog(LOG_DEBUG, "url=%s", url);	

	status = alf_perform_json_curl(curl, url, NULL, &json_res);
	if(status) {
		res = -status;
		goto end_free;
	} else {
		json_object * dirents = json_object_object_get(json_res, "dirents");
		int total = json_get_int(json_res, "total");
		int i = 0;
		for(i = 0; i < total; i++) {
			json_object * ent = json_object_array_get_idx(dirents, i);
			const char * name = json_get_string(ent, "name");
			struct stat typeinfo;
			typeinfo.st_mode = json_get_int(ent, "type");

			filler(buf, name, &typeinfo, 0);
		}

	}

end_free:
	if(url != NULL)
		free(url);
	if(json_res != NULL) 
		json_object_put(json_res);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;
}

static int alf_open(const char *path, struct fuse_file_info *fi)
{
	struct fuse_context * fuse_ctx = NULL;
	int res = -EIO;
	struct alf_context * alf_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	char * url = NULL;
	json_object * json_res = NULL;
	json_object * json_req = NULL;

	syslog(LOG_DEBUG, "FUSE OPEN");
	syslog(LOG_DEBUG, "path=%s", path);
	log_file_info(fi);

	alf_record_stats(path, "O");

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;

	curl = alf_create_curl(alf_ctx);

	url = alf_sprintf("%s/open", alf_ctx->endpoint);

	syslog(LOG_DEBUG, "url=%s", url);	

	json_req = json_object_new_object();
	json_object_object_add(json_req, "path", json_object_new_string(path));
	json_object_object_add(json_req, "base", json_object_new_string(alf_ctx->mount_base));
	json_object_object_add(json_req, "flags", json_object_new_int(fi->flags));
	status = alf_perform_json_curl(curl, url, json_req, &json_res);
	if(status) {
		res = -status;
		syslog(LOG_DEBUG, "open error: status=%i", res);	
		goto end_free;
	} else {
		pthread_rwlock_wrlock(&alf_refs_lock);
		struct alf_ref * ref = NULL;
		HASH_FIND_STR(alf_refs, path, ref);
		if(!ref) {
			syslog(LOG_DEBUG, "wip cache MISS: path=%s", path);	
			ref = (struct alf_ref*) malloc(sizeof(struct alf_ref)); 
			ref->uuid = strdup(json_get_string(json_res, "uuid"));
			ref->path = strdup(path);
			ref->cachefd = -1;
			ref->flags = fi->flags;
			ref->dirty = false;
			ref->opens = 1;
			alf_cache_update(alf_ctx, ref);
			HASH_ADD_KEYPTR( hh, alf_refs, ref->path, strlen(ref->path), ref);	
		} else {
			ref->opens++;
			syslog(LOG_DEBUG, "wip cache HIT: path=%s, opens=%i", path, ref->opens);	
		}
		fi->fh = (long)ref;
		pthread_rwlock_unlock(&alf_refs_lock);

		res = 0;
	}

end_free:
	if(url != NULL)
		free(url);
	if(json_res != NULL) 
		json_object_put(json_res);
	if(json_req != NULL) 
		json_object_put(json_req);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);
	return res;
}

static int alf_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	syslog(LOG_DEBUG, "FUSE READ CACHE");
	int res = 0;
	struct alf_ref * ref = (struct alf_ref*) fi->fh;
	if(ref == NULL) {
		syslog(LOG_DEBUG, "no REF!!!");
		return -EIO;
	}
	syslog(LOG_DEBUG, "path=%s, cacheid=%s, cachefd=%i", path, ref->uuid, ref->cachefd);
	syslog(LOG_DEBUG, "size=%zu, offset=%zu", size, offset);

	alf_record_stats(path, "R");

	res = pread(ref->cachefd, buf, size, offset);
	return res;
}
/*
static int alf_read_direct(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	int res = 0;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	struct reader_helper read_info;
	long http_code = 0;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	char * url = NULL;

	syslog(LOG_DEBUG, "FUSE READ");
	syslog(LOG_DEBUG, "path=%s", path);
	syslog(LOG_DEBUG, "size=%zu, offset=%zu", size, offset);
	log_file_info(fi);

	// 1. check file in cache is up to date
	// 2. if not, download file and add to cache
	// 3. serve read request from cached file

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;

	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

	url = alf_sprintf("%s/read?base=%s&path=%s&size=%zu&offset=%zu", alf_ctx->endpoint, escaped_mount_base, escaped_path, size, offset);

	syslog(LOG_DEBUG, "url=%s", url);

	read_info.target = buf;
	read_info.size = size;
	read_info.written = 0;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_read);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_info);

	status = curl_easy_perform(curl);
	if(status) {
		syslog(LOG_DEBUG, "remote error: status=%i", status);
		res = -EIO;
		goto end_free;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if(http_code != 200) {
		syslog(LOG_DEBUG, "remote error: http status=%li", http_code);
		switch(http_code) {
			case 404L:
				res = -ENOENT;
				break;
			default:
				res = -EIO;
		}
		goto end_free;
	} else {
		syslog(LOG_DEBUG, "read bytes: total=%zu", read_info.written);
		res = read_info.written;
	}

end_free:
	if(url != NULL)
		free(url);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(curl != NULL)
		curl_easy_cleanup(curl);

	return res;
}
*/
static int alf_write(const char * path , const char * buffer, size_t size , off_t offset, struct fuse_file_info * fi)
{
	int res = 0;

	struct alf_ref * ref = (struct alf_ref*) fi->fh;

	syslog(LOG_DEBUG, "FUSE WRITE CACHE");
	syslog(LOG_DEBUG, "path=%s, cacheid=%s, cachefd=%i", path, ref->uuid, ref->cachefd);
	syslog(LOG_DEBUG, "size=%zu, offset=%zu", size, offset);

	alf_record_stats(path, "W");

	ref->dirty = true;	

	res = pwrite(ref->cachefd, buffer, size, offset);


	return res;
}
/*
static int alf_write_direct(const char * path , const char * buffer, size_t size , off_t offset, struct fuse_file_info * fi)
{
	int res = 0;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	long http_code = 0;
	char * url = NULL;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;

	syslog(LOG_DEBUG, "FUSE WRITE DIRECT");
	syslog(LOG_DEBUG, "path=%s", path);
	syslog(LOG_DEBUG, "size=%zu, offset=%zu", size, offset);
	log_file_info(fi);

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

	url = alf_sprintf("%s/write?base=%s&path=%s&size=%zu&offset=%zu", alf_ctx->endpoint, escaped_mount_base, escaped_path, size, offset);

	syslog(LOG_DEBUG, "url=%s", url);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buffer);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, size);

	status = curl_easy_perform(curl);
	if(status) {
		syslog(LOG_DEBUG, "remote error: status=%i", status);
		res = -EIO;
		goto end_free;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if(http_code != 200) {
		syslog(LOG_DEBUG, "remote error: http status=%li", http_code);
		switch(http_code) {
			case 404L:
				res = -ENOENT;
				break;
			default:
				res = -EIO;
		}
		goto end_free;
	} else {
		syslog(LOG_DEBUG, "write successful");
		res = size;
	}

end_free:

	if(url != NULL)
		free(url);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(curl != NULL)
		curl_easy_cleanup(curl);

	return res;

}
*/

static int alf_unlink_node(const char * path, const char * resource)
{
	int res = 0;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	json_object * json_res = NULL;
	char * url = NULL;

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;

	alf_record_stats(path, "U");

	curl = alf_create_curl(alf_ctx);
	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

	url = alf_sprintf("%s/%s?base=%s&path=%s", alf_ctx->endpoint, resource, escaped_mount_base, escaped_path);

	syslog(LOG_DEBUG, "url=%s", url);

	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

	status = alf_perform_json_curl(curl, url, NULL, &json_res);

	if(status) {
		res = -status;
		goto end_free;
	} else {
		syslog(LOG_DEBUG, "unlink successful");
		res = 0;
	}

end_free:
	if(json_res != NULL)
		json_object_put(json_res);
	if(url != NULL)
		free(url);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;
}

static int alf_unlink(const char * path)
{

	syslog(LOG_DEBUG, "FUSE UNLINK");
	syslog(LOG_DEBUG, "unlink: path=%s", path);

	int res = alf_unlink_node(path, "unlink");

	return res;
}


static int alf_rmdir(const char * path)
{

	syslog(LOG_DEBUG, "FUSE RMDIR");
	syslog(LOG_DEBUG, "rmdir: path=%s", path);

	int res = alf_unlink_node(path, "rmdir");

	return res;
}

static int alf_create_node(const char * path, mode_t mode, const char * alf_type, struct fuse_file_info * fi)
{
	int res = 0;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	json_object * json_req = NULL;
	json_object * json_res = NULL;
	char * temppath = NULL;
	char * url = NULL;

	syslog(LOG_DEBUG, "FUSE CREATE NODE");
	syslog(LOG_DEBUG, "path=%s, mode=%i, alftype=%s", path, mode, alf_type);

	alf_record_stats(path, "C");

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	json_req = json_object_new_object();
	json_object_object_add(json_req, "path", json_object_new_string(path));
	json_object_object_add(json_req, "type", json_object_new_string(alf_type));
	json_object_object_add(json_req, "base", json_object_new_string(alf_ctx->mount_base));
	json_object_object_add(json_req, "mode", json_object_new_int(mode));
	if(fi != NULL) {
		json_object_object_add(json_req, "flags", json_object_new_int(fi->flags));
	}

	url = alf_sprintf("%s/create", alf_ctx->endpoint);

	syslog(LOG_DEBUG, "url=%s", url);
	status = alf_perform_json_curl(curl, url, json_req, &json_res);
	if(status) {
		res = -status;
		goto end_free;
	} else {
		syslog(LOG_DEBUG, "json: %s", json_object_to_json_string(json_res));
		if(!strcmp("cm:content", alf_type)){
			syslog(LOG_DEBUG, "create call ok");
			pthread_rwlock_wrlock(&alf_refs_lock);
			syslog(LOG_DEBUG, "lock ok");	
			struct alf_ref * ref = NULL;
			syslog(LOG_DEBUG, "alf_refs=%p, path=%s, ref=%p", alf_refs, path, ref);
			HASH_FIND_STR(alf_refs, path, ref);
			syslog(LOG_DEBUG, "hash lookup ok");	
			if(ref != NULL) {
				// found a cached ref this is an error on create
				syslog(LOG_DEBUG, "ERROR: found a cache entry: path=%s", path);
				res = -EIO;
				pthread_rwlock_unlock(&alf_refs_lock);
				goto end_free;
			}
			ref = (struct alf_ref*) malloc(sizeof(struct alf_ref)); 

			memset(ref, 0, sizeof(struct alf_ref));
			ref->uuid = strdup(json_get_string(json_res, "uuid"));
			ref->path = strdup(path);
			ref->flags = fi->flags;
			ref->dirty = false;
			ref->opens = 1;
			temppath = alf_sprintf("%s/alf-%s.bin", alf_ctx->cache_dir, ref->uuid);
			syslog(LOG_DEBUG, "tmp file=%s, p=%p", temppath, ref);
			int tmpflags = O_RDWR | O_CREAT | O_EXCL;
			int tmpmode = S_IRUSR | S_IWUSR;
			ref->cachefd = open(temppath, tmpflags, tmpmode);
			if(ref->cachefd < 0) {
				syslog(LOG_DEBUG, "open failed: errno=%d", errno);
				res = -errno;
				goto end_free;
			}
			fi->fh = (long)ref;

			HASH_ADD_KEYPTR( hh, alf_refs, ref->path, strlen(ref->path), ref);	
			pthread_rwlock_unlock(&alf_refs_lock);
		}

	}

end_free:
	if(url != NULL)
		free(url);
	if(temppath != NULL)
		free(temppath);
	if(json_res != NULL)
		json_object_put(json_res);
	if(json_req != NULL)
		json_object_put(json_req);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;
}

static int alf_mkdir(const char* path, mode_t mode)
{
	syslog(LOG_DEBUG, "FUSE MKDIR");
	syslog(LOG_DEBUG, "path=%s", path);
	syslog(LOG_DEBUG, "create mode oktal: mode=%o, S_IFREG=%o, S_IFDIR=%o, S_IFLNK=%o", mode, mode & S_IFREG, mode & S_IFDIR, mode & S_IFLNK);
	int res = alf_create_node(path, mode, "cm:folder", NULL);
	return res;
}

static int alf_create(const char * path, mode_t mode, struct fuse_file_info * fi)
{
	syslog(LOG_DEBUG, "FUSE CREATE");
	syslog(LOG_DEBUG, "path=%s", path);
	syslog(LOG_DEBUG, "create mode oktal: mode=%o, S_IFREG=%o, S_IFDIR=%o, S_IFLNK=%o", mode, mode & S_IFREG, mode & S_IFDIR, mode & S_IFLNK);
	log_file_info(fi);

	int res = alf_create_node(path, mode, "cm:content", fi);

	return res;
}

static int alf_getxattr(const char * path, const char * key, char * value, size_t size)
{

	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	json_object * json_res = NULL;
	char * escaped_path = NULL;
	char * escaped_key = NULL;
	char * escaped_mount_base = NULL;
	int res = -EIO;
	char * url = NULL;

	syslog(LOG_DEBUG, "FUSE GETXATTR");
	syslog(LOG_DEBUG, "path=%s, key=%s, size=%zu", path, key, size);

	// fast return if not alf. prefixed
	if(strncmp("alf.", key, 4)) {
		return -ENOATTR;
	}

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);
	escaped_key = curl_easy_escape(curl, key, 0);

	url = alf_sprintf("%s/xattr?base=%s&path=%s&key=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path, escaped_key);

	syslog(LOG_DEBUG, "url=%s", url);

	status = alf_perform_json_curl(curl, url, NULL, &json_res);
	if(status) {
		res = -status;
		goto end_free;
	} else {
		syslog(LOG_DEBUG, "json: %s", json_object_to_json_string(json_res));
		const char * src = json_get_string(json_res, "value");
		syslog(LOG_DEBUG, "attr val=%s", src);
		size_t vallen = 0;
		if(src != NULL) {
			vallen = strlen(src);
			if(size > vallen) {
				strcpy(value, src);
			} else if(size != 0) {
				res = -ERANGE;
			}

			res = vallen + 1;
		} else {
			res = 0;	
		}
	}

end_free:
	if(json_res != NULL)
		json_object_put(json_res);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(escaped_key != NULL)
		curl_free(escaped_key);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;

}

static int alf_setxattr(const char * path, const char * key, const char * value, size_t size, int mode)
{

	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	struct curl_slist *headers = NULL;
	char * url = NULL;
	json_object * json_res = NULL;
	char * escaped_path = NULL;
	char * escaped_key = NULL;
	char * escaped_mount_base = NULL;
	int res = -EIO;
	const char * mode_create = "create";
	const char * mode_replace = "replace";
	const char * mode_createorreplace = "createorreplace";
	const char * mode_param = NULL;

	syslog(LOG_DEBUG, "FUSE SETXATTR");
	syslog(LOG_DEBUG, "path=%s, key=%s, size=%zu, mode create=%i, mode replace=%i", path, key, size, mode & XATTR_CREATE,  mode & XATTR_REPLACE);

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);
	escaped_key = curl_easy_escape(curl, key, 0);
	mode_param = mode_createorreplace;
	if((mode & XATTR_CREATE) == XATTR_CREATE) {
		mode_param = mode_create;
	} else if((mode & XATTR_REPLACE) == XATTR_REPLACE) {
		mode_param = mode_replace;
	}
	url = alf_sprintf("%s/xattr?base=%s&path=%s&key=%s&mode=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path, escaped_key, mode_param);

	syslog(LOG_DEBUG, "url=%s", url);

	// TODO support for binary stuff too 
	headers = curl_slist_append(headers, "Content-Type: text/plain;charst=utf8");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, value);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, size);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);         

	status = alf_perform_json_curl(curl, url, NULL, &json_res);
	if(status) {
		res = -status;
		goto end_free;
	} else {
		res = 0;	
	}

end_free:
	if(json_res != NULL)
		json_object_put(json_res);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(escaped_key != NULL)
		curl_free(escaped_key);
	if(headers != NULL)
		curl_slist_free_all(headers);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;

}


static int alf_removexattr(const char * path, const char * key)
{
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	json_object * json_res = NULL;
	char * escaped_path = NULL; 
	char * escaped_mount_base = NULL;
	char * escaped_key = NULL;
	int res = -EIO;
	char * url = NULL;

	syslog(LOG_DEBUG, "FUSE REMOVEXATTR");
	syslog(LOG_DEBUG, "path=%s, key=%s", path, key);

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);
	escaped_key = curl_easy_escape(curl, key, 0);

	url = alf_sprintf("%s/xattr?base=%s&path=%s&key=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path, escaped_key);

	syslog(LOG_DEBUG, "url=%s", url);

	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	status = alf_perform_json_curl(curl, url, NULL, &json_res);
	if(status) {
		res = -status;
		goto end_free;
	} else {
		res = 0;
	}

end_free:
	if(json_res != NULL)
		json_object_put(json_res);
	if(escaped_path != NULL)	
		curl_free(escaped_path);
	if(escaped_mount_base != NULL) 
		curl_free(escaped_mount_base);
	if(escaped_key != NULL) 
		curl_free(escaped_key);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;

}
static int alf_listxattr(const char * path, char * list, size_t size)
{

	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	json_object * json_res = NULL;
	char * escaped_path = NULL; 
	char * escaped_mount_base = NULL;
	int res = -EIO;
	char * url = NULL;

	syslog(LOG_DEBUG, "FUSE LISTXATTR");
	syslog(LOG_DEBUG, "path=%s, size=%zu", path, size);


	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

	url = alf_sprintf("%s/xattr?base=%s&path=%s&mode=onlykeys", alf_ctx->endpoint, escaped_mount_base, escaped_path);

	syslog(LOG_DEBUG, "url=%s", url);

	status = alf_perform_json_curl(curl, url, NULL, &json_res);

	if(status) {
		syslog(LOG_DEBUG, "stats=%i", status);
		res = -status;
		syslog(LOG_DEBUG, "json=%s", json_object_to_json_string(json_res));
		goto end_free;
	} else {
		size_t listlen = 0;
		size_t written = 0;
		syslog(LOG_DEBUG, "json=%s", json_object_to_json_string(json_res));
		int numkeys = json_object_array_length(json_res);
		int i;
		for(i = 0; i < numkeys; i++) {
			json_object * keyobject = json_object_array_get_idx(json_res,i);
			if(keyobject != NULL) {
				const char * key = json_object_get_string(keyobject);

				size_t keylen = strlen(key);
				listlen += keylen + 1;
				if(size >= listlen) {
					// copy keys if buffer size is > 0
					strcpy(list + written, key);
					written += keylen + 1;
				} else if(size != 0) {
					res = -ERANGE;
					goto end_free;
				}	
			}
		}	
		res = listlen;

	}

end_free:
	// TODO free url
	if(json_res != NULL)
		json_object_put(json_res);
	if(escaped_path != NULL)	
		curl_free(escaped_path);
	if(escaped_mount_base != NULL) 
		curl_free(escaped_mount_base);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;

}

static int alf_truncate(const char * path, off_t offset)
{
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	json_object * json_res = NULL;
	json_object * json_req = NULL;
	char * escaped_path = NULL;
	char * escaped_key = NULL;
	char * escaped_mount_base = NULL;
	int res = -EIO;
	char * url = NULL;
	struct alf_ref * ref = NULL;

	syslog(LOG_DEBUG, "FUSE TRUNCATE");
	syslog(LOG_DEBUG, "path=%s, offset=%zu", path, offset);

	alf_record_stats(path, "V");

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	
	pthread_rwlock_wrlock(&alf_refs_lock);
	HASH_FIND_STR(alf_refs, path, ref);
	pthread_rwlock_unlock(&alf_refs_lock);
	if(!ref) {
		curl = alf_create_curl(alf_ctx);

		escaped_path = curl_easy_escape(curl, path, 0);
		escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

		syslog(LOG_DEBUG, "local truncate cache miss, truncate remote: path=%s", path);	
		url = alf_sprintf("%s/truncate?base=%s&path=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path);
		syslog(LOG_DEBUG, "url=%s", url);

		json_req = json_object_new_object();
		json_object_object_add(json_req, "offset", json_object_new_int(offset));

		status = alf_perform_json_curl(curl, url, json_req, &json_res);
		if(status) {
			res = -status;
			goto end_free;
		} else {
			res = 0;	
		}
	} else {
		syslog(LOG_DEBUG, "local truncate cache hit: path=%s, opens=%i", path, ref->opens);	
		ref->dirty = true;
		if(ftruncate(ref->cachefd, offset) == -1) {
			res = -errno;
		} else {
			res = 0;
		}
		
	}


end_free:
	if(json_res != NULL)
		json_object_put(json_res);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(escaped_key != NULL)
		curl_free(escaped_key);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;

}


static int alf_rename(const char * path, const char * newpath)
{
	int res = -EIO;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	int status = 0;
	char * url = NULL;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	json_object * json_req = NULL;
	json_object * json_res = NULL;

	syslog(LOG_DEBUG, "FUSE RENAME");
	syslog(LOG_DEBUG, "path=%s, newpath=%s", path, newpath);

	char * statinfo = alf_sprintf("RN(from=%s)", path);
	alf_record_stats(newpath, statinfo);
	free(statinfo);
	statinfo = alf_sprintf("RN(to=%s)", newpath);
	alf_record_stats(path, statinfo);
	free(statinfo);

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);
	url = alf_sprintf("%s/rename?base=%s&path=%s", alf_ctx->endpoint, escaped_mount_base, escaped_path);
	curl = alf_create_curl(alf_ctx);

	json_req = json_object_new_object();
	json_object_object_add(json_req, "newpath", json_object_new_string(newpath));

	syslog(LOG_DEBUG, "url=%s", url);	

	status = alf_perform_json_curl(curl, url, json_req, &json_res);
	if(status) {
		res = -status;
		goto end_free;
	} else {
		res = 0;
	}


end_free:
	if(url != NULL)
		free(url);
	if(json_req != NULL)
		json_object_put(json_req);
	if(json_res != NULL)
		json_object_put(json_res);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;
}

static int alf_flush(const char * path, struct fuse_file_info *fi)
{
	int res = -EIO;

	syslog(LOG_DEBUG, "FUSE FLUSH");
	syslog(LOG_DEBUG, "path=%s", path);

	alf_record_stats(path, "F");
	res = 0;

	return res;
}

static int alf_fsync(const char * path, int datasync, struct fuse_file_info *fi)
{
	int res = -EIO;

	syslog(LOG_DEBUG, "FUSE FSYNC");
	syslog(LOG_DEBUG, "path=%s", path);

	alf_record_stats(path, "S");
	res = 0;

	return res;
}

static int alf_release(const char * path, struct fuse_file_info * fi)
{
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	char * url = NULL;
	struct stat cachestat;
	size_t size = 0;
	off_t offset = 0;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	const char * etag = NULL;
	json_object * json_res = NULL;
	FILE * instream = NULL;

	syslog(LOG_DEBUG, "FUSE RELEASE");
	syslog(LOG_DEBUG, "path=%s", path);


	pthread_rwlock_wrlock(&alf_refs_lock);	

	struct alf_ref * ref = (struct alf_ref*)fi->fh;

	syslog(LOG_DEBUG, "p=%p, uuid=%s, fd=%i, opens=%i, dirty=%i", ref, ref->uuid, ref->cachefd, ref->opens, ref->dirty);
	//TODO: lock individual entry too
	ref->opens--;
	if(ref->opens < 0) {
		syslog(LOG_DEBUG, "RELEASE open count drops negative: path=%s", path);
		goto end_free;
	} else if(ref->opens == 0 && ref->dirty) { // upload if refcount drops to zero and changed locally
		syslog(LOG_DEBUG, "RELEASE update remote: path=%s", path);
		memset(&cachestat, 0, sizeof(struct stat));
		if(fstat(ref->cachefd, &cachestat) == -1) {
			syslog(LOG_DEBUG, "error in fstat: %d", errno);
			goto end_free;
		}
		size = cachestat.st_size;
		instream = fdopen(ref->cachefd, "r");

		fuse_ctx = fuse_get_context();
		alf_ctx = (struct alf_context *) fuse_ctx->private_data;

		curl = alf_create_curl(alf_ctx);

		alf_record_stats(path, "^");

		escaped_path = curl_easy_escape(curl, path, 0);
		escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);
		

		url = alf_sprintf("%s/write?base=%s&path=%s&size=%zu&offset=%zu&truncate=true&mtime_sec=%lu", alf_ctx->endpoint, escaped_mount_base, escaped_path, size, offset, cachestat.st_mtime);

		syslog(LOG_DEBUG, "url=%s", url);
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

		curl_easy_setopt(curl, CURLOPT_READDATA, (void*)instream);

		status = alf_perform_json_curl(curl, url, NULL, &json_res);
		if(status) {
			syslog(LOG_DEBUG, "remote write failed: path=%s", path);
			goto end_free;
		} else {
			etag = json_get_string(json_res, "etag");
		
			syslog(LOG_DEBUG, "write successful: etag=%s", etag);
			
			alf_cache_update_etag(alf_ctx, ref, etag);
		}
	} else  {
		syslog(LOG_DEBUG, "NOT updating remote: opens=%i, dirty=%i", ref->opens, ref->dirty);
	}

end_free:
	alf_record_stats(path, "X");
	if(url != NULL)
		free(url);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);
	if(json_res != NULL)
		json_object_put(json_res);
	if(instream != NULL)
		fclose(instream);
	if(ref != NULL && ref->opens <= 0) { // free if no refs left
		HASH_DEL(alf_refs,  ref);		
		if(ref->cachefd)	
			close(ref->cachefd);
		if(ref->uuid != NULL)
			free((void*)ref->uuid);
		free(ref);
	}

	// return value is ignored
	pthread_rwlock_unlock(&alf_refs_lock);
	syslog(LOG_DEBUG, "exiting RELEASE: path=%s", path);
	return -EIO;
}

static int alf_statfs(const char * path, struct statvfs * statfsinfo)
{
	int res = -EIO;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	char * url = NULL;
	json_object * json_res = NULL;

	syslog(LOG_DEBUG, "FUSE STATFS");
	syslog(LOG_DEBUG, "path=%s", path);
	alf_record_stats(path, "%");

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	url = alf_sprintf("%s/statfs", alf_ctx->endpoint);

	syslog(LOG_DEBUG, "url=%s", url);

	status = alf_perform_json_curl(curl, url, NULL, &json_res);
	if(status) {
		res = -status;
		goto end_free;
	} else {
		syslog(LOG_DEBUG, "json=%s", json_object_to_json_string(json_res));
		long usableBytes = atol(json_get_string(json_res, "freeBytes"));
		long totalBytes = atol(json_get_string(json_res, "totalBytes"));
		int bsize = 4096;
		statfsinfo->f_bsize = bsize;
		statfsinfo->f_frsize = 0; //ignored 
		statfsinfo->f_blocks = totalBytes / bsize;
		statfsinfo->f_bfree = usableBytes / bsize;
		statfsinfo->f_bavail = usableBytes / bsize;
		statfsinfo->f_files = 0;
		statfsinfo->f_ffree = 0;
		statfsinfo->f_favail = 0; //ignored
		statfsinfo->f_fsid = 0; //ignored
		statfsinfo->f_flag = 0; //ignored
		statfsinfo->f_namemax = json_get_int(json_res, "maxFilename");

		res = 0;	
	}

end_free:
	if(json_res != NULL)
		json_object_put(json_res);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;
}

static int alf_utimens(const char * path, const struct timespec tv[2])
{
	int res = -EIO;
	struct alf_context * alf_ctx = NULL;
	struct fuse_context * fuse_ctx = NULL;
	CURL * curl = NULL;
	CURLcode status = 1;
	char * url = NULL;
	json_object * json_res = NULL;
	json_object * json_req = NULL;
	struct timespec atime = tv[0];
	struct timespec mtime = tv[1];
	char * atime_sec = NULL;
	char * atime_nsec = NULL;
	char * mtime_sec = NULL;
	char * mtime_nsec = NULL;
	char * escaped_path = NULL;
	char * escaped_mount_base = NULL;
	struct alf_ref * ref = NULL;

	syslog(LOG_DEBUG, "FUSE UTIMENS: path=%s", path);
	alf_record_stats(path, "T");

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;
	curl = alf_create_curl(alf_ctx);

	escaped_path = curl_easy_escape(curl, path, 0);
	escaped_mount_base = curl_easy_escape(curl, alf_ctx->mount_base, 0);

	url = alf_sprintf("%s/utimens?path=%s&base=%s", alf_ctx->endpoint, escaped_path, escaped_mount_base);

	syslog(LOG_DEBUG, "url=%s", url);

	pthread_rwlock_wrlock(&alf_refs_lock);	
	HASH_FIND_STR(alf_refs, path, ref);
	pthread_rwlock_unlock(&alf_refs_lock);
	if(!ref) {
		syslog(LOG_DEBUG, "no wip entry, doing remote time: path=%s", path);
		atime_sec = alf_sprintf("%lu", atime.tv_sec);
		atime_nsec = alf_sprintf("%ld", atime.tv_nsec);
		mtime_sec = alf_sprintf("%lu", mtime.tv_sec);
		mtime_nsec = alf_sprintf("%ld", mtime.tv_nsec);
		syslog(LOG_DEBUG, "atime.sec=%s, atime.nsec=%s, mtime.sec=%s, mtime.nsec=%s", atime_sec, atime_nsec, mtime_sec, mtime_nsec);

		json_req = json_object_new_object();
		json_object_object_add(json_req, "atime_sec", json_object_new_string(atime_sec));
		json_object_object_add(json_req, "atime_nsec", json_object_new_string(atime_nsec));
		json_object_object_add(json_req, "mtime_sec", json_object_new_string(mtime_sec));
		json_object_object_add(json_req, "mtime_nsec", json_object_new_string(mtime_nsec));

		status = alf_perform_json_curl(curl, url, json_req, &json_res);
		if(status) {
			res = -status;
			goto end_free;
		} else {
			syslog(LOG_DEBUG, "json response=%s", json_object_to_json_string(json_res));
			res = 0;	
		}	
	} else {
		syslog(LOG_DEBUG, "wip entry found for timing: path=%s", path);
		if(futimens(ref->cachefd, tv) == -1) {
			syslog(LOG_DEBUG, "setting wip time failed: path=%s", path);
			res = -errno;
		}	
	}


end_free:
	if(atime_sec != NULL)
		free(atime_sec);
	if(atime_nsec != NULL)
		free(atime_nsec);
	if(mtime_sec != NULL)
		free(mtime_sec);
	if(mtime_nsec != NULL)
		free(mtime_nsec);
	if(escaped_path != NULL)
		curl_free(escaped_path);
	if(escaped_mount_base != NULL)
		curl_free(escaped_mount_base);
	if(json_res != NULL)
		json_object_put(json_res);
	if(json_req != NULL)
		json_object_put(json_req);
	if(curl != NULL)
		alf_release_curl(alf_ctx, curl);

	return res;

}

// runs a cache cleanup
static void alf_purge_cache(struct alf_context * alf_ctx) {
	// 1. loop through cache dir
	// 2. select files matching alf-%s.bin
	// 3. if older than cutoff
	// 4. check if its in used in cache
	// 5. remove from cache refs
	// 6. unlink from cache dir
	
	DIR* cachedir = NULL;
	//struct dirent * entry = NULL;
	//int st = -1;
	//struct stat sinfo;
	//time_t now = time(NULL);
	//time_t cutofftime = now - alf_ctx->hk_maxcacheseconds;
	//struct alf_ref * refstat = NULL;
	//struct alf_ref * tmp_refstat = NULL;

	cachedir = opendir(alf_ctx->cache_dir);
	if(cachedir == NULL) {
		syslog(LOG_DEBUG, "error checking cache dir: no=%d, msg=%s", errno, strerror(errno));
		return;
	}
	goto end_free;
/*
	while (true) {
		entry = readdir(cachedir);
		if(entry == NULL) {
			// this is end of dir or error TODO: check for error
			break;
		}
		if(strncmp(entry->d_name, "alf-", 4) == 0 ) {
			int sidx = strlen(entry->d_name);
			char * suffix = entry->d_name + (sidx - 4);
			if(strncmp(suffix, ".bin", 4)) {
				char * full_path = alf_sprintf("%s/%s", alf_ctx->cache_dir, entry->d_name);
				stat(full_path, &sinfo);
				free(full_path);
				if(entry->d_mtime < cutofftime) {
					// ok fine to remove if not in use
					int found = 0;
					HASH_ITER(hh, alf_refs, refstat, tmp_refstat) {
						if(strcmp(uuid, refstat->uuid)) {
							found++;
						}
					}
					if(found == 0) {
						
					}
				}	
			}

		}

	}
*/	
end_free:
	if(cachedir != NULL)
		closedir(cachedir);	

}

// background cache housekeeping main
static void * alf_cache_housekeeping(void * th_data) {
	struct alf_context * alf_ctx = (struct alf_context *) th_data;
	struct timespec timeToWait;
	struct timeval now;
	int rt;

	syslog(LOG_DEBUG, "cache housekeeping background job started: sleepsec=%d", alf_ctx->hk_sleepseconds);

	while(alf_ctx->hk_cancelRequest == 0) {
		gettimeofday(&now,NULL);
		timeToWait.tv_sec = now.tv_sec + alf_ctx->hk_sleepseconds;
		timeToWait.tv_nsec = now.tv_usec*1000;

		pthread_mutex_lock(&hk_mutex);
		rt = pthread_cond_timedwait(&hk_wait_condition, &hk_mutex, &timeToWait);
		pthread_mutex_unlock(&hk_mutex);
		if(rt && rt != ETIMEDOUT) { // error
			syslog(LOG_DEBUG, "error on thread wait: e=%d, msg=%s", rt, strerror(rt) );
		} else {
			// TODO: should also check if timeout value really passed
			if(alf_ctx->hk_cancelRequest == 0) {
				syslog(LOG_DEBUG, "running cache housekeeping");	
				alf_purge_cache(alf_ctx);
			}

		}
	}
	
	syslog(LOG_DEBUG, "exiting cache housekeeping background job");
	pthread_exit(NULL);
}

static void * alf_init(struct fuse_conn_info *conn)
{
	struct fuse_context * fuse_ctx;
	struct alf_context * alf_ctx;

	syslog(LOG_INFO, "Alf fuse fs startup");

	syslog(LOG_DEBUG, "call back init");
	syslog(LOG_DEBUG, "fuse protocol version: %u.%u", conn->proto_major, conn->proto_minor);
	syslog(LOG_DEBUG, "fuse: async_read=%u, max_write=%u, max_readahead=%u", conn->async_read, conn->max_write, conn->max_readahead);

	fuse_ctx = fuse_get_context();
	alf_ctx = (struct alf_context *) fuse_ctx->private_data;

	syslog(LOG_DEBUG, "fuse ctx: uid=%u, gid=%u, pid=%u, umask=%u", fuse_ctx->uid, fuse_ctx->gid, fuse_ctx->pid, fuse_ctx->umask);
	syslog(LOG_DEBUG, "alf ctx: userid=%s, password=%s, protocol=%s, host=%s, port=%i, io_base=%s, mount_base=%s, debug=%i", alf_ctx->userid, (alf_ctx->password == NULL ? "(null)" : "****"), alf_ctx->protocol, alf_ctx->host, alf_ctx->port, alf_ctx->io_base, alf_ctx->mount_base, alf_ctx->debug);

	// starting housekeeping job
	if(alf_ctx->hk_sleepseconds <= 0) {
		alf_ctx->hk_sleepseconds = 60;
	}
	int err = pthread_create(&alf_ctx->cache_housekeeping_thread, NULL, alf_cache_housekeeping, alf_ctx);
	if(err) {
		syslog(LOG_ERR, "could not start housekeeping job: error=%d, msg=%s", err, strerror(err));
	}
	

	return alf_ctx;
}


static void alf_destroy(void * data)
{
	struct alf_context * alf_ctx = (struct alf_context *)data;

	syslog(LOG_DEBUG, "call back destroy");

	// wait for housekeeping job
	syslog(LOG_DEBUG, "wait for housekeeping job to exit");
	
	alf_ctx->hk_cancelRequest = 1;
	int rt = pthread_cond_broadcast(&hk_wait_condition);
	if(rt) {
		syslog(LOG_DEBUG, "error on thread wakeup: e=%d, msg=%s", rt, strerror(rt) );
	}
	int je = pthread_join(alf_ctx->cache_housekeeping_thread, NULL);
	if(je) {
		syslog(LOG_DEBUG, "error on thread join: e=%d, msg=%s", je, strerror(je) );
	}
	syslog(LOG_DEBUG, "housekeeping job exited");

	syslog(LOG_INFO, "Alf fuse fs shutdown");
}

static struct fuse_operations alf_oper = {
	.getattr	= alf_getattr,
	.readdir	= alf_readdir,
	.open		= alf_open,
	.release	= alf_release,
	.create		= alf_create,
	.read		= alf_read,
	.write		= alf_write,
	.init           = alf_init,
	.destroy        = alf_destroy,
	.unlink		= alf_unlink,
	.mkdir		= alf_mkdir,
	.rmdir		= alf_rmdir,
	.listxattr	= alf_listxattr,
	.getxattr	= alf_getxattr,
	.setxattr	= alf_setxattr,
	.removexattr	= alf_removexattr,
	.truncate	= alf_truncate,
	.rename		= alf_rename,
	.flush		= alf_flush,
	.fsync		= alf_fsync,
	.statfs		= alf_statfs,
	.utimens	= alf_utimens
};


static void alf_openssl_locking_function(int mode, int n, const char * file, int line)
{
	if (mode & CRYPTO_LOCK) {
		pthread_mutex_lock(&openssl_mutex_buf[n]);
	} else  {
		pthread_mutex_unlock(&openssl_mutex_buf[n]);
	}
}

static unsigned long alf_openssl_id_function(void)
{
	return ((unsigned long)pthread_self());
}

// openssl opening callback
static int alf_thread_setup(void)
{
	int i;

	openssl_mutex_buf = malloc(CRYPTO_num_locks(  ) * sizeof(pthread_mutex_t));
	if (!openssl_mutex_buf)
		return 0;

	for (i = 0;  i < CRYPTO_num_locks();  i++) {
		pthread_mutex_init(&openssl_mutex_buf[i], NULL);
	}

	CRYPTO_set_id_callback(alf_openssl_id_function);
	CRYPTO_set_locking_callback(alf_openssl_locking_function);

	return 1;
}

// openssl closing callback
static int alf_thread_cleanup(void)
{
	int i;

	if (!openssl_mutex_buf)
		return 0;

	CRYPTO_set_id_callback(NULL);
	CRYPTO_set_locking_callback(NULL);

	for (i = 0;  i < CRYPTO_num_locks();  i++) {
		pthread_mutex_destroy(&openssl_mutex_buf[i]);
	}

	free(openssl_mutex_buf);
	openssl_mutex_buf = NULL;

	return 1;
}

int main(int argc, char *argv[])
{
	int fuse_ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct alf_context alf_context;
	struct alf_stat_record * stat = NULL;
	struct alf_stat_record * tmp_stat = NULL;
	struct alf_con_cache * tmp_con_cache = NULL;
	struct alf_con_cache * elt = NULL;

	openlog("alf-fuse", LOG_PID|LOG_CONS, LOG_DAEMON);

	if(pthread_rwlock_init(&alf_stats_lock, NULL)) {
		fprintf(stderr, "failed to init stats lock");
		return -1;
	}
	if(pthread_rwlock_init(&alf_refs_lock, NULL)) {
		fprintf(stderr, "failed to init refs lock");
		return -1;
	}

	alf_thread_setup();



	curl_global_init(CURL_GLOBAL_ALL);

	memset(&alf_context, 0, sizeof(struct alf_context));
	if (fuse_opt_parse(&args, &alf_context, alf_mount_options, NULL) == -1) {
		fprintf(stderr, "option parsing error, exiting");
		return -1;
	}

	// precompute endpoint to simplify url construction in callbacks
	alf_context.endpoint = alf_sprintf("%s://%s:%i%s", alf_context.protocol, alf_context.host, alf_context.port, alf_context.io_base);
	alf_context.hk_cancelRequest = 0;

	fuse_ret = fuse_main(args.argc, args.argv, &alf_oper, &alf_context);

	fuse_opt_free_args(&args);

	alf_thread_cleanup();

	// free stats and dump info
	HASH_ITER(hh, alf_stats, stat, tmp_stat) {
		syslog(LOG_DEBUG, "path %s\n", stat->path);
		syslog(LOG_DEBUG, "  ops %s\n", stat->opslist);
		HASH_DEL(alf_stats, stat);
		free(stat);
	}

	// cleanup con cache
	LL_FOREACH_SAFE(alf_con_cache, elt, tmp_con_cache) {
		CURL * curl = elt->curl;
		LL_DELETE(alf_con_cache, elt);
		free(elt);
		curl_easy_cleanup(curl);
	}	

	curl_global_cleanup();

	if(alf_context.endpoint !=NULL)
		free(alf_context.endpoint);	

	syslog(LOG_DEBUG, "exiting alf fuse: exit code=%i", fuse_ret);

	closelog();
	return fuse_ret;
}
