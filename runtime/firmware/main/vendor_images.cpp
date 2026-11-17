/* The vendor's images kept current on the wall; see vendor_images.h and
 * UPDATES.md "Vendor images". */
#include "vendor_images.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "flight_data.h"        /* ArduinoJson */
#include "https_get.h"
#include "big_buffer.h"
extern "C" {
#include "owner_sha256.h"
}
#include "owner_tls_root.h"     /* GTS Root R4: cdn.theflightwall.com */
#include "fw_assets.h"
#include "memory_probe.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs.h"

using namespace flightwall;

namespace {

constexpr const char *Host = "cdn.theflightwall.com";
constexpr const char *ManifestPath = "/filesystem/mini-v1/manifest.json";
constexpr const char *UrlPrefix = "https://cdn.theflightwall.com/filesystem/mini-v1/";
constexpr uint32_t Piece = 128 * 1024;
constexpr uint32_t Sector = 4096;
constexpr uint32_t FirstCheckMs = 2 * 60000;
constexpr uint32_t WeekMs = 7u * 24 * 3600 * 1000;
constexpr uint32_t RetryMs = 30 * 60000;
constexpr uint32_t PieceRetryMs = 15000;
constexpr int PieceTries = 5;
constexpr const char *Ns = "fw_owned";          /* never a vendor namespace */

/* The vendor's OTA signing key "ota-prod-2026-03" (P-256), as the vendor
 * firmware 4.2.4 embeds it (FUN_4203171c), uncompressed: 04 || X || Y. */
const uint8_t VendorKey[65] = {
    0x04,
    0xbe, 0xd1, 0xe0, 0xde, 0x36, 0xfb, 0xae, 0xe4, 0x1c, 0x57, 0x0b, 0xe4, 0x24, 0xc1, 0xd8, 0x1e,
    0xda, 0x4e, 0x6e, 0xe7, 0x69, 0x29, 0x34, 0xe3, 0x9e, 0xf2, 0xf8, 0x83, 0x7a, 0x3d, 0x23, 0x29,
    0xde, 0x8a, 0x8a, 0x98, 0x7e, 0x69, 0xba, 0x55, 0xd6, 0x47, 0xad, 0xe0, 0x43, 0x4b, 0xf2, 0xaf,
    0x56, 0x94, 0x89, 0xe7, 0x7f, 0xe3, 0x14, 0x51, 0x1d, 0x71, 0xa9, 0x38, 0x13, 0x86, 0x06, 0xde,
};

struct Manifest {
    char version[16];
    char path[128];                 /* the image's path on Host */
    char sha256[65];
    uint32_t size, epoch;
    uint8_t artifact_sig[80];
    size_t artifact_sig_len;
};

enum Phase { Idle, Downloading };
Phase s_phase = Idle;
Manifest *s_mp;                     /* while checking or downloading */
#define s_m (*s_mp)
uint32_t s_total;
uint32_t s_done, s_next_ms, s_started_ms;
int s_piece_fails;
bool s_dirty_checked;
const char *s_state = "idle";
const char *s_error = "";
char s_installed[16], s_available[16];
uint8_t *s_chunk;                   /* 4 KiB while downloading */

uint32_t now_ms(){ return (uint32_t)(esp_timer_get_time() / 1000); }

void free_manifest(){ std::free(s_mp); s_mp = nullptr; }

const esp_partition_t *partition(){
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
}

void report(const char *what, const char *detail){
    char extra[160];
    std::snprintf(extra, sizeof extra, "\"what\":\"%s\",\"detail\":\"%s\",\"installed\":\"%s\",\"available\":\"%s\",\"done\":%u",
                  what, detail ? detail : "", s_installed, s_available, (unsigned)s_done);
    fw_mem_report("vendor_images", extra);
}

/* ------------------------------------------------------------ NVS (owned namespace) */

bool nvs_get(const char *key, char *out, size_t cap){
    nvs_handle_t h;
    size_t n = cap;
    out[0] = 0;
    if (nvs_open(Ns, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, key, out, &n) == ESP_OK;
    nvs_close(h);
    if (!ok) out[0] = 0;
    return ok;
}
uint32_t nvs_u32(const char *key){
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(Ns, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u32(h, key, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}
void nvs_put(const char *ver, uint32_t epoch, uint8_t dirty){
    nvs_handle_t h;
    if (nvs_open(Ns, NVS_READWRITE, &h) != ESP_OK) return;
    if (ver) nvs_set_str(h, "fs_ver", ver);
    if (ver) nvs_set_u32(h, "fs_epoch", epoch);
    nvs_set_u8(h, "fs_dirty", dirty);
    nvs_commit(h);
    nvs_close(h);
}
bool dirty(){
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(Ns, NVS_READONLY, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, "fs_dirty", &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v != 0;
}

/* ------------------------------------------------------------ versions */

/* FUN_4202ec48: major.minor.patch, each decimal (anything else counts 0). */
void parts(const char *v, long out[3]){
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < 3 && v && *v; ++i) {
        out[i] = std::strtol(v, nullptr, 10);
        v = std::strchr(v, '.');
        if (v) ++v;
    }
}
int compare(const char *a, const char *b){
    long x[3], y[3];
    parts(a, x);
    parts(b, y);
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
uint32_t epoch_of(const char *v){                    /* FUN_4202edfc */
    long p[3];
    parts(v, p);
    for (long &q : p) q = q < 0 ? 0 : q > 999 ? 999 : q;
    return (uint32_t)(p[0] * 1000000 + p[1] * 1000 + p[2]);
}

/* What is on the wall: /version.txt (the vendor firmware deletes it once read),
 * else what this firmware installed. healthy = it mounts and has the default logo. */
void installed(bool *healthy, bool *has_42){
    char buf[24] = "";
    *healthy = fw_assets_mount_partition() && fw_assets_exists("/images/airlines/square/default.png");
    *has_42 = *healthy && fw_assets_exists("/images/weather/32/clear_day.png");
    s_installed[0] = 0;
    if (*healthy) {
        int n = fw_assets_read_file("/version.txt", reinterpret_cast<uint8_t *>(buf), sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            for (char *p = buf; *p; ++p) if (*p == '\r' || *p == '\n' || *p == ' ') { *p = 0; break; }
            std::snprintf(s_installed, sizeof s_installed, "%.15s", buf);
        }
    }
    if (!s_installed[0]) nvs_get("fs_ver", s_installed, sizeof s_installed);
}

/* ------------------------------------------------------------ manifest */

int b64(char c){
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
         : c == '+' ? 62 : c == '/' ? 63 : -1;
}
size_t base64(const char *in, uint8_t *out, size_t cap){
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in && *in != '='; ++in) {
        int v = b64(*in);
        if (v < 0) return 0;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) return 0;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

bool verify(const uint8_t digest[32], const uint8_t *sig, size_t sig_len){
    uint8_t *arena = fw_big_take(5000);
    if (!arena) return false;
    bool ok = fw_tls_ecdsa_p256_verify(arena, fw_big_bytes, VendorKey, sizeof VendorKey, digest, sig, sig_len);
    fw_big_give();
    return ok;
}

/* Fetches and checks the manifest (vendor_images.h); false with s_error set. */
bool manifest(){
    char *body = static_cast<char *>(std::malloc(4096));
    size_t len = 0;
    unsigned status;
    uint32_t retry;
    if (!body) { s_error = "memory"; return false; }
    int stage = fw_https_get(Host, ManifestPath, nullptr, reinterpret_cast<const uint8_t *>(owner_tls_gts_r4),
                             sizeof owner_tls_gts_r4, &status, &retry, [&](HttpBody &b){
        for (int c; (c = b.read()) >= 0;) if (len < 4095) body[len++] = (char)c;
        return true;
    });
    body[len] = 0;
    if (stage || status != 200) { std::free(body); s_error = "manifest fetch failed"; return false; }
    DynamicJsonDocument doc(3072);
    bool parsed = !deserializeJson(doc, body, len);
    std::free(body);
    if (!parsed) { s_error = "manifest unreadable"; return false; }

    const esp_partition_t *p = partition();
    const char *variant = !p ? "default" : p->size >= 0x25F000 ? "2-5mb-fs" : p->size <= 0x110000 ? "1mb-fs" : "default";
    auto sfx = [&](const char *key, char *out, size_t cap){
        char k[48];
        std::snprintf(k, sizeof k, "%s_%s", key, variant);
        const char *v = doc[k] | "";
        if (!*v) v = doc[key] | "";
        std::snprintf(out, cap, "%s", v);
    };
    if (std::strcmp(variant, "default")) {           /* the variant only when all of it is there */
        char k[48];
        for (const char *key : {"url", "sha256", "size", "artifact_signature", "manifest_signature"}) {
            std::snprintf(k, sizeof k, "%s_%s", key, variant);
            if (doc[k].isNull()) { variant = "default"; break; }
        }
    }
    char url[200], sha[80], asig[140], asig_url[200], msig[140], size_s[16], epoch_s[16];
    sfx("url", url, sizeof url);
    sfx("sha256", sha, sizeof sha);
    sfx("artifact_signature", asig, sizeof asig);
    sfx("artifact_signature_url", asig_url, sizeof asig_url);
    sfx("manifest_signature", msig, sizeof msig);
    {
        char k[32];
        std::snprintf(k, sizeof k, "size_%s", variant);
        uint32_t size = doc[k] | 0u;
        if (!size) size = doc["size"] | 0u;
        std::snprintf(size_s, sizeof size_s, "%u", (unsigned)size);
        std::snprintf(epoch_s, sizeof epoch_s, "%u", (unsigned)(doc["rollback_epoch"] | 0u));
        s_m.size = size;
        s_m.epoch = doc["rollback_epoch"] | 0u;
    }
    const char *type = doc["type"] | "", *model = doc["device_model"] | "", *track = doc["ota_track"] | "";
    const char *profile = doc["storage_security_profile"] | "", *version = doc["version"] | "";
    const char *alg = doc["signature_alg"] | "", *key_id = doc["key_id"] | "";
    std::snprintf(s_available, sizeof s_available, "%s", version);
    if (!*url || !*sha || !*asig || !*msig || !*version || !s_m.size) { s_error = "manifest incomplete"; return false; }
    if (std::strcmp(alg, "ecdsa-p256-sha256")) { s_error = "unsupported signature algorithm"; return false; }
    if (std::strcmp(key_id, "ota-prod-2026-03")) { s_error = "unknown signing key"; return false; }

    /* FUN_4202f53c: the signed text, key=value lines in this order */
    OwnerSha256 h;
    uint8_t digest[32], sig[80];
    owner_sha256_init(&h);
    auto line = [&](const char *k, const char *v){
        owner_sha256_update(&h, k, std::strlen(k));
        owner_sha256_update(&h, "=", 1);
        owner_sha256_update(&h, v, std::strlen(v));
        owner_sha256_update(&h, "\n", 1);
    };
    line("type", type); line("device_model", model); line("ota_track", track);
    line("storage_security_profile", profile); line("version", version); line("fs_size_variant", variant);
    line("url", url); line("size", size_s); line("sha256", sha); line("artifact_signature", asig);
    line("artifact_signature_url", asig_url); line("signature_alg", alg); line("key_id", key_id);
    line("rollback_epoch", epoch_s);
    owner_sha256_final(&h, digest);
    size_t sig_len = base64(msig, sig, sizeof sig);
    if (!sig_len || !verify(digest, sig, sig_len)) { s_error = "manifest signature does not verify"; return false; }

    /* FUN_42031df4: this wall's model, track and storage */
    if (std::strcmp(type, "filesystem") || std::strcmp(model, "mini-v1") || std::strcmp(track, "legacy") ||
        std::strcmp(profile, "plain-nvs-v1")) { s_error = "not for this wall"; return false; }
    const size_t origin = std::strlen("https://cdn.theflightwall.com");
    if (std::strncmp(url, UrlPrefix, std::strlen(UrlPrefix)) || std::strlen(url) - origin >= sizeof s_m.path) {
        s_error = "unexpected image address";
        return false;
    }
    if (!p || s_m.size > p->size) { s_error = "image too large"; return false; }
    std::snprintf(s_m.path, sizeof s_m.path, "%s", url + origin);
    std::snprintf(s_m.version, sizeof s_m.version, "%s", version);
    if (std::strlen(sha) != 64) { s_error = "bad sha256"; return false; }
    for (size_t i = 0; i < 64; ++i) {
        char c = sha[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { s_error = "bad sha256"; return false; }
    }
    std::snprintf(s_m.sha256, sizeof s_m.sha256, "%.64s", sha);
    s_m.artifact_sig_len = base64(asig, s_m.artifact_sig, sizeof s_m.artifact_sig);
    if (!s_m.artifact_sig_len) { s_error = "bad artifact signature"; return false; }
    return true;
}

/* ------------------------------------------------------------ download */

void begin_download(){
    s_phase = Downloading;
    s_done = 0;
    s_piece_fails = 0;
    s_state = "downloading";
    s_error = "";
    s_started_ms = now_ms();
    s_total = s_m.size;
    fw_assets_hold(true);                       /* the screens draw silhouettes and owned icons meanwhile */
    nvs_put(nullptr, 0, 1);                     /* a restart before the end resumes the download */
    report("download_start", s_m.version);
}

void fail(const char *why){
    s_error = why;
    s_state = "failed";
    report("failed", why);
}

/* One range into the partition at s_done. */
bool piece(const esp_partition_t *p){
    uint32_t len = s_m.size - s_done < Piece ? s_m.size - s_done : Piece;
    uint32_t erase_to = (s_done + len + Sector - 1) / Sector * Sector;
    if (erase_to > p->size) erase_to = p->size;
    if (esp_partition_erase_range(p, s_done, erase_to - s_done) != ESP_OK) return false;
    char range[48];
    std::snprintf(range, sizeof range, "Range: bytes=%u-%u\r\n", (unsigned)s_done, (unsigned)(s_done + len - 1));
    unsigned status;
    uint32_t retry, got = 0;
    bool wrote = true;
    int stage = fw_https_get(Host, s_m.path, range, reinterpret_cast<const uint8_t *>(owner_tls_gts_r4),
                             sizeof owner_tls_gts_r4, &status, &retry, [&](HttpBody &b){
        if (b.status() != 206) { while (b.read() >= 0) {} return true; }
        size_t fill = 0;
        for (int c; (c = b.read()) >= 0;) {
            if (got + fill >= len) { wrote = false; break; }      /* more than asked for */
            s_chunk[fill++] = (uint8_t)c;
            if (fill == Sector) {
                wrote = wrote && esp_partition_write(p, s_done + got, s_chunk, fill) == ESP_OK;
                got += fill;
                fill = 0;
            }
        }
        if (fill) { wrote = wrote && esp_partition_write(p, s_done + got, s_chunk, fill) == ESP_OK; got += fill; }
        return true;
    });
    if (stage || status != 206 || !wrote || got != len) return false;
    s_done += len;
    return true;
}

/* The whole image read back from flash: the manifest's SHA-256 and the vendor's signature of it. */
bool check(const esp_partition_t *p){
    OwnerSha256 h;
    uint8_t digest[32];
    char hex[65];
    owner_sha256_init(&h);
    for (uint32_t at = 0; at < s_m.size; at += Sector) {
        uint32_t n = s_m.size - at < Sector ? s_m.size - at : Sector;
        if (esp_partition_read(p, at, s_chunk, n) != ESP_OK) { s_error = "read back failed"; return false; }
        owner_sha256_update(&h, s_chunk, n);
    }
    owner_sha256_final(&h, digest);
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (std::strcmp(hex, s_m.sha256)) { s_error = "sha256 mismatch"; return false; }
    if (!verify(digest, s_m.artifact_sig, s_m.artifact_sig_len)) { s_error = "image signature does not verify"; return false; }
    return true;
}

void finish(const esp_partition_t *p){
    bool ok = check(p);
    std::free(s_chunk);
    s_chunk = nullptr;
    s_phase = Idle;
    if (!ok) {                                  /* stays held and dirty: downloaded again later */
        fail(s_error);
        free_manifest();
        s_next_ms = now_ms() + RetryMs;
        return;
    }
    fw_assets_hold(false);
    bool healthy, has_42;
    installed(&healthy, &has_42);
    if (!healthy) { fw_assets_hold(true); fail("installed images do not mount"); free_manifest(); s_next_ms = now_ms() + RetryMs; return; }
    nvs_put(s_m.version, s_m.epoch, 0);
    std::snprintf(s_installed, sizeof s_installed, "%s", s_m.version);
    free_manifest();
    s_state = "installed";
    char took[24];
    std::snprintf(took, sizeof took, "%us", (unsigned)((now_ms() - s_started_ms) / 1000));
    report("installed", took);
    s_next_ms = now_ms() + WeekMs;
}

} // namespace

extern "C" void fw_vendor_images_step(bool enabled){
    uint32_t now = now_ms();
    const esp_partition_t *p = partition();
    if (!p) return;
    if (!s_dirty_checked) {                     /* cut short before a restart: keep the screens off it */
        s_dirty_checked = true;
        if (dirty()) { fw_assets_hold(true); s_state = "failed"; s_error = "interrupted"; s_next_ms = 0; }
        else s_next_ms = now + FirstCheckMs;
    }
    if (!enabled && s_phase == Idle) {
        if (!fw_assets_held()) s_state = "off";
        return;                                 /* an interrupted install stays held until it is finished */
    }
    if (s_next_ms && (int32_t)(now - s_next_ms) < 0) return;
    if (std::time(nullptr) < 1700000000) return;          /* TLS needs the clock */

    if (s_phase == Downloading) {
        if (piece(p)) {
            s_piece_fails = 0;
            s_next_ms = 0;
            if (s_done >= s_m.size) finish(p);
        } else if (++s_piece_fails >= PieceTries) {
            std::free(s_chunk);
            s_chunk = nullptr;
            s_phase = Idle;
            fail("download failed");
            free_manifest();
            s_next_ms = now_ms() + RetryMs;
        } else {
            s_next_ms = now_ms() + PieceRetryMs;
        }
        return;
    }

    s_state = "checking";
    bool held = fw_assets_held(), healthy = false, has_42 = false;
    if (!held) installed(&healthy, &has_42);
    if (!s_mp && !(s_mp = static_cast<Manifest *>(std::calloc(1, sizeof(Manifest))))) {
        s_state = "failed";
        s_error = "memory";
        s_next_ms = now_ms() + RetryMs;
        return;
    }
    if (!manifest()) {
        s_state = "failed";
        report("check_failed", s_error);
        free_manifest();
        s_next_ms = now_ms() + RetryMs;
        return;
    }
    uint32_t epoch = nvs_u32("fs_epoch");
    if (!epoch) epoch = epoch_of(s_installed);
    bool newer = compare(s_m.version, s_installed) > 0;
    bool need = held || !healthy || (s_installed[0] ? newer : !has_42);
    if (!need) {
        s_state = "up_to_date";
        s_error = "";
        free_manifest();
        s_next_ms = now_ms() + WeekMs;
        return;
    }
    if (s_installed[0] && healthy && s_m.epoch < epoch) {     /* never back to an older release */
        fail("rollback epoch regressed");
        free_manifest();
        s_next_ms = now_ms() + WeekMs;
        return;
    }
    s_chunk = static_cast<uint8_t *>(std::malloc(Sector));
    if (!s_chunk) { fail("memory"); free_manifest(); s_next_ms = now_ms() + RetryMs; return; }
    begin_download();
    s_next_ms = 0;
}

extern "C" void fw_vendor_images_status(FwVendorImagesStatus *out){
    std::memset(out, 0, sizeof *out);
    std::snprintf(out->installed, sizeof out->installed, "%s", s_installed);
    std::snprintf(out->available, sizeof out->available, "%s", s_available);
    out->state = s_state;
    out->error = s_error;
    out->done = s_done;
    out->total = s_phase == Downloading || s_done ? s_total : 0;
}
