#ifndef NTEXTS_APP_STRINGS_H
#define NTEXTS_APP_STRINGS_H

#define L(zh, en, fr) (app_locale == 2 ? (fr) : app_locale == 1 ? (en) : (zh))

#define TXT_ENTER_CONTINUE L("Enter继续", "Enter to continue", "Enter pour continuer")
#define TXT_ESC_OR_ENTER_BACK L("Esc/Enter返回", "Esc/Enter back", "Esc/Enter retour")
#define TXT_ESC_OR_ENTER_STAY L("Esc/Enter停在此页", "Esc/Enter stay here", "Esc/Enter rester ici")
#define TXT_HOME_BROWSE_DOCS L("0浏览文档", "0 Browse files", "0 Parcourir les fichiers")
#define TXT_SETTINGS L("(-)设置", "(-) Settings", "(-) Réglages")
#define TXT_SEARCH_MODE_HINT L("搜索中: Enter/下 下一处 上 上一处 Esc退出", "Search: Enter/Down next Up prev Esc exit", "Recherche: Enter/Bas suiv. Haut préc. Esc fin")
#define TXT_SEARCH_NOT_FOUND L("没有找到这个词", "No matches", "Aucun résultat")
#define TXT_SEARCH_NO_NEXT L("没有找到下一个", "No next match", "Aucun résultat suivant")
#define TXT_SEARCH_NO_PREV L("没有找到上一个", "No previous match", "Aucun résultat précédent")
#define TXT_TUTORIAL_DONE L("教学进度", "Tutorial progress", "Progression du tutoriel")
#define TXT_TUTORIAL_RESET_DONE L("已重置教学进度，下次启动会重新显示", "Tutorial resets on next launch", "Tutoriel réinitialisé au prochain démarrage")
#define TXT_TUTORIAL_SKIP_DONE L("已跳过所有教学，可在设置中重置", "Tutorial skipped; reset in settings", "Tutoriel ignoré ; réinitialiser dans Réglages")
#define TXT_CHAPTER_DIRECTORY L("章节目录", "Chapters", "Chapitres")
#define TXT_CHAPTER_DIRECTORY_LIMIT L("章节目录（前1024项）", "Chapters (first 1024)", "Chapitres (1024 premiers)")
#define TXT_NO_CHAPTERS L("未检测到章节标题", "No chapter headings found", "Aucun titre de chapitre détecté")
#define TXT_CHAPTER_RULES L("章节规则", "Chapter rules", "Règles de chapitre")
#define TXT_ADD_CHAPTER_RULE L("添加规则", "Add rule", "Ajouter une règle")
#define TXT_RULE_INPUT_HINT L("输入全局规则（*多字符，?单字符）", "Rule: * many chars, ? one char", "Règle : * plusieurs, ? un caractère")
#define TXT_RULE_INVALID L("规则不能为空或必须包含普通字符", "Rule needs ordinary characters", "La règle doit contenir un caractère normal")
#define TXT_RULE_DUPLICATE L("规则已存在", "Rule already exists", "Règle déjà présente")
#define TXT_RULE_FULL L("规则已满（最多8条）", "Limit: 8 rules", "Maximum : 8 règles")

#define TXT_FOOTER_HOME L("Enter打开  0浏览目录  (-)设置  Esc退出", "Enter open  0 browse  (-) settings  Esc exit", "Enter ouvrir  0 fichiers  (-) régl.  Esc quitter")
#define TXT_FOOTER_BROWSER L("Enter打开  F筛选  Esc上级/返回", "Enter open  F filter  Esc up/back", "Enter ouvrir  F filtrer  Esc retour")
#define TXT_FOOTER_LIST L("1-9直达  Enter选择  Esc返回", "1-9 select  Enter open  Esc back", "1-9 choisir  Enter ouvrir  Esc retour")
#endif
