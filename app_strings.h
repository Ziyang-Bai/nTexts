#ifndef NTEXTS_APP_STRINGS_H
#define NTEXTS_APP_STRINGS_H

#define L(zh, en, fr) (app_locale == 2 ? (fr) : app_locale == 1 ? (en) : (zh))

#define TXT_ENTER_CONTINUE L("Enter继续", "Enter to continue", "Enter pour continuer")
#define TXT_ESC_OR_ENTER_BACK L("Esc/Enter返回", "Esc/Enter back", "Esc/Enter retour")
#define TXT_ESC_OR_ENTER_STAY L("Esc/Enter停在此页", "Esc/Enter stay here", "Esc/Enter rester ici")
#define TXT_HOME_BROWSE_DOCS L("0浏览文档", "0 Browse files", "0 Parcourir les fichiers")
#define TXT_SETTINGS L("(-)设置", "(-) Settings", "(-) Réglages")
#define TXT_SEARCH_MODE_HINT L("Enter/下 下一处 上 上一处 Esc结束 H:提示", "Enter/Down next Up prev Esc end H:tips", "Enter/Bas suiv. Haut préc. Esc fin H:aide")
#define TXT_SEARCH_NOT_FOUND L("没有找到这个词", "No matches", "Aucun résultat")
#define TXT_SEARCH_NO_NEXT L("没有找到下一个", "No next match", "Aucun résultat suivant")
#define TXT_SEARCH_NO_PREV L("没有找到上一个", "No previous match", "Aucun résultat précédent")
#define TXT_HINTS_CLOSE_ONCE L("Enter/Esc：本页提示本次启动不再自动显示", "Enter/Esc: close here until next launch", "Enter/Esc : fermer ici pour ce lancement")
#define TXT_HINTS_DISABLE L("0：永久关闭所有自动提示", "0: stop ALL auto hints permanently", "0 : arrêter TOUTE aide auto définitivement")
#define TXT_HINTS_REOPEN L("H：关闭自动提示后仍可重开", "H: reopen even with auto hints off", "H : revoir même sans aide automatique")
#define TXT_HINTS_RESTORE L("设置第7项：重新开启自动提示", "Settings 7: restore automatic hints", "Réglage 7 : réactiver l'aide automatique")
#define TXT_CHAPTER_DIRECTORY L("章节目录", "Chapters", "Chapitres")
#define TXT_CHAPTER_DIRECTORY_LIMIT L("章节目录（前1024项）", "Chapters (first 1024)", "Chapitres (1024 premiers)")
#define TXT_NO_CHAPTERS L("未检测到章节标题", "No chapter headings found", "Aucun titre de chapitre détecté")
#define TXT_CHAPTER_RULES L("章节规则", "Chapter rules", "Règles de chapitre")
#define TXT_ADD_CHAPTER_RULE L("添加规则", "Add rule", "Ajouter une règle")
#define TXT_RULE_INPUT_HINT L("输入全局规则（*多字符，?单字符）", "Rule: * many chars, ? one char", "Règle : * plusieurs, ? un caractère")
#define TXT_RULE_INVALID L("规则不能为空或必须包含普通字符", "Rule needs ordinary characters", "La règle doit contenir un caractère normal")
#define TXT_RULE_DUPLICATE L("规则已存在", "Rule already exists", "Règle déjà présente")
#define TXT_RULE_FULL L("规则已满（最多8条）", "Limit: 8 rules", "Maximum : 8 règles")

#define TXT_FOOTER_HOME L("Enter打开 0文档 (-)设置 H提示 Esc退出", "Enter open 0 files (-)settings H:tips Esc exit", "Enter ouvrir 0 fichiers (-)régl. H:aide Esc")
#define TXT_FOOTER_BROWSER L("Enter打开 F筛选 H提示 Esc上级/返回", "Enter open F filter H:tips Esc up/back", "Enter ouvrir F filtrer H:aide Esc retour")
#define TXT_FOOTER_LIST L("1-9直达  Enter选择  Esc返回", "1-9 select  Enter open  Esc back", "1-9 choisir  Enter ouvrir  Esc retour")
#endif
