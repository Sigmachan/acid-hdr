/*
 * kms-hdr (cosmic-hdr.c) — Universal KMS HDR injector for Linux
 * HDR10 + BT.2020/DCI-P3 color pipeline via DRM atomic, compositor-agnostic.
 *
 * Two pipeline modes (auto-detected by GPU driver):
 *   AMD/Intel: DEGAMMA (sRGB→linear) → CTM (BT.709→gamut) → GAMMA (linear→PQ)
 *              + HDR_OUTPUT_METADATA + Colorspace=BT2020_RGB
 *   NVIDIA:    legacy gamma ramp (sRGB→PQ, no gamut expansion)
 *              + HDR_OUTPUT_METADATA + Colorspace=BT2020_RGB
 *
 * Steals DRM master via VT switch (tty1→tty2→tty1, screen blanks ~0.5s).
 * Properties persist after master release (any compositor).
 *
 * Usage (must be root / pkexec):
 *   kms-hdr                               apply (reads /etc/kms-hdr.conf)
 *   kms-hdr reset                         restore SDR
 *   kms-hdr --save --sdr-nits 203 ...    save to conf + apply
 *   kms-hdr --card /dev/dri/card1        override DRM device (auto-detected by default)
 *   kms-hdr --connector HDMI-A-2         override connector name
 *   kms-hdr --sdr-nits 203              set SDR white brightness (nits)
 *   kms-hdr --peak-nits 800             set display peak luminance (nits)
 *   kms-hdr --gamut 100                 gamut expansion blend 0-100%
 *   kms-hdr --gamut-mode [bt2020|dci-p3|srgb]
 *   kms-hdr --bpc [8|10|12]             request output bit depth
 *   kms-hdr --saturation [50-200]       colour saturation % (BT.709 matrix)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/vt.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>

/* ── config ──────────────────────────────────────────────────────────────── */
#define CONF_PATH  "/etc/kms-hdr.conf"
#define LUT_SIZE   4096
#define MAX_CARDS  8
#define MASTER_RETRY_MS  500   /* ms between drmSetMaster retries */
#define MASTER_RETRIES   12   /* total retries (~6 seconds) */

/*
 * SDR_NITS: brightness of SDR white in HDR mode (nits).
 *   KDE default: 200. Standard reference: 203 (CTA-861-H).
 *   Lower = dimmer desktop. Higher = brighter desktop.
 *
 * PEAK_NITS: display mastering peak — tells TV the max content brightness.
 *   Set to your display's actual peak (A85H OLED ≈ 800-1000 nits).
 *   This is now meaningful because we properly PQ-encode the signal.
 *
 * GAMUT: 0 = identity CTM, 100 = full BT.709→BT.2020 expansion.
 */
#define DEFAULT_SDR_NITS   203
#define DEFAULT_PEAK_NITS  800
#define DEFAULT_GAMUT      100
#define DEFAULT_GAMUT_MODE  0   /* 0=BT.2020, 1=DCI-P3 */
#define DEFAULT_MAX_BPC     10
#define DEFAULT_SATURATION  100 /* 100 = neutral; 150 = vivid; 50 = desaturated */

static void load_conf(int *sdr_nits, int *peak_nits, int *gamut_pct,
                      int *gamut_mode, int *max_bpc, int *saturation) {
    *sdr_nits   = DEFAULT_SDR_NITS;
    *peak_nits  = DEFAULT_PEAK_NITS;
    *gamut_pct  = DEFAULT_GAMUT;
    *gamut_mode = DEFAULT_GAMUT_MODE;
    *max_bpc    = DEFAULT_MAX_BPC;
    *saturation = DEFAULT_SATURATION;
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v; char s[64];
        if (sscanf(line, "SDR_NITS=%d",     &v) == 1) *sdr_nits   = v;
        if (sscanf(line, "PEAK_NITS=%d",    &v) == 1) *peak_nits  = v;
        if (sscanf(line, "GAMUT=%d",        &v) == 1) *gamut_pct  = v;
        if (sscanf(line, "MAX_BPC=%d",      &v) == 1) *max_bpc    = v;
        if (sscanf(line, "SATURATION=%d",   &v) == 1) *saturation = v;
        if (sscanf(line, "GAMUT_MODE=%63s", s)  == 1)
            *gamut_mode = (strncmp(s, "dci-p3", 6) == 0) ? 1 : 0;
    }
    fclose(f);
}

static void save_conf(int sdr_nits, int peak_nits, int gamut_pct,
                      int gamut_mode, int max_bpc, int saturation) {
    FILE *f = fopen(CONF_PATH, "w");
    if (!f) { perror("save_conf: fopen " CONF_PATH); return; }
    fprintf(f, "SDR_NITS=%d\nPEAK_NITS=%d\nGAMUT=%d\nMAX_BPC=%d\nGAMUT_MODE=%s\nSATURATION=%d\n",
            sdr_nits, peak_nits, gamut_pct, max_bpc,
            gamut_mode == 1 ? "dci-p3" : (gamut_mode == 2 ? "srgb" : "bt2020"),
            saturation);
    fclose(f);
    printf("saved: %s\n", CONF_PATH);
}

/* Auto-detect the DRM card device and connector name.
 * Enumerates /sys/class/drm/cardN-* entries; picks the first connector
 * with a non-empty EDID (= connected display).
 * card_out: "/dev/dri/cardN" · conn_out: "HDMI-A-2" (connector name without cardN-) */
static int find_drm_device(char *card_out, size_t card_sz,
                            char *conn_out, size_t conn_sz) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return -1;

    struct dirent *e;
    while ((e = readdir(d))) {
        /* Match "cardN-TYPE-M" — skip bare "cardN" and non-card entries */
        if (strncmp(e->d_name, "card", 4) != 0) continue;
        char *dash = strchr(e->d_name + 4, '-');
        if (!dash) continue;

        /* Read EDID to confirm display is connected */
        char edid_path[512];
        snprintf(edid_path, sizeof(edid_path), "/sys/class/drm/%s/edid", e->d_name);
        int efd = open(edid_path, O_RDONLY);
        if (efd < 0) continue;
        char buf[1]; int n = read(efd, buf, 1); close(efd);
        if (n <= 0) continue;

        /* Found a connected display */
        int card_num = atoi(e->d_name + 4);
        snprintf(card_out, card_sz, "/dev/dri/card%d", card_num);
        snprintf(conn_out, conn_sz, "%s", dash + 1);   /* e.g. "HDMI-A-2" */
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

/* ── EDID auto-configuration ─────────────────────────────────────────────── */
/*
 * Reads the EDID from sysfs and extracts:
 *   - HDR peak luminance (CTA-861 HDR Static Metadata ext_tag=6, max_luminance byte)
 *   - BT.2020 support   (CTA-861 Colorimetry ext_tag=5, bit 7)
 *   - DCI-P3 support    (CTA-861 Colorimetry ext_tag=5, bit 1)
 * Used to fill in optimal defaults when no explicit CLI flags are given.
 */
static int parse_edid_caps(const char *path,
                            int *peak_nits, int *bt2020, int *dcip3) {
    *peak_nits = 0; *bt2020 = 0; *dcip3 = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t edid[2048]; memset(edid, 0, sizeof(edid));
    size_t n = fread(edid, 1, sizeof(edid), f);
    fclose(f);
    if (n < 128) return -1;
    for (size_t bs = 128; bs + 128 <= n; bs += 128) {
        if (edid[bs] != 0x02) continue;
        uint8_t dtd = edid[bs + 2];
        for (size_t i = 4; i < (size_t)dtd && bs + i < n; ) {
            uint8_t tag = (edid[bs + i] >> 5) & 0x7;
            uint8_t len = edid[bs + i] & 0x1f;
            if (bs + i + 1 + (size_t)len > n) break;
            const uint8_t *d = &edid[bs + i + 1];
            if (tag == 7 && len > 1) {
                const uint8_t *p = d + 1;
                size_t plen = len - 1;
                if (d[0] == 6 && plen > 2 && p[2] != 0)    /* HDR Static Metadata */
                    *peak_nits = (int)(50.0 * pow(2.0, p[2] / 32.0));
                else if (d[0] == 5 && plen > 0) {           /* Colorimetry */
                    *bt2020 = (p[0] & 0x80) ? 1 : 0;
                    *dcip3  = (p[0] & 0x02) ? 1 : 0;
                }
            }
            i += 1 + (size_t)len;
        }
    }
    return 0;
}

/* ── LUT helpers ─────────────────────────────────────────────────────────── */
typedef struct { uint16_t r, g, b, pad; } drm_lut_entry;

static double srgb_to_linear(double x) {
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

/* SMPTE ST2084 (PQ) encode: linear light [0,1] → PQ code [0,1]
 * where linear 1.0 corresponds to 10000 cd/m². */
static double linear_to_pq(double L) {
    if (L <= 0.0) return 0.0;
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    double Lm = pow(L, m1);
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);
}

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* DEGAMMA: sRGB gamma → linear [0,1] */
static drm_lut_entry *build_degamma_srgb(int n) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    for (int i = 0; i < n; i++) {
        double x = (double)i / (n - 1);
        uint16_t v = (uint16_t)(clamp01(srgb_to_linear(x)) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* GAMMA: linear [0,1] → PQ code, where linear 1.0 = sdr_nits cd/m².
 * Matches KDE's approach: SDR white maps to sdr_nits on the PQ curve. */
static drm_lut_entry *build_gamma_pq(int n, double sdr_nits) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    double scale = sdr_nits / 10000.0;  /* normalize to PQ range */
    for (int i = 0; i < n; i++) {
        double x = (double)i / (n - 1); /* linear light [0,1], 1=SDR white */
        double pq = linear_to_pq(x * scale);
        uint16_t v = (uint16_t)(clamp01(pq) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* Identity LUT for reset */
static drm_lut_entry *build_linear_lut(int n) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    for (int i = 0; i < n; i++) {
        uint16_t v = (uint16_t)((double)i / (n - 1) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* ── CTM ─────────────────────────────────────────────────────────────────── */
/*
 * BT.709 → BT.2020 gamut expansion.
 * Derived from (BT.2020←XYZ) × (XYZ←BT.709), D65 white point.
 * Applied in linear light domain (between DEGAMMA and GAMMA).
 */
static const double CTM_709_TO_2020[3][3] = {
    { 0.627504,  0.329275,  0.043303 },
    { 0.069108,  0.919519,  0.011360 },
    { 0.016394,  0.088011,  0.895380 },
};

/*
 * BT.709 → DCI-P3 D65 gamut expansion.
 * Derived from (P3-D65←XYZ) × (XYZ←BT.709), D65 white point.
 * P3 is a middle ground: wider than sRGB, not as wide as BT.2020.
 */
static const double CTM_709_TO_DCIP3[3][3] = {
    { 0.822461,  0.177538,  0.000000 },
    { 0.033195,  0.966805,  0.000000 },
    { 0.017083,  0.072397,  0.910520 },
};

static void build_ctm(const double m[3][3], uint64_t out[9]) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        double v = m[r][c];
        uint64_t mag = (uint64_t)(fabs(v) * (1ULL << 32)) & ~(1ULL << 63);
        if (v < 0) mag |= (1ULL << 63);
        out[r * 3 + c] = mag;
    }
}

static void build_ctm_identity(uint64_t out[9]) {
    double id[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    build_ctm(id, out);
}

/* BT.709 saturation matrix. s=1.0 → identity; s>1 → vivid; s<1 → desaturated. */
static void build_sat_mat(double s, double out[3][3]) {
    double Ry = 0.2126, Gy = 0.7152, By = 0.0722;
    out[0][0] = (1-s)*Ry + s; out[0][1] = (1-s)*Gy;      out[0][2] = (1-s)*By;
    out[1][0] = (1-s)*Ry;     out[1][1] = (1-s)*Gy + s;  out[1][2] = (1-s)*By;
    out[2][0] = (1-s)*Ry;     out[2][1] = (1-s)*Gy;      out[2][2] = (1-s)*By + s;
}

/* out = a × b  (3×3 matrix multiply) */
static void mat_mul_3x3(const double a[3][3], const double b[3][3], double out[3][3]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            out[r][c] = 0;
            for (int k = 0; k < 3; k++) out[r][c] += a[r][k] * b[k][c];
        }
}

/* ── HDR10 metadata ──────────────────────────────────────────────────────── */
/* Mirrors struct hdr_output_metadata from <drm/drm_mode.h>. sizeof == 32. */
typedef struct {
    uint32_t metadata_type;
    uint8_t  eotf;
    uint8_t  metadata_descriptor;
    uint16_t display_primaries[3][2];
    uint16_t white_point[2];
    uint16_t max_display_mastering_luminance;
    uint16_t min_display_mastering_luminance;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
} hdr_meta_t;

static hdr_meta_t build_hdr_meta(int peak_nits, int sdr_nits) {
    hdr_meta_t m = {0};
    m.metadata_type      = 0;   /* HDMI_STATIC_METADATA_TYPE1 */
    m.eotf               = 2;   /* PQ / ST2084 */
    m.metadata_descriptor = 0;
    /* BT.2020 primaries × 50000 (CTA-861 order: G, B, R) */
    m.display_primaries[0][0] = (uint16_t)(0.170 * 50000); /* G x */
    m.display_primaries[0][1] = (uint16_t)(0.797 * 50000); /* G y */
    m.display_primaries[1][0] = (uint16_t)(0.131 * 50000); /* B x */
    m.display_primaries[1][1] = (uint16_t)(0.046 * 50000); /* B y */
    m.display_primaries[2][0] = (uint16_t)(0.708 * 50000); /* R x */
    m.display_primaries[2][1] = (uint16_t)(0.292 * 50000); /* R y */
    m.white_point[0] = (uint16_t)(0.3127 * 50000);         /* D65 */
    m.white_point[1] = (uint16_t)(0.3290 * 50000);
    m.max_display_mastering_luminance = (uint16_t)peak_nits;
    m.min_display_mastering_luminance = 1;                  /* 0.0001 cd/m² OLED */
    m.max_content_light_level        = (uint16_t)peak_nits;
    m.max_frame_average_light_level  = (uint16_t)sdr_nits;
    return m;
}

/* ── NVIDIA legacy gamma fallback ────────────────────────────────────────── */
/*
 * NVIDIA nvidia-drm does not expose DEGAMMA_LUT/CTM/GAMMA_LUT atomic properties.
 * However it does support the legacy DRM_IOCTL_MODE_SETGAMMA call — the same
 * path KWin Plasma 6 uses for NVIDIA HDR.
 *
 * Result: PQ tonemapping (sRGB white → correct PQ nit target) WITHOUT gamut
 * expansion (no CTM = sRGB only). HDR signal still reaches the display via
 * HDR_OUTPUT_METADATA + Colorspace on the connector.
 */
static int set_nvidia_gamma_pq(int fd, uint32_t crtc_id, double sdr_nits, int reset) {
    drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtc_id);
    if (!crtc || crtc->gamma_size == 0) {
        if (crtc) drmModeFreeCrtc(crtc);
        fprintf(stderr, "NVIDIA gamma: crtc has no gamma table\n");
        return -1;
    }
    uint32_t gs = crtc->gamma_size;
    drmModeFreeCrtc(crtc);

    uint16_t *r = malloc(gs * sizeof(uint16_t));
    uint16_t *g = malloc(gs * sizeof(uint16_t));
    uint16_t *b = malloc(gs * sizeof(uint16_t));

    for (uint32_t i = 0; i < gs; i++) {
        uint16_t v;
        if (reset) {
            v = (uint16_t)((double)i / (gs - 1) * 65535.0 + 0.5);
        } else {
            double x      = (double)i / (gs - 1);
            double linear = srgb_to_linear(x);
            double pq     = linear_to_pq(linear * sdr_nits / 10000.0);
            v = (uint16_t)(clamp01(pq) * 65535.0 + 0.5);
        }
        r[i] = g[i] = b[i] = v;
    }

    int ret = drmModeCrtcSetGamma(fd, crtc_id, gs, r, g, b);
    free(r); free(g); free(b);
    return ret;
}

/* ── property helpers ────────────────────────────────────────────────────── */
static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < props->count_props && !result; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, name) == 0) result = p->prop_id;
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props);
    return result;
}

static uint64_t get_enum_val(int fd, uint32_t prop_id, const char *enum_name) {
    drmModePropertyPtr p = drmModeGetProperty(fd, prop_id);
    if (!p) return 0;
    uint64_t val = 0; int found = 0;
    for (int i = 0; i < p->count_enums; i++) {
        if (strcmp(p->enums[i].name, enum_name) == 0) {
            val = (uint64_t)p->enums[i].value; found = 1; break;
        }
    }
    if (!found) {
        printf("  enum '%s' not found on prop %u; available:", enum_name, prop_id);
        for (int i = 0; i < p->count_enums; i++)
            printf(" %s=%llu", p->enums[i].name, (unsigned long long)p->enums[i].value);
        printf("\n");
    }
    drmModeFreeProperty(p);
    return found ? val : 0;
}

static uint32_t mk_blob(int fd, const void *data, size_t sz) {
    uint32_t id = 0;
    drmModeCreatePropertyBlob(fd, data, sz, &id);
    return id;
}

/* ── VT switch ───────────────────────────────────────────────────────────── */
static int vt_switch(int tty_fd, int target_vt) {
    if (ioctl(tty_fd, VT_ACTIVATE,  target_vt) < 0) { perror("VT_ACTIVATE");  return -1; }
    if (ioctl(tty_fd, VT_WAITACTIVE, target_vt) < 0) { perror("VT_WAITACTIVE"); return -1; }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] [reset]\n"
        "\n"
        "Options:\n"
        "  --card /dev/dri/cardN    DRM device (alias: --output; auto-detected)\n"
        "  --connector NAME         Connector, e.g. HDMI-A-2 (alias: --display; auto-detected)\n"
        "  --save                   Write settings to " CONF_PATH " before applying\n"
        "  --sdr-nits N             SDR white brightness in nits (default: 203 or from EDID)\n"
        "  --peak-nits N            Display peak luminance in nits (default: EDID or 800)\n"
        "  --gamut N                Gamut expansion blend 0-100%% (default 100)\n"
        "  --gamut-mode MODE        bt2020 | dci-p3 | srgb (default: from EDID colorimetry)\n"
        "  --saturation N           Color intensity 50-200%% (100=neutral, 150=vivid; default 100)\n"
        "  --bpc N                  Output bit depth: 8, 10, or 12 (default 10)\n"
        "  --no-vt-switch           Skip VT2 switch — try direct DRM master (headless / boot use)\n"
        "  reset                    Restore SDR (identity pipeline)\n"
        "  --help                   Show this message\n"
        "\n"
        "Defaults are auto-configured from EDID (peak nits, gamut) when not set.\n"
        "GPU driver is auto-detected: AMD/Intel = full pipeline; NVIDIA = PQ gamma only.\n"
        "HDMI-CEC: if /dev/cec0 is present, announces active source after HDR enable.\n",
        argv0);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    int reset = 0, save = 0, sdr_nits, peak_nits, gamut_pct, gamut_mode, max_bpc, saturation_pct;
    int no_vt_switch = 0;
    int explicit_peak = 0, explicit_gamut_mode = 0;
    load_conf(&sdr_nits, &peak_nits, &gamut_pct, &gamut_mode, &max_bpc, &saturation_pct);

    char card_path[64] = {0};
    char conn_override[64] = {0};

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "reset")            == 0) reset          = 1;
        else if (strcmp(argv[i], "--help")            == 0 ||
                 strcmp(argv[i], "-h")                == 0) { usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "--save")            == 0) save           = 1;
        else if (strcmp(argv[i], "--no-vt-switch")    == 0) no_vt_switch   = 1;
        else if ((strcmp(argv[i], "--card")       == 0 ||
                  strcmp(argv[i], "--output")     == 0) && i+1 < argc)
            snprintf(card_path, sizeof(card_path), "%s", argv[++i]);
        else if ((strcmp(argv[i], "--connector")  == 0 ||
                  strcmp(argv[i], "--display")    == 0) && i+1 < argc)
            snprintf(conn_override, sizeof(conn_override), "%s", argv[++i]);
        else if (strcmp(argv[i], "--sdr-nits")   == 0 && i+1 < argc) sdr_nits  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--peak-nits")  == 0 && i+1 < argc)
            { peak_nits = atoi(argv[++i]); explicit_peak = 1; }
        else if (strcmp(argv[i], "--gamut")      == 0 && i+1 < argc) gamut_pct = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bpc")        == 0 && i+1 < argc) max_bpc   = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gamut-mode") == 0 && i+1 < argc) {
            const char *m = argv[++i];
            gamut_mode = strcmp(m, "dci-p3") == 0 ? 1 :
                         strcmp(m, "srgb")   == 0 ? 2 : 0;
            explicit_gamut_mode = 1;
        }
        else if (strcmp(argv[i], "--saturation") == 0 && i+1 < argc) saturation_pct = atoi(argv[++i]);
        else { fprintf(stderr, "unknown arg: %s  (try --help)\n", argv[i]); return 1; }
    }

    if (geteuid() != 0) { fprintf(stderr, "must run as root (pkexec or sudo)\n"); return 1; }

    /* Save config before applying if requested */
    if (save && !reset)
        save_conf(sdr_nits, peak_nits, gamut_pct, gamut_mode, max_bpc, saturation_pct);

    /* Auto-detect DRM device if not specified */
    char auto_conn[64] = {0};
    if (card_path[0] == '\0') {
        if (find_drm_device(card_path, sizeof(card_path),
                            auto_conn, sizeof(auto_conn)) != 0) {
            fprintf(stderr, "no connected display found in /sys/class/drm — "
                            "use --card and --connector\n");
            return 1;
        }
        printf("auto-detected: %s  connector: %s\n", card_path, auto_conn);
    }
    /* conn_override takes priority; auto_conn is the fallback */
    const char *want_conn = conn_override[0] ? conn_override :
                            auto_conn[0]      ? auto_conn    : NULL;

    /* ── EDID auto-configuration ──────────────────────────────────────────── */
    if (!reset && want_conn) {
        char sysfs_edid[512];
        const char *cname = strrchr(card_path, '/');
        cname = cname ? cname + 1 : card_path;
        snprintf(sysfs_edid, sizeof(sysfs_edid),
                 "/sys/class/drm/%s-%s/edid", cname, want_conn);

        int edid_peak = 0, edid_bt2020 = 0, edid_dcip3 = 0;
        if (parse_edid_caps(sysfs_edid, &edid_peak, &edid_bt2020, &edid_dcip3) == 0) {
            printf("EDID auto-detect: peak=%d nits  BT.2020=%s  DCI-P3=%s\n",
                   edid_peak, edid_bt2020 ? "✓" : "—", edid_dcip3 ? "✓" : "—");
            if (!explicit_peak && edid_peak > 0) {
                peak_nits = edid_peak;
                printf("  → peak-nits set to %d (from EDID)\n", peak_nits);
            }
            if (!explicit_gamut_mode) {
                if (edid_bt2020)       { gamut_mode = 0; printf("  → gamut-mode: BT.2020 (from EDID)\n"); }
                else if (edid_dcip3)   { gamut_mode = 1; printf("  → gamut-mode: DCI-P3 (from EDID)\n"); }
                else                   { gamut_mode = 2; printf("  → gamut-mode: sRGB (display lacks wide gamut)\n"); }
            }
        }
    }

    const char *gmode_str = gamut_mode == 1 ? "DCI-P3" :
                            gamut_mode == 2 ? "sRGB"   : "BT.2020";
    printf("card: %s  connector: %s\n"
           "config: sdr_nits=%d  peak_nits=%d  gamut=%d%%  mode=%s  bpc=%d  saturation=%d%%\n",
           card_path, want_conn ? want_conn : "any",
           sdr_nits, peak_nits, gamut_pct, gmode_str, max_bpc, saturation_pct);

    int tty_fd = -1;
    if (!no_vt_switch) {
        tty_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (tty_fd < 0) {
            fprintf(stderr, "open /dev/tty1: %s — retrying without VT switch\n", strerror(errno));
            no_vt_switch = 1;
        }
    }
    if (!no_vt_switch) {
        printf("switching to VT2 (screen will blank ~0.5s)...\n");
        if (vt_switch(tty_fd, 2) < 0) {
            fprintf(stderr, "VT switch failed — proceeding without it\n");
            close(tty_fd); tty_fd = -1; no_vt_switch = 1;
        } else {
            usleep(300000);
        }
    } else {
        printf("no-vt-switch mode: attempting direct DRM master acquisition\n");
    }

    int fd = open(card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(card_path);
        if (tty_fd >= 0) { vt_switch(tty_fd, 1); close(tty_fd); }
        return 1;
    }

    /* ── Detect GPU driver ──────────────────────────────────────────────── */
    drmVersionPtr drv = drmGetVersion(fd);
    int is_nvidia = drv && strstr(drv->name, "nvidia") != NULL;
    printf("GPU driver: %s  (pipeline mode: %s)\n",
           drv ? drv->name : "unknown",
           is_nvidia ? "metadata-only (HDR_OUTPUT_METADATA + Colorspace + BPC)"
                     : "full (DEGAMMA+CTM+GAMMA+HDR_OUTPUT_METADATA)");
    if (drv) drmFreeVersion(drv);

    if (is_nvidia && !reset) {
        printf("NVIDIA: DEGAMMA_LUT/CTM/GAMMA_LUT not exposed by nvidia-drm.\n"
               "  HDR10 signal will be sent; no software tonemapping/gamut expansion.\n"
               "  SDR desktop may appear washed — use Gamescope/mpv for HDR content.\n");
    }

#define VT_CLEANUP()  do { if (tty_fd >= 0) { vt_switch(tty_fd, 1); close(tty_fd); } } while(0)

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "drmSetClientCap: %s\n", strerror(errno));
        VT_CLEANUP(); return 1;
    }
    /* Retry loop — compositor may take a moment to release on VT switch */
    int master_ok = 0;
    for (int r = 0; r < MASTER_RETRIES; r++) {
        if (drmSetMaster(fd) == 0) { master_ok = 1; break; }
        if (r == 0) fprintf(stderr, "waiting for DRM master...\n");
        usleep(MASTER_RETRY_MS * 1000);
    }
    if (!master_ok) {
        fprintf(stderr, "drmSetMaster failed after %d retries: %s\n",
                MASTER_RETRIES, strerror(errno));
        close(fd); VT_CLEANUP(); return 1;
    }
    printf("DRM master acquired\n");

    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); drmDropMaster(fd); VT_CLEANUP(); return 1; }

    /* Find connector and CRTC — match by name if want_conn given, else first connected */
    static const char *conn_type_names[] = {
        "Unknown","VGA","DVII","DVID","DVIA","Composite","SVIDEO",
        "LVDS","Component","9PinDIN","DisplayPort","HDMI-A","HDMI-B",
        "TV","eDP","Virtual","DSI","DPI","Writeback","SPI","USB"
    };
    uint32_t conn_id = 0, crtc_id = 0;
    for (int i = 0; i < res->count_connectors && !conn_id; i++) {
        drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection != DRM_MODE_CONNECTED) { drmModeFreeConnector(c); continue; }

        /* Build connector name string e.g. "HDMI-A-2" */
        const char *type_str = (c->connector_type < 21)
                               ? conn_type_names[c->connector_type] : "Unknown";
        char cname[64];
        snprintf(cname, sizeof(cname), "%s-%u", type_str, c->connector_type_id);

        int match = (want_conn == NULL) ||
                    (strcasecmp(cname, want_conn) == 0) ||
                    (strstr(cname, want_conn) != NULL);

        if (match) {
            conn_id = c->connector_id;
            if (c->encoder_id) {
                drmModeEncoderPtr enc = drmModeGetEncoder(fd, c->encoder_id);
                if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
            }
            printf("using connector: %s (id=%u)  CRTC=%u\n", cname, conn_id, crtc_id);
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);

    if (!conn_id || !crtc_id) {
        fprintf(stderr, "no connected %s connector/CRTC found\n",
                want_conn ? want_conn : "display");
        drmDropMaster(fd); VT_CLEANUP(); return 1;
    }

    /* ── Build LUTs and CTM ─────────────────────────────────────────────── */
    drm_lut_entry *deg_lut, *gam_lut;
    uint64_t ctm9[9];

    if (reset) {
        deg_lut = build_linear_lut(LUT_SIZE);
        gam_lut = build_linear_lut(LUT_SIZE);
        build_ctm_identity(ctm9);
    } else {
        deg_lut = build_degamma_srgb(LUT_SIZE);
        gam_lut = build_gamma_pq(LUT_SIZE, (double)sdr_nits);
        /* Build gamut expansion matrix (identity → target, blended by gamut_pct). */
        double t = gamut_pct / 100.0;
        double gamut_mat[3][3];
        const double (*target)[3] = (gamut_mode == 1) ? CTM_709_TO_DCIP3 : CTM_709_TO_2020;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                gamut_mat[r][c] = (r==c ? 1.0 : 0.0) * (1.0-t) + target[r][c] * t;
        /* Apply saturation matrix first (in sRGB linear), then gamut expansion:
         * combined = gamut_mat × sat_mat  (right-to-left in transform order). */
        double sat_mat[3][3];
        build_sat_mat(saturation_pct / 100.0, sat_mat);
        double combined[3][3];
        mat_mul_3x3(gamut_mat, sat_mat, combined);
        build_ctm((const double(*)[3])combined, ctm9);
    }

    uint32_t deg_blob = mk_blob(fd, deg_lut, LUT_SIZE * sizeof(drm_lut_entry));
    uint32_t gam_blob = mk_blob(fd, gam_lut, LUT_SIZE * sizeof(drm_lut_entry));
    uint32_t ctm_blob = mk_blob(fd, ctm9, sizeof(ctm9));
    free(deg_lut); free(gam_lut);
    printf("blobs: DEGAMMA=%u CTM=%u GAMMA=%u\n", deg_blob, ctm_blob, gam_blob);

    /* ── Get property IDs ───────────────────────────────────────────────── */
    uint32_t p_deg    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "DEGAMMA_LUT");
    uint32_t p_ctm    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "CTM");
    uint32_t p_gam    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "GAMMA_LUT");
    uint32_t p_hdr    = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA");
    uint32_t p_cspace = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "Colorspace");
    printf("props: DEGAMMA=%u CTM=%u GAMMA=%u HDR=%u Colorspace=%u\n",
           p_deg, p_ctm, p_gam, p_hdr, p_cspace);

    /* ── Commit color pipeline ──────────────────────────────────────────── */
    int ret;
    if (is_nvidia) {
        /*
         * NVIDIA path: legacy 1D gamma ramp (PQ tonemapping, no gamut expansion).
         * Same approach as KWin Plasma 6 on NVIDIA.
         * Gamut expansion requires CTM which nvidia-drm does not expose.
         */
        ret = set_nvidia_gamma_pq(fd, crtc_id, (double)sdr_nits, reset);
        printf("NVIDIA legacy gamma (sRGB→PQ): ret=%d  errno=%d (%s)\n",
               ret, errno, ret ? strerror(errno) : "ok");
    } else {
        /*
         * AMD/Intel path: full atomic pipeline — DEGAMMA→CTM→GAMMA.
         * Provides both PQ tonemapping AND gamut expansion (BT.2020/DCI-P3).
         */
        drmModeAtomicReqPtr req = drmModeAtomicAlloc();
        if (p_deg) drmModeAtomicAddProperty(req, crtc_id, p_deg, reset ? 0 : deg_blob);
        if (p_ctm) drmModeAtomicAddProperty(req, crtc_id, p_ctm, reset ? 0 : ctm_blob);
        if (p_gam) drmModeAtomicAddProperty(req, crtc_id, p_gam, reset ? 0 : gam_blob);
        ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        printf("AMD/Intel color pipeline commit ret=%d  errno=%d (%s)\n",
               ret, errno, ret ? strerror(errno) : "ok");
        drmModeAtomicFree(req);
    }

    /* ── Commit HDR metadata + colorspace ──────────────────────────────── */
    int hdr_ret = -1;
    if (p_hdr && p_cspace) {
        uint64_t cspace_val = reset ? 0 : get_enum_val(fd, p_cspace, "BT2020_RGB");

        hdr_meta_t hdr_m  = build_hdr_meta(peak_nits, sdr_nits);
        uint32_t hdr_blob = (reset || cspace_val == 0) ? 0
                            : mk_blob(fd, &hdr_m, sizeof(hdr_m));

        uint32_t p_crtc_id  = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        uint32_t p_crtc_act = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "ACTIVE");
        uint32_t p_mode_id  = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "MODE_ID");
        uint32_t p_bpc      = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,  "max_requested_bpc");
        uint32_t p_vrr      = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,  "vrr_enabled");

        /* Save VRR state and disable it before modeset to avoid black-screen glitch */
        uint64_t vrr_saved = 0;
        if (p_vrr) {
            drmModeObjectPropertiesPtr oprops =
                drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
            if (oprops) {
                for (uint32_t i = 0; i < oprops->count_props; i++) {
                    if (oprops->props[i] == p_vrr) { vrr_saved = oprops->prop_values[i]; break; }
                }
                drmModeFreeObjectProperties(oprops);
            }
            if (vrr_saved) {
                drmModeAtomicReqPtr vrr_req = drmModeAtomicAlloc();
                drmModeAtomicAddProperty(vrr_req, conn_id, p_vrr, 0);
                drmModeAtomicCommit(fd, vrr_req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
                drmModeAtomicFree(vrr_req);
                usleep(50000);  /* 50 ms — let panel exit VRR mode */
                printf("VRR disabled (was %llu) before modeset\n", (unsigned long long)vrr_saved);
            }
        }

        drmModeCrtcPtr cur = drmModeGetCrtc(fd, crtc_id);
        uint32_t mode_blob = 0;
        if (cur && p_mode_id) {
            drmModeCreatePropertyBlob(fd, &cur->mode, sizeof(cur->mode), &mode_blob);
            drmModeFreeCrtc(cur);
        }

        drmModeAtomicReqPtr req2 = drmModeAtomicAlloc();
        if (p_crtc_id)          drmModeAtomicAddProperty(req2, conn_id, p_crtc_id,  crtc_id);
                                 drmModeAtomicAddProperty(req2, conn_id, p_hdr,      hdr_blob);
                                 drmModeAtomicAddProperty(req2, conn_id, p_cspace,   cspace_val);
        if (p_bpc && !reset)    drmModeAtomicAddProperty(req2, conn_id, p_bpc,      (uint64_t)max_bpc);
        if (p_crtc_act)         drmModeAtomicAddProperty(req2, crtc_id, p_crtc_act, 1);
        if (p_mode_id && mode_blob)
                                drmModeAtomicAddProperty(req2, crtc_id, p_mode_id,  mode_blob);

        /* Try without ALLOW_MODESET first (avoids blank if driver supports it) */
        hdr_ret = drmModeAtomicCommit(fd, req2, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        if (hdr_ret != 0) {
            hdr_ret = drmModeAtomicCommit(fd, req2, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
            printf("HDR+Colorspace modeset ret=%d  errno=%d (%s)\n",
                   hdr_ret, errno, hdr_ret ? strerror(errno) : "ok");
        } else {
            printf("HDR+Colorspace non-blocking commit: ok\n");
        }
        drmModeAtomicFree(req2);
        if (hdr_blob)  drmModeDestroyPropertyBlob(fd, hdr_blob);
        if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);

        /* Restore VRR if it was enabled */
        if (p_vrr && vrr_saved && hdr_ret == 0) {
            usleep(100000);  /* 100 ms — let HDR modeset settle */
            drmModeAtomicReqPtr vrr_req = drmModeAtomicAlloc();
            drmModeAtomicAddProperty(vrr_req, conn_id, p_vrr, vrr_saved);
            int vrr_ret = drmModeAtomicCommit(fd, vrr_req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
            drmModeAtomicFree(vrr_req);
            printf("VRR restored ret=%d\n", vrr_ret);
        }
    }

    drmDropMaster(fd);
    drmModeDestroyPropertyBlob(fd, deg_blob);
    drmModeDestroyPropertyBlob(fd, ctm_blob);
    drmModeDestroyPropertyBlob(fd, gam_blob);
    close(fd);

    if (tty_fd >= 0) {
        printf("DRM master released — switching back to VT1...\n");
        vt_switch(tty_fd, 1);
        close(tty_fd);
    }

    if (ret == 0 && hdr_ret == 0) {
        if (reset) {
            printf("✓ reset: SDR restored\n");
        } else if (is_nvidia) {
            printf("✓ NVIDIA HDR ACTIVE (PQ tonemapping only — no gamut expansion)\n"
                   "  sRGB→PQ via legacy gamma  SDR white=%d nits  peak=%d nits  bpc=%d\n"
                   "  For wide-color gamut on NVIDIA: use Gamescope (Vulkan-based)\n",
                   sdr_nits, peak_nits, max_bpc);
        } else {
            printf("✓ HDR10 ACTIVE: sRGB→linear→%s→PQ pipeline live\n"
                   "  SDR white=%d nits  peak=%d nits  gamut=%d%%  saturation=%d%%  bpc=%d\n",
                   gmode_str, sdr_nits, peak_nits, gamut_pct, saturation_pct, max_bpc);
        }

        /* HDMI-CEC: announce active source so the TV switches input automatically */
        if (!reset && access("/dev/cec0", F_OK) == 0) {
            int cec = system("cec-ctl --device=0 --active-source phys-addr=0.0.0.0"
                             " >/dev/null 2>&1");
            printf("HDMI-CEC: %s\n", cec == 0
                   ? "active-source announced ✓"
                   : "cec-ctl not found — install v4l-utils for CEC control");
        }
    } else {
        printf("pipeline ret=%d  HDR ret=%d\n", ret, hdr_ret);
    }
    return (ret != 0);
}
