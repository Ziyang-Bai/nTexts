#!/usr/bin/env python3
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "app.c").read_text(encoding="utf-8")
STRINGS = (ROOT / "app_strings.h").read_text(encoding="utf-8")


def body_of(function_name):
    match = re.search(r"static\s+[\w\s\*]+?\s+" + re.escape(function_name) + r"\s*\([^;]*\)\s*\{", APP)
    if not match:
        raise AssertionError(f"could not find definition for {function_name}")
    brace = APP.index("{", match.start())
    depth = 0
    for i in range(brace, len(APP)):
        if APP[i] == "{":
            depth += 1
        elif APP[i] == "}":
            depth -= 1
            if depth == 0:
                return APP[brace + 1:i]
    raise AssertionError(f"could not parse {function_name}")


def test_settings_menu_has_only_reset_tutorial_control():
    settings = body_of("settings_menu")
    assert "重置教学进度" in settings
    assert "教学中心" not in settings
    assert "跳过所有教学" not in settings
    assert "key <= 26" in settings
    assert "char items[6][72]" in settings


def test_reader_tutorial_does_not_draw_status_under_overlay():
    draw_page = body_of("draw_page")
    assert "if (r->tutorial_mode) {\n        draw_tutorial_overlay(r);" in draw_page
    tutorial_branch = draw_page.split("if (r->tutorial_mode)", 1)[1].split("} else {", 1)[0]
    assert "draw_text(r->gc, status" not in tutorial_branch
    assert "gui_gc_drawLine" not in tutorial_branch


def test_reader_tutorial_overlay_stays_above_footer():
    overlay = body_of("draw_tutorial_overlay")
    assert "int box_y = 126;" in overlay
    assert "box_y = 164" not in overlay
    assert "SCREEN_H - 15" not in overlay


def test_interactive_tutorial_requires_real_keys_and_keeps_skip_inside_tutorial():
    tutorial = body_of("tutorial_center_page")
    assert "TutorialStep steps[]" in tutorial
    assert "按 %s 继续" in tutorial
    assert "key == step->key" in tutorial
    assert "0跳过所有教学" in tutorial
    assert "TUTORIAL_ALL_SKIPPED" in tutorial


def test_tutorial_strings_remain_available_for_messages():
    assert "TXT_TUTORIAL_RESET_DONE" in STRINGS
    assert "TXT_TUTORIAL_SKIP_DONE" in STRINGS


if __name__ == "__main__":
    tests = [
        test_settings_menu_has_only_reset_tutorial_control,
        test_reader_tutorial_does_not_draw_status_under_overlay,
        test_reader_tutorial_overlay_stays_above_footer,
        test_interactive_tutorial_requires_real_keys_and_keeps_skip_inside_tutorial,
        test_tutorial_strings_remain_available_for_messages,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} static tests passed")
