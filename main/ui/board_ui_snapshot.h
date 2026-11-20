#pragma once

#include <stdbool.h>

/**
 * @brief 创建并发布进程期快照锁，可在分配失败后重试。
 * @return 锁已可用为 true，分配失败为 false。
 * @pre 初始化 owner 串行调用，锁一经发布保留到进程结束。
 * @note 发布前更新接口安全跳过，JSON 读取返回 NULL，发布后读写由同一互斥锁同步。
 */
bool board_ui_snapshot_init(void);
/**
 * @brief 清空对话显示快照。
 * @post 锁内释放全部对话行，未初始化或同步失败时跳过。
 */
void board_ui_snapshot_clear_conversation(void);
/**
 * @brief 清空启动日志显示快照。
 * @post 锁内释放全部启动行，未初始化或同步失败时跳过。
 */
void board_ui_snapshot_clear_boot(void);
/**
 * @brief 清空工具输出显示快照。
 * @post 锁内释放全部工具行，未初始化或同步失败时跳过。
 */
void board_ui_snapshot_clear_tool(void);
/**
 * @brief 清空绘制命令显示快照。
 * @post 锁内释放全部绘制行，未初始化或同步失败时跳过。
 */
void board_ui_snapshot_clear_draw(void);
/**
 * @brief 锁内复制状态栏文本。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串，最多保留 1023 字节。
 * @post 成功时状态 NUL 终止，未初始化或同步失败保持原状态。
 */
void board_ui_snapshot_set_status(const char *text);
/**
 * @brief 向最多 80 行的启动显示环追加一行。
 * @param[in] line 调用期 NUL 文本借用，NULL 忽略，最多复制 512 字节。
 * @post 满环覆盖最早一行，分配或同步失败保持原内容。
 */
void board_ui_snapshot_append_boot(const char *line);
/**
 * @brief 向最多 80 行的对话显示环追加一行。
 * @param[in] line 调用期 NUL 文本借用，NULL 忽略，最多复制 512 字节。
 * @post 满环覆盖最早一行，分配或同步失败保持原内容。
 */
void board_ui_snapshot_append_conversation(const char *line);
/**
 * @brief 替换对话显示末行，空环时安装首行。
 * @param[in] line 合并后的完整 NUL 文本借用，NULL 忽略，最多复制 512 字节。
 * @post 新副本就绪后才归还旧行，失败保持原内容。
 */
void board_ui_snapshot_replace_last_conversation(const char *line);
/**
 * @brief 向最多 80 行的工具输出显示环追加一行。
 * @param[in] line 调用期 NUL 文本借用，NULL 忽略，最多复制 512 字节。
 * @post 满环覆盖最早一行，分配或同步失败保持原内容。
 */
void board_ui_snapshot_append_tool(const char *line);
/**
 * @brief 将格式化文本追加到最多 80 行的绘制显示环。
 * @param[in] fmt 调用期 printf 格式借用，NULL 忽略。
 * @param[in] ... 与格式匹配的参数和调用期字符串借用。
 * @post 格式化错误时跳过，成功最多保留 512 字节，复制失败保持原环。
 */
void board_ui_snapshot_append_drawf(const char *fmt, ...);
/**
 * @brief 在同一锁定区间编码全部显示环和状态栏，交付独立 JSON 副本。
 * @param[in] ready 按值写入快照的 UI 就绪标志，其读取由调用者同步。
 * @return 拥有的 NUL JSON，调用者 free，未初始化、同步或任一分配失败为 NULL。
 * @note 每行截取为有效 UTF-8 输入的完整字符前缀，其他非控制字节原样编码，接口不校验输入编码。
 */
char *board_ui_screen_snapshot_json(bool ready);
