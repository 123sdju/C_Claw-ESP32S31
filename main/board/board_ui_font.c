#include "board_ui_font.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"

#include "esp_log.h"

#if __has_include("lv_binfont_loader.h")
#include "lv_binfont_loader.h"
#define BOARD_UI_HAS_BINFONT 1
#elif __has_include("lvgl/src/font/binfont_loader/lv_binfont_loader.h")
#include "lvgl/src/font/binfont_loader/lv_binfont_loader.h"
#define BOARD_UI_HAS_BINFONT 1
#elif __has_include("font/binfont_loader/lv_binfont_loader.h")
#include "font/binfont_loader/lv_binfont_loader.h"
#define BOARD_UI_HAS_BINFONT 1
#else
#define BOARD_UI_HAS_BINFONT 0
#endif

#ifndef LV_USE_FS_MEMFS
#define LV_USE_FS_MEMFS 0
#endif

/* 条件声明思源黑体 SC CJK 字体（16px 或 14px 回退） */
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);
#elif LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
LV_FONT_DECLARE(lv_font_source_han_sans_sc_14_cjk);
#endif

#define UI_BINFONT_MAX_CMAPS 4096U
#define UI_BINFONT_MAX_GLYPHS 8192U

#ifndef BOARD_UI_FONT_PSRAM_ALIGN
#define BOARD_UI_FONT_PSRAM_ALIGN 64U
#endif

#ifndef BOARD_UI_FONT_LOAD_TO_PSRAM
#define BOARD_UI_FONT_LOAD_TO_PSRAM 1
#endif

static const char *TAG = "board_ui_font";

typedef struct {
    uint32_t version;
    uint16_t tables_count;
    uint16_t font_size;
    uint16_t ascent;
    int16_t descent;
    uint16_t typo_ascent;
    int16_t typo_descent;
    uint16_t typo_line_gap;
    int16_t min_y;
    int16_t max_y;
    uint16_t default_advance_width;
    uint16_t kerning_scale;
    uint8_t index_to_loc_format;
    uint8_t glyph_id_format;
    uint8_t advance_width_format;
    uint8_t bits_per_pixel;
    uint8_t xy_bits;
    uint8_t wh_bits;
    uint8_t advance_width_bits;
    uint8_t compression_id;
    uint8_t subpixels_mode;
    uint8_t padding;
    int16_t underline_position;
    uint16_t underline_thickness;
} ui_binfont_header_t;

typedef struct {
    uint32_t data_offset;
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    uint16_t data_entries_count;
    uint8_t format_type;
    uint8_t padding;
} ui_binfont_cmap_table_t;

static lv_font_t *s_external_font;
static const lv_font_t *s_current_font;
static uint8_t *s_external_font_buf;
static uint32_t s_external_font_buf_size;
static char s_loaded_font_path[160];
static char s_font_path[160] = BOARD_UI_FONT_PATH_DEFAULT;

/*
 * 格式化字节数为人可读字符串（B/KB/MB）。
 * 参数: bytes    - 字节数
 *       out      - 输出缓冲区
 *       out_size - 输出缓冲区大小
 */
static void format_bytes(size_t bytes, char *out, size_t out_size)
{
    const char *unit = "B";
    double value = (double)bytes;

    if (bytes >= 1024 * 1024) {
        value /= 1024.0 * 1024.0;
        unit = "MB";
    } else if (bytes >= 1024) {
        value /= 1024.0;
        unit = "KB";
    }

    if (strcmp(unit, "B") == 0) {
        snprintf(out, out_size, "%uB", (unsigned)bytes);
    } else {
        snprintf(out, out_size, "%.2f%s", value, unit);
    }
}

/*
 * 返回 LVGL 内置默认字体。
 * 返回: LV_FONT_DEFAULT 指针
 */
static const lv_font_t *font_builtin(void)
{
    return LV_FONT_DEFAULT;
}

/*
 * 获取当前活跃字体。若外部字体已加载成功则返回外部字体，否则返回内置字体。
 * 返回: 当前 lv_font_t 指针
 */
const lv_font_t *board_ui_font_current(void)
{
    return s_current_font ? s_current_font : font_builtin();
}

/*
 * 设置外部字体文件路径。下次 load_external 将从此路径加载。
 * 参数: path - 字体文件路径，NULL 或空则使用默认路径
 */
void board_ui_font_set_path(const char *path)
{
    snprintf(s_font_path, sizeof(s_font_path), "%s", path && path[0] ? path : BOARD_UI_FONT_PATH_DEFAULT);
}

/*
 * 将用户路径转换为 POSIX VFS 路径（跳过驱动器前缀 A: 等）。
 * 参数: path - 用户路径（可能含 A: 前缀）
 * 返回: VFS 路径指针（跳过驱动器前缀后的位置）
 */
static const char *font_vfs_path(const char *path)
{
    return (path && path[0] && path[1] == ':') ? path + 2 : path;
}

/*
 * 将普通路径转换为 LVGL 文件系统路径（添加 A: 前缀）。
 * 参数: path - 原始路径
 *       buf  - 输出缓冲区
 *       len  - 缓冲区大小
 * 返回: LVGL 格式路径
 */
static const char *font_lvgl_path(const char *path, char *buf, size_t len)
{
    if (!path || !path[0] || path[1] == ':') {
        return path;
    }
    snprintf(buf, len, "A:%s", path);
    return buf;
}

/*
 * 从 LVGL 文件系统精确读取指定长度数据。
 * 参数: file - 已打开文件句柄
 *       buf  - 目标缓冲区
 *       len  - 要读取的字节数
 * 返回: true 读取成功且长度匹配
 */
static bool lv_fs_read_exact(lv_fs_file_t *file, void *buf, uint32_t len)
{
    uint32_t read = 0;
    return lv_fs_read(file, buf, len, &read) == LV_FS_RES_OK && read == len;
}

/*
 * 读取二进制字体块标签和长度，验证标签四字节是否匹配。
 * 参数: file      - 已打开文件句柄
 *       file_size - 文件总大小
 *       start     - 块起始偏移
 *       label     - 期望四字节标签（如 "cmap"）
 *       length    - 输出块长度
 * 返回: true 读取成功且标签匹配
 */
static bool binfont_read_label(lv_fs_file_t *file, uint32_t file_size,
                               uint32_t start, const char *label, uint32_t *length)
{
    char got[4];

    if (start > file_size || file_size - start < 8U) return false;
    if (lv_fs_seek(file, start, LV_FS_SEEK_SET) != LV_FS_RES_OK) return false;
    if (!lv_fs_read_exact(file, length, sizeof(*length))) return false;
    if (!lv_fs_read_exact(file, got, sizeof(got))) return false;
    if (memcmp(got, label, sizeof(got)) != 0) return false;
    if (*length < 8U || *length > file_size - start) return false;
    return true;
}

/*
 * 验证二进制字体 bit-per-pixel 值是否合法（1/2/4/8）。
 * 参数: bpp - 每像素比特数
 * 返回: true 合法
 */
static bool binfont_valid_bpp(uint8_t bpp)
{
    return bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8;
}

/*
 * 验证二进制字体 cmap 表数量、格式类型和数据偏移有效性。
 * 参数: file              - 已打开文件句柄
 *       file_size         - 文件总大小
 *       cmaps_start       - cmap 块起始偏移
 *       cmaps_length_out  - 输出 cmap 块长度
 * 返回: true 验证通过
 */
static bool binfont_validate_cmaps(lv_fs_file_t *file, uint32_t file_size,
                                   uint32_t cmaps_start, uint32_t *cmaps_length_out)
{
    uint32_t cmaps_length = 0;
    uint32_t cmap_count = 0;

    if (!binfont_read_label(file, file_size, cmaps_start, "cmap", &cmaps_length)) return false;
    if (cmaps_length < 12U) return false;
    if (!lv_fs_read_exact(file, &cmap_count, sizeof(cmap_count))) return false;
    if (cmap_count == 0 || cmap_count > UI_BINFONT_MAX_CMAPS) return false;

    uint32_t table_bytes = cmap_count * (uint32_t)sizeof(ui_binfont_cmap_table_t);
    if (table_bytes / sizeof(ui_binfont_cmap_table_t) != cmap_count) return false;
    if (cmaps_length < 12U + table_bytes) return false;

    for (uint32_t i = 0; i < cmap_count; ++i) {
        ui_binfont_cmap_table_t table;
        uint32_t data_size = 0;

        if (!lv_fs_read_exact(file, &table, sizeof(table))) return false;
        switch (table.format_type) {
        case 0:
            data_size = table.data_entries_count;
            break;
        case 1:
            data_size = (uint32_t)table.data_entries_count * 4U;
            break;
        case 2:
            data_size = 0;
            break;
        case 3:
            data_size = (uint32_t)table.data_entries_count * 2U;
            break;
        default:
            return false;
        }
        if (table.data_offset > cmaps_length) return false;
        if (data_size > cmaps_length - table.data_offset) return false;
        if (table.range_length == 0) return false;
    }

    *cmaps_length_out = cmaps_length;
    return true;
}

/*
 * 逐字节验证二进制字体的 loca 表和 glyf 表结构完整性和可偏移性。
 * 参数: file         - 已打开的 LVGL 文件句柄
 *       file_size    - 字体文件总大小
 *       loca_start   - loca 表起始偏移
 *       header       - 已解析的字体头部信息
 * 返回: true 验证通过
 */
static bool binfont_validate_loca_and_glyph(lv_fs_file_t *file, uint32_t file_size,
                                            uint32_t loca_start,
                                            const ui_binfont_header_t *header)
{
    uint32_t loca_length = 0;
    uint32_t glyph_start = 0;
    uint32_t glyph_length = 0;
    uint32_t loca_count = 0;
    uint32_t offset_size = header->index_to_loc_format == 0 ? 2U : 4U;
    uint32_t prev_offset = 0;

    if (!binfont_read_label(file, file_size, loca_start, "loca", &loca_length)) return false;
    if (!lv_fs_read_exact(file, &loca_count, sizeof(loca_count))) return false;
    if (loca_count == 0 || loca_count > UI_BINFONT_MAX_GLYPHS) return false;
    if (loca_count > (UINT32_MAX / offset_size)) return false;
    if (loca_length < 12U + loca_count * offset_size) return false;

    glyph_start = loca_start + loca_length;
    if (glyph_start < loca_start) return false;
    if (!binfont_read_label(file, file_size, glyph_start, "glyf", &glyph_length)) return false;
    if (lv_fs_seek(file, loca_start + 12U, LV_FS_SEEK_SET) != LV_FS_RES_OK) return false;

    for (uint32_t i = 0; i < loca_count; ++i) {
        uint32_t offset = 0;
        if (offset_size == 2U) {
            uint16_t short_offset = 0;
            if (!lv_fs_read_exact(file, &short_offset, sizeof(short_offset))) return false;
            offset = short_offset;
        } else {
            if (!lv_fs_read_exact(file, &offset, sizeof(offset))) return false;
        }
        if (offset < prev_offset || offset > glyph_length) return false;
        prev_offset = offset;
    }

    return true;
}

/*
 * 完整验证二进制字体文件：检查 header/cmaps/loca/glyf 结构完整性和合法性。
 * 参数: lvgl_path - LVGL 文件系统路径
 *       file_size - 字体文件大小（字节）
 * 返回: true 验证通过，false 格式无效或不支持
 */
static bool binfont_preflight(const char *lvgl_path, uint32_t file_size)
{
    lv_fs_file_t file;
    lv_fs_res_t fs_res = lv_fs_open(&file, lvgl_path, LV_FS_MODE_RD);
    if (fs_res != LV_FS_RES_OK) {
        ESP_LOGW(TAG, "lv_fs_open failed: %s, fs_res=%d", lvgl_path, (int)fs_res);
        return false;
    }

    uint32_t header_length = 0;
    uint32_t cmaps_length = 0;
    ui_binfont_header_t header;
    bool ok = false;

    if (!binfont_read_label(&file, file_size, 0, "head", &header_length)) goto done;
    if (header_length < 8U + sizeof(header)) goto done;
    if (!lv_fs_read_exact(&file, &header, sizeof(header))) goto done;
    if (header.tables_count < 3 || header.tables_count > 4) goto done;
    if (header.font_size == 0 || header.font_size > 96) goto done;
    int32_t line_height = (int32_t)header.ascent - (int32_t)header.descent;
    if (line_height <= 0 || line_height > 256) goto done;
    if (header.index_to_loc_format > 1) goto done;
    if (header.glyph_id_format > 1) goto done;
    if (header.advance_width_format > 1) goto done;
    if (!binfont_valid_bpp(header.bits_per_pixel)) goto done;
    if (header.xy_bits == 0 || header.xy_bits > 16) goto done;
    if (header.wh_bits == 0 || header.wh_bits > 16) goto done;
    if (header.advance_width_bits > 16) goto done;
    if (header.compression_id > 2) goto done;

    if (!binfont_validate_cmaps(&file, file_size, header_length, &cmaps_length)) goto done;
    ok = binfont_validate_loca_and_glyph(&file, file_size, header_length + cmaps_length, &header);

done:
    lv_fs_close(&file);
    if (!ok) {
        char size_text[16];
        format_bytes(file_size, size_text, sizeof(size_text));
        ESP_LOGW(TAG, "Invalid or unsupported binfont: %s, size=%s (%" PRIu32 "B)",
                 lvgl_path, size_text, file_size);
    }
    return ok;
}


/*
 * 释放候选字体缓冲区。
 * 参数: buf - 指向字体缓冲指针的指针，释放后置 NULL
 */
static void free_candidate_font_buffer(uint8_t **buf)
{
    if (buf && *buf) {
        heap_caps_free(*buf);
        *buf = NULL;
    }
}

/*
 * 加载字体文件到 PSRAM 缓冲区并返回完整内容。
 * 参数: vfs_path - 文件系统路径（POSIX open 路径）
 *       size     - 文件大小
 * 返回: 对齐分配的 PSRAM 缓冲区指针，失败返回 NULL
 */
static uint8_t *load_file_to_psram(const char *vfs_path, uint32_t size)
{
    if (!vfs_path || !vfs_path[0] || size == 0) {
        return NULL;
    }

    int fd = open(vfs_path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "font open failed: %s, errno=%d", vfs_path, errno);
        return NULL;
    }

    uint8_t *buf = heap_caps_aligned_alloc(BOARD_UI_FONT_PSRAM_ALIGN,
                                           size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        close(fd);
        ESP_LOGW(TAG, "font PSRAM alloc failed: %s, size=%" PRIu32 "", vfs_path, size);
        return NULL;
    }

    size_t total = 0;
    while (total < size) {
        ssize_t chunk = read(fd, buf + total, size - total);
        if (chunk < 0) {
            ESP_LOGW(TAG, "font read failed: %s, errno=%d", vfs_path, errno);
            break;
        }
        if (chunk == 0) {
            break;
        }
        total += (size_t)chunk;
    }

    close(fd);

    if (total != size) {
        ESP_LOGW(TAG, "font read incomplete: %s, got=%u expected=%" PRIu32 "",
                 vfs_path, (unsigned)total, size);
        heap_caps_free(buf);
        return NULL;
    }

    ESP_LOGI(TAG, "font file loaded into PSRAM: ptr=%p size=%" PRIu32 " align=%u",
             buf, size, (unsigned)BOARD_UI_FONT_PSRAM_ALIGN);
    return buf;
}

/*
 * 从文件系统加载外部二进制字体（思源黑体 CJK 回退字体）。
 * 支持 PSRAM 预加载 + lv_binfont_create_from_buffer 路径和传统 lv_binfont_create 文件路径两种方式。
 * 返回: true 加载成功并切换字体，false 失败（回退到 LV_FONT_DEFAULT）
 */
bool board_ui_font_load_external(void)
{
    s_current_font = font_builtin();

    struct stat st = {0};
    const char *vfs_path = font_vfs_path(s_font_path);
    char lvgl_path_buf[sizeof(s_font_path) + 2];
    const char *lvgl_path = font_lvgl_path(s_font_path, lvgl_path_buf, sizeof(lvgl_path_buf));

    if (stat(vfs_path, &st) == 0) {
        char font_size_text[16];
        if (st.st_size > 0) {
            format_bytes((size_t)st.st_size, font_size_text, sizeof(font_size_text));
        } else {
            snprintf(font_size_text, sizeof(font_size_text), "%ldB", (long)st.st_size);
        }

        if (s_external_font && strcmp(s_loaded_font_path, lvgl_path) == 0) {
            s_current_font = s_external_font;
            return true;
        }

#if BOARD_UI_HAS_BINFONT
        {
            if (st.st_size <= 0 || st.st_size > UINT32_MAX) {
                ESP_LOGW(TAG, "font size out of range: %s, size=%s (%ldB)",
                         lvgl_path, font_size_text, (long)st.st_size);
                goto font_fallback;
            }
            if (!binfont_preflight(lvgl_path, (uint32_t)st.st_size)) {
                goto font_fallback;
            }

#if BOARD_UI_FONT_LOAD_TO_PSRAM && LV_USE_FS_MEMFS
            uint8_t *candidate_buf = load_file_to_psram(vfs_path, (uint32_t)st.st_size);
            if (candidate_buf) {
                lv_font_t *candidate_font = lv_binfont_create_from_buffer(candidate_buf, (uint32_t)st.st_size);
                if (candidate_font) {
                    /*
                     * Keep the backing buffer alive for the lifetime of the font.
                     * Do not destroy the old font here: existing labels may still reference it
                     * until board_ui reapplies fonts to the object tree after this function returns.
                     */
                    s_external_font = candidate_font;
                    s_external_font_buf = candidate_buf;
                    s_external_font_buf_size = (uint32_t)st.st_size;
                    s_current_font = s_external_font;
                    snprintf(s_loaded_font_path, sizeof(s_loaded_font_path), "%s", lvgl_path);
                    ESP_LOGI(TAG, "Loaded binfont from PSRAM buffer: %s, ptr=%p, size=%s (%ldB)",
                             lvgl_path, s_external_font_buf, font_size_text, (long)st.st_size);
                    return true;
                }

                ESP_LOGW(TAG, "lv_binfont_create_from_buffer failed: %s, size=%s (%ldB)",
                         lvgl_path, font_size_text, (long)st.st_size);
                free_candidate_font_buffer(&candidate_buf);
            }
#endif

            s_external_font = lv_binfont_create(lvgl_path);
            if (s_external_font) {
                s_external_font_buf = NULL;
                s_external_font_buf_size = 0;
                s_current_font = s_external_font;
                snprintf(s_loaded_font_path, sizeof(s_loaded_font_path), "%s", lvgl_path);
                ESP_LOGI(TAG, "Loaded binfont from filesystem: %s, size=%s (%ldB)",
                         lvgl_path, font_size_text, (long)st.st_size);
                return true;
            }
            ESP_LOGW(TAG, "lv_binfont_create failed: %s, size=%s (%ldB)",
                     lvgl_path, font_size_text, (long)st.st_size);
        }
#else
        {
            ESP_LOGW(TAG, "External binfont loading unavailable: %s", lvgl_path);
        }
#endif
    } else {
        ESP_LOGW(TAG, "font stat failed: %s, errno=%d", vfs_path, errno);
    }

font_fallback:
    ESP_LOGW(TAG, "External font unavailable, fallback active: %s", s_font_path);
    return false;
}
