"""验证构建副本中的真实 LVGL 字体创建/销毁函数及 OOM 清理。

原生文件和分配接口由本地模型提供，字体二进制解析与设备堆另行验证。
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def production_function(text, signature):
    """按真实定义边界读取待验证函数原文。"""
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


MODEL = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {int id;} lv_fs_file_t;
typedef int lv_fs_res_t;
typedef struct {void *glyph_ids,*values;} lv_font_fmt_txt_kern_pair_t;
typedef struct {void *class_pair_values,*left_class_mapping,*right_class_mapping;} lv_font_fmt_txt_kern_classes_t;
typedef struct {void *glyph_id_ofs_list,*unicode_list;} lv_font_fmt_txt_cmap_t;
typedef struct {int kern_classes,cmap_num;void *kern_dsc,*glyph_bitmap,*glyph_dsc;lv_font_fmt_txt_cmap_t *cmaps;} lv_font_fmt_txt_dsc_t;
typedef struct {lv_font_fmt_txt_dsc_t *dsc;} lv_font_t;
#define LV_FS_RES_OK 0
#define LV_FS_MODE_RD 1
#define LV_ASSERT_NULL(pointer) assert(pointer)
#define LV_ASSERT_MALLOC(pointer) assert(pointer)
#define LV_LOG_WARN(...) ((void)0)
static int allocations,fail_at,live,files,load_calls;
static void *lv_malloc(size_t bytes) {
    if(++allocations==fail_at)return NULL;
    void *pointer=malloc(bytes);if(pointer)live++;return pointer;
}
static void *lv_malloc_zeroed(size_t bytes) {
    void *pointer=lv_malloc(bytes);if(pointer)memset(pointer,0,bytes);return pointer;
}
static void lv_free(void *pointer) {if(pointer){assert(live>0);live--;free(pointer);}}
static lv_fs_res_t lv_fs_open(lv_fs_file_t *file,const char *path,int mode) {
    assert(path&&mode==LV_FS_MODE_RD);file->id=++files;return LV_FS_RES_OK;
}
static lv_fs_res_t lv_fs_close(lv_fs_file_t *file) {
    assert(file->id&&files==1);file->id=0;files--;return LV_FS_RES_OK;
}
static bool lvgl_load_font(lv_fs_file_t *file,lv_font_t *font) {
    assert(file->id&&font);load_calls++;
    font->dsc=lv_malloc_zeroed(sizeof(*font->dsc));
    if(!font->dsc)return false;
    font->dsc->glyph_bitmap=lv_malloc(16);
    return font->dsc->glyph_bitmap!=NULL;
}
void lv_binfont_destroy(lv_font_t *font);
'''


class FontLoaderCleanup(unittest.TestCase):
    def test_generated_loader_oom(self):
        """执行真实构建修正，核对首分配、descriptor 和后续部分失败的释放。"""
        original = ROOT / "managed_components/lvgl__lvgl/src/font/binfont_loader/lv_binfont_loader.c"
        if not original.is_file():
            raise RuntimeError("需要已解析的本地 LVGL 依赖源码")
        with tempfile.TemporaryDirectory(prefix="cclaw-font-loader-") as folder:
            work = Path(folder)
            project = work / "CMakeLists.txt"
            project.write_text('''cmake_minimum_required(VERSION 3.22)
project(font_cleanup LANGUAGES C)
set(original "''' + original.as_posix() + '''")
add_library(font_dependency STATIC "${original}")
function(idf_component_get_property output component property)
  if(property STREQUAL "COMPONENT_LIB")
    set(${output} font_dependency PARENT_SCOPE)
  else()
    set(${output} "''' + (ROOT / "managed_components/lvgl__lvgl").as_posix() + '''" PARENT_SCOPE)
  endif()
endfunction()
include("''' + (ROOT / "cmake/lvgl_font_loader.cmake").as_posix() + '''")
get_target_property(actual font_dependency SOURCES)
file(WRITE "${CMAKE_BINARY_DIR}/selected.txt" "${actual}")
''', encoding="utf-8")
            subprocess.run(["cmake", "-S", str(work), "-B", str(work / "build")], check=True, stdout=subprocess.PIPE)
            selected = (work / "build/selected.txt").read_text(encoding="utf-8")
            self.assertEqual(selected, (work / "build/cclaw-lvgl/lv_binfont_loader.c").as_posix())
            patched = Path(selected).read_text(encoding="utf-8")
            original_text = original.read_text(encoding="utf-8")
            prefix = original_text[:original_text.index("lv_font_t * lv_binfont_create(")]
            self.assertTrue(patched.startswith(prefix))
            code = MODEL + production_function(patched, "lv_font_t * lv_binfont_create(")
            code += production_function(patched, "void lv_binfont_destroy(")
            code += r'''
int main(void) {
    for(int failure=1;failure<=3;failure++) {
        allocations=0;fail_at=failure;load_calls=0;
        assert(lv_binfont_create("font.bin")==NULL);
        assert(!files&&!live);
        assert(load_calls==(failure==1?0:1));
    }
    allocations=0;fail_at=0;
    lv_font_t *font=lv_binfont_create("font.bin");
    assert(font&&font->dsc&&font->dsc->glyph_bitmap&&!files&&live==3);
    lv_binfont_destroy(font);lv_binfont_destroy(NULL);assert(!live);
    puts("LVGL font first allocation, descriptor and partial cleanup: PASS");return 0;
}
'''
            (work / "test.c").write_text(code, encoding="utf-8")
            command = ["cc", "-std=c11", "-Wall", "-Wextra", "-o", str(work / "test"), str(work / "test.c")]
            if os.environ.get("CCLAW_TEST_SANITIZERS") == "1":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            subprocess.run(command, check=True)
            subprocess.run([str(work / "test")], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
