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


def test_old_reader_tutorial_flow_is_removed():
    forbidden = [
        "tutorial_mode",
        "tutorial_step",
        "draw_tutorial_overlay",
        "create_tutorial_demo",
        "cleanup_tutorial_demo",
        "TUTORIAL_DEMO_FILE",
        "TXT_TUTORIAL_STEP",
    ]
    for token in forbidden:
        assert token not in APP
        assert token not in STRINGS
    assert not re.search(r"open_reader\([^;\n]+,\s*[01]\)", APP)


def test_interactive_tutorial_requires_real_keys_and_keeps_skip_inside_tutorial():
    tutorial = body_of("tutorial_center_page")
    assert "TutorialStep steps[]" in tutorial
    assert "panel_y = step->y > 118 ? 18 : 136;" in tutorial
    assert "按%s继续" in tutorial
    assert "key == step->key" in tutorial
    assert "0跳过" in tutorial
    assert "FOOTER_Y" not in tutorial
    assert "step->body3" not in tutorial
    assert "TUTORIAL_ALL_SKIPPED" in tutorial
    assert tutorial.count("{TUTORIAL_SCREEN_") >= 28


def test_interactive_tutorial_simulates_results_after_keys():
    tutorial = body_of("tutorial_center_page")
    for screen in [
        "TUTORIAL_SCREEN_BROWSER_SELECTED",
        "TUTORIAL_SCREEN_READER_PAGE2",
        "TUTORIAL_SCREEN_READER_TOP",
        "TUTORIAL_SCREEN_SEARCH_NEXT",
        "TUTORIAL_SCREEN_JUMP",
        "TUTORIAL_SCREEN_JUMP_INPUT",
        "TUTORIAL_SCREEN_JUMP_RESULT",
        "TUTORIAL_SCREEN_BOOKMARK",
        "TUTORIAL_SCREEN_BOOKMARK_LIST",
        "TUTORIAL_SCREEN_BOOKMARK_ACTION",
        "TUTORIAL_SCREEN_BOOKMARK_JUMP",
        "TUTORIAL_SCREEN_BOOKMARK_DELETE",
        "TUTORIAL_SCREEN_FILE_INFO",
        "TUTORIAL_SCREEN_HOME_RECENT",
        "TUTORIAL_SCREEN_SETTINGS_FONT",
        "TUTORIAL_SCREEN_SETTINGS_THEME",
        "TUTORIAL_SCREEN_SETTINGS_MARGIN",
    ]:
        assert screen in tutorial
    assert "教学中心" not in tutorial
    assert "跳过所有教学" not in tutorial


def test_tutorial_mock_reader_uses_wrapping_for_long_lines():
    mock = body_of("tutorial_mock_screen")
    assert 'draw_wrapped_text(gc, "按快捷键可以搜索、跳转、书签和打开菜单。"' in mock
    assert "这是第二页。刚才按右键以后，页面已经前进。" in mock
    assert "已在当前位置添加书签。" in mock
    assert "书签已删除" in mock
    assert "输入: 50" in mock
    assert "文件信息" in mock
    assert "Ctrl+下 已经跳到结尾。" in mock
    assert "TXT_SEARCH_MODE_HINT" not in mock
    assert "教学中心" not in mock
    assert "跳过所有教学" not in mock
    assert "Del删除" not in mock


def test_tutorial_logs_steps_and_keys_for_serial_debugging():
    tutorial = body_of("tutorial_center_page")
    assert 'app_log("tutorial", "step %d/%d title=%s expect=%s ctrl=%d"' in tutorial
    assert 'app_log("tutorial", "key step=%d got=%d ctrl=%d expect=%d need_ctrl=%d"' in tutorial


def test_search_boundary_messages_distinguish_current_hit_from_no_match():
    search = body_of("do_search")
    assert "continuing = reuse && r->hit_offset;" in search
    assert "continuing ? TXT_SEARCH_NO_NEXT : TXT_SEARCH_NOT_FOUND" in search
    assert "continuing ? TXT_SEARCH_NO_PREV : TXT_SEARCH_NOT_FOUND" in search
    assert "已到末尾，未找到" not in search
    assert "已到开头，未找到" not in search
    assert "TXT_SEARCH_NOT_FOUND" in STRINGS
    assert "TXT_SEARCH_NO_NEXT" in STRINGS
    assert "TXT_SEARCH_NO_PREV" in STRINGS


def test_tutorial_strings_remain_available_for_messages():
    assert "TXT_TUTORIAL_RESET_DONE" in STRINGS
    assert "TXT_TUTORIAL_SKIP_DONE" in STRINGS


if __name__ == "__main__":
    tests = [
        test_settings_menu_has_only_reset_tutorial_control,
        test_old_reader_tutorial_flow_is_removed,
        test_interactive_tutorial_requires_real_keys_and_keeps_skip_inside_tutorial,
        test_interactive_tutorial_simulates_results_after_keys,
        test_tutorial_mock_reader_uses_wrapping_for_long_lines,
        test_tutorial_logs_steps_and_keys_for_serial_debugging,
        test_search_boundary_messages_distinguish_current_hit_from_no_match,
        test_tutorial_strings_remain_available_for_messages,
    ]
    for test in tests:
        test()
    print(f"{len(tests)} static tests passed")
