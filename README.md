# nTexts

TI-Nspire CX / CX II 的轻量纯文本阅读器，使用 Ndless SDK 构建。

## 功能

- 首页显示最近阅读 10 本，也可切换到 Documents 目录浏览器
- 从目录浏览器打开 `.txt` / `.txt.tns` 文件，兼容通过参数直接打开文件
- 流式读取文本，不一次性载入整本书
- BOM 优先识别编码；无 BOM 时采样验证 UTF-8，否则按 GB18030 解码
- 支持 UTF-8、GB18030、CRLF、LF、制表符和损坏字节替代显示
- 首次打开按当前字号/边距建立页面索引，缓存后再次打开更快
- 自动保存阅读进度，支持最多 20 个书签
- 搜索、继续查找、向前查找、百分比/页码/物理行号跳转
- 小/中/大字号，浅色/深色/护眼主题，窄/宽边距
- 文件信息显示路径、大小、编码、总行数、总页数及修改时间

正文通过 Ndless 的系统 Graphics Context 使用计算器 OS 字体绘制，UTF-8 会转换为 UTF-16；中文语言版 OS 可直接使用系统中文字形，无需在程序中打包完整中文字库。

按键：

- 左/右：翻页
- 上/下：按原 TXT 物理行移动
- Ctrl+上/下：跳到首尾
- Menu：功能菜单
- B：添加书签
- F：搜索
- G：跳转
- Esc：返回书库或上级目录

持久化数据位于 Documents 下的 `nTexts` 数据目录。全局设置、最近阅读、每书状态和页面索引分离保存，并使用临时文件加 `rename` 写入。

注意：Ndless 公开的 `show_msg_user_input` 会走 ASCII 包装层。本版已经把搜索词、历史和匹配逻辑按 UTF-16 保存与处理，但中文系统输入框的私有 UTF-16 入口需要在真机 OS 上确认符号后替换；在此之前，中文搜索可复用历史词，ASCII 搜索可直接输入。

## 构建

仓库提供了 WSL 构建脚本，会自动定位默认安装在
`$HOME/Ndless/ndless-sdk` 的 SDK，并兼容 SDK 与 WSL 中 Boost SONAME 不一致的问题：

```sh
chmod +x tools/build-wsl.sh
./tools/build-wsl.sh
```

如果 SDK 位于其他位置：`NDLESS_SDK=/path/to/ndless-sdk ./tools/build-wsl.sh`。

生成的 `nTexts.tns` 传入计算器后直接运行。文本文件建议命名为 `book.txt.tns`，以便 TI-Nspire 文件系统接受并显示它。

## 清理

```sh
./tools/build-wsl.sh clean
```
