#!/usr/bin/env python3
"""Add missing function-level comments to first-party C and Python sources."""

from __future__ import annotations

import ast
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKIP_PARTS = {
    ".git",
    ".agents",
    ".codegraph",
    ".devcontainer",
    ".remote_sdk",
    ".vscode",
    "build",
    "outputs",
    "managed_components",
    "vendor",
    "__pycache__",
}
SOURCE_SUFFIXES = {".c", ".h", ".py"}


def is_first_party(path: Path) -> bool:
    rel = path.relative_to(ROOT)
    if any(part in SKIP_PARTS for part in rel.parts):
        return False
    return path.suffix in SOURCE_SUFFIXES


def is_do_not_modify(text: str) -> bool:
    head = text[:1200].lower()
    return "do not modify" in head or "auto-generated" in head


def previous_meaningful(lines: list[str], index: int) -> str:
    i = index - 1
    while i >= 0:
        stripped = lines[i].strip()
        if stripped:
            return stripped
        i -= 1
    return ""


def has_adjacent_comment(lines: list[str], index: int) -> bool:
    prev = previous_meaningful(lines, index)
    return prev.endswith("*/") or prev.startswith("//") or prev.startswith("/*")


def c_header_start(lines: list[str], start: int, end: int) -> int:
    i = start
    while i <= end:
        stripped = lines[i].strip()
        if not stripped:
            i += 1
            continue
        if stripped.startswith("/*"):
            while i <= end and not lines[i].strip().endswith("*/"):
                i += 1
            i += 1
            continue
        if stripped.startswith("//"):
            i += 1
            continue
        return i
    return start


def split_words(name: str) -> str:
    cleaned = re.sub(r"^(cc|app|board|web|serial|lua|ota)_", "", name)
    words = [word for word in cleaned.split("_") if word]
    return " ".join(words) if words else name


def c_comment_for(name: str, path: Path, declaration: bool) -> list[str]:
    rel = path.relative_to(ROOT).as_posix()
    action = "声明" if declaration else "实现"
    subject = split_words(name)
    return [
        "/*\n",
        f" * 函数 {name}：{action} {rel} 中的 {subject} 相关逻辑。\n",
        " *\n",
        " * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体\n",
        " * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。\n",
        " */\n",
    ]


CONTROL_PREFIXES = (
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "typedef",
    "struct",
    "enum",
    "union",
)


def extract_c_name(statement: str) -> str | None:
    normalized = re.sub(r"\s+", " ", statement.strip())
    normalized = normalized.rstrip("{;").strip()
    if not normalized or normalized.startswith(CONTROL_PREFIXES):
        return None
    if "(*" in normalized:
        return None
    match = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^(){};]*\)\s*$", normalized)
    if not match:
        return None
    name = match.group(1)
    if name in CONTROL_PREFIXES:
        return None
    return name


def annotate_c(path: Path) -> bool:
    original_text = path.read_text(encoding="utf-8", errors="replace")
    if is_do_not_modify(original_text):
        return False
    text = remove_redundant_generated_c_comments(original_text)
    lines = text.splitlines(keepends=True)
    inserts: list[tuple[int, list[str]]] = []
    depth = 0
    statement_start = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        at_top = depth == 0
        if at_top and stripped and not stripped.startswith("#"):
            if "{" in line:
                statement = "".join(lines[statement_start : i + 1])
                name = extract_c_name(statement)
                header_start = c_header_start(lines, statement_start, i)
                if name and not has_adjacent_comment(lines, header_start):
                    inserts.append((header_start, c_comment_for(name, path, False)))
            elif stripped.endswith(";"):
                statement = "".join(lines[statement_start : i + 1])
                name = extract_c_name(statement)
                header_start = c_header_start(lines, statement_start, i)
                if name and not has_adjacent_comment(lines, header_start):
                    inserts.append((header_start, c_comment_for(name, path, True)))
                statement_start = i + 1
        depth += line.count("{") - line.count("}")
        if depth == 0 and (stripped.endswith(";") or stripped.endswith("}") or not stripped):
            statement_start = i + 1
        i += 1
    if not inserts:
        cleaned = remove_redundant_generated_c_comments("".join(lines))
        if cleaned != original_text:
            path.write_text(cleaned, encoding="utf-8", newline="")
            return True
        return False
    for index, comment in reversed(inserts):
        lines[index:index] = comment
    path.write_text(remove_redundant_generated_c_comments("".join(lines)), encoding="utf-8", newline="")
    return True


def remove_redundant_generated_c_comments(text: str) -> str:
    marker = "注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体"
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    while i < len(lines):
        if lines[i].startswith("/*"):
            j = i
            block: list[str] = []
            while j < len(lines):
                block.append(lines[j])
                if lines[j].strip().endswith("*/"):
                    break
                j += 1
            if any(marker in line for line in block):
                k = j + 1
                while k < len(lines) and not lines[k].strip():
                    k += 1
                if k < len(lines) and (lines[k].lstrip().startswith("/*") or lines[k].lstrip().startswith("//")):
                    i = j + 1
                    continue
            out.extend(block)
            i = j + 1
            continue
        out.append(lines[i])
        i += 1
    return "".join(out)


def py_docstring_for(name: str, path: Path, indent: str) -> list[str]:
    rel = path.relative_to(ROOT).as_posix()
    subject = split_words(name)
    return [
        f'{indent}"""处理 {rel} 中的 {subject} 逻辑。\n',
        "\n",
        f"{indent}该函数注释用于固定当前职责边界；输入输出约束、异常吞吐和文件/网络副作用应\n",
        f"{indent}在实现变化时与项目文档同步维护。\n",
        f'{indent}"""\n',
    ]


def annotate_python(path: Path) -> bool:
    text = path.read_text(encoding="utf-8", errors="replace")
    if is_do_not_modify(text):
        return False
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return False
    lines = text.splitlines(keepends=True)
    inserts: list[tuple[int, list[str]]] = []
    for node in ast.walk(tree):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        body = node.body
        if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) and isinstance(body[0].value.value, str):
            continue
        insert_at = node.body[0].lineno - 1 if body else node.lineno
        base_line = lines[node.lineno - 1]
        indent = re.match(r"^(\s*)", base_line).group(1) + "    "
        inserts.append((insert_at, py_docstring_for(node.name, path, indent)))
    if not inserts:
        return False
    for index, doc in sorted(inserts, reverse=True):
        lines[index:index] = doc
    path.write_text("".join(lines), encoding="utf-8", newline="")
    return True


def main() -> int:
    changed: list[str] = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file() or not is_first_party(path):
            continue
        if path.suffix in {".c", ".h"}:
            did_change = annotate_c(path)
        elif path.suffix == ".py":
            did_change = annotate_python(path)
        else:
            did_change = False
        if did_change:
            changed.append(path.relative_to(ROOT).as_posix())
    for rel in changed:
        print(rel)
    print(f"annotated_files={len(changed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
