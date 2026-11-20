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
static char s_font_path[160] = BOARD_UI_FONT_PATH_DEFAULT;
static char s_loaded_font_path[sizeof(s_font_path) + 2];

/**
 * @brief 将字节数格式化为 B、KB 或 MB，较大单位保留两位小数。
 * @param[in] bytes 原始字节数。
 * @param[out] out 调用者输出区，out_size 非零时须有效。
 * @param[in] out_size 输出区容量，单位为字节，包含 NUL，可为零。
 * @post 按 snprintf 容量截断，非零容量的有效输出区保持 NUL 终止。
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

/**
 * @brief 取得编译配置选定的内置字体。
 * @return 进程期有效的 LV_FONT_DEFAULT 借用，无需释放。
 */
static const lv_font_t *font_builtin(void)
{
    return LV_FONT_DEFAULT;
}

/**
 * @brief 取得最近一次字体事务选定的字体，首次加载前使用内置字体。
 * @return 字体借用，外部字体的借用止于 owner 完成下一次重绑并释放退休字体。
 * @pre 调用者与加载、路径更新及退休释放串行，UI 通过 LVGL 锁保护该状态。
 */
const lv_font_t *board_ui_font_current(void)
{
    return s_current_font ? s_current_font : font_builtin();
}

/**
 * @brief 将下次加载使用的路径复制到固定配置容量。
 * @param[in] path 调用期 NUL 字符串借用，NULL 或空串恢复默认路径，超长路径按容量截断。
 * @post 路径始终 NUL 终止，当前字体及已创建的标签保持现状。
 * @pre owner 排斥并发路径更新和加载，调用者不得将全长路径截断误认为加载成功。
 */
void board_ui_font_set_path(const char *path)
{
    snprintf(s_font_path, sizeof(s_font_path), "%s", path && path[0] ? path : BOARD_UI_FONT_PATH_DEFAULT);
}

/**
 * @brief 跳过单字符 LVGL 驱动器前缀，得到 stat/open 使用的 VFS 路径。
 * @param[in] path NUL 路径借用，可 NULL。
 * @return 原字符串或前移两字节的借用，NULL 输入返回 NULL。
 */
static const char *font_vfs_path(const char *path)
{
    return (path && path[0] && path[1] == ':') ? path + 2 : path;
}

/**
 * @brief 为没有单字符驱动器前缀的路径添加 A:。
 * @param[in] path 调用期 NUL 路径借用，可 NULL。
 * @param[out] buf 需要补前缀时使用的独占输出区。
 * @param[in] len buf 的字节容量，须容纳完整前缀、路径和 NUL。
 * @return 已有前缀或空输入直接返回 path，否则返回 buf，格式化失败返回 NULL。
 */
static const char *font_lvgl_path(const char *path, char *buf, size_t len)
{
    if (!path || !path[0] || path[1] == ':') {
        return path;
    }
    int written = snprintf(buf, len, "A:%s", path);
    return written >= 0 && (size_t)written < len ? buf : NULL;
}

/**
 * @brief 从当前位置读取一个完整定长字段。
 * @param[in,out] file 调用者独占的已打开 LVGL 文件，读取推进位置。
 * @param[out] buf 至少 len 字节的有效输出区，失败时可能已写入前缀。
 * @param[in] len 要读取的字节数。
 * @return 原生读取成功且长度完全相等为 true，错误或短读为 false。
 */
static bool lv_fs_read_exact(lv_fs_file_t *file, void *buf, uint32_t len)
{
    uint32_t read = 0;
    return lv_fs_read(file, buf, len, &read) == LV_FS_RES_OK && read == len;
}

/**
 * @brief 定位并核对字体块的四字节标签和文件内长度边界。
 * @param[in,out] file 独占打开文件，成功后位置位于块头之后，失败也可能改变位置。
 * @param[in] file_size 预先取得的文件字节数，调用期文件须稳定。
 * @param[in] start 块起始字节偏移。
 * @param[in] label 至少四字节的期望标签借用。
 * @param[out] length 有效输出槽，读取长度后即更新，false 时不得使用。
 * @return 完整块头、标签及块范围均有效为 true，否则 false。
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

/**
 * @brief 判断位图每像素位数是否属于预检支持的四种格式。
 * @param[in] bpp 每像素位数。
 * @return 1、2、4、8 为 true，其余为 false。
 */
static bool binfont_valid_bpp(uint8_t bpp)
{
    return bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8;
}

/**
 * @brief 核对 cmap 数量、支持的格式和各数据段在块内的范围。
 * @param[in,out] file 独占打开文件，定位并逐项读取会改变文件位置。
 * @param[in] file_size 稳定文件的总字节数。
 * @param[in] cmaps_start cmap 块起始字节偏移。
 * @param[out] cmaps_length_out 有效长度输出槽，仅成功时写入块的字节长度。
 * @return 数量在 1 到 4096 且所检查字段有效为 true，否则 false，无堆分配。
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

/**
 * @brief 检查 loca 计数、索引存储范围及到 glyf 块的非递减偏移。
 * @param[in,out] file 独占打开文件，成功或失败均可能改变文件位置。
 * @param[in] file_size 稳定文件的总字节数。
 * @param[in] loca_start loca 块起始字节偏移。
 * @param[in] header 已通过头部格式检查的头借用，决定二字节或四字节索引。
 * @return 计数在 1 到 8192 且所有检查通过为 true，短读或范围错误为 false。
 * @note 此处不解码字形位流，原生加载器继续负责字形和 kerning 的解析。
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

/**
 * @brief 打开独立读句柄，预检支持的 head、cmap、loca 与 glyf 边界。
 * @param[in] lvgl_path 有效 LVGL 路径借用，文件在预检和实际加载期间须由 owner 保持稳定。
 * @param[in] file_size stat 得到且可用 uint32_t 表示的文件字节数。
 * @return 本层预检通过为 true，打开、读取或已检查字段失败为 false。
 * @note 本层不验证全部字形位流和 kerning，退出前关闭自己打开的句柄，关闭结果不覆盖预检结果。
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


/**
 * @brief 归还尚未保留的 PSRAM 文件副本。
 * @param[in,out] buf 拥有指针槽，可 NULL，非空对象须由 heap_caps 分配。
 * @post 非空槽归零，原缓冲区归还，重复空槽调用无副作用。
 */
static void free_candidate_font_buffer(uint8_t **buf)
{
    if (buf && *buf) {
        heap_caps_free(*buf);
        *buf = NULL;
    }
}

/**
 * @brief 将稳定文件的指定长度完整复制到对齐 PSRAM 区。
 * @param[in] vfs_path 调用期 VFS 路径借用，NULL 或空串失败。
 * @param[in] size 要复制的非零字节数，来自本轮 stat。
 * @return 拥有的 size 字节缓冲区，调用者用 heap_caps_free 归还，打开、分配、短读或读取失败返回 NULL。
 * @note EINTR 重试，所有分支关闭独立 fd，关闭错误不改变已读取副本的结果。
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
            if (errno == EINTR) continue;
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

/**
 * @brief 尝试加载配置字体并选定当前字体，把被替换字体交回 UI 事务退休。
 * @param[out] out_retired_font 必填拥有输出槽，替换或回退时交付旧外部字体，同路径缓存命中置 NULL。
 * @return 外部字体可用为 true，加载失败为 false 并选用内置字体，输出槽 NULL 时不改变状态。
 * @pre owner 持有 LVGL 锁，排斥路径更新，文件及文件系统在预检和构造期间保持有效。
 * @note 当前原生加载器复制全部字体表并关闭 memfs，因此成功构造后立即释放文件副本。
 * @post owner 须在同一次锁内重绑全部旧字体使用者，再调用 board_ui_font_release_retired。
 */
bool board_ui_font_load_external(lv_font_t **out_retired_font)
{
    if (!out_retired_font) return false;
    *out_retired_font = s_external_font;
    s_current_font = font_builtin();

    struct stat st = {0};
    const char *vfs_path = font_vfs_path(s_font_path);
    char lvgl_path_buf[sizeof(s_font_path) + 2];
    const char *lvgl_path = font_lvgl_path(s_font_path, lvgl_path_buf, sizeof(lvgl_path_buf));

    if (lvgl_path && stat(vfs_path, &st) == 0) {
        char font_size_text[16];
        if (st.st_size > 0) {
            format_bytes((size_t)st.st_size, font_size_text, sizeof(font_size_text));
        } else {
            snprintf(font_size_text, sizeof(font_size_text), "%ldB", (long)st.st_size);
        }

        if (s_external_font && strcmp(s_loaded_font_path, lvgl_path) == 0) {
            s_current_font = s_external_font;
            *out_retired_font = NULL;
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
                    /* 原生构造已复制字形、cmap 与 kerning，文件副本到此结束借用。 */
                    free_candidate_font_buffer(&candidate_buf);
                    s_external_font = candidate_font;
                    s_current_font = s_external_font;
                    snprintf(s_loaded_font_path, sizeof(s_loaded_font_path), "%s", lvgl_path);
                    ESP_LOGI(TAG, "Loaded binfont from PSRAM buffer: %s, size=%s (%ldB)",
                             lvgl_path, font_size_text, (long)st.st_size);
                    return true;
                }

                ESP_LOGW(TAG, "lv_binfont_create_from_buffer failed: %s, size=%s (%ldB)",
                         lvgl_path, font_size_text, (long)st.st_size);
                free_candidate_font_buffer(&candidate_buf);
            }
#endif

            s_external_font = lv_binfont_create(lvgl_path);
            if (s_external_font) {
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
    s_external_font = NULL;
    s_loaded_font_path[0] = '\0';
    ESP_LOGW(TAG, "External font unavailable, fallback active: %s", s_font_path);
    return false;
}

/**
 * @brief 销毁已经从全部对象树移除的外部字体。
 * @param[in,out] font 拥有退休字体的槽，可 NULL，非空字体必须来自本模块的加载输出。
 * @post 字体及原生复制的所有字体表归还，输出槽清空。
 * @pre owner 持 LVGL 锁，旧字体的标签、绘制任务及其他借用均已结束。
 */
void board_ui_font_release_retired(lv_font_t **font)
{
    if (!font || !*font) return;
#if BOARD_UI_HAS_BINFONT
    lv_binfont_destroy(*font);
#endif
    *font = NULL;
}
