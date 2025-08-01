#include "../obk_config.h"

#include "../new_common.h"
#include "../logging/logging.h"
#include "../httpserver/new_http.h"
#include "../new_pins.h"
#include "../jsmn/jsmn_h.h"
#include "../ota/ota.h"
#include "../hal/hal_wifi.h"
#include "../hal/hal_flashVars.h"
#include "../littlefs/our_lfs.h"
#include "lwip/sockets.h"

#define DEFAULT_FLASH_LEN 0x200000

#if PLATFORM_XRADIO

#include <image/flash.h>
#include "ota/ota.h"

uint32_t flash_read(uint32_t flash, uint32_t addr, void* buf, uint32_t size);
#define FLASH_INDEX_XR809 0

#elif PLATFORM_BL602
#include <hal_boot2.h>
#include <utils_sha256.h>
#include <bl_mtd.h>
#include <bl_flash.h>

#elif defined(PLATFORM_W800) || defined(PLATFORM_W600)

#include "wm_internal_flash.h"
#include "wm_socket_fwup.h"
#include "wm_fwup.h"

#elif PLATFORM_LN882H

#include "hal/hal_flash.h"
#include "flash_partition_table.h"

#elif PLATFORM_ESPIDF

#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "esp_flash_spi_init.h"

#elif PLATFORM_REALTEK

#include "flash_api.h"
#include "device_lock.h"
#include "sys_api.h"

extern flash_t flash;

#if PLATFORM_RTL87X0C

#include "ota_8710c.h"

extern uint32_t sys_update_ota_get_curr_fw_idx(void);
extern uint32_t sys_update_ota_prepare_addr(void);
extern void sys_disable_fast_boot(void);
extern void get_fw_info(uint32_t* targetFWaddr, uint32_t* currentFWaddr, uint32_t* fw1_sn, uint32_t* fw2_sn);
static flash_t flash_ota;

#elif PLATFORM_RTL8710B

#include "rtl8710b_ota.h"

extern uint32_t current_fw_idx;

#elif PLATFORM_RTL8710A

extern uint32_t current_fw_idx;

#undef DEFAULT_FLASH_LEN
#define DEFAULT_FLASH_LEN 0x400000

#elif PLATFORM_RTL8720D

#include "rtl8721d_boot.h"
#include "rtl8721d_ota.h"
#include "diag.h"
#include "wdt_api.h"

extern uint32_t current_fw_idx;
extern uint8_t flash_size_8720;

#undef DEFAULT_FLASH_LEN
#define DEFAULT_FLASH_LEN (flash_size_8720 << 20)

#endif

#elif PLATFORM_ECR6600

#include "flash.h"
extern int ota_init(void);
extern int ota_write(unsigned char* data, unsigned int len);
extern int ota_done(bool reset);

#elif PLATFORM_TR6260

#include "otaHal.h"
#include "drv_spiflash.h"

#else

extern unsigned int flash_read(char* user_buf, unsigned int count, unsigned int address);

#endif

#include "../new_cfg.h"
// Commands register, execution API and cmd tokenizer
#include "../cmnds/cmd_public.h"

#ifndef OBK_DISABLE_ALL_DRIVERS
#include "../driver/drv_local.h"
#endif

#define MAX_JSON_VALUE_LENGTH   128


static int http_rest_error(http_request_t* request, int code, char* msg);

static int http_rest_get(http_request_t* request);
static int http_rest_post(http_request_t* request);
static int http_rest_app(http_request_t* request);

static int http_rest_post_pins(http_request_t* request);
static int http_rest_get_pins(http_request_t* request);

static int http_rest_get_channelTypes(http_request_t* request);
static int http_rest_post_channelTypes(http_request_t* request);

static int http_rest_get_seriallog(http_request_t* request);

static int http_rest_post_logconfig(http_request_t* request);
static int http_rest_get_logconfig(http_request_t* request);

#if ENABLE_LITTLEFS
static int http_rest_get_lfs_delete(http_request_t* request);
static int http_rest_get_lfs_file(http_request_t* request);
static int http_rest_run_lfs_file(http_request_t* request);
static int http_rest_post_lfs_file(http_request_t* request);
#endif

static int http_rest_post_reboot(http_request_t* request);
static int http_rest_post_flash(http_request_t* request, int startaddr, int maxaddr);
static int http_rest_get_flash(http_request_t* request, int startaddr, int len);
static int http_rest_get_flash_advanced(http_request_t* request);
static int http_rest_post_flash_advanced(http_request_t* request);

static int http_rest_get_info(http_request_t* request);

static int http_rest_get_dumpconfig(http_request_t* request);
static int http_rest_get_testconfig(http_request_t* request);

static int http_rest_post_channels(http_request_t* request);
static int http_rest_get_channels(http_request_t* request);

static int http_rest_get_flash_vars_test(http_request_t* request);

static int http_rest_post_cmd(http_request_t* request);


void init_rest() {
	HTTP_RegisterCallback("/api/", HTTP_GET, http_rest_get, 1);
	HTTP_RegisterCallback("/api/", HTTP_POST, http_rest_post, 1);
	HTTP_RegisterCallback("/app", HTTP_GET, http_rest_app, 1);
}

/* Extracts string token value into outBuffer (128 char). Returns true if the operation was successful. */
bool tryGetTokenString(const char* json, jsmntok_t* tok, char* outBuffer) {
	int length;
	if (tok == NULL || tok->type != JSMN_STRING) {
		return false;
	}

	length = tok->end - tok->start;

	//Don't have enough buffer
	if (length > MAX_JSON_VALUE_LENGTH) {
		return false;
	}

	memset(outBuffer, '\0', MAX_JSON_VALUE_LENGTH); //Wipe previous value
	strncpy(outBuffer, json + tok->start, length);
	return true;
}

static int http_rest_get(http_request_t* request) {
	ADDLOG_DEBUG(LOG_FEATURE_API, "GET of %s", request->url);

	if (!strcmp(request->url, "api/channels")) {
		return http_rest_get_channels(request);
	}

	if (!strcmp(request->url, "api/pins")) {
		return http_rest_get_pins(request);
	}
	if (!strcmp(request->url, "api/channelTypes")) {
		return http_rest_get_channelTypes(request);
	}
	if (!strcmp(request->url, "api/logconfig")) {
		return http_rest_get_logconfig(request);
	}

	if (!strncmp(request->url, "api/seriallog", 13)) {
		return http_rest_get_seriallog(request);
	}

#if ENABLE_LITTLEFS
	if (!strcmp(request->url, "api/fsblock")) {
		uint32_t newsize = CFG_GetLFS_Size();
		uint32_t newstart = (LFS_BLOCKS_END - newsize);

		newsize = (newsize / LFS_BLOCK_SIZE) * LFS_BLOCK_SIZE;

		// double check again that we're within bounds - don't want
		// boot overwrite or anything nasty....
		if (newstart < LFS_BLOCKS_START_MIN) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}
		if ((newstart + newsize > LFS_BLOCKS_END) ||
			(newstart + newsize < LFS_BLOCKS_START_MIN)) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}

		return http_rest_get_flash(request, newstart, newsize);
	}
#endif

#if ENABLE_LITTLEFS
	if (!strncmp(request->url, "api/lfs/", 8)) {
		return http_rest_get_lfs_file(request);
	}
	if (!strncmp(request->url, "api/run/", 8)) {
		return http_rest_run_lfs_file(request);
	}
	if (!strncmp(request->url, "api/del/", 8)) {
		return http_rest_get_lfs_delete(request);
	}
#endif

	if (!strcmp(request->url, "api/info")) {
		return http_rest_get_info(request);
	}

	if (!strncmp(request->url, "api/flash/", 10)) {
		return http_rest_get_flash_advanced(request);
	}

	if (!strcmp(request->url, "api/dumpconfig")) {
		return http_rest_get_dumpconfig(request);
	}

	if (!strcmp(request->url, "api/testconfig")) {
		return http_rest_get_testconfig(request);
	}

	if (!strncmp(request->url, "api/testflashvars", 17)) {
		return http_rest_get_flash_vars_test(request);
	}

	http_setup(request, httpMimeTypeHTML);
	http_html_start(request, "GET REST API");
	poststr(request, "GET of ");
	poststr(request, request->url);
	http_html_end(request);
	poststr(request, NULL);
	return 0;
}

static int http_rest_post(http_request_t* request) {
	char tmp[20];
	ADDLOG_DEBUG(LOG_FEATURE_API, "POST to %s", request->url);

	if (!strcmp(request->url, "api/channels")) {
		return http_rest_post_channels(request);
	}
	
	if (!strcmp(request->url, "api/pins")) {
		return http_rest_post_pins(request);
	}
	if (!strcmp(request->url, "api/channelTypes")) {
		return http_rest_post_channelTypes(request);
	}
	if (!strcmp(request->url, "api/logconfig")) {
		return http_rest_post_logconfig(request);
	}

	if (!strcmp(request->url, "api/reboot")) {
		return http_rest_post_reboot(request);
	}
	if (!strcmp(request->url, "api/ota")) {
#if PLATFORM_BEKEN
		return http_rest_post_flash(request, START_ADR_OF_BK_PARTITION_OTA, LFS_BLOCKS_END);
#elif PLATFORM_W600
		return http_rest_post_flash(request, -1, -1);
#elif PLATFORM_W800
		return http_rest_post_flash(request, -1, -1);
#elif PLATFORM_BL602
		return http_rest_post_flash(request, -1, -1);
#elif PLATFORM_LN882H
		return http_rest_post_flash(request, -1, -1);
#elif PLATFORM_ESPIDF
		return http_rest_post_flash(request, -1, -1);
#elif PLATFORM_REALTEK
		return http_rest_post_flash(request, 0, -1);
#elif PLATFORM_ECR6600 || PLATFORM_TR6260
		return http_rest_post_flash(request, -1, -1);
#elif PLATFORM_XRADIO && !PLATFORM_XR809
		return http_rest_post_flash(request, 0, -1);
#else
		// TODO
		ADDLOG_DEBUG(LOG_FEATURE_API, "No OTA");
#endif
	}
	if (!strncmp(request->url, "api/flash/", 10)) {
		return http_rest_post_flash_advanced(request);
	}

	if (!strcmp(request->url, "api/cmnd")) {
		return http_rest_post_cmd(request);
	}


#if ENABLE_LITTLEFS
	if (!strcmp(request->url, "api/fsblock")) {
		if (lfs_present()) {
			release_lfs();
		}
		uint32_t newsize = CFG_GetLFS_Size();
		uint32_t newstart = (LFS_BLOCKS_END - newsize);

		newsize = (newsize / LFS_BLOCK_SIZE) * LFS_BLOCK_SIZE;

		// double check again that we're within bounds - don't want
		// boot overwrite or anything nasty....
		if (newstart < LFS_BLOCKS_START_MIN) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}
		if ((newstart + newsize > LFS_BLOCKS_END) ||
			(newstart + newsize < LFS_BLOCKS_START_MIN)) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}

		// we are writing the lfs block
		int res = http_rest_post_flash(request, newstart, LFS_BLOCKS_END);
		// initialise the filesystem, it should be there now.
		// don't create if it does not mount
		init_lfs(0);
		return res;
	}
	if (!strncmp(request->url, "api/lfs/", 8)) {
		return http_rest_post_lfs_file(request);
	}
#endif

	http_setup(request, httpMimeTypeHTML);
	http_html_start(request, "POST REST API");
	poststr(request, "POST to ");
	poststr(request, request->url);
	poststr(request, "<br/>Content Length:");
	sprintf(tmp, "%d", request->contentLength);
	poststr(request, tmp);
	poststr(request, "<br/>Content:[");
	poststr(request, request->bodystart);
	poststr(request, "]<br/>");
	http_html_end(request);
	poststr(request, NULL);
	return 0;
}

static int http_rest_app(http_request_t* request) {
	const char* webhost = CFG_GetWebappRoot();
	const char* ourip = HAL_GetMyIPString(); //CFG_GetOurIP();
	http_setup(request, httpMimeTypeHTML);
	if (webhost && ourip) {
		poststr(request, htmlDoctype);

		poststr(request, "<head><title>");
		poststr(request, CFG_GetDeviceName());
		poststr(request, "</title>");

		poststr(request, htmlShortcutIcon);
		poststr(request, htmlHeadMeta);
		hprintf255(request, "<script>var root='%s',device='http://%s';</script>", webhost, ourip);
		hprintf255(request, "<script src='%s/startup.js'></script>", webhost);
		poststr(request, "</head><body></body></html>");
	}
	else {
		http_html_start(request, "Not available");
		poststr(request, htmlFooterReturnToMainPage);
		poststr(request, "no APP available<br/>");
		http_html_end(request);
	}
	poststr(request, NULL);
	return 0;
}

#if ENABLE_LITTLEFS

int EndsWith(const char* str, const char* suffix)
{
	if (!str || !suffix)
		return 0;
	size_t lenstr = strlen(str);
	size_t lensuffix = strlen(suffix);
	if (lensuffix > lenstr)
		return 0;
	return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}
char *my_memmem(const char *haystack, int haystack_len, const char *needle, int needle_len) {
	if (needle_len == 0 || haystack_len < needle_len)
		return NULL;

	for (int i = 0; i <= haystack_len - needle_len; i++) {
		if (memcmp(haystack + i, needle, needle_len) == 0)
			return (char *)(haystack + i);
	}
	return NULL;
}
typedef struct berryBuilder_s {

	char berry_buffer[4096];
	int berry_len;
} berryBuilder_t;

void BB_Start(berryBuilder_t *b)
{
	b->berry_buffer[0] = 0;
	b->berry_len = 0;
}
void BB_AddCode(berryBuilder_t *b, const char *start, const char *end) {
	int len;
	if (end) {
		len = end - start;
	}
	else {
		len = strlen(start);
	}
	memcpy(&b->berry_buffer[b->berry_len], start, len);
	b->berry_len += len;
}
void BB_AddText(berryBuilder_t *b, const char *fname, const char *start, const char *end) {
	BB_AddCode(b, " echo(\"",0);
#if 0
	BB_AddCode(b, start, end);
#else
	const char *p = start;
	const char *limit = end ? end : (start + strlen(start));
	while (p < limit) {
		char c = *p++;
		switch (c) {
		case '\\': BB_AddCode(b, "\\\\", 0); break;
		case '\"': BB_AddCode(b, "\\\"", 0); break;
		case '\n': BB_AddCode(b, "\\n", 0); break;
		case '\r': BB_AddCode(b, "\\r", 0); break;
		case '\t': BB_AddCode(b, "\\t", 0); break;
		default:
			BB_AddCode(b, &c, &c + 1);
			break;
		}
	}

#endif
	BB_AddCode(b, "\")", 0);
}
void eval_berry_snippet(const char *s);
void Berry_SaveRequest(http_request_t *r);
void BB_Run(berryBuilder_t *b)
{
	b->berry_buffer[b->berry_len] = 0;
	eval_berry_snippet(b->berry_buffer);
}
int http_runBerryFile(http_request_t *request, const char *fname) {
	Berry_SaveRequest(request);
	berryBuilder_t bb;
	BB_Start(&bb);
	char *data = (char*)LFS_ReadFile(fname);
	if (data == 0)
		return 0;
	http_setup(request, httpMimeTypeHTML);
	char *p = data;
	while (*p) {
		char *btag = strstr(p, "<?b");
		if (!btag) {
			break;
		}
		BB_AddText(&bb, fname, p, btag);
		char *etag = strstr(btag, "?>");

		BB_AddCode(&bb, btag + 3, etag);

		p = etag + 2;
	}
	const char *s = p;
	while (*p)
		p++;
	BB_AddText(&bb, fname, s, p);
	free(data);
	BB_Run(&bb);
	return 1;
}
static int http_rest_run_lfs_file(http_request_t* request) {
	char* fpath;
	// don't start LFS just because we're trying to read a file -
	// it won't exist anyway
	if (!lfs_present()) {
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, NULL);
		return 0;
	}
#if ENABLE_OBK_BERRY
	const char* base = request->url + strlen("api/lfs/");
	const char* q = strchr(base, '?');
	size_t len = q ? (size_t)(q - base) : strlen(base);
	fpath = os_malloc(len + 1);
	memcpy(fpath, base, len);
	fpath[len] = '\0';
	int ran = http_runBerryFile(request, fpath);
	if (ran==0) 
#endif
	{
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, NULL);
		return 0;
	}
	free(fpath);
	return 0;
}
static int http_rest_get_lfs_file(http_request_t* request) {
	char* fpath;
	char* buff;
	int len;
	int lfsres;
	int total = 0;
	lfs_file_t* file;
	char *args;

	// don't start LFS just because we're trying to read a file -
	// it won't exist anyway
	if (!lfs_present()) {
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, NULL);
		return 0;
	}

	fpath = os_malloc(strlen(request->url) - strlen("api/lfs/") + 1);

	buff = os_malloc(1024);
	file = os_malloc(sizeof(lfs_file_t));
	memset(file, 0, sizeof(lfs_file_t));

	strcpy(fpath, request->url + strlen("api/lfs/"));

	// strip HTTP args with ?
	args = strchr(fpath, '?');
	if (args) {
		*args = 0;
	}

	ADDLOG_DEBUG(LOG_FEATURE_API, "LFS read of %s", fpath);
	lfsres = lfs_file_open(&lfs, file, fpath, LFS_O_RDONLY);

	if (lfsres == -21) {
		lfs_dir_t* dir;
		ADDLOG_DEBUG(LOG_FEATURE_API, "%s is a folder", fpath);
		dir = os_malloc(sizeof(lfs_dir_t));
		os_memset(dir, 0, sizeof(*dir));
		// if the thing is a folder.
		lfsres = lfs_dir_open(&lfs, dir, fpath);

		if (lfsres >= 0) {
			// this is needed during iteration...?
			struct lfs_info info;
			int count = 0;
			http_setup(request, httpMimeTypeJson);
			ADDLOG_DEBUG(LOG_FEATURE_API, "opened folder %s lfs result %d", fpath, lfsres);
			hprintf255(request, "{\"dir\":\"%s\",\"content\":[", fpath);
			do {
				// Read an entry in the directory
				//
				// Fills out the info structure, based on the specified file or directory.
				// Returns a positive value on success, 0 at the end of directory,
				// or a negative error code on failure.
				lfsres = lfs_dir_read(&lfs, dir, &info);
				if (lfsres > 0) {
					if (count) poststr(request, ",");
					hprintf255(request, "{\"name\":\"%s\",\"type\":%d,\"size\":%d}",
						info.name, info.type, info.size);
				}
				else {
					if (lfsres < 0) {
						if (count) poststr(request, ",");
						hprintf255(request, "{\"error\":%d}", lfsres);
					}
				}
				count++;
			} while (lfsres > 0);

			hprintf255(request, "]}");

			lfs_dir_close(&lfs, dir);
			if (dir) os_free(dir);
			dir = NULL;
		}
		else {
			if (dir) os_free(dir);
			dir = NULL;
			request->responseCode = HTTP_RESPONSE_NOT_FOUND;
			http_setup(request, httpMimeTypeJson);
			ADDLOG_DEBUG(LOG_FEATURE_API, "failed to open %s lfs result %d", fpath, lfsres);
			hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, lfsres);
		}
	}
	else {
		ADDLOG_DEBUG(LOG_FEATURE_API, "LFS open [%s] gives %d", fpath, lfsres);
		if (lfsres >= 0) {
			const char* mimetype = httpMimeTypeBinary;
			do {
				if (EndsWith(fpath, ".ico")) {
					mimetype = "image/x-icon";
					break;
				}
				if (EndsWith(fpath, ".js") || EndsWith(fpath, ".vue")) {
					mimetype = httpMimeTypeJavascript;
					break;
				}
				if (EndsWith(fpath, ".json")) {
					mimetype = httpMimeTypeJson;
					break;
				}
				if (EndsWith(fpath, ".html")) {
					mimetype = httpMimeTypeHTML;
					break;
				}
				if (EndsWith(fpath, ".css")) {
					mimetype = httpMimeTypeCSS;
					break;
				}
				break;
			} while (0);

			http_setup(request, mimetype);
//#if ENABLE_OBK_BERRY
//			http_runBerryFile(request, fpath);
//#else
			do {
				len = lfs_file_read(&lfs, file, buff, 1024);
				total += len;
				if (len) {
					//ADDLOG_DEBUG(LOG_FEATURE_API, "%d bytes read", len);
					postany(request, buff, len);
				}
		} while (len > 0);
//#endif
			lfs_file_close(&lfs, file);
			ADDLOG_DEBUG(LOG_FEATURE_API, "%d total bytes read", total);
		}
		else {
			request->responseCode = HTTP_RESPONSE_NOT_FOUND;
			http_setup(request, httpMimeTypeJson);
			ADDLOG_DEBUG(LOG_FEATURE_API, "failed to open %s lfs result %d", fpath, lfsres);
			hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, lfsres);
		}
	}
	poststr(request, NULL);
	if (fpath) os_free(fpath);
	if (file) os_free(file);
	if (buff) os_free(buff);
	return 0;
}

static int http_rest_get_lfs_delete(http_request_t* request) {
	char* fpath;
	int lfsres;

	// don't start LFS just because we're trying to read a file -
	// it won't exist anyway
	if (!lfs_present()) {
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, "Not found");
		poststr(request, NULL);
		return 0;
	}

	fpath = os_malloc(strlen(request->url) - strlen("api/del/") + 1);

	strcpy(fpath, request->url + strlen("api/del/"));

	ADDLOG_DEBUG(LOG_FEATURE_API, "LFS delete of %s", fpath);
	lfsres = lfs_remove(&lfs, fpath);

	if (lfsres == LFS_ERR_OK) {
		ADDLOG_DEBUG(LOG_FEATURE_API, "LFS delete of %s OK", fpath);

		poststr(request, "OK");
	}
	else {
		ADDLOG_DEBUG(LOG_FEATURE_API, "LFS delete of %s error %i", fpath, lfsres);
		poststr(request, "Error");
	}
	poststr(request, NULL);
	if (fpath) os_free(fpath);
	return 0;
}

static int http_rest_post_lfs_file(http_request_t* request) {
	int len;
	int lfsres;
	int total = 0;
	int loops = 0;

	// allocated variables
	lfs_file_t* file;
	char* fpath;
	char* folder;

	// create if it does not exist
	init_lfs(1);

	if (!lfs_present()) {
		request->responseCode = 400;
		http_setup(request, httpMimeTypeText);
		poststr(request, "LittleFS is not available");
		poststr(request, NULL);
		return 0;
	}

	fpath = os_malloc(strlen(request->url) - strlen("api/lfs/") + 1);
	file = os_malloc(sizeof(lfs_file_t));
	memset(file, 0, sizeof(lfs_file_t));

	strcpy(fpath, request->url + strlen("api/lfs/"));
	ADDLOG_DEBUG(LOG_FEATURE_API, "LFS write of %s len %d", fpath, request->contentLength);

	folder = strchr(fpath, '/');
	if (folder) {
		int folderlen = folder - fpath;
		folder = os_malloc(folderlen + 1);
		strncpy(folder, fpath, folderlen);
		folder[folderlen] = 0;
		ADDLOG_DEBUG(LOG_FEATURE_API, "file is in folder %s try to create", folder);
		lfsres = lfs_mkdir(&lfs, folder);
		if (lfsres < 0) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "mkdir error %d", lfsres);
		}
	}

	//ADDLOG_DEBUG(LOG_FEATURE_API, "LFS write of %s len %d", fpath, request->contentLength);

	lfsres = lfs_file_open(&lfs, file, fpath, LFS_O_RDWR | LFS_O_CREAT);
	if (lfsres >= 0) {
		//ADDLOG_DEBUG(LOG_FEATURE_API, "opened %s");
		int towrite = request->bodylen;
		char* writebuf = request->bodystart;
		int writelen = request->bodylen;
		if (request->contentLength >= 0) {
			towrite = request->contentLength;
		}
		//ADDLOG_DEBUG(LOG_FEATURE_API, "bodylen %d, contentlen %d", request->bodylen, request->contentLength);

		if (writelen < 0) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "ABORTED: %d bytes to write", writelen);
			lfs_file_close(&lfs, file);
			request->responseCode = HTTP_RESPONSE_SERVER_ERROR;
			http_setup(request, httpMimeTypeJson);
			hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, -20);
			goto exit;
		}

		do {
			loops++;
			if (loops > 10) {
				loops = 0;
				rtos_delay_milliseconds(10);
			}
			//ADDLOG_DEBUG(LOG_FEATURE_API, "%d bytes to write", writelen);
			len = lfs_file_write(&lfs, file, writebuf, writelen);
			if (len < 0) {
				ADDLOG_ERROR(LOG_FEATURE_API, "Failed to write to %s with error %i", fpath,len);
				break;
			}
			total += len;
			if (len > 0) {
				//ADDLOG_DEBUG(LOG_FEATURE_API, "%d bytes written", len);
			}
			towrite -= len;
			if (towrite > 0) {
				writebuf = request->received;
				writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
				if (writelen < 0) {
					ADDLOG_DEBUG(LOG_FEATURE_API, "recv returned %d - end of data - remaining %d", writelen, towrite);
				}
			}
		} while ((towrite > 0) && (writelen >= 0));

		// no more data
		lfs_file_truncate(&lfs, file, total);

		//ADDLOG_DEBUG(LOG_FEATURE_API, "closing %s", fpath);
		lfs_file_close(&lfs, file);
		ADDLOG_DEBUG(LOG_FEATURE_API, "%d total bytes written", total);
		http_setup(request, httpMimeTypeJson);
		hprintf255(request, "{\"fname\":\"%s\",\"size\":%d}", fpath, total);
	}
	else {
		request->responseCode = HTTP_RESPONSE_SERVER_ERROR;
		http_setup(request, httpMimeTypeJson);
		ADDLOG_DEBUG(LOG_FEATURE_API, "failed to open %s err %d", fpath, lfsres);
		hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, lfsres);
	}
exit:
	poststr(request, NULL);
	if (folder) os_free(folder);
	if (file) os_free(file);
	if (fpath) os_free(fpath);
	return 0;
}

// static int http_favicon(http_request_t* request) {
// 	request->url = "api/lfs/favicon.ico";
// 	return http_rest_get_lfs_file(request);
// }

#else
// static int http_favicon(http_request_t* request) {
// 	request->responseCode = HTTP_RESPONSE_NOT_FOUND;
// 	http_setup(request, httpMimeTypeHTML);
// 	poststr(request, NULL);
// 	return 0;
// }
#endif



static int http_rest_get_seriallog(http_request_t* request) {
	if (request->url[strlen(request->url) - 1] == '1') {
		direct_serial_log = 1;
	}
	else {
		direct_serial_log = 0;
	}
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "Direct serial logging set to %d", direct_serial_log);
	poststr(request, NULL);
	return 0;
}



static int http_rest_get_pins(http_request_t* request) {
	int i;
	int maxNonZero;
	http_setup(request, httpMimeTypeJson);
	poststr(request, "{\"rolenames\":[");
	for (i = 0; i < IOR_Total_Options; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "\"%s\"", htmlPinRoleNames[i]);
	}
	poststr(request, "],\"roles\":[");

	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "%d", g_cfg.pins.roles[i]);
	}
	// TODO: maybe we should cull futher channels that are not used?
	// I support many channels because I plan to use 16x relays module with I2C MCP23017 driver

	// find max non-zero ch
	//maxNonZero = -1;
	//for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
	//	if (g_cfg.pins.channels[i] != 0) {
	//		maxNonZero = i;
	//	}
	//}

	poststr(request, "],\"channels\":[");
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "%d", g_cfg.pins.channels[i]);
	}
	// find max non-zero ch2
	maxNonZero = -1;	
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (g_cfg.pins.channels2[i] != 0) {
			maxNonZero = i;
		}
	}
	if (maxNonZero != -1) {
		poststr(request, "],\"channels2\":[");
		for (i = 0; i <= maxNonZero; i++) {
			if (i) {
				hprintf255(request, ",");
			}
			hprintf255(request, "%d", g_cfg.pins.channels2[i]);
		}
	}
	poststr(request, "],\"states\":[");
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "%d", CHANNEL_Get(g_cfg.pins.channels[i]));
	}
	poststr(request, "]}");
	poststr(request, NULL);
	return 0;
}


static int http_rest_get_channelTypes(http_request_t* request) {
	int i;

	http_setup(request, httpMimeTypeJson);
	poststr(request, "{\"typenames\":[");
	for (i = 0; i < ChType_Max; i++) {
		if (i) {
			hprintf255(request, ",\"%s\"", g_channelTypeNames[i]);
		}
		else {
			hprintf255(request, "\"%s\"", g_channelTypeNames[i]);
		}
	}
	poststr(request, "],\"types\":[");

	for (i = 0; i < CHANNEL_MAX; i++) {
		if (i) {
			hprintf255(request, ",%d", g_cfg.pins.channelTypes[i]);
		}
		else {
			hprintf255(request, "%d", g_cfg.pins.channelTypes[i]);
		}
	}
	poststr(request, "]}");
	poststr(request, NULL);
	return 0;
}



////////////////////////////
// log config
static int http_rest_get_logconfig(http_request_t* request) {
	int i;
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"level\":%d,", g_loglevel);
	hprintf255(request, "\"features\":%d,", logfeatures);
	poststr(request, "\"levelnames\":[");
	for (i = 0; i < LOG_MAX; i++) {
		if (i) {
			hprintf255(request, ",\"%s\"", loglevelnames[i]);
		}
		else {
			hprintf255(request, "\"%s\"", loglevelnames[i]);
		}
	}
	poststr(request, "],\"featurenames\":[");
	for (i = 0; i < LOG_FEATURE_MAX; i++) {
		if (i) {
			hprintf255(request, ",\"%s\"", logfeaturenames[i]);
		}
		else {
			hprintf255(request, "\"%s\"", logfeaturenames[i]);
		}
	}
	poststr(request, "]}");
	poststr(request, NULL);
	return 0;
}

static int http_rest_post_logconfig(http_request_t* request) {
	int i;
	int r;
	char tmp[64];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	http_setup(request, httpMimeTypeText);
	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * 128);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		poststr(request, NULL);
		os_free(p);
		os_free(t);
		return 0;
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Object expected", r);
		poststr(request, NULL);
		os_free(p);
		os_free(t);
		return 0;
	}

	//sprintf(tmp,"parsed JSON: %s\n", json_str);
	//poststr(request, tmp);
	//poststr(request, NULL);

		/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		if (jsoneq(json_str, &t[i], "level") == 0) {
			if (t[i + 1].type != JSMN_PRIMITIVE) {
				continue; /* We expect groups to be an array of strings */
			}
			g_loglevel = atoi(json_str + t[i + 1].start);
			i += t[i + 1].size + 1;
		}
		else if (jsoneq(json_str, &t[i], "features") == 0) {
			if (t[i + 1].type != JSMN_PRIMITIVE) {
				continue; /* We expect groups to be an array of strings */
			}
			logfeatures = atoi(json_str + t[i + 1].start);;
			i += t[i + 1].size + 1;
		}
		else {
			ADDLOG_ERROR(LOG_FEATURE_API, "Unexpected key: %.*s", t[i].end - t[i].start,
				json_str + t[i].start);
			snprintf(tmp, sizeof(tmp), "Unexpected key: %.*s\n", t[i].end - t[i].start,
				json_str + t[i].start);
			poststr(request, tmp);
		}
	}

	poststr(request, NULL);
	os_free(p);
	os_free(t);
	return 0;
}

/////////////////////////////////////////////////


static int http_rest_get_info(http_request_t* request) {
	char macstr[3 * 6 + 1];
	long int* pAllGenericFlags = (long int*)&g_cfg.genericFlags;

	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"uptime_s\":%d,", g_secondsElapsed);
	hprintf255(request, "\"build\":\"%s\",", g_build_str);
	hprintf255(request, "\"ip\":\"%s\",", HAL_GetMyIPString());
	hprintf255(request, "\"mac\":\"%s\",", HAL_GetMACStr(macstr));
	hprintf255(request, "\"flags\":\"%ld\",", *pAllGenericFlags);
	hprintf255(request, "\"mqtthost\":\"%s:%d\",", CFG_GetMQTTHost(), CFG_GetMQTTPort());
	hprintf255(request, "\"mqtttopic\":\"%s\",", CFG_GetMQTTClientId());
	hprintf255(request, "\"chipset\":\"%s\",", PLATFORM_MCU_NAME);
	hprintf255(request, "\"webapp\":\"%s\",", CFG_GetWebappRoot());
	hprintf255(request, "\"shortName\":\"%s\",", CFG_GetShortDeviceName());
	poststr(request, "\"startcmd\":\"");
	// This can be longer than 255
	poststr_escapedForJSON(request, CFG_GetShortStartupCommand());
	poststr(request, "\",");
#ifndef OBK_DISABLE_ALL_DRIVERS
	hprintf255(request, "\"supportsSSDP\":%d,", DRV_IsRunning("SSDP") ? 1 : 0);
#else
	hprintf255(request, "\"supportsSSDP\":0,");
#endif

	hprintf255(request, "\"supportsClientDeviceDB\":true}");

	poststr(request, NULL);
	return 0;
}

static int http_rest_post_pins(http_request_t* request) {
	int i;
	int r;
	char tmp[64];
	int iChanged = 0;
	char tokenStrValue[MAX_JSON_VALUE_LENGTH + 1];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * TOKEN_COUNT);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		sprintf(tmp, "Failed to parse JSON: %d\n", r);
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Object expected", r);
		sprintf(tmp, "Object expected\n");
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		if (tryGetTokenString(json_str, &t[i], tokenStrValue) != true) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "Parsing failed");
			continue;
		}
		//ADDLOG_DEBUG(LOG_FEATURE_API, "parsed %s", tokenStrValue);

		if (strcmp(tokenStrValue, "roles") == 0) {
			int j;
			if (t[i + 1].type != JSMN_ARRAY) {
				continue; /* We expect groups to be an array of strings */
			}
			for (j = 0; j < t[i + 1].size; j++) {
				int roleval, pr;
				jsmntok_t* g = &t[i + j + 2];
				roleval = atoi(json_str + g->start);
				pr = PIN_GetPinRoleForPinIndex(j);
				if (pr != roleval) {
					PIN_SetPinRoleForPinIndex(j, roleval);
					iChanged++;
				}
			}
			i += t[i + 1].size + 1;
		}
		else if (strcmp(tokenStrValue, "channels") == 0) {
			int j;
			if (t[i + 1].type != JSMN_ARRAY) {
				continue; /* We expect groups to be an array of strings */
			}
			for (j = 0; j < t[i + 1].size; j++) {
				int chanval, pr;
				jsmntok_t* g = &t[i + j + 2];
				chanval = atoi(json_str + g->start);
				pr = PIN_GetPinChannelForPinIndex(j);
				if (pr != chanval) {
					PIN_SetPinChannelForPinIndex(j, chanval);
					iChanged++;
				}
			}
			i += t[i + 1].size + 1;
		}
		else if (strcmp(tokenStrValue, "deviceFlag") == 0) {
			int flag;
			jsmntok_t* flagTok = &t[i + 1];
			if (flagTok == NULL || flagTok->type != JSMN_PRIMITIVE) {
				continue;
			}

			flag = atoi(json_str + flagTok->start);
			ADDLOG_DEBUG(LOG_FEATURE_API, "received deviceFlag %d", flag);

			if (flag >= 0 && flag <= 10) {
				CFG_SetFlag(flag, true);
				iChanged++;
			}

			i += t[i + 1].size + 1;
		}
		else if (strcmp(tokenStrValue, "deviceCommand") == 0) {
			if (tryGetTokenString(json_str, &t[i + 1], tokenStrValue) == true) {
				ADDLOG_DEBUG(LOG_FEATURE_API, "received deviceCommand %s", tokenStrValue);
				CFG_SetShortStartupCommand_AndExecuteNow(tokenStrValue);
				iChanged++;
			}

			i += t[i + 1].size + 1;
		}
		else {
			ADDLOG_ERROR(LOG_FEATURE_API, "Unexpected key: %.*s", t[i].end - t[i].start,
				json_str + t[i].start);
		}
	}
	if (iChanged) {
		CFG_Save_SetupTimer();
		ADDLOG_DEBUG(LOG_FEATURE_API, "Changed %d - saved to flash", iChanged);
	}

	os_free(p);
	os_free(t);
	return http_rest_error(request, 200, "OK");
}

static int http_rest_post_channelTypes(http_request_t* request) {
	int i;
	int r;
	char tmp[64];
	int iChanged = 0;
	char tokenStrValue[MAX_JSON_VALUE_LENGTH + 1];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * TOKEN_COUNT);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		sprintf(tmp, "Failed to parse JSON: %d\n", r);
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Object expected", r);
		sprintf(tmp, "Object expected\n");
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		if (tryGetTokenString(json_str, &t[i], tokenStrValue) != true) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "Parsing failed");
			continue;
		}
		//ADDLOG_DEBUG(LOG_FEATURE_API, "parsed %s", tokenStrValue);

		if (strcmp(tokenStrValue, "types") == 0) {
			int j;
			if (t[i + 1].type != JSMN_ARRAY) {
				continue; /* We expect groups to be an array of strings */
			}
			for (j = 0; j < t[i + 1].size; j++) {
				int typeval, pr;
				jsmntok_t* g = &t[i + j + 2];
				typeval = atoi(json_str + g->start);
				pr = CHANNEL_GetType(j);
				if (pr != typeval) {
					CHANNEL_SetType(j, typeval);
					iChanged++;
				}
			}
			i += t[i + 1].size + 1;
		}
		else {
			ADDLOG_ERROR(LOG_FEATURE_API, "Unexpected key: %.*s", t[i].end - t[i].start,
				json_str + t[i].start);
		}
	}
	if (iChanged) {
		CFG_Save_SetupTimer();
		ADDLOG_DEBUG(LOG_FEATURE_API, "Changed %d - saved to flash", iChanged);
	}

	os_free(p);
	os_free(t);
	return http_rest_error(request, 200, "OK");
}

static int http_rest_error(http_request_t* request, int code, char* msg) {
	request->responseCode = code;
	http_setup(request, httpMimeTypeJson);
	if (code != 200) {
		hprintf255(request, "{\"error\":%d, \"msg\":\"%s\"}", code, msg);
	}
	else {
		hprintf255(request, "{\"success\":%d, \"msg\":\"%s\"}", code, msg);
	}
	poststr(request, NULL);
	return 0;
}

#if PLATFORM_BL602

typedef struct ota_header {
	union {
		struct {
			uint8_t header[16];

			uint8_t type[4];//RAW XZ
			uint32_t len;//body len
			uint8_t pad0[8];

			uint8_t ver_hardware[16];
			uint8_t ver_software[16];

			uint8_t sha256[32];
			uint32_t unpacked_len;//full len
		} s;
		uint8_t _pad[512];
	} u;
} ota_header_t;
#define OTA_HEADER_SIZE (sizeof(ota_header_t))

static int _check_ota_header(ota_header_t *ota_header, uint32_t *ota_len, int *use_xz)
{
	char str[33];//assume max segment size
	int i;

	memcpy(str, ota_header->u.s.header, sizeof(ota_header->u.s.header));
	str[sizeof(ota_header->u.s.header)] = '\0';
	puts("[OTA] [HEADER] ota header is ");
	puts(str);
	puts("\r\n");

	memcpy(str, ota_header->u.s.type, sizeof(ota_header->u.s.type));
	str[sizeof(ota_header->u.s.type)] = '\0';
	puts("[OTA] [HEADER] file type is ");
	puts(str);
	puts("\r\n");
	if (strstr(str, "XZ")) {
		*use_xz = 1;
	}
	else {
		*use_xz = 0;
	}

	memcpy(ota_len, &(ota_header->u.s.len), 4);
	printf("[OTA] [HEADER] file length (exclude ota header) is %lu\r\n", *ota_len);

	memcpy(str, ota_header->u.s.ver_hardware, sizeof(ota_header->u.s.ver_hardware));
	str[sizeof(ota_header->u.s.ver_hardware)] = '\0';
	puts("[OTA] [HEADER] ver_hardware is ");
	puts(str);
	puts("\r\n");

	memcpy(str, ota_header->u.s.ver_software, sizeof(ota_header->u.s.ver_software));
	str[sizeof(ota_header->u.s.ver_software)] = '\0';
	puts("[OTA] [HEADER] ver_software is ");
	puts(str);
	puts("\r\n");

	memcpy(str, ota_header->u.s.sha256, sizeof(ota_header->u.s.sha256));
	str[sizeof(ota_header->u.s.sha256)] = '\0';
	puts("[OTA] [HEADER] sha256 is ");
	for (i = 0; i < sizeof(ota_header->u.s.sha256); i++) {
		printf("%02X", str[i]);
	}
	puts("\r\n");

	return 0;
}
#endif

#if PLATFORM_LN882H
#include "ota_port.h"
#include "ota_image.h"
#include "ota_types.h"
#include "hal/hal_flash.h"
#include "netif/ethernetif.h"
#include "flash_partition_table.h"


#define KV_OTA_UPG_STATE           ("kv_ota_upg_state")
#define HTTP_OTA_DEMO_STACK_SIZE   (1024 * 16)

#define SECTOR_SIZE_4KB            (1024 * 4)

static char g_http_uri_buff[512] = "http://192.168.122.48:9090/ota-images/otaimage-v1.3.bin";

// a block to save http data.
static char *temp4K_buf = NULL;
static int   temp4k_offset = 0;

// where to save OTA data in flash.
static int32_t flash_ota_start_addr = OTA_SPACE_OFFSET;
static int32_t flash_ota_offset = 0;
static uint8_t is_persistent_started = LN_FALSE;
static uint8_t is_ready_to_verify = LN_FALSE;
static uint8_t is_precheck_ok = LN_FALSE;
static uint8_t httpc_ota_started = 0;

/**
 * @brief Pre-check the image file to be downloaded.
 *
 * @attention None
 *
 * @param[in]  app_offset  The offset of the APP partition in Flash.
 * @param[in]  ota_hdr     pointer to ota partition info struct.
 *
 * @return  whether the check is successful.
 * @retval  #LN_TRUE     successful.
 * @retval  #LN_FALSE    failed.
 */
static int ota_download_precheck(uint32_t app_offset, image_hdr_t * ota_hdr)
{

	image_hdr_t *app_hdr = NULL;
	if (NULL == (app_hdr = OS_Malloc(sizeof(image_hdr_t)))) {
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "[%s:%d] malloc failed.\r\n", __func__, __LINE__);
		return LN_FALSE;
	}

	if (OTA_ERR_NONE != image_header_fast_read(app_offset, app_hdr)) {
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "failed to read app header.\r\n");
		goto ret_err;
	}

	if ((ota_hdr->image_type == IMAGE_TYPE_ORIGINAL) || \
		(ota_hdr->image_type == IMAGE_TYPE_ORIGINAL_XZ))
	{
		// check version
		if (((ota_hdr->ver.ver_major << 8) + ota_hdr->ver.ver_minor) == \
			((app_hdr->ver.ver_major << 8) + app_hdr->ver.ver_minor)) {
			ADDLOG_DEBUG(LOG_FEATURE_OTA, "[%s:%d] same version, do not upgrade!\r\n",
				__func__, __LINE__);
		}

		// check file size
		if (((ota_hdr->img_size_orig + sizeof(image_hdr_t)) > APP_SPACE_SIZE) || \
			((ota_hdr->img_size_orig_xz + sizeof(image_hdr_t)) > OTA_SPACE_SIZE)) {
			ADDLOG_DEBUG(LOG_FEATURE_OTA, "[%s:%d] size check failed.\r\n", __func__, __LINE__);
			goto ret_err;
		}
	}
	else {
		//image type not support!
		goto ret_err;
	}

	OS_Free(app_hdr);
	return LN_TRUE;

ret_err:
	OS_Free(app_hdr);
	return LN_FALSE;
}

static int ota_persistent_start(void)
{
	if (NULL == temp4K_buf) {
		temp4K_buf = OS_Malloc(SECTOR_SIZE_4KB);
		if (NULL == temp4K_buf) {
			LOG(LOG_LVL_INFO,"failed to alloc 4KB!!!\r\n");
			return LN_FALSE;
		}
		memset(temp4K_buf, 0, SECTOR_SIZE_4KB);
	}

	temp4k_offset = 0;
	flash_ota_start_addr = OTA_SPACE_OFFSET;
	flash_ota_offset = 0;
	is_persistent_started = LN_TRUE;
	return LN_TRUE;
}

/**
 * @brief Save block to flash.
 *
 * @param buf
 * @param buf_len
 * @return return LN_TRUE on success, LN_FALSE on failure.
 */
static int ota_persistent_write(const char *buf, const int32_t buf_len)
{
	int part_len = SECTOR_SIZE_4KB; // we might have a buffer so large, that we need to write multiple 4K segments ...
	int buf_offset = 0; // ... and we need to keep track, what is already written

	if (!is_persistent_started) {
		return LN_TRUE;
	}

	if (temp4k_offset + buf_len < SECTOR_SIZE_4KB) {
		// just copy all buf data to temp4K_buf
		memcpy(temp4K_buf + temp4k_offset, buf, buf_len);
		temp4k_offset += buf_len;
		part_len = 0;
	}
	while (part_len >= SECTOR_SIZE_4KB) {           // so we didn't copy all data to buffer (part_len would be 0 then)
		// just copy part of buf to temp4K_buf
		part_len = temp4k_offset + buf_len - buf_offset - SECTOR_SIZE_4KB;		// beware, this can be > SECTOR_SIZE_4KB !!!
		memcpy(temp4K_buf + temp4k_offset, buf + buf_offset, buf_len - buf_offset - part_len);
		temp4k_offset += buf_len - buf_offset - part_len;
		buf_offset = buf_len - part_len;

		if (temp4k_offset >= SECTOR_SIZE_4KB ) {		
			// write to flash
			ADDLOG_DEBUG(LOG_FEATURE_OTA, "write at flash: 0x%08x (temp4k_offset=%i)\r\n", flash_ota_start_addr + flash_ota_offset,temp4k_offset);

			if (flash_ota_offset == 0) {
				if (LN_TRUE != ota_download_precheck(APP_SPACE_OFFSET, (image_hdr_t *)temp4K_buf)) 
				{
					ADDLOG_DEBUG(LOG_FEATURE_OTA, "ota download precheck failed!\r\n");
					is_precheck_ok = LN_FALSE;
					return LN_FALSE;
				}
			is_precheck_ok = LN_TRUE;
			}

			hal_flash_erase(flash_ota_start_addr + flash_ota_offset, SECTOR_SIZE_4KB);
			hal_flash_program(flash_ota_start_addr + flash_ota_offset, SECTOR_SIZE_4KB, (uint8_t *)temp4K_buf);

			flash_ota_offset += SECTOR_SIZE_4KB;
			memset(temp4K_buf, 0, SECTOR_SIZE_4KB);
			temp4k_offset = 0;
		}
	}
	if (part_len > 0) {
		memcpy(temp4K_buf + temp4k_offset, buf + (buf_len - part_len), part_len);
		temp4k_offset += part_len;
	}

	return LN_TRUE;
}

/**
 * @brief save last block and clear flags.
 * @return return LN_TRUE on success, LN_FALSE on failure.
 */
static int ota_persistent_finish(void)
{
	if (!is_persistent_started) {
		return LN_FALSE;
	}

	// write to flash
	ADDLOG_DEBUG(LOG_FEATURE_OTA, "write at flash: 0x%08x\r\n", flash_ota_start_addr + flash_ota_offset);
	hal_flash_erase(flash_ota_start_addr + flash_ota_offset, SECTOR_SIZE_4KB);
	hal_flash_program(flash_ota_start_addr + flash_ota_offset, SECTOR_SIZE_4KB, (uint8_t *)temp4K_buf);

	OS_Free(temp4K_buf);
	temp4K_buf = NULL;
	temp4k_offset = 0;

	flash_ota_offset = 0;
	is_persistent_started = LN_FALSE;
	return LN_TRUE;
}

static int update_ota_state(void)
{
	upg_state_t state = UPG_STATE_DOWNLOAD_OK;
	ln_nvds_set_ota_upg_state(state);
	return LN_TRUE;
}
/**
 * @brief check ota image header, body.
 * @return return LN_TRUE on success, LN_FALSE on failure.
 */
static int ota_verify_download(void)
{
	image_hdr_t ota_header;

	ADDLOG_DEBUG(LOG_FEATURE_OTA, "Succeed to verify OTA image content.\r\n");
	if (OTA_ERR_NONE != image_header_fast_read(OTA_SPACE_OFFSET, &ota_header)) {
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "failed to read ota header.\r\n");
		return LN_FALSE;
	}

	if (OTA_ERR_NONE != image_header_verify(&ota_header)) {
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "failed to verify ota header.\r\n");
		return LN_FALSE;
	}

	if (OTA_ERR_NONE != image_body_verify(OTA_SPACE_OFFSET, &ota_header)) {
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "failed to verify ota body.\r\n");
		return LN_FALSE;
	}

	ADDLOG_DEBUG(LOG_FEATURE_OTA, "Succeed to verify OTA image content.\r\n");
	return LN_TRUE;
}
#endif

static int http_rest_post_flash(http_request_t* request, int startaddr, int maxaddr)
{
	int total = 0;
	int towrite = request->bodylen;
	char* writebuf = request->bodystart;
	int writelen = request->bodylen;
	int fsize = 0;

	ADDLOG_DEBUG(LOG_FEATURE_OTA, "OTA post len %d", request->contentLength);

#ifdef PLATFORM_W600
	int nRetCode = 0;
	char error_message[256];

	if(writelen < 0)
	{
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "ABORTED: %d bytes to write", writelen);
		return http_rest_error(request, -20, "writelen < 0");
	}

	struct pbuf* p;

	//Data is uploaded in 1024 sized chunks, creating a bigger buffer just in case this assumption changes.
	//The code below is based on sdk\OpenW600\src\app\ota\wm_http_fwup.c
	char* Buffer = (char*)os_malloc(2048 + 3);
	memset(Buffer, 0, 2048 + 3);

	if(request->contentLength >= 0)
	{
		towrite = request->contentLength;
	}

	int recvLen = 0;
	int totalLen = 0;
	//printf("\ntowrite %d writelen=%d\n", towrite, writelen);

	do
	{
		if(writelen > 0)
		{
			//bk_printf("Copying %d from writebuf to Buffer towrite=%d\n", writelen, towrite);
			memcpy(Buffer + 3, writebuf, writelen);

			if(recvLen == 0)
			{
				T_BOOTER* booter = (T_BOOTER*)(Buffer + 3);
				bk_printf("magic_no=%u, img_type=%u, zip_type=%u\n", booter->magic_no, booter->img_type, booter->zip_type);

				if(TRUE == tls_fwup_img_header_check(booter))
				{
					totalLen = booter->upd_img_len + sizeof(T_BOOTER);
					OTA_ResetProgress();
					OTA_SetTotalBytes(totalLen);
				}
				else
				{
					sprintf(error_message, "Image header check failed");
					nRetCode = -19;
					break;
				}

				nRetCode = socket_fwup_accept(0, ERR_OK);
				if(nRetCode != ERR_OK)
				{
					sprintf(error_message, "Firmware update startup failed");
					break;
				}
			}

			p = pbuf_alloc(PBUF_TRANSPORT, writelen + 3, PBUF_REF);
			if(!p)
			{
				sprintf(error_message, "Unable to allocate memory for buffer");
				nRetCode = -18;
				break;
			}

			if(recvLen == 0)
			{
				*Buffer = SOCKET_FWUP_START;
			}
			else if(recvLen == (totalLen - writelen))
			{
				*Buffer = SOCKET_FWUP_END;
			}
			else
			{
				*Buffer = SOCKET_FWUP_DATA;
			}

			*(Buffer + 1) = (writelen >> 8) & 0xFF;
			*(Buffer + 2) = writelen & 0xFF;
			p->payload = Buffer;
			p->len = p->tot_len = writelen + 3;

			nRetCode = socket_fwup_recv(0, p, ERR_OK);
			if(nRetCode != ERR_OK)
			{
				sprintf(error_message, "Firmware data processing failed");
				break;
			}
			else
			{
				OTA_IncrementProgress(writelen);
				recvLen += writelen;
				printf("Downloaded %d / %d\n", recvLen, totalLen);
			}

			towrite -= writelen;
		}

		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				sprintf(error_message, "recv returned %d - end of data - remaining %d", writelen, towrite);
				nRetCode = -17;
			}
		}
	} while((nRetCode == 0) && (towrite > 0) && (writelen >= 0));

	tls_mem_free(Buffer);

	if(nRetCode != 0)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, error_message);
		socket_fwup_err(0, nRetCode);
		return http_rest_error(request, nRetCode, error_message);
	}


#elif PLATFORM_W800
	int nRetCode = 0;
	char error_message[256];

	if(writelen < 0)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "ABORTED: %d bytes to write", writelen);
		return http_rest_error(request, -20, "writelen < 0");
	}

	struct pbuf* p;

	//The code below is based on W600 code and adopted to the differences in sdk\OpenW800\src\app\ota\wm_http_fwup.c
	// fiexd crashing caused by not checking "writelen" before doing memcpy
	// e.g. if more than 2 packets arrived before next loop, writelen could be > 2048 !!
#define FWUP_MSG_SIZE			 3
#define MAX_BUFF_SIZE			 2048
	char* Buffer = (char*)os_malloc(MAX_BUFF_SIZE + FWUP_MSG_SIZE);

	if(!Buffer)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "ABORTED: failed to allocate buffer");
		return http_rest_error(request, -20, "");
	}

	if(request->contentLength >= 0)
	{
		towrite = request->contentLength;
	}

	int recvLen = 0;
	int totalLen = 0;
	printf("\ntowrite %d writelen=%d\n", towrite, writelen);

	do
	{
		while(writelen > 0)
		{
			int actwrite = writelen < MAX_BUFF_SIZE ? writelen : MAX_BUFF_SIZE;	// mustn't write more than Buffers size! Will crash else!
			//bk_printf("Copying %d from writebuf to Buffer (writelen=%d) towrite=%d -- free_heap:%d\n", actwrite, writelen, towrite, xPortGetFreeHeapSize());
			memset(Buffer, 0, MAX_BUFF_SIZE + FWUP_MSG_SIZE);
			memcpy(Buffer + FWUP_MSG_SIZE, writebuf, actwrite);
			if(recvLen == 0)
			{
				IMAGE_HEADER_PARAM_ST *booter = (IMAGE_HEADER_PARAM_ST*)(Buffer + FWUP_MSG_SIZE);
				bk_printf("magic_no=%u, img_type=%u, zip_type=%u, signature=%u\n",
				booter->magic_no, booter->img_attr.b.img_type, booter->img_attr.b.zip_type, booter->img_attr.b.signature);

				if(TRUE == tls_fwup_img_header_check(booter))
				{
					totalLen = booter->img_len + sizeof(IMAGE_HEADER_PARAM_ST);
					if (booter->img_attr.b.signature)
					{
							totalLen += 128;
					}
				}
				else
				{
					sprintf(error_message, "Image header check failed");
					nRetCode = -19;
					break;
				}

				nRetCode = socket_fwup_accept(0, ERR_OK);
				if(nRetCode != ERR_OK)
				{
					sprintf(error_message, "Firmware update startup failed");
					break;
				}
			}

			p = pbuf_alloc(PBUF_TRANSPORT, actwrite + FWUP_MSG_SIZE, PBUF_REF);
			if(!p)
			{
				sprintf(error_message, "Unable to allocate memory for buffer");
				nRetCode = -18;
				break;
			}

			if(recvLen == 0)
			{
				*Buffer = SOCKET_FWUP_START;
			}
			else if(recvLen == (totalLen - actwrite))
			{
				*Buffer = SOCKET_FWUP_END;
			}
			else
			{
				*Buffer = SOCKET_FWUP_DATA;
			}

			*(Buffer + 1) = (actwrite >> 8) & 0xFF;
			*(Buffer + 2) = actwrite & 0xFF;
			p->payload = Buffer;
			p->len = p->tot_len = actwrite + FWUP_MSG_SIZE;

			nRetCode = socket_fwup_recv(0, p, ERR_OK);
			if(nRetCode != ERR_OK)
			{
				sprintf(error_message, "Firmware data processing failed");
				break;
			}
			else
			{
				recvLen += actwrite;
			}

			towrite -= actwrite;
			writelen -= actwrite;	// calculate, how much is left to write
			writebuf += actwrite;	// in case, we only wrote part of buffer, advance in buffer
		}

		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				sprintf(error_message, "recv returned %d - end of data - remaining %d", writelen, towrite);
				nRetCode = -17;
			}
		}
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Downloaded %d / %d", recvLen, totalLen);
		rtos_delay_milliseconds(10);	// give some time for flashing - will else increase used memory fast 
	} while((nRetCode == 0) && (towrite > 0) && (writelen >= 0));
	bk_printf("Download completed (%d / %d)\n", recvLen, totalLen);
	if(Buffer) os_free(Buffer);
	if(p) pbuf_free(p);


	if(nRetCode != 0)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, error_message);
		socket_fwup_err(0, nRetCode);
		return http_rest_error(request, nRetCode, error_message);
	}


#elif PLATFORM_BL602
	int sockfd, i;
	int ret;
	struct hostent* hostinfo;
	uint8_t* recv_buffer;
	struct sockaddr_in dest;
	iot_sha256_context ctx;
	uint8_t sha256_result[32];
	uint8_t sha256_img[32];
	bl_mtd_handle_t handle;
	//init_ota(startaddr);


#define OTA_PROGRAM_SIZE (512)
	int ota_header_found, use_xz;
	ota_header_t* ota_header = 0;

	ret = bl_mtd_open(BL_MTD_PARTITION_NAME_FW_DEFAULT, &handle, BL_MTD_OPEN_FLAG_BACKUP);
	if(ret)
	{
		return http_rest_error(request, -20, "Open Default FW partition failed");
	}

	recv_buffer = pvPortMalloc(OTA_PROGRAM_SIZE);

	unsigned int buffer_offset, flash_offset, ota_addr;
	uint32_t bin_size, part_size, running_size;
	uint8_t activeID;
	HALPartition_Entry_Config ptEntry;

	activeID = hal_boot2_get_active_partition();

	printf("Starting OTA test. OTA bin addr is %p, incoming len %i\r\n", recv_buffer, writelen);

	printf("[OTA] [TEST] activeID is %u\r\n", activeID);

	if(hal_boot2_get_active_entries(BOOT2_PARTITION_TYPE_FW, &ptEntry))
	{
		printf("PtTable_Get_Active_Entries fail\r\n");
		vPortFree(recv_buffer);
		bl_mtd_close(handle);
		return http_rest_error(request, -20, "PtTable_Get_Active_Entries fail");
	}
	ota_addr = ptEntry.Address[!ptEntry.activeIndex];
	bin_size = ptEntry.maxLen[!ptEntry.activeIndex];
	part_size = ptEntry.maxLen[!ptEntry.activeIndex];
	running_size = ptEntry.maxLen[ptEntry.activeIndex];
	(void)part_size;
	/*XXX if you use bin_size is product env, you may want to set bin_size to the actual
	 * OTA BIN size, and also you need to splilt XIP_SFlash_Erase_With_Lock into
	 * serveral pieces. Partition size vs bin_size check is also needed
	 */
	printf("Starting OTA test. OTA size is %lu\r\n", bin_size);

	printf("[OTA] [TEST] activeIndex is %u, use OTA address=%08x\r\n", ptEntry.activeIndex, (unsigned int)ota_addr);

	printf("[OTA] [TEST] Erase flash with size %lu...", bin_size);
	hal_update_mfg_ptable();

	//Erase in chunks, because erasing everything at once is slow and causes issues with http connection
	uint32_t erase_offset = 0;
	uint32_t erase_len = 0;
	while(erase_offset < bin_size)
	{
		erase_len = bin_size - erase_offset;
		if(erase_len > 0x10000)
		{
			erase_len = 0x10000; //Erase in 64kb chunks
		}
		bl_mtd_erase(handle, erase_offset, erase_len);
		printf("[OTA] Erased:  %lu / %lu \r\n", erase_offset, erase_len);
		erase_offset += erase_len;
		rtos_delay_milliseconds(100);
	}
	printf("[OTA] Done\r\n");

	if(request->contentLength >= 0)
	{
		towrite = request->contentLength;
	}

	// get header
	// recv_buffer	
	//buffer_offset = 0;
	//do {
	//	int take_len;

	//	take_len = OTA_PROGRAM_SIZE - buffer_offset;

	//	memcpy(recv_buffer + buffer_offset, writebuf, writelen);
	//	buffer_offset += writelen;


	//	if (towrite > 0) {
	//		writebuf = request->received;
	//		writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
	//		if (writelen < 0) {
	//			ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
	//		}
	//	}
	//} while(true)

	buffer_offset = 0;
	flash_offset = 0;
	ota_header = 0;
	use_xz = 0;

	utils_sha256_init(&ctx);
	utils_sha256_starts(&ctx);
	memset(sha256_result, 0, sizeof(sha256_result));
	do
	{
		char* useBuf = writebuf;
		int useLen = writelen;

		if(ota_header == 0)
		{
			int take_len;

			// how much left for header?
			take_len = OTA_PROGRAM_SIZE - buffer_offset;
			// clamp to available len
			if(take_len > useLen)
				take_len = useLen;
			printf("Header takes %i. ", take_len);
			memcpy(recv_buffer + buffer_offset, writebuf, take_len);
			buffer_offset += take_len;
			useBuf = writebuf + take_len;
			useLen = writelen - take_len;

			if(buffer_offset >= OTA_PROGRAM_SIZE)
			{
				ota_header = (ota_header_t*)recv_buffer;
				if(strncmp((const char*)ota_header, "BL60X_OTA", 9))
				{
					return http_rest_error(request, -20, "Invalid header ident");
				}
			}
		}


		if(ota_header && useLen)
		{


			if(flash_offset + useLen >= part_size)
			{
				return http_rest_error(request, -20, "Too large bin");
			}
			if(ota_header->u.s.unpacked_len != 0xFFFFFFFF && running_size < ota_header->u.s.unpacked_len)
			{
				ADDLOG_ERROR(LOG_FEATURE_OTA, "Unpacked OTA image size (%u) is bigger than running partition size (%u)", ota_header->u.s.unpacked_len, running_size);
				return http_rest_error(request, -20, "");
			}
			//ADDLOG_DEBUG(LOG_FEATURE_OTA, "%d bytes to write", writelen);
			//add_otadata((unsigned char*)writebuf, writelen);

			printf("Flash takes %i. ", useLen);
			utils_sha256_update(&ctx, (byte*)useBuf, useLen);
			bl_mtd_write(handle, flash_offset, useLen, (byte*)useBuf);
			flash_offset += useLen;
		}

		total += writelen;
		startaddr += writelen;
		towrite -= writelen;


		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			}
		}
	} while((towrite > 0) && (writelen >= 0));

	if(ota_header == 0)
	{
		return http_rest_error(request, -20, "No header found");
	}
	utils_sha256_finish(&ctx, sha256_result);
	puts("\r\nCalculated SHA256 Checksum:");
	for(i = 0; i < sizeof(sha256_result); i++)
	{
		printf("%02X", sha256_result[i]);
	}
	puts("\r\nHeader SHA256 Checksum:");
	for(i = 0; i < sizeof(sha256_result); i++)
	{
		printf("%02X", ota_header->u.s.sha256[i]);
	}
	if(memcmp(ota_header->u.s.sha256, sha256_result, sizeof(sha256_img)))
	{
		/*Error found*/
		return http_rest_error(request, -20, "SHA256 NOT Correct");
	}
	printf("[OTA] [TCP] prepare OTA partition info\r\n");
	ptEntry.len = total;
	printf("[OTA] [TCP] Update PARTITION, partition len is %lu\r\n", ptEntry.len);
	hal_boot2_update_ptable(&ptEntry);
	printf("[OTA] [TCP] Rebooting\r\n");
	//close_ota();
	vPortFree(recv_buffer);
	utils_sha256_free(&ctx);
	bl_mtd_close(handle);

#elif PLATFORM_LN882H
	ADDLOG_DEBUG(LOG_FEATURE_OTA, "Ota start!\r\n");
	if(LN_TRUE != ota_persistent_start())
	{
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Ota start error, exit...\r\n");
		return 0;
	}

	if(request->contentLength >= 0)
	{
		towrite = request->contentLength;
	}

	do
	{
		//ADDLOG_DEBUG(LOG_FEATURE_OTA, "%d bytes to write", writelen);

		if(LN_TRUE != ota_persistent_write(writebuf, writelen))
		{
			//	ADDLOG_DEBUG(LOG_FEATURE_OTA, "ota write err.\r\n");
			return -1;
		}

		rtos_delay_milliseconds(10);
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Writelen %i at %i", writelen, total);
		total += writelen;
		startaddr += writelen;
		towrite -= writelen;
		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			}
		}
	} while((towrite > 0) && (writelen >= 0));

	ota_persistent_finish();
	is_ready_to_verify = LN_TRUE;
	ADDLOG_DEBUG(LOG_FEATURE_OTA, "cb info: recv %d finished, no more data to deal with.\r\n", towrite);


	ADDLOG_DEBUG(LOG_FEATURE_OTA, "http client job done, exit...\r\n");
	if(LN_TRUE == is_precheck_ok)
	{
		if((LN_TRUE == is_ready_to_verify) && (LN_TRUE == ota_verify_download()))
		{
			update_ota_state();
			//ln_chip_reboot();
		}
		else
		{
			ADDLOG_DEBUG(LOG_FEATURE_OTA, "Veri bad\r\n");
		}
	}
	else
	{
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Precheck bad\r\n");
	}


#elif PLATFORM_ESPIDF

	ADDLOG_DEBUG(LOG_FEATURE_OTA, "Ota start!\r\n");
	esp_err_t err;
	esp_ota_handle_t update_handle = 0;
	const esp_partition_t* update_partition = NULL;
	const esp_partition_t* running = esp_ota_get_running_partition();
	update_partition = esp_ota_get_next_update_partition(NULL);
	if(request->contentLength >= 0)
	{
		fsize = towrite = request->contentLength;
	}

	esp_wifi_set_ps(WIFI_PS_NONE);
	bool image_header_was_checked = false;
	do
	{
		if(image_header_was_checked == false)
		{
			esp_app_desc_t new_app_info;
			if(towrite > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
			{
				memcpy(&new_app_info, &writebuf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "New firmware version: %s", new_app_info.version);

				esp_app_desc_t running_app_info;
				if(esp_ota_get_partition_description(running, &running_app_info) == ESP_OK)
				{
					ADDLOG_DEBUG(LOG_FEATURE_OTA, "Running firmware version: %s", running_app_info.version);
				}

				image_header_was_checked = true;

				err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
				if(err != ESP_OK)
				{
					ADDLOG_ERROR(LOG_FEATURE_OTA, "esp_ota_begin failed (%s)", esp_err_to_name(err));
					esp_ota_abort(update_handle);
					return -1;
				}
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "esp_ota_begin succeeded");
			}
			else
			{
				ADDLOG_ERROR(LOG_FEATURE_OTA, "received package is not fit len");
				esp_ota_abort(update_handle);
				return -1;
			}
		}
		err = esp_ota_write(update_handle, (const void*)writebuf, writelen);
		if(err != ESP_OK)
		{
			esp_ota_abort(update_handle);
			return -1;
		}

		ADDLOG_DEBUG(LOG_FEATURE_OTA, "OTA in progress: %.1f%%", (100 - ((float)towrite / fsize) * 100));
		total += writelen;
		startaddr += writelen;
		towrite -= writelen;

		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			}
		}
	} while((towrite > 0) && (writelen >= 0));

	ADDLOG_INFO(LOG_FEATURE_OTA, "OTA in progress: 100%%, total Write binary data length: %d", total);

	err = esp_ota_end(update_handle);
	if(err != ESP_OK)
	{
		if(err == ESP_ERR_OTA_VALIDATE_FAILED)
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "Image validation failed, image is corrupted");
		}
		else
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "esp_ota_end failed (%s)!", esp_err_to_name(err));
		}
		return -1;
	}
	err = esp_ota_set_boot_partition(update_partition);
	if(err != ESP_OK)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
		return -1;
	}

#elif PLATFORM_RTL87X0C

	uint32_t NewFWLen = 0, NewFWAddr = 0;
	uint32_t address = 0;
	uint32_t curr_fw_idx = 0;
	uint32_t flash_checksum = 0;
	uint32_t targetFWaddr;
	uint32_t currentFWaddr;
	uint32_t fw1_sn;
	uint32_t fw2_sn;
	_file_checksum file_checksum;
	file_checksum.u = 0;
	unsigned char sig_backup[32];
	int ret = 1;

	if(request->contentLength >= 0)
	{
		towrite = request->contentLength;
	}
	NewFWAddr = sys_update_ota_prepare_addr();
	if(NewFWAddr == -1)
	{
		ret = -1;
		goto update_ota_exit;
	}
	get_fw_info(&targetFWaddr, &currentFWaddr, &fw1_sn, &fw2_sn);
	ADDLOG_INFO(LOG_FEATURE_OTA, "Current FW addr: 0x%08X, target FW addr: 0x%08X, fw1 sn: %u, fw2 sn: %u", currentFWaddr, targetFWaddr, fw1_sn, fw2_sn);
	curr_fw_idx = sys_update_ota_get_curr_fw_idx();
	ADDLOG_INFO(LOG_FEATURE_OTA, "Current firmware index is %d", curr_fw_idx);
	int reserase = update_ota_erase_upg_region(towrite, 0, NewFWAddr);
	NewFWLen = towrite;
	if(reserase == -1)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Erase failed");
		ret = -1;
		goto update_ota_exit;
	}
	if(NewFWAddr != ~0x0)
	{
		address = NewFWAddr;
		ADDLOG_INFO(LOG_FEATURE_OTA, "Start to read data %i bytes", NewFWLen);
	}
	do
	{
		// back up signature and only write it to flash till the end of OTA
		if(startaddr < 32)
		{
			memcpy(sig_backup + startaddr, writebuf, (startaddr + writelen > 32 ? (32 - startaddr) : writelen));
			memset(writebuf, 0xFF, (startaddr + writelen > 32 ? (32 - startaddr) : writelen));
			ADDLOG_DEBUG(LOG_FEATURE_OTA, "sig_backup for% d bytes from index% d", (startaddr + writelen > 32 ? (32 - startaddr) : writelen), startaddr);
		}

		device_mutex_lock(RT_DEV_LOCK_FLASH);
		if(flash_burst_write(&flash_ota, address + startaddr, writelen, (uint8_t*)writebuf) < 0)
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "Write stream failed");
			device_mutex_unlock(RT_DEV_LOCK_FLASH);
			ret = -1;
			goto update_ota_exit;
		}
		device_mutex_unlock(RT_DEV_LOCK_FLASH);

		rtos_delay_milliseconds(10);
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Writelen %i at %i", writelen, total);
		total += writelen;
		startaddr += writelen;
		towrite -= writelen;

		// checksum attached at file end
		if(startaddr + writelen > NewFWLen - 4)
		{
			file_checksum.c[0] = writebuf[writelen - 4];
			file_checksum.c[1] = writebuf[writelen - 3];
			file_checksum.c[2] = writebuf[writelen - 2];
			file_checksum.c[3] = writebuf[writelen - 1];
		}
		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			}
		}
	} while((towrite > 0) && (writelen >= 0));
	ADDLOG_DEBUG(LOG_FEATURE_OTA, "%d total bytes written, verifying checksum", total);
	uint8_t* buf = (uint8_t*)os_malloc(2048);
	memset(buf, 0, 2048);
	// read flash data back and calculate checksum
	for(int i = 0; i < NewFWLen; i += 2048)
	{
		int k;
		int rlen = (startaddr - 4 - i) > 2048 ? 2048 : (startaddr - 4 - i);
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_stream_read(&flash_ota, NewFWAddr + i, rlen, buf);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		for(k = 0; k < rlen; k++)
		{
			if(i + k < 32)
			{
				flash_checksum += sig_backup[i + k];
			}
			else
			{
				flash_checksum += buf[k];
			}
		}
	}

	ADDLOG_INFO(LOG_FEATURE_OTA, "flash checksum 0x%8x attached checksum 0x%8x", flash_checksum, file_checksum.u);

	if(file_checksum.u != flash_checksum)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "The checksum is wrong!");
		ret = -1;
		goto update_ota_exit;
	}
	ret = update_ota_signature(sig_backup, NewFWAddr);
	if(ret == -1)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Update signature fail");
		goto update_ota_exit;
	}
update_ota_exit:
	if(ret != -1)
	{
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_write_word(&flash, targetFWaddr, 4294967295);
		flash_write_word(&flash, currentFWaddr, 0);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);

		sys_disable_fast_boot();
		if(buf) free(buf);
	}
	else
	{
		if(buf) free(buf);
		return http_rest_error(request, ret, "error");
	}

#elif PLATFORM_RTL8710B

	int NewImg2BlkSize = 0;
	uint32_t NewFWLen = 0, NewFWAddr = 0;
	uint32_t address = 0;
	uint32_t flash_checksum = 0;
	uint32_t ota2_addr = OTA2_ADDR;
	union { uint32_t u; unsigned char c[4]; } file_checksum;
	unsigned char sig_backup[32];
	int ret = 1;
	char* hbuf = NULL;
	bool foundhdr = false;

	if(request->contentLength > 0)
	{
		towrite = request->contentLength;
	}
	else
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Content-length is 0");
		goto update_ota_exit;
	}
	int ota1_len = 0, ota2_len = 0;
	char* msg = "Incorrect amount of bytes received: %i, required: %i";

	writebuf = request->received;
	writelen = recv(request->fd, &ota1_len, sizeof(ota1_len), 0);
	if(writelen != sizeof(ota1_len))
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, msg, writelen, sizeof(ota1_len));
		ret = -1;
		goto update_ota_exit;
	}

	writebuf = request->received;
	writelen = recv(request->fd, &ota2_len, sizeof(ota2_len), 0);
	if(writelen != sizeof(ota2_len))
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, msg, writelen, sizeof(ota2_len));
		ret = -1;
		goto update_ota_exit;
	}
	ADDLOG_INFO(LOG_FEATURE_OTA, "OTA1 len: %u, OTA2 len: %u", ota1_len, ota2_len);

	if(ota1_len <= 0 || ota2_len <= 0)
	{
		ret = -1;
		goto update_ota_exit;
	}
	towrite -= 8;

	if(current_fw_idx == OTA_INDEX_1)
	{
		towrite = ota2_len;
		// skip ota1
		int toskip = ota1_len;
		do
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax < toskip ? request->receivedLenmax : toskip, 0);
			ADDLOG_EXTRADEBUG(LOG_FEATURE_OTA, "Skipping %i at %i", writelen, total);
			total += writelen;
			toskip -= writelen;
		} while((toskip > 0) && (writelen >= 0));
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Skipped %i bytes, towrite: %i", total, towrite);
	}
	else
	{
		towrite = ota1_len;
	}

	writelen = 0;
	total = 0;
	NewFWAddr = update_ota_prepare_addr();
	ADDLOG_INFO(LOG_FEATURE_OTA, "OTA address: %#010x, len: %u", NewFWAddr - SPI_FLASH_BASE, towrite);
	if(NewFWAddr == -1 || NewFWAddr == 0xFFFFFFFF)
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Wrong OTA address:", NewFWAddr);
		goto update_ota_exit;
	}
	NewFWLen = towrite;
	if(NewFWAddr == OTA1_ADDR)
	{
		if(NewFWLen > (OTA2_ADDR - OTA1_ADDR))
		{
			// firmware size too large
			ret = -1;
			ADDLOG_ERROR(LOG_FEATURE_OTA, "image size should not cross OTA2");
			goto update_ota_exit;
		}
	}
	else if(NewFWAddr == OTA2_ADDR)
	{
		if(NewFWLen > (0x195000 + SPI_FLASH_BASE - OTA2_ADDR))
		{
			ret = -1;
			ADDLOG_ERROR(LOG_FEATURE_OTA, "image size crosses OTA2 boundary");
			goto update_ota_exit;
		}
	}
	else if(NewFWAddr == 0xFFFFFFFF)
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "update address is invalid");
		goto update_ota_exit;
	}
	address = NewFWAddr - SPI_FLASH_BASE;
	NewImg2BlkSize = ((NewFWLen - 1) / 4096) + 1;
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	for(int i = 0; i < NewImg2BlkSize; i++)
	{
		flash_erase_sector(&flash, address + i * 4096);
	}
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	OTF_Mask(1, (address), NewImg2BlkSize, 1);

	do
	{
		if(startaddr < 32)
		{
			memcpy(sig_backup + startaddr, writebuf, (startaddr + writelen > 32 ? (32 - startaddr) : writelen));
			memset(writebuf, 0xFF, (startaddr + writelen > 32 ? (32 - startaddr) : writelen));
			ADDLOG_DEBUG(LOG_FEATURE_OTA, "sig_backup for% d bytes from index% d", (startaddr + writelen > 32 ? (32 - startaddr) : writelen), startaddr);
		}
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		if(flash_burst_write(&flash, address + startaddr, writelen, (uint8_t*)writebuf) < 0)
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "Write stream failed");
			device_mutex_unlock(RT_DEV_LOCK_FLASH);
			ret = -1;
			goto update_ota_exit;
		}
		device_mutex_unlock(RT_DEV_LOCK_FLASH);

		rtos_delay_milliseconds(10);
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Writelen %i at %i", writelen, total);
		total += writelen;
		startaddr += writelen;
		towrite -= writelen;

		if(startaddr + writelen > NewFWLen - 4)
		{
			file_checksum.c[0] = writebuf[writelen - 4];
			file_checksum.c[1] = writebuf[writelen - 3];
			file_checksum.c[2] = writebuf[writelen - 2];
			file_checksum.c[3] = writebuf[writelen - 1];
		}

		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax < towrite ? request->receivedLenmax : towrite, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			}
		}
	} while((towrite > 0) && (writelen >= 0));

	uint8_t* buf = (uint8_t*)os_malloc(2048);
	memset(buf, 0, 2048);
	for(int i = 0; i < NewFWLen; i += 2048)
	{
		int k;
		int rlen = (startaddr - 4 - i) > 2048 ? 2048 : (startaddr - 4 - i);
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_stream_read(&flash, address + i, rlen, buf);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		for(k = 0; k < rlen; k++)
		{
			if(i + k < 32)
			{
				flash_checksum += sig_backup[i + k];
			}
			else
			{
				flash_checksum += buf[k];
			}
		}
	}
	ADDLOG_INFO(LOG_FEATURE_OTA, "Update file size = %d flash checksum 0x%8x attached checksum 0x%8x", NewFWLen, flash_checksum, file_checksum.u);
	OTF_Mask(1, (address), NewImg2BlkSize, 0);
	if(file_checksum.u == flash_checksum)
	{
		ADDLOG_INFO(LOG_FEATURE_OTA, "Update OTA success!");

		ret = 0;
	}
	else
	{
		/*if checksum error, clear the signature zone which has been
		written in flash in case of boot from the wrong firmware*/
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_erase_sector(&flash, address);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		ret = -1;
	}
	// make it to be similar to rtl8720c - write signature only after success - to prevent boot failure if something goes wrong
	if(flash_burst_write(&flash, address + 16, 16, sig_backup + 16) < 0)
	{
		ret = -1;
	}
	else
	{
		if(flash_burst_write(&flash, address, 16, sig_backup) < 0)
		{
			ret = -1;
		}
	}
	if(current_fw_idx != OTA_INDEX_1)
	{
		// receive file fully
		int toskip = ota2_len;
		do
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax < toskip ? request->receivedLenmax : toskip, 0);
			total += writelen;
			toskip -= writelen;
		} while((toskip > 0) && (writelen >= 0));
	}
update_ota_exit:
	if(ret != -1)
	{
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		if(current_fw_idx == OTA_INDEX_1)
		{
			OTA_Change(OTA_INDEX_2);
			//ota_write_ota2_addr(OTA2_ADDR);
		}
		else
		{
			OTA_Change(OTA_INDEX_1);
			//ota_write_ota2_addr(0xffffffff);
		}
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		if(buf) free(buf);
	}
	else
	{
		if(buf) free(buf);
		ADDLOG_ERROR(LOG_FEATURE_OTA, "OTA failed");
		return http_rest_error(request, ret, "error");
	}

#elif PLATFORM_RTL8710A

	uint32_t NewFWLen = 0, NewFWAddr = 0;
	uint32_t address = 0;
	uint32_t flash_checksum = 0;
	union { uint32_t u; unsigned char c[4]; } file_checksum;
	int ret = 0;

	if(request->contentLength > 0)
	{
		towrite = request->contentLength;
	}
	else
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Content-length is 0");
		goto update_ota_exit;
	}

	NewFWAddr = update_ota_prepare_addr();
	if(NewFWAddr == -1)
	{
		goto update_ota_exit;
	}

	int reserase = update_ota_erase_upg_region(towrite, 0, NewFWAddr);
	NewFWLen = towrite;
	if(reserase == -1)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Erase failed");
		ret = -1;
		goto update_ota_exit;
	}

	address = NewFWAddr;
	ADDLOG_INFO(LOG_FEATURE_OTA, "Start to read data %i bytes, flash address: 0x%8x", NewFWLen, address);

	do
	{
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		if(flash_stream_write(&flash, address + startaddr, writelen, (uint8_t*)writebuf) < 0)
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "Write stream failed");
			device_mutex_unlock(RT_DEV_LOCK_FLASH);
			ret = -1;
			goto update_ota_exit;
		}
		device_mutex_unlock(RT_DEV_LOCK_FLASH);

		rtos_delay_milliseconds(10);
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Writelen %i at %i", writelen, total);
		total += writelen;
		startaddr += writelen;
		towrite -= writelen;

		// checksum attached at file end
		if(startaddr + writelen > NewFWLen - 4)
		{
			file_checksum.c[0] = writebuf[writelen - 4];
			file_checksum.c[1] = writebuf[writelen - 3];
			file_checksum.c[2] = writebuf[writelen - 2];
			file_checksum.c[3] = writebuf[writelen - 1];
		}
		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			}
		}
	} while((towrite > 0) && (writelen >= 0));
	ADDLOG_DEBUG(LOG_FEATURE_OTA, "%d total bytes written, verifying checksum %u", total, flash_checksum);
	uint8_t* buf = (uint8_t*)os_malloc(512);
	memset(buf, 0, 512);
	// read flash data back and calculate checksum
	for(int i = 0; i < NewFWLen; i += 512)
	{
		int k;
		int rlen = (NewFWLen - 4 - i) > 512 ? 512 : (NewFWLen - 4 - i);
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_stream_read(&flash, NewFWAddr + i, rlen, buf);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		for(k = 0; k < rlen; k++)
		{
			flash_checksum += buf[k];
		}
	}

	ADDLOG_INFO(LOG_FEATURE_OTA, "flash checksum 0x%8x attached checksum 0x%8x", flash_checksum, file_checksum.u);
	delay_ms(50);

	ret = update_ota_checksum(&file_checksum, flash_checksum, NewFWAddr);
	if(ret == -1)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "The checksum is wrong!");
		goto update_ota_exit;
	}

update_ota_exit:
	if(ret != -1)
	{
		//device_mutex_lock(RT_DEV_LOCK_FLASH);
		//device_mutex_unlock(RT_DEV_LOCK_FLASH);
		if(buf) free(buf);
	}
	else
	{
		if(buf) free(buf);
		ADDLOG_ERROR(LOG_FEATURE_OTA, "OTA failed");
		return http_rest_error(request, ret, "error");
	}

#elif PLATFORM_RTL8720D

	int ret = -1;
	uint32_t ota_target_index = OTA_INDEX_2;
	update_file_hdr* pOtaFileHdr;
	update_file_img_hdr* pOtaFileImgHdr;
	update_ota_target_hdr OtaTargetHdr;
	uint32_t ImageCnt, TempLen;
	update_dw_info DownloadInfo[MAX_IMG_NUM];

	int size = 0;
	int read_bytes;
	int read_bytes_buf;
	uint8_t* buf;
	uint32_t OtaFg = 0;
	uint32_t IncFg = 0;
	int RemainBytes = 0;
	uint32_t SigCnt = 0;
	uint32_t TempCnt = 0;
	uint8_t* signature;
	uint32_t sector_cnt = 0;

	if(request->contentLength > 0)
	{
		towrite = request->contentLength;
	}
	else
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Content-length is 0");
		goto update_ota_exit;
	}

	if(flash_size_8720 == 2)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Only 2MB flash detected - OTA is not supported");
		ret = -1;
		goto update_ota_exit;
	}

	memset((uint8_t*)&OtaTargetHdr, 0, sizeof(update_ota_target_hdr));

	DBG_INFO_MSG_OFF(MODULE_FLASH);

	ADDLOG_INFO(LOG_FEATURE_OTA, "Current firmware index is %d", current_fw_idx + 1);
	if(current_fw_idx == OTA_INDEX_1)
	{
		ota_target_index = OTA_INDEX_2;
	}
	else
	{
		ota_target_index = OTA_INDEX_1;
	}

	// get ota header
	writebuf = request->received;
	writelen = recv(request->fd, writebuf, 16, 0);
	if(writelen != 16)
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "failed to recv file header");
		ret = -1;
		goto update_ota_exit;
	}

	pOtaFileHdr = (update_file_hdr*)writebuf;
	pOtaFileImgHdr = (update_file_img_hdr*)(writebuf + 8);

	OtaTargetHdr.FileHdr.FwVer = pOtaFileHdr->FwVer;
	OtaTargetHdr.FileHdr.HdrNum = pOtaFileHdr->HdrNum;

	writelen = recv(request->fd, writebuf + 16, (pOtaFileHdr->HdrNum * pOtaFileImgHdr->ImgHdrLen) - 8, 0);
	writelen = (pOtaFileHdr->HdrNum * pOtaFileImgHdr->ImgHdrLen) + 8;

	// verify ota header
	if(!get_ota_tartget_header((uint8_t*)writebuf, writelen, &OtaTargetHdr, ota_target_index))
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Get OTA header failed");
		goto update_ota_exit;
	}

	//ADDLOG_INFO(LOG_FEATURE_OTA, "Erasing...");
	for(int i = 0; i < OtaTargetHdr.ValidImgCnt; i++)
	{
		ADDLOG_INFO(LOG_FEATURE_OTA, "Target addr:0x%08x, img len: %i", OtaTargetHdr.FileImgHdr[i].FlashAddr, OtaTargetHdr.FileImgHdr[i].ImgLen);
		if(OtaTargetHdr.FileImgHdr[i].ImgLen >= 0x1A8000)
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "Img%i too big, skipping", i);
			OtaTargetHdr.FileImgHdr[i].ImgLen = 0;
			continue;
		}
		//watchdog_stop();
		//erase_ota_target_flash(OtaTargetHdr.FileImgHdr[i].FlashAddr, OtaTargetHdr.FileImgHdr[i].ImgLen);
		//watchdog_start();
	}

	memset((uint8_t*)DownloadInfo, 0, MAX_IMG_NUM * sizeof(update_dw_info));

	ImageCnt = OtaTargetHdr.ValidImgCnt;
	for(uint32_t i = 0; i < ImageCnt; i++)
	{
		if(OtaTargetHdr.FileImgHdr[i].ImgLen == 0)
		{
			DownloadInfo[i].ImageLen = 0;
			continue;
		}
		/* get OTA image and Write New Image to flash, skip the signature,
			not write signature first for power down protection*/
		DownloadInfo[i].ImgId = OTA_IMAG;
		DownloadInfo[i].FlashAddr = OtaTargetHdr.FileImgHdr[i].FlashAddr - SPI_FLASH_BASE + 8;
		DownloadInfo[i].ImageLen = OtaTargetHdr.FileImgHdr[i].ImgLen - 8; /*skip the signature*/
		DownloadInfo[i].ImgOffset = OtaTargetHdr.FileImgHdr[i].Offset;
	}

	/*initialize the reveiving counter*/
	TempLen = (OtaTargetHdr.FileHdr.HdrNum * OtaTargetHdr.FileImgHdr[0].ImgHdrLen) + sizeof(update_file_hdr);

	for(uint32_t i = 0; i < ImageCnt; i++)
	{
		if(DownloadInfo[i].ImageLen == 0) continue;
		/*the next image length*/
		RemainBytes = DownloadInfo[i].ImageLen;
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Remain: %i", RemainBytes);
		signature = &(OtaTargetHdr.Sign[i][0]);
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		FLASH_EraseXIP(EraseSector, DownloadInfo[i].FlashAddr - SPI_FLASH_BASE);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);

		/*download the new firmware from server*/
		while(RemainBytes > 0)
		{
			buf = (uint8_t*)request->received;
			if(IncFg == 1)
			{
				IncFg = 0;
				read_bytes = read_bytes_buf;
			}
			else
			{
				memset(buf, 0, request->receivedLenmax);
				read_bytes = recv(request->fd, buf, request->receivedLenmax, 0);
				if(read_bytes == 0)
				{
					break; // Read end
				}
				if(read_bytes < 0)
				{
					//OtaImgSize = -1;
					//printf("\n\r[%s] Read socket failed", __FUNCTION__);
					//ret = -1;
					//goto update_ota_exit;
					break;
				}
				read_bytes_buf = read_bytes;
				TempLen += read_bytes;
			}

			if(TempLen > DownloadInfo[i].ImgOffset)
			{
				if(!OtaFg)
				{
					/*reach the desired image, the first packet process*/
					OtaFg = 1;
					TempCnt = TempLen - DownloadInfo[i].ImgOffset;
					if(TempCnt < 8)
					{
						SigCnt = TempCnt;
					}
					else
					{
						SigCnt = 8;
					}

					memcpy(signature, buf + read_bytes - TempCnt, SigCnt);

					if((SigCnt < 8) || (TempCnt - 8 == 0))
					{
						continue;
					}

					buf = buf + (read_bytes - TempCnt + 8);
					read_bytes = TempCnt - 8;
				}
				else
				{
					/*normal packet process*/
					if(SigCnt < 8)
					{
						if(read_bytes < (int)(8 - SigCnt))
						{
							memcpy(signature + SigCnt, buf, read_bytes);
							SigCnt += read_bytes;
							continue;
						}
						else
						{
							memcpy(signature + SigCnt, buf, (8 - SigCnt));
							buf = buf + (8 - SigCnt);
							read_bytes -= (8 - SigCnt);
							SigCnt = 8;
							if(!read_bytes)
							{
								continue;
							}
						}
					}
				}

				RemainBytes -= read_bytes;
				if(RemainBytes < 0)
				{
					read_bytes = read_bytes - (-RemainBytes);
				}

				device_mutex_lock(RT_DEV_LOCK_FLASH);
				if(DownloadInfo[i].FlashAddr + size >= DownloadInfo[i].FlashAddr + sector_cnt * 4096)
				{
					sector_cnt++;
					FLASH_EraseXIP(EraseSector, DownloadInfo[i].FlashAddr - SPI_FLASH_BASE + sector_cnt * 4096);
				}
				if(ota_writestream_user(DownloadInfo[i].FlashAddr + size, read_bytes, buf) < 0)
				{
					ADDLOG_ERROR(LOG_FEATURE_OTA, "Writing failed");
					device_mutex_unlock(RT_DEV_LOCK_FLASH);
					ret = -1;
					goto update_ota_exit;
				}
				device_mutex_unlock(RT_DEV_LOCK_FLASH);
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "Written %i bytes at 0x%08x", read_bytes, DownloadInfo[i].FlashAddr + size);
				rtos_delay_milliseconds(5);
				size += read_bytes;
			}
		}

		if((uint32_t)size != (OtaTargetHdr.FileImgHdr[i].ImgLen - 8))
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "Received size != ota size");
			ret = -1;
			goto update_ota_exit;
		}

		/*update flag status*/
		size = 0;
		OtaFg = 0;
		IncFg = 1;
	}

	if(verify_ota_checksum(&OtaTargetHdr))
	{
		if(!change_ota_signature(&OtaTargetHdr, ota_target_index))
		{
			ADDLOG_ERROR(LOG_FEATURE_OTA, "Change signature failed");
			ret = -1;
			goto update_ota_exit;
		}
		ret = 0;
	}

update_ota_exit:
	if(ret != -1)
	{
		ADDLOG_INFO(LOG_FEATURE_OTA, "OTA is successful");
		total = RemainBytes;
	}
	else
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "OTA failed");
		return http_rest_error(request, ret, "error");
	}

#elif PLATFORM_ECR6600 || PLATFORM_TR6260

#if PLATFORM_TR6260
#define OTA_INIT otaHal_init
#define OTA_WRITE otaHal_write
#define OTA_DONE(x) otaHal_done()
#else
#define OTA_INIT ota_init
#define OTA_WRITE ota_write
#define OTA_DONE(x) ota_done(x)
#endif
	int ret = 0;

	if(request->contentLength > 0)
	{
		towrite = request->contentLength;
	}
	else
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Content-length is 0");
		goto update_ota_exit;
	}

	if(OTA_INIT() != 0)
	{
		ret = -1;
		goto update_ota_exit;
	}

	do
	{
		if(OTA_WRITE((unsigned char*)writebuf, writelen) != 0)
		{
			ret = -1;
			goto update_ota_exit;
		}
		delay_ms(10);
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Writelen %i at %i", writelen, total);
		total += writelen;
		startaddr += writelen;
		towrite -= writelen;
		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
				ret = -1;
			}
		}
	} while((towrite > 0) && (writelen >= 0));

update_ota_exit:
	if(ret != -1)
	{
		ADDLOG_INFO(LOG_FEATURE_OTA, "OTA is successful");
		OTA_DONE(0);
	}
	else
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "OTA failed. Reboot to retry");
		return http_rest_error(request, ret, "error");
	}

#elif PLATFORM_XRADIO

bool recvfp = true;

ota_status_t ota_update_rest_init(void* url)
{
	return OTA_STATUS_OK;
}
ota_status_t ota_update_rest_get(uint8_t* buf, uint32_t buf_size, uint32_t* recv_size, uint8_t* eof_flag)
{
	if(recvfp)
	{
		//free(buf);
		//recvfp = false;
		//buf = writebuf;
		//*recv_size = writelen;
		//return OTA_STATUS_OK;
		int bsize = (writelen > buf_size ? buf_size : writelen);
		memcpy(buf, writebuf + startaddr, bsize);
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Writelen %i at %i", bsize, startaddr);
		startaddr += bsize;
		*recv_size = bsize;
		*eof_flag = 0;
		total += bsize;
		towrite -= bsize;
		writelen -= bsize;
		recvfp = writelen > 0;
		return OTA_STATUS_OK;
	}
	if(towrite > 0)
	{
		*recv_size = writelen = recv(request->fd, buf, (request->receivedLenmax > buf_size ? buf_size : request->receivedLenmax), 0);
		//*recv_size = writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "Writelen %i at %i", writelen, total);
		if(writelen < 0)
		{
			ADDLOG_INFO(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			*eof_flag = 1;
			*recv_size = 0;
			return OTA_STATUS_OK;
			//return OTA_STATUS_ERROR;
		}
	}
	total += writelen;
	towrite -= writelen;

	if((towrite > 0) && (writelen >= 0))
	{
		*eof_flag = 0;
		rtos_delay_milliseconds(10);
		return OTA_STATUS_OK;
	}
	*eof_flag = 1;
	return OTA_STATUS_OK;
}

	int ret = 0;
	uint32_t* verify_value;
	ota_verify_t verify_type;
	ota_verify_data_t verify_data;
	
	if(request->contentLength > 0)
	{
		towrite = request->contentLength;
	}
	else
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "Content-length is 0");
		goto update_ota_exit;
	}

	ota_init();

	if(ota_update_image(NULL, ota_update_rest_init, ota_update_rest_get) != OTA_STATUS_OK)
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "ota_update_image failed");
		goto update_ota_exit;
	}

	if(ota_get_verify_data(&verify_data) != OTA_STATUS_OK)
	{
		ADDLOG_INFO(LOG_FEATURE_OTA, "ota_get_verify_data not ok, OTA_VERIFY_NONE");
		verify_type = OTA_VERIFY_NONE;
		verify_value = NULL;
	}
	else
	{
		verify_type = verify_data.ov_type;
		ADDLOG_INFO(LOG_FEATURE_OTA, "ota_get_verify_data ok");
		verify_value = (uint32_t*)(verify_data.ov_data);
	}

	if(ota_verify_image(verify_type, verify_value) != OTA_STATUS_OK)
	{
		ret = -1;
		ADDLOG_ERROR(LOG_FEATURE_OTA, "OTA verify image failed");
		goto update_ota_exit;
	}

update_ota_exit:
	if(ret != -1)
	{
		ADDLOG_INFO(LOG_FEATURE_OTA, "OTA is successful");
	}
	else
	{
		ADDLOG_ERROR(LOG_FEATURE_OTA, "OTA failed.");
		return http_rest_error(request, ret, "error");
	}
#else

	init_ota(startaddr);

	if(request->contentLength >= 0)
	{
		towrite = request->contentLength;
	}

	if(writelen < 0 || (startaddr + writelen > maxaddr))
	{
		ADDLOG_DEBUG(LOG_FEATURE_OTA, "ABORTED: %d bytes to write", writelen);
		return http_rest_error(request, -20, "writelen < 0 or end > 0x200000");
	}

	do
	{
		//ADDLOG_DEBUG(LOG_FEATURE_OTA, "%d bytes to write", writelen);
		add_otadata((unsigned char*)writebuf, writelen);
		total += writelen;
		startaddr += writelen;
		towrite -= writelen;
		if(towrite > 0)
		{
			writebuf = request->received;
			writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
			if(writelen < 0)
			{
				ADDLOG_DEBUG(LOG_FEATURE_OTA, "recv returned %d - end of data - remaining %d", writelen, towrite);
			}
		}
	} while((towrite > 0) && (writelen >= 0));
	close_ota();
#endif
	ADDLOG_DEBUG(LOG_FEATURE_OTA, "%d total bytes written", total);
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"size\":%d}", total);
	poststr(request, NULL);
	CFG_IncrementOTACount();
	return 0;
}

static int http_rest_post_reboot(http_request_t* request) {
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"reboot\":%d}", 3);
	ADDLOG_DEBUG(LOG_FEATURE_API, "Rebooting in 3 seconds...");
	RESET_ScheduleModuleReset(3);
	poststr(request, NULL);
	return 0;
}

static int http_rest_get_flash_advanced(http_request_t* request) {
	char* params = request->url + 10;
	int startaddr = 0;
	int len = 0;
	int sres;
	sres = sscanf(params, "%x-%x", &startaddr, &len);
	if (sres == 2) {
		return http_rest_get_flash(request, startaddr, len);
	}
	return http_rest_error(request, -1, "invalid url");
}

static int http_rest_post_flash_advanced(http_request_t* request) {
	char* params = request->url + 10;
	int startaddr = 0;
	int sres;
	sres = sscanf(params, "%x", &startaddr);
	if (sres == 1 && startaddr >= START_ADR_OF_BK_PARTITION_OTA) {
		// allow up to end of flash
		return http_rest_post_flash(request, startaddr, 0x200000);
	}
	return http_rest_error(request, -1, "invalid url");
}

static int http_rest_get_flash(http_request_t* request, int startaddr, int len) {
	char* buffer;
	int res;

	if (startaddr < 0 || (startaddr + len > DEFAULT_FLASH_LEN)) {
		return http_rest_error(request, -1, "requested flash read out of range");
	}

	int bufferSize = 1024;
	buffer = os_malloc(bufferSize);
	memset(buffer, 0, bufferSize);

	http_setup(request, httpMimeTypeBinary);
	while (len) {
		int readlen = len;
		if (readlen > 1024) {
			readlen = 1024;
		}
#if PLATFORM_BEKEN
		res = flash_read((char*)buffer, readlen, startaddr);
#elif PLATFORM_XRADIO
		//uint32_t flash_read(uint32_t flash, uint32_t addr,void *buf, uint32_t size)
		res = flash_read(0, startaddr, buffer, readlen);
#elif PLATFORM_XR872
		res = 0;
#elif PLATFORM_BL602
		res = bl_flash_read(startaddr, (uint8_t *)buffer, readlen);
#elif PLATFORM_W600 || PLATFORM_W800
		res = tls_fls_read(startaddr, (uint8_t*)buffer, readlen);
#elif PLATFORM_LN882H
		res = hal_flash_read(startaddr, readlen, (uint8_t *)buffer);
#elif PLATFORM_ESPIDF
		res = esp_flash_read(NULL, (void*)buffer, startaddr, readlen);
#elif PLATFORM_TR6260
		res = hal_spiflash_read(startaddr, (uint8_t*)buffer, readlen);
#elif PLATFORM_ECR6600
		res = drv_spiflash_read(startaddr, (uint8_t*)buffer, readlen);
#elif PLATFORM_REALTEK
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_stream_read(&flash, startaddr, readlen, (uint8_t*)buffer);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
#else
		res = 0;
#endif
		startaddr += readlen;
		len -= readlen;
		postany(request, buffer, readlen);
	}
	poststr(request, NULL);
	os_free(buffer);
	return 0;
}


static int http_rest_get_dumpconfig(http_request_t* request) {



	http_setup(request, httpMimeTypeText);
	poststr(request, NULL);
	return 0;
}



#ifdef TESTCONFIG_ENABLE
// added for OpenBK7231T
typedef struct item_new_test_config
{
	INFO_ITEM_ST head;
	char somename[64];
}ITEM_NEW_TEST_CONFIG, * ITEM_NEW_TEST_CONFIG_PTR;

ITEM_NEW_TEST_CONFIG testconfig;
#endif

static int http_rest_get_testconfig(http_request_t* request) {
	return http_rest_error(request, 400, "unsupported");
	return 0;
}

static int http_rest_get_flash_vars_test(http_request_t* request) {
	//#if PLATFORM_XR809
	//    return http_rest_error(request, 400, "flash vars unsupported");
	//#elif PLATFORM_BL602
	//    return http_rest_error(request, 400, "flash vars unsupported");
	//#else
	//#ifndef DISABLE_FLASH_VARS_VARS
	//    char *params = request->url + 17;
	//    int increment = 0;
	//    int len = 0;
	//    int sres;
	//    int i;
	//    char tmp[128];
	//    FLASH_VARS_STRUCTURE data, *p;
	//
	//    p = &flash_vars;
	//
	//    sres = sscanf(params, "%x-%x", &increment, &len);
	//
	//    ADDLOG_DEBUG(LOG_FEATURE_API, "http_rest_get_flash_vars_test %d %d returned %d", increment, len, sres);
	//
	//    if (increment == 10){
	//        flash_vars_read(&data);
	//        p = &data;
	//    } else {
	//        for (i = 0; i < increment; i++){
	//            HAL_FlashVars_IncreaseBootCount();
	//        }
	//        for (i = 0; i < len; i++){
	//            HAL_FlashVars_SaveBootComplete();
	//        }
	//    }
	//
	//    sprintf(tmp, "offset %d, boot count %d, boot success %d, bootfailures %d",
	//        flash_vars_offset,
	//        p->boot_count,
	//        p->boot_success_count,
	//        p->boot_count - p->boot_success_count );
	//
	//    return http_rest_error(request, 200, tmp);
	//#else
	return http_rest_error(request, 400, "flash test unsupported");
}


static int http_rest_get_channels(http_request_t* request) {
	int i;
	int addcomma = 0;
	/*typedef struct pinsState_s {
		byte roles[32];
		byte channels[32];
	} pinsState_t;

	extern pinsState_t g_pins;
	*/
	http_setup(request, httpMimeTypeJson);
	poststr(request, "{");

	// TODO: maybe we should cull futher channels that are not used?
	// I support many channels because I plan to use 16x relays module with I2C MCP23017 driver
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		// "i" is a pin index
		// Get channel index and role
		int ch = PIN_GetPinChannelForPinIndex(i);
		int role = PIN_GetPinRoleForPinIndex(i);
		if (role) {
			if (addcomma) {
				hprintf255(request, ",");
			}
			hprintf255(request, "\"%d\":%d", ch, CHANNEL_Get(ch));
			addcomma = 1;
		}
	}
	poststr(request, "}");
	poststr(request, NULL);
	return 0;
}

// currently crashes the MCU - maybe stack overflow?
static int http_rest_post_channels(http_request_t* request) {
	int i;
	int r;
	char tmp[64];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * 128);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		sprintf(tmp, "Failed to parse JSON: %d\n", r);
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_ARRAY) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Array expected", r);
		sprintf(tmp, "Object expected\n");
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		int chanval;
		jsmntok_t* g = &t[i];
		chanval = atoi(json_str + g->start);
		CHANNEL_Set(i - 1, chanval, 0);
		ADDLOG_DEBUG(LOG_FEATURE_API, "Set of chan %d to %d", i,
			chanval);
	}

	os_free(p);
	os_free(t);
	return http_rest_error(request, 200, "OK");
	return 0;
}



static int http_rest_post_cmd(http_request_t* request) {
	commandResult_t res;
	int code;
	const char *reply;
	const char *type;
	const char* cmd = request->bodystart;
	res = CMD_ExecuteCommand(cmd, COMMAND_FLAG_SOURCE_CONSOLE);
	reply = CMD_GetResultString(res);
	if (1) {
		addLogAdv(LOG_INFO, LOG_FEATURE_CMD, "[WebApp Cmd '%s' Result] %s", cmd, reply);
	}
	if (res != CMD_RES_OK) {
		type = "error";
		if (res == CMD_RES_UNKNOWN_COMMAND) {
			code = 501;
		}
		else {
			code = 400;
		}
	}
	else {
		type = "success";
		code = 200;
	}

	request->responseCode = code;
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"%s\":%d, \"msg\":\"%s\", \"res\":", type, code, reply);
#if ENABLE_TASMOTA_JSON
	JSON_ProcessCommandReply(cmd, skipToNextWord(cmd), request, (jsonCb_t)hprintf255, COMMAND_FLAG_SOURCE_HTTP);
#endif
	hprintf255(request, "}", code, reply);
	poststr(request, NULL);
	return 0;
}

