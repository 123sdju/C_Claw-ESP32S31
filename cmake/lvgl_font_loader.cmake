# 将当前依赖中的两处字体 OOM 清理修正应用到构建目录副本。
# 源码仍由 LVGL 组件编译，保留其 include、配置、链接依赖与原始许可。
idf_component_get_property(cclaw_lvgl_lib lvgl__lvgl COMPONENT_LIB)
idf_component_get_property(cclaw_lvgl_dir lvgl__lvgl COMPONENT_DIR)
set(cclaw_font_source "${cclaw_lvgl_dir}/src/font/binfont_loader/lv_binfont_loader.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cclaw_font_source}")
file(READ "${cclaw_font_source}" cclaw_font_original)
set(cclaw_font_content "${cclaw_font_original}")

set(cclaw_font_allocate_before [=[    lv_font_t * font = lv_malloc_zeroed(sizeof(lv_font_t));
    LV_ASSERT_MALLOC(font);]=])
set(cclaw_font_allocate_after [=[    lv_font_t * font = lv_malloc_zeroed(sizeof(lv_font_t));
    /* 分配失败仍须关闭本次打开的文件，将失败交给字体候选事务处理。 */
    if(font == NULL) {
        lv_fs_close(&file);
        return NULL;
    }]=])
set(cclaw_font_destroy_before [=[    if(dsc == NULL) return;]=])
set(cclaw_font_destroy_after [=[    if(dsc == NULL) {
        /* descriptor 尚未分配时，font 自身仍是需要归还的拥有分配。 */
        lv_free(font);
        return;
    }]=])

foreach(cclaw_font_fix IN ITEMS allocate destroy)
    set(cclaw_font_before "${cclaw_font_${cclaw_font_fix}_before}")
    set(cclaw_font_after "${cclaw_font_${cclaw_font_fix}_after}")
    string(FIND "${cclaw_font_content}" "${cclaw_font_before}" cclaw_font_position)
    if(cclaw_font_position LESS 0)
        message(FATAL_ERROR "LVGL 字体加载器已变化，请核对 ${cclaw_font_fix} 的失败清理后更新构建修正")
    endif()
    string(LENGTH "${cclaw_font_before}" cclaw_font_match_length)
    math(EXPR cclaw_font_match_end "${cclaw_font_position} + ${cclaw_font_match_length}")
    string(SUBSTRING "${cclaw_font_content}" ${cclaw_font_match_end} -1 cclaw_font_tail)
    string(FIND "${cclaw_font_tail}" "${cclaw_font_before}" cclaw_font_duplicate)
    if(NOT cclaw_font_duplicate LESS 0)
        message(FATAL_ERROR "LVGL 字体修正出现多处候选，请核对 ${cclaw_font_fix} 的实际函数范围")
    endif()
    string(REPLACE "${cclaw_font_before}" "${cclaw_font_after}" cclaw_font_content "${cclaw_font_content}")
endforeach()

set(cclaw_font_generated "${CMAKE_CURRENT_BINARY_DIR}/cclaw-lvgl/lv_binfont_loader.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cclaw-lvgl")
# configure_file 仅在内容变化时更新副本，避免每次配置都触发整个链接。
set(cclaw_font_template "${CMAKE_CURRENT_BINARY_DIR}/cclaw-lvgl/lv_binfont_loader.c.in")
file(WRITE "${cclaw_font_template}" "${cclaw_font_content}")
configure_file("${cclaw_font_template}" "${cclaw_font_generated}" COPYONLY)
get_target_property(cclaw_lvgl_sources "${cclaw_lvgl_lib}" SOURCES)
list(FIND cclaw_lvgl_sources "${cclaw_font_source}" cclaw_font_source_index)
if(cclaw_font_source_index LESS 0)
    message(FATAL_ERROR "LVGL 字体原始单元未出现在组件源清单中，无法完成构建修正")
endif()
list(REMOVE_ITEM cclaw_lvgl_sources "${cclaw_font_source}")
list(APPEND cclaw_lvgl_sources "${cclaw_font_generated}")
set_property(TARGET "${cclaw_lvgl_lib}" PROPERTY SOURCES "${cclaw_lvgl_sources}")
target_include_directories("${cclaw_lvgl_lib}" PRIVATE "${cclaw_lvgl_dir}/src/font/binfont_loader")
